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
    TickType_t shutdownConfirmDeadlineTick;
    bool startupRecoveryPending;
    /* True when this episode began with, or saw, a mains loss. A manual
       shutdown with mains present must not wake the host afterwards. */
    bool episodeNeedsWake;
    /* Last observed host health, used so the idle state can report host_state
       honestly instead of assuming the host is up. */
    bool hostKnownDown;
    TickType_t nextIdleHostCheckTick;
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

/* How long to keep watching the host for shutdown confirmation after the
   request has been sent. A host stopping many services can take a while, and
   power may return during that window. */
static const uint32_t SHUTDOWN_CONFIRM_TIMEOUT_MS = 300000;

/* Idle health probe cadence, used only while the host is believed to be down.
   This never sends Wake-on-LAN; it only keeps host_state accurate. */
static const uint32_t IDLE_HOST_CHECK_INTERVAL_MS = 60000;

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
        ups.shutdownConfirmDeadlineTick = xTaskGetTickCount() + pdMS_TO_TICKS(SHUTDOWN_CONFIRM_TIMEOUT_MS);
        /* This path exists only because mains was lost, so the host should be
           woken once the power situation allows it. */
        ups.episodeNeedsWake = true;

        shutdown_result_t result = shutdown_client_send();
        if (result == SHUTDOWN_REQUEST_FAILED)
        {
            ESP_LOGE(TAG, "Shutdown request was not delivered");
            event_log_record(EVT_SHUTDOWN_FAILED, 0);
            /* Do not revert to IDLE here! The host might still be shutting down 
               since it drops the HTTP connection instantly. Let process_host_state 
               confirm whether the host goes down or stays up. */
        }
        else if (result == SHUTDOWN_REQUEST_UNCONFIRMED)
        {
            ESP_LOGW(TAG, "Shutdown request sent, no response yet; letting the host state decide");
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
        ups.shutdownConfirmDeadlineTick = xTaskGetTickCount() + pdMS_TO_TICKS(SHUTDOWN_CONFIRM_TIMEOUT_MS);
        /* A manual shutdown only implies a later wake if mains is already out,
           because then this is really an outage being handled early. With mains
           present the host must stay off. */
        ups.episodeNeedsWake = (ups.powerState == POWER_LOST);

        if (shutdown_client_send() == SHUTDOWN_REQUEST_FAILED)
        {
            ESP_LOGE(TAG, "Manual Shutdown request was not delivered");
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
    /* Any mains loss during an episode means the host must be woken once power
       is back, even if the episode started as a manual shutdown. */
    ups.episodeNeedsWake = true;

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
            ups.hostKnownDown = false;

            if (power_sensor_read() == POWER_PRESENT)
            {
                /* The shutdown request has already been sent, so "host still
                   reachable" does not mean the shutdown did not happen: a host
                   stopping many services keeps answering for tens of seconds.
                   Dropping the episode here used to lose the wake path when the
                   host finished going down just after mains returned. */
                if (xTaskGetTickCount() < ups.shutdownConfirmDeadlineTick)
                {
                    log_host_wait_state(ups.episodeNeedsWake
                                            ? "Power is back, waiting for the host to finish shutting down"
                                            : "Shutdown in progress, waiting for the host to go offline");
                    return;
                }

                ESP_LOGE(TAG, "Host still reachable %lu s after the shutdown request, giving up",
                         (unsigned long)(SHUTDOWN_CONFIRM_TIMEOUT_MS / 1000));
                portENTER_CRITICAL(&s_ups_lock);
                ups.controllerState = CONTROLLER_IDLE;
                portEXIT_CRITICAL(&s_ups_lock);
                s_last_host_wait_log = NULL;
                mqtt_publish_last_event("shutdown did not take effect");
                event_log_record(EVT_SHUTDOWN_FAILED, 3);
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

                if (shutdown_client_send() == SHUTDOWN_REQUEST_FAILED)
                {
                    ESP_LOGE(TAG, "Shutdown retry was not delivered");
                }
                ups.nextShutdownRetryTick = now + pdMS_TO_TICKS(retry_interval_ms);
            }

            log_host_wait_state("Shutdown in progress, waiting for host to go offline");
            return;
        }

        ESP_LOGI(TAG, "Host shutdown confirmed");
        s_last_host_wait_log = NULL;
        ups.hostKnownDown = true;
        mqtt_publish_host_state("down");
        mqtt_publish_last_event("host shutdown confirmed");
        event_log_record(EVT_HOST_DOWN_CONFIRMED, 0);

        if (power_sensor_read() == POWER_PRESENT)
        {
            if (ups.episodeNeedsWake)
            {
                ESP_LOGI(TAG, "Power is back, sending Wake-on-LAN");
                send_wake_signal();
                return;
            }

            /* Manual shutdown with mains present: the host must stay off. */
            ESP_LOGI(TAG, "Manual shutdown complete with mains present, leaving host off");
            portENTER_CRITICAL(&s_ups_lock);
            ups.controllerState = CONTROLLER_IDLE;
            portEXIT_CRITICAL(&s_ups_lock);
            mqtt_publish_last_event("host shut down manually");
            ups.nextIdleHostCheckTick = xTaskGetTickCount() + pdMS_TO_TICKS(IDLE_HOST_CHECK_INTERVAL_MS);
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
        ups.hostKnownDown = false;
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
        ups.hostKnownDown = false;
        ESP_LOGI(TAG, "Startup recovery check: host is already online");
        ups_publish_current_state();
        return;
    }

    ups.startupRecoveryPending = false;
    s_last_host_wait_log = NULL;
    ups.hostKnownDown = true;
    ESP_LOGI(TAG, "Startup recovery check: host is offline while power is present, sending Wake-on-LAN");
    send_wake_signal();
    ups_publish_current_state();
}

/**
 * Keeps host_state accurate while idle after a host was seen to go down, for
 * example following a manual shutdown. This only probes health; it never sends
 * Wake-on-LAN, so an intentionally powered-off host stays off.
 */
static void process_idle_host_check(void)
{
    if (ups.controllerState != CONTROLLER_IDLE || ups.startupRecoveryPending)
    {
        return;
    }

    if (!ups.hostKnownDown || !wifi_manager_is_connected())
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (now < ups.nextIdleHostCheckTick)
    {
        return;
    }
    ups.nextIdleHostCheckTick = now + pdMS_TO_TICKS(IDLE_HOST_CHECK_INTERVAL_MS);

    if (shutdown_client_check_host_reachability() == HOST_REACHABLE)
    {
        ups.hostKnownDown = false;
        ESP_LOGI(TAG, "Host is back online");
        mqtt_publish_host_state("up");
        mqtt_publish_last_event("host back online");
        event_log_record(EVT_HOST_BACK_ONLINE, 0);
    }
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
        mqtt_publish_host_state(ups.hostKnownDown ? "down" : "up");
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
    ups.shutdownConfirmDeadlineTick = 0;
    ups.episodeNeedsWake = false;
    ups.hostKnownDown = false;
    ups.nextIdleHostCheckTick = 0;

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
    process_idle_host_check();
}
