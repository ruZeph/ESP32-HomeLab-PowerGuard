#pragma once

#include <stdint.h>

typedef struct
{
    uint32_t power_poll_interval_ms;
    uint32_t shutdown_delay_seconds;

    const char *wifi_ssid;
    const char *wifi_password;

    const char *shutdown_url;   // Example: http://192.168.1.50:8080/shutdown
} ups_config_t;

extern const ups_config_t g_ups_config;