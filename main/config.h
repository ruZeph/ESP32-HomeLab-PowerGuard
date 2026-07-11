#pragma once

#include <stdint.h>

typedef struct
{
    uint32_t power_poll_interval_ms;
    uint32_t shutdown_delay_seconds;
} ups_config_t;

extern const ups_config_t g_ups_config;