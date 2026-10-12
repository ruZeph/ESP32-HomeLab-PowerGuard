#include "time_sync.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

#define TAG "TIMESYNC"

/* The ESP32 boots with its clock near the epoch. Any year below this means the
   clock has not been set, which also covers the sync callback being missed. */
#define EARLIEST_VALID_YEAR 2024

#define DEFAULT_NTP_SERVER "pool.ntp.org"

static void on_time_synced(struct timeval *tv)
{
    (void)tv;

    char stamp[24];
    if (time_sync_format_utc(stamp, sizeof(stamp)))
    {
        ESP_LOGI(TAG, "Clock synchronised, UTC now %s", stamp);
    }
    else
    {
        ESP_LOGW(TAG, "Received an SNTP reply but the resulting time is implausible");
    }
}

bool time_sync_format_utc(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 21)
    {
        return false;
    }

    time_t now = 0;
    time(&now);

    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL)
    {
        return false;
    }

    if ((utc.tm_year + 1900) < EARLIEST_VALID_YEAR)
    {
        return false;
    }

    return strftime(buf, buf_len, "%Y-%m-%dT%H:%M:%SZ", &utc) > 0;
}

bool time_sync_is_valid(void)
{
    char stamp[24];
    return time_sync_format_utc(stamp, sizeof(stamp));
}

void time_sync_start(void)
{
    const char *server = DEFAULT_NTP_SERVER;

    if (g_ups_config.ntp_server != NULL && g_ups_config.ntp_server[0] != '\0')
    {
        server = g_ups_config.ntp_server;
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.sync_cb = on_time_synced;
    cfg.start = true;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SNTP init failed: %s, timestamps will be omitted", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "SNTP started, server=%s, clock is UTC", server);
}
