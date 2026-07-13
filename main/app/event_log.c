#include "event_log.h"

#include <stdio.h>
#include <string.h>

#include "mqtt_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "EVTLOG"

#define NVS_NAMESPACE   "evt_log"
#define NVS_KEY_COUNT   "count"
#define NVS_KEY_HEAD    "head"
#define MAX_EVENTS      30

/* Packed 5-byte event entry stored in NVS as a blob */
typedef struct __attribute__((packed))
{
    uint32_t uptime_s;
    uint8_t  type;
    uint8_t  extra;
} nvs_event_t;

/* RAM mirror of the ring buffer */
static nvs_event_t s_events[MAX_EVENTS];
static uint8_t     s_count = 0;  /* number of stored events (max MAX_EVENTS) */
static uint8_t     s_head  = 0;  /* next write index */

static const char *event_type_str(uint8_t type)
{
    switch ((event_type_t)type)
    {
    case EVT_ESP_BOOTED:         return "ESP_BOOTED";
    case EVT_POWER_LOST:         return "POWER_LOST";
    case EVT_POWER_RESTORED:     return "POWER_RESTORED";
    case EVT_SHUTDOWN_TRIGGERED: return "SHUTDOWN_TRIGGERED";
    case EVT_SHUTDOWN_FAILED:    return "SHUTDOWN_FAILED";
    case EVT_SHUTDOWN_CANCELLED: return "SHUTDOWN_CANCELLED";
    case EVT_HOST_DOWN_CONFIRMED:return "HOST_DOWN_CONFIRMED";
    case EVT_HOST_BACK_ONLINE:   return "HOST_BACK_ONLINE";
    case EVT_WOL_SENT:           return "WOL_SENT";
    case EVT_WOL_RETRY:          return "WOL_RETRY";
    case EVT_WOL_FAILED:         return "WOL_FAILED";
    default:                     return "UNKNOWN";
    }
}

/* ---- NVS persistence ---- */

static void save_to_nvs(void)
{
    nvs_handle_t h;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS for write");
        return;
    }

    nvs_set_u8(h, NVS_KEY_COUNT, s_count);
    nvs_set_u8(h, NVS_KEY_HEAD,  s_head);
    nvs_set_blob(h, "events", s_events, sizeof(s_events));
    nvs_commit(h);
    nvs_close(h);
}

static void load_from_nvs(void)
{
    nvs_handle_t h;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGI(TAG, "No previous event log found in NVS");
        return;
    }

    size_t blob_size = sizeof(s_events);
    if (nvs_get_blob(h, "events", s_events, &blob_size) != ESP_OK)
    {
        nvs_close(h);
        return;
    }

    nvs_get_u8(h, NVS_KEY_COUNT, &s_count);
    nvs_get_u8(h, NVS_KEY_HEAD,  &s_head);
    nvs_close(h);

    if (s_count > MAX_EVENTS)
    {
        s_count = 0;
        s_head  = 0;
    }

    ESP_LOGI(TAG, "Loaded %u events from NVS", s_count);
}

/* ---- Public API ---- */

void event_log_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    memset(s_events, 0, sizeof(s_events));
    s_count = 0;
    s_head  = 0;

    load_from_nvs();
}

void event_log_record(event_type_t type, uint8_t extra)
{
    uint32_t uptime_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);

    s_events[s_head].uptime_s = uptime_s;
    s_events[s_head].type     = (uint8_t)type;
    s_events[s_head].extra    = extra;

    s_head = (s_head + 1) % MAX_EVENTS;
    if (s_count < MAX_EVENTS)
    {
        s_count++;
    }

    ESP_LOGI(TAG, "[t=%lu] %s (extra=%u)",
             (unsigned long)uptime_s, event_type_str((uint8_t)type), extra);

    save_to_nvs();

    mqtt_publish_event_log();
}

int event_log_format_json(char *buf, int buf_size)
{
    if (buf_size < 3 || s_count == 0)
    {
        if (buf_size >= 14)
        {
            snprintf(buf, buf_size, "{\"events\":[]}");
            return 13;
        }
        return 0;
    }

    int pos = 0;
    int written = snprintf(buf + pos, buf_size - pos, "{\"events\":[");
    if (written > 0) {
        pos += written;
    }

    /* Walk from oldest to newest */
    uint8_t start = (s_count < MAX_EVENTS) ? 0 : s_head;

    for (uint8_t i = 0; i < s_count; i++)
    {
        uint8_t idx = (start + i) % MAX_EVENTS;
        const nvs_event_t *e = &s_events[idx];

        int written;
        if (e->extra != 0)
        {
            written = snprintf(buf + pos, buf_size - pos,
                               "%s{\"t\":%lu,\"e\":\"%s\",\"x\":%u}",
                               (i > 0) ? "," : "",
                               (unsigned long)e->uptime_s,
                               event_type_str(e->type),
                               e->extra);
        }
        else
        {
            written = snprintf(buf + pos, buf_size - pos,
                               "%s{\"t\":%lu,\"e\":\"%s\"}",
                               (i > 0) ? "," : "",
                               (unsigned long)e->uptime_s,
                               event_type_str(e->type));
        }

        if (written < 0 || pos + written >= buf_size - 2)
        {
            break;
        }
        pos += written;
    }

    buf[pos++] = ']';
    buf[pos++] = '}';
    buf[pos]   = '\0';
    return pos;
}

void event_log_clear(void)
{
    memset(s_events, 0, sizeof(s_events));
    s_count = 0;
    s_head  = 0;
    save_to_nvs();
    ESP_LOGI(TAG, "Event log cleared");
}
