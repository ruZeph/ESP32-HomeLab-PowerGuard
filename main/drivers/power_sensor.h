#pragma once

#include "driver/gpio.h"

typedef enum
{
    POWER_UNKNOWN = -1,
    POWER_LOST = 0,
    POWER_PRESENT = 1
} power_state_t;

void power_sensor_init(void);
power_state_t power_sensor_read(void);
