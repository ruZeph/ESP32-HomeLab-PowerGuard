#include "ups.h"

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "leds.h"
#include "power_sensor.h"

#include "shutdown_client.h"
#include "wake_on_lan.h"

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
} ups_controller_t;

static ups_controller_t ups = {
    .powerState = POWER_UNKNOWN,
    .controllerState = CONTROLLER_IDLE,
    .shutdownTimer = NULL,
};

static portMUX_TYPE s_ups_lock = portMUX_INITIALIZER_UNLOCKED;

static void on_power_lost(void);
static void on_power_restored(void);
static void handle_power_change(power_state_t state);
static void start_countdown(void);
static void cancel_countdown(void);
static void send_wake_signal(void);
static void process_host_state(void);
static void log_host_wait_state(const char *message);
static void shutdown_timer_cb(TimerHandle_t timer);

static const char *s_last_host_wait_log = NULL;

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

        if (shutdown_client_send() != ESP_OK)
        {
            ESP_LOGE(TAG, "Shutdown request failed");

            portENTER_CRITICAL(&s_ups_lock);
            if (ups.controllerState == CONTROLLER_WAITING_FOR_HOST_DOWN)
            {
                ups.controllerState = CONTROLLER_IDLE;
            }
            portEXIT_CRITICAL(&s_ups_lock);
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

    ESP_LOGW(TAG, "Power LOST");
    ESP_LOGW(TAG, "Shutdown countdown started (%lu s)",
             (unsigned long)g_ups_config.shutdown_delay_seconds);
}

static void cancel_countdown(void)
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
}

static void send_wake_signal(void)
{
    if (wake_on_lan_send(g_ups_config.wake_mac) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send Wake-on-LAN packet");
        return;
    }

    portENTER_CRITICAL(&s_ups_lock);
    ups.controllerState = CONTROLLER_WAITING_FOR_HOST_UP;
    portEXIT_CRITICAL(&s_ups_lock);

    s_last_host_wait_log = NULL;
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

static void on_power_lost(void)
{
    leds_set_power_state(POWER_LOST);
    start_countdown();
}

static void on_power_restored(void)
{
    leds_set_power_state(POWER_PRESENT);
    ESP_LOGI(TAG, "Power RESTORED");

    cancel_countdown();

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
        host_reachability_t reachability = shutdown_client_check_host_reachability();

        if (reachability == HOST_REACHABLE)
        {
            log_host_wait_state("Shutdown in progress, waiting for host to go offline");
            return;
        }

        ESP_LOGI(TAG, "Host shutdown confirmed");
        s_last_host_wait_log = NULL;

        if (ups.powerState == POWER_PRESENT)
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
        host_reachability_t reachability = shutdown_client_check_host_reachability();

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
    }
}

void ups_init(void)
{
    power_sensor_init();
    leds_init();

    ups.powerState = POWER_UNKNOWN;
    ups.controllerState = CONTROLLER_IDLE;

    ups.shutdownTimer = xTimerCreate(
        "ups_shutdown",
        pdMS_TO_TICKS(g_ups_config.shutdown_delay_seconds * 1000UL),
        pdFALSE,
        NULL,
        shutdown_timer_cb);

    configASSERT(ups.shutdownTimer != NULL);

    ESP_LOGI(TAG, "UPS controller initialized");
}

void ups_update(void)
{
    power_state_t currentState = power_sensor_read();

    if (currentState != ups.powerState)
    {
        ups.powerState = currentState;
        handle_power_change(currentState);
    }

    process_host_state();
}
