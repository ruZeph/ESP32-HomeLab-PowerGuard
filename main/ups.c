#include "ups.h"

#include <stdio.h>

#include "esp_log.h"
#include "leds.h"
#include "power_sensor.h"

#define TAG "UPS"
#define UPS_COUNTDOWN_SECONDS 10U

static ups_controller_t ups = {
    .powerState = POWER_UNKNOWN,
    .controllerState = CONTROLLER_IDLE,
    .countdownStartTick = 0,
    .countdownDurationTicks = 0,
    .countdownSeconds = UPS_COUNTDOWN_SECONDS,
    .lastReportedRemainingSeconds = 0,
    .shutdownSent = false,
};

static void start_countdown(void)
{
    if (ups.controllerState != CONTROLLER_IDLE)
    {
        return;
    }

    ups.controllerState = CONTROLLER_COUNTDOWN;
    ups.shutdownSent = false;
    ups.countdownStartTick = xTaskGetTickCount();
    ups.countdownDurationTicks = pdMS_TO_TICKS(ups.countdownSeconds * 1000UL);
    ups.lastReportedRemainingSeconds = ups.countdownSeconds;

    ESP_LOGW(TAG, "Shutdown countdown started (%lu s)", (unsigned long)ups.countdownSeconds);
    ESP_LOGW(TAG, "Shutdown in %lu...", (unsigned long)ups.lastReportedRemainingSeconds);
}

static void cancel_countdown(void)
{
    if (ups.controllerState != CONTROLLER_COUNTDOWN)
    {
        return;
    }

    ups.controllerState = CONTROLLER_IDLE;
    ESP_LOGI(TAG, "Shutdown cancelled");
}

static void process_countdown(void)
{
    if (ups.controllerState != CONTROLLER_COUNTDOWN)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - ups.countdownStartTick;

    if (elapsed >= ups.countdownDurationTicks)
    {
        ups.controllerState = CONTROLLER_SHUTDOWN_SENT;
        ups.shutdownSent = true;
        ESP_LOGE(TAG, "Shutdown Triggered");
        return;
    }

    TickType_t remainingTicks = ups.countdownDurationTicks - elapsed;
    TickType_t oneSecondTicks = pdMS_TO_TICKS(1000);

    uint32_t remainingSeconds = (uint32_t)((remainingTicks + oneSecondTicks - 1) / oneSecondTicks);

    if (remainingSeconds != ups.lastReportedRemainingSeconds)
    {
        ups.lastReportedRemainingSeconds = remainingSeconds;
        ESP_LOGW(TAG, "Shutdown in %lu...", (unsigned long)remainingSeconds);
    }
}

static void on_power_lost(void)
{
    leds_set_power_state(POWER_LOST);
    ESP_LOGW(TAG, "Power LOST");
    start_countdown();
}

static void on_power_restored(void)
{
    leds_set_power_state(POWER_PRESENT);
    ESP_LOGI(TAG, "Power RESTORED");

    if (ups.controllerState == CONTROLLER_COUNTDOWN)
    {
        cancel_countdown();
    }
    else if (ups.controllerState == CONTROLLER_SHUTDOWN_SENT)
    {
        ESP_LOGI(TAG, "Power restored after shutdown trigger");
        ups.controllerState = CONTROLLER_IDLE;
        ups.shutdownSent = false;
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

void ups_init(void)
{
    power_sensor_init();
    leds_init();

    ups.powerState = POWER_UNKNOWN;
    ups.controllerState = CONTROLLER_IDLE;
    ups.shutdownSent = false;
    ups.countdownStartTick = 0;
    ups.countdownDurationTicks = pdMS_TO_TICKS(UPS_COUNTDOWN_SECONDS * 1000UL);
    ups.countdownSeconds = UPS_COUNTDOWN_SECONDS;
    ups.lastReportedRemainingSeconds = 0;

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

    process_countdown();
}