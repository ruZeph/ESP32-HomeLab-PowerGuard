#pragma once

#include <stdint.h>

typedef struct
{
    uint32_t power_poll_interval_ms;
    uint32_t shutdown_delay_seconds;
    uint32_t health_check_timeout_ms;

    const char *wifi_ssid;
    const char *wifi_password;

    const char *wake_mac;
    const char *shutdown_url;         // Example: http://192.168.0.89:8787/api/v1/host/poweroff
    const char *shutdown_bearer_token;
    const char *healthcheck_url;      // Example: http://192.168.0.89:8787/health
} ups_config_t;

extern const ups_config_t g_ups_config;
