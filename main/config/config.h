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

    const char *mqtt_broker_url;      // Example: mqtt://192.168.0.89
    uint32_t    mqtt_port;            // Example: 1883
    const char *mqtt_username;        // Example: esp32
    const char *mqtt_password;        // Broker password (keep in gitignored config.c)

    uint32_t    ota_port;             // LAN OTA HTTP port, 0 uses the default 3232
    const char *ota_bearer_token;     // Token required on OTA requests, NULL to allow without auth

    const char *ntp_server;           // NTP host for the wall clock, NULL uses pool.ntp.org
    const char *device_hostname;      // DHCP hostname, NULL uses "powerguard"
} ups_config_t;

extern const ups_config_t g_ups_config;
