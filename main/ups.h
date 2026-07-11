#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "power_sensor.h"

typedef enum
{
    CONTROLLER_IDLE,
    CONTROLLER_COUNTDOWN,
    CONTROLLER_SHUTDOWN_SENT
} controller_state_t;

typedef struct
{
    power_state_t powerState;
    controller_state_t controllerState;

    TickType_t countdownStartTick;
    TickType_t countdownDurationTicks;

    uint32_t countdownSeconds;
    uint32_t lastReportedRemainingSeconds;

    bool shutdownSent;
} ups_controller_t;

void ups_init(void);
void ups_update(void);