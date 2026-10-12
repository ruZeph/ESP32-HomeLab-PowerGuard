#include "ups.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "leds.h"
#include "power_sensor.h"

#include "event_log.h"
#include "mqtt_manager.h"
#include "shutdown_client.h"
#include "wake_on_lan.h"
#include "wifi_manager.h"

#define TAG "UPS"

typedef enum
{
    CONTROLLER_IDLE,
    CONTROLLER_COUNTDOWN,
    CONTROLLER_WAITING_FOR_HOST_DOWN,
    CONTROLLER_HOST_DOWN_WAITING_FOR_POWER,
    CONTROLLER_WAITING_FOR_HOST_UP
} controller_state_t;

typedef struct
{
    power_state_t powerState;
    controller_state_t controllerState;
    TimerHandle_t shutdownTimer;
    TickType_t nextHostCheckTick;
    TickType_t nextWolRetryTick;
    TickType_t countdownStartTick;
    uint32_t wolRetriesSent;
    uint32_t shutdownRetriesSent;
    TickType_t nextShutdownRetryTick;
    bool startupRecoveryPending;
} ups_controller_t;

static ups_controller_t ups = {
    .powerState = POWER_UNKNOWN,
    .controllerState = CONTROLLER_IDLE,
    .shutdownTimer = NULL,
    .nextHostCheckTick = 0,
    .nextWolRetryTick = 0,
    .shutdownRetriesSent = 0,
    .nextShutdownRetryTick = 0,
    .startupRecoveryPending = true,
};

static portMUX_TYPE s_ups_lock = portMUX_INITIALIZER_UNLOCKED;

static void on_power_lost(void);
static void on_power_restored(void);
static void handle_power_change(power_state_t state);
static void start_countdown(void);

static void send_wake_signal(void);
static void process_host_state(void);
static void process_startup_recovery(void);
static void log_host_wait_state(const char *message);
static bool host_check_is_due(void);
static void schedule_next_host_check(uint32_t delay_ms);
static void shutdown_timer_cb(TimerHandle_t timer);

static const char *s_last_host_wait_log = NULL;
static const uint32_t HOST_CHECK_RETRY_INTERVAL_MS = 3000;
static const uint32_t WOL_RETRY_INTERVAL_MS = 5000;
static const uint32_t MAX_WOL_RETRIES = 20;

/* After this many 15 s shutdown retries the controller stops hammering the
   host but keeps trying on a slow cadence: power is out and the host is still
   up, so giving up entirely would be worse than retrying less often. */
static const uint32_t MAX_SHUTDOWN_RETRIES = 20;
static const uint32_t SHUTDOWN_RETRY_BACKOFF_MS = 300000;

/* Remaining seconds most recently published, so the countdown is only sent
   when the value changes. UINT32_MAX means nothing has been published yet. */
static uint32_t s_last_published_countdown = UINT32_MAX;

static void shutdown_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    bool should_send = false;

    portENTER_CRITICAL(&s_ups_lock);
    if (ups.controllerState == CONTROLLER_COUNTDOWN)
    {
        ups.controllerState = CONTROLLER_WAITING_FOR_HOST_DOWN;
        should_send = true;
    }
    portEXIT_CRITICAL(&s_ups_lock);

    if (should_send)
    {
        ESP_LOGE(TAG, "Shutdown Triggered");
        mqtt_publish_countdown("0");
        mqtt_publish_last_event("shutdown triggered");
        event_log_record(EVT_SHUTDOWN_TRIGGERED, 0);

        ups.shutdownRetriesSent = 0;
        ups.nextShutdownRetryTick = xTaskGetTickCount() + pdMS_TO_TICKS(15000);

        if (shutdown_client_send() != ESP_OK)
        {
            ESP_LOGE(TAG, "Shutdown request failed");
            event_log_record(EVT_SHUTDOWN_FAILED, 0);
            /* Do not revert to IDLE here! The host might still be shutting down 
               since it drops the HTTP connection instantly. Let process_host_state 
               confirm whether the host goes down or stays up. */
        }
    }
}

static void start_countdown(void)
{
    bool can_start = false;

    portENTER_CRITICAL(&s_ups_lock);
    if (ups.controllerState == CONTROLLER_IDLE)
    {
        ups.controllerState = CONTROLLER_COUNTDOWN;
        can_start = true;
    }
    portEXIT_CRITICAL(&s_ups_lock);

    if (!can_start)
    {
        return;
    }

    TickType_t delay_ticks = pdMS_TO_TICKS(g_ups_config.shutdown_delay_seconds * 1000UL);

    if (xTimerChangePeriod(ups.shutdownTimer, delay_ticks, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to arm shutdown timer");
        portENTER_CRITICAL(&s_ups_lock);
        ups.controllerState = CONTROLLER_IDLE;
        portEXIT_CRITICAL(&s_ups_lock);
        return;
    }

    if (xTimerStart(ups.shutdownTimer, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to start shutdown timer");
        portENTER_CRITICAL(&s_ups_lock);
        ups.controllerState = CONTROLLER_IDLE;
        portEXIT_CRITICAL(&s_ups_lock);
        return;
    }

    ups.countdownStartTick = xTaskGetTickCount();

    ESP_LOGW(TAG, "Power LOST");
    ESP_LOGW(TAG, "Shutdown countdown started (%lu s)",
             (unsigned long)g_ups_config.shutdown_delay_seconds);

    char secs[12];
    snprintf(secs, sizeof(secs), "%lu",
             (unsigned long)g_ups_config.shutdown_delay_seconds);
    mqtt_publish_power_state("lost");
    mqtt_publish_countdown(secs);
    mqtt_publish_last_event("power lost, countdown started");
    event_log_record(EVT_POWER_LOST, 0);
}

void ups_cancel_countdown(void)
{
    bool was_countdown = false;

    portENTER_CRITICAL(&s_ups_lock);
    if (ups.controllerState == CONTROLLER_COUNTDOWN)
    {
        ups.controllerState = CONTROLLER_IDLE;
        was_countdown = true;
    }
    portEXIT_CRITICAL(&s_ups_lock);

    if (!was_countdown)
    {
        return;
    }

    if (xTimerStop(ups.shutdownTimer, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to stop shutdown timer");
    }

    ESP_LOGI(TAG, "Shutdown cancelled");
    mqtt_publish_countdown("idle");
    mqtt_publish_last_event("shutdown cancelled");
    event_log_record(EVT_SHUTDOWN_CANCELLED, 0);
}

void ups_force_shutdown(void)
{
    bool should_send = false;

    portENTER_CRITICAL(&s_ups_lock);
    if (ups.controllerState == CONTROLLER_IDLE || ups.controllerState == CONTROLLER_COUNTDOWN)
    {
        if (ups.controllerState == CONTROLLER_COUNTDOWN)
        {
            xTimerStop(ups.shutdownTimer, 0);
        }
        ups.controllerState = CONTROLLER_WAITING_FOR_HOST_DOWN;
        should_send = true;
    }
    portEXIT_CRITICAL(&s_ups_lock);

    if (should_send)
    {
        ESP_LOGE(TAG, "Manual Shutdown Triggered");
        mqtt_publish_countdown("0");
        mqtt_publish_last_event("manual shutdown triggered");
        event_log_record(EVT_SHUTDOWN_TRIGGERED, 1);

        ups.shutdownRetriesSent = 0;
        ups.nextShutdownRetryTick = xTaskGetTickCount() + pdMS_TO_TICKS(15000);

        if (shutdown_client_send() != ESP_OK)
        {
            ESP_LOGE(TAG, "Manual Shutdown request failed");
            event_log_record(EVT_SHUTDOWN_FAILED, 1);
            /* Do not revert to IDLE here, let process_host_state handle it */
        }
    }
}

static void send_wake_signal(void)
{
    if (wake_on_lan_send(g_ups_config.wake_mac) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send Wake-on-LAN packet");
        event_log_record(EVT_WOL_FAILED, 0);
        return;
    }

    portENTER_CRITICAL(&s_ups_lock);
    ups.controllerState = CONTROLLER_WAITING_FOR_HOST_UP;
    portEXIT_CRITICAL(&s_ups_lock);

    ups.wolRetriesSent = 1;
    ups.nextWolRetryTick = xTaskGetTickCount() + pdMS_TO_TICKS(WOL_RETRY_INTERVAL_MS);
    schedule_next_host_check(HOST_CHECK_RETRY_INTERVAL_MS);
    s_last_host_wait_log = NULL;
    mqtt_publish_last_event("WOL sent");
    event_log_record(EVT_WOL_SENT, 0);
}

static void log_host_wait_state(const char *message)
{
    if (s_last_host_wait_log == message)
    {
        return;
    }

    s_last_host_wait_log = message;
    ESP_LOGI(TAG, "%s", message);
}

static bool host_check_is_due(void)
{
    TickType_t now = xTaskGetTickCount();
    return now >= ups.nextHostCheckTick;
}

static void schedule_next_host_check(uint32_t delay_ms)
{
    ups.nextHostCheckTick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
}

static void on_power_lost(void)
{
    leds_set_power_state(POWER_LOST);
    start_countdown();
}

static void on_power_restored(void)
{
    leds_set_power_state(POWER_PRESENT);
    ESP_LOGI(TAG, "Power RESTORED");
    mqtt_publish_power_state("online");
    mqtt_publish_last_event("power restored");
    event_log_record(EVT_POWER_RESTORED, 0);

    ups_cancel_countdown();

    portENTER_CRITICAL(&s_ups_lock);
    controller_state_t state = ups.controllerState;
    portEXIT_CRITICAL(&s_ups_lock);

    if (state == CONTROLLER_WAITING_FOR_HOST_DOWN)
    {
        ESP_LOGI(TAG, "Power returned while shutdown is still in progress, Wake-on-LAN will wait");
        return;
    }

    if (state == CONTROLLER_HOST_DOWN_WAITING_FOR_POWER)
    {
        ESP_LOGI(TAG, "Power restored after confirmed shutdown, sending Wake-on-LAN");
        send_wake_signal();
    }
}

static void handle_power_change(power_state_t state)
{
    switch (state)
    {
    case POWER_PRESENT:
        on_power_restored();
        break;

    case POWER_LOST:
        on_power_lost();
        break;

    default:
        break;
    }
}

static void process_host_state(void)
{
    controller_state_t state;

    portENTER_CRITICAL(&s_ups_lock);
    state = ups.controllerState;
    portEXIT_CRITICAL(&s_ups_lock);

    if (state == CONTROLLER_WAITING_FOR_HOST_DOWN)
    {
        if (!host_check_is_due())
        {
            return;
        }

        host_reachability_t reachability = shutdown_client_check_host_reachability();
        schedule_next_host_check(HOST_CHECK_RETRY_INTERVAL_MS);

        if (reachability == HOST_REACHABLE)
        {
            if (power_sensor_read() == POWER_PRESENT)
            {
                ESP_LOGI(TAG, "Power is back and host never went offline. Aborting shutdown wait.");
                portENTER_CRITICAL(&s_ups_lock);
                ups.controllerState = CONTROLLER_IDLE;
                portEXIT_CRITICAL(&s_ups_lock);
                return;
            }

            TickType_t now = xTaskGetTickCount();
            if (now >= ups.nextShutdownRetryTick)
            {
                ups.shutdownRetriesSent++;

                uint32_t retry_interval_ms = 15000;

                if (ups.shutdownRetriesSent > MAX_SHUTDOWN_RETRIES)
                {
                    retry_interval_ms = SHUTDOWN_RETRY_BACKOFF_MS;

                    if (ups.shutdownRetriesSent == MAX_SHUTDOWN_RETRIES + 1)
                    {
                        ESP_LOGE(TAG, "Host still reachable after %lu shutdown attempts, backing off to every %lu s",
                                 (unsigned long)MAX_SHUTDOWN_RETRIES,
                                 (unsigned long)(SHUTDOWN_RETRY_BACKOFF_MS / 1000));
                        mqtt_publish_last_event("shutdown retries exhausted");
                        event_log_record(EVT_SHUTDOWN_FAILED, 2);
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "Host still reachable, retrying shutdown command (%lu)", (unsigned long)ups.shutdownRetriesSent);
                }

                if (shutdown_client_send() != ESP_OK)
                {
                    ESP_LOGE(TAG, "Shutdown retry failed");
                }
                ups.nextShutdownRetryTick = now + pdMS_TO_TICKS(retry_interval_ms);
            }

            log_host_wait_state("Shutdown in progress, waiting for host to go offline");
            return;
        }

        ESP_LOGI(TAG, "Host shutdown confirmed");
        s_last_host_wait_log = NULL;
        mqtt_publish_host_state("down");
        mqtt_publish_last_event("host shutdown confirmed");
        event_log_record(EVT_HOST_DOWN_CONFIRMED, 0);

        if (power_sensor_read() == POWER_PRESENT)
        {
            ESP_LOGI(TAG, "Power is back, sending Wake-on-LAN");
            send_wake_signal();
            return;
        }

        portENTER_CRITICAL(&s_ups_lock);
        ups.controllerState = CONTROLLER_HOST_DOWN_WAITING_FOR_POWER;
        portEXIT_CRITICAL(&s_ups_lock);
        log_host_wait_state("Host is off, waiting for power restoration before Wake-on-LAN");
        return;
    }

    if (state == CONTROLLER_WAITING_FOR_HOST_UP)
    {
        /* Retry WOL periodically in case the first packet was lost */
        TickType_t now = xTaskGetTickCount();
        if (ups.wolRetriesSent < MAX_WOL_RETRIES && now >= ups.nextWolRetryTick)
        {
            ESP_LOGI(TAG, "WOL retry %lu/%lu",
                     (unsigned long)(ups.wolRetriesSent + 1),
                     (unsigned long)MAX_WOL_RETRIES);
            event_log_record(EVT_WOL_RETRY, ups.wolRetriesSent + 1);
            if (wake_on_lan_send(g_ups_config.wake_mac) == ESP_OK)
            {
                ups.wolRetriesSent++;
            }
            ups.nextWolRetryTick = now + pdMS_TO_TICKS(WOL_RETRY_INTERVAL_MS);
        }

        if (!host_check_is_due())
        {
            return;
        }

        host_reachability_t reachability = shutdown_client_check_host_reachability();
        schedule_next_host_check(HOST_CHECK_RETRY_INTERVAL_MS);

        if (reachability != HOST_REACHABLE)
        {
            log_host_wait_state("Wake sent, waiting for host health check to come back online");
            return;
        }

        portENTER_CRITICAL(&s_ups_lock);
        ups.controllerState = CONTROLLER_IDLE;
        portEXIT_CRITICAL(&s_ups_lock);
        s_last_host_wait_log = NULL;
        ESP_LOGI(TAG, "Host is back online");
        mqtt_publish_host_state("up");
        mqtt_publish_countdown("idle");
        mqtt_publish_last_event("host back online");
        event_log_record(EVT_HOST_BACK_ONLINE, 0);
    }
}

static void process_startup_recovery(void)
{
    if (!ups.startupRecoveryPending || ups.powerState != POWER_PRESENT)
    {
        return;
    }

    if (!wifi_manager_is_connected())
    {
        log_host_wait_state("Startup recovery pending, waiting for Wi-Fi before host check");
        return;
    }

    host_reachability_t reachability = shutdown_client_check_host_reachability();

    if (reachability == HOST_REACHABLE)
    {
        ups.startupRecoveryPending = false;
        s_last_host_wait_log = NULL;
        ESP_LOGI(TAG, "Startup recovery check: host is already online");
        ups_publish_current_state();
        return;
    }

    ups.startupRecoveryPending = false;
    s_last_host_wait_log = NULL;
    ESP_LOGI(TAG, "Startup recovery check: host is offline while power is present, sending Wake-on-LAN");
    send_wake_signal();
    ups_publish_current_state();
}

void ups_publish_current_state(void)
{
    if (ups.powerState == POWER_PRESENT)
    {
        mqtt_publish_power_state("online");
    }
    else if (ups.powerState == POWER_LOST)
    {
        mqtt_publish_power_state("lost");
    }

    portENTER_CRITICAL(&s_ups_lock);
    controller_state_t ctrl = ups.controllerState;
    portEXIT_CRITICAL(&s_ups_lock);

    if (ctrl == CONTROLLER_IDLE && !ups.startupRecoveryPending)
    {
        mqtt_publish_host_state("up");
        mqtt_publish_countdown("idle");
    }
    else if (ctrl == CONTROLLER_HOST_DOWN_WAITING_FOR_POWER || ctrl == CONTROLLER_WAITING_FOR_HOST_UP)
    {
        mqtt_publish_host_state("down");
        mqtt_publish_countdown("idle");
    }
    else if (ctrl == CONTROLLER_WAITING_FOR_HOST_DOWN)
    {
        mqtt_publish_host_state("up");
        mqtt_publish_countdown("0");
    }
    // Note: if CONTROLLER_COUNTDOWN, the live loop will publish countdown on next tick
}

bool ups_is_reboot_safe(void)
{
    bool safe;

    portENTER_CRITICAL(&s_ups_lock);
    safe = (ups.controllerState == CONTROLLER_IDLE);
    portEXIT_CRITICAL(&s_ups_lock);

    return safe;
}

void ups_init(void)
{
    power_sensor_init();
    leds_init();

    ups.powerState = POWER_UNKNOWN;
    ups.controllerState = CONTROLLER_IDLE;
    ups.nextHostCheckTick = 0;
    ups.countdownStartTick = 0;
    ups.startupRecoveryPending = true;

    ups.shutdownTimer = xTimerCreate(
        "ups_shutdown",
        pdMS_TO_TICKS(g_ups_config.shutdown_delay_seconds * 1000UL),
        pdFALSE,
        NULL,
        shutdown_timer_cb);

    configASSERT(ups.shutdownTimer != NULL);

    ESP_LOGI(TAG, "UPS controller initialized");
    event_log_record(EVT_ESP_BOOTED, 0);
}

void ups_update(void)
{
    power_state_t currentState = power_sensor_read();

    if (currentState != ups.powerState)
    {
        ups.powerState = currentState;
        handle_power_change(currentState);
    }

    /* Publish the live countdown, but only when the remaining second changes.
       This loop runs every power_poll_interval_ms (100 ms), so publishing
       unconditionally would push ~10 identical messages per second for the
       whole countdown. */
    portENTER_CRITICAL(&s_ups_lock);
    controller_state_t ctrl = ups.controllerState;
    portEXIT_CRITICAL(&s_ups_lock);

    if (ctrl == CONTROLLER_COUNTDOWN)
    {
        TickType_t elapsed = xTaskGetTickCount() - ups.countdownStartTick;
        uint32_t elapsed_s = (uint32_t)(elapsed / configTICK_RATE_HZ);
        uint32_t total_s = g_ups_config.shutdown_delay_seconds;
        uint32_t remaining = (elapsed_s < total_s) ? (total_s - elapsed_s) : 0;

        if (remaining != s_last_published_countdown)
        {
            char buf[12];
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)remaining);
            mqtt_publish_countdown(buf);
            s_last_published_countdown = remaining;
        }
    }
    else
    {
        s_last_published_countdown = UINT32_MAX;
    }

    process_startup_recovery();
    process_host_state();
}
