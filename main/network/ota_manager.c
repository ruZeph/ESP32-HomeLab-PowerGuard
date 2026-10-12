#include "ota_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_manager.h"
#include "ups.h"

#define TAG "OTA"

#define OTA_URI_PATH        "/ota"
#define OTA_STATUS_URI_PATH "/ota/status"
#define OTA_DEFAULT_PORT    3232
#define OTA_CHUNK_SIZE      4096

static httpd_handle_t s_server = NULL;
static volatile bool s_in_progress = false;

/* Only one upload can be in flight at a time, so a single buffer is enough. */
static char s_chunk[OTA_CHUNK_SIZE];

static uint16_t ota_port(void)
{
    return (uint16_t)(g_ups_config.ota_port != 0 ? g_ups_config.ota_port : OTA_DEFAULT_PORT);
}

static bool upload_is_authorised(httpd_req_t *req)
{
    const char *token = g_ups_config.ota_bearer_token;

    if (token == NULL || token[0] == '\0')
    {
        return true;
    }

    char expected[160];
    int written = snprintf(expected, sizeof(expected), "Bearer %s", token);
    if (written <= 0 || written >= (int)sizeof(expected))
    {
        return false;
    }

    char received[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", received, sizeof(received)) != ESP_OK)
    {
        return false;
    }

    return strcmp(received, expected) == 0;
}

static void publish_status(const char *state)
{
    mqtt_publish_ota_status(state);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!upload_is_authorised(req))
    {
        ESP_LOGW(TAG, "Rejected update: missing or invalid bearer token");
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid OTA token");
    }

    if (s_in_progress)
    {
        return httpd_resp_send_custom_err(req, "503 Service Unavailable",
                                          "An update is already in progress");
    }

    if (!ups_is_reboot_safe())
    {
        ESP_LOGW(TAG, "Rejected update: controller is handling a power event");
        return httpd_resp_send_custom_err(req, "503 Service Unavailable",
                                          "Power event in progress, retry when the controller is idle");
    }

    if (req->content_len <= 0)
    {
        return httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED, "Content-Length required");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL)
    {
        ESP_LOGE(TAG, "No OTA partition available to write");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
    }

    if ((size_t)req->content_len > target->size)
    {
        ESP_LOGE(TAG, "Image too large: %d bytes, partition '%s' holds %lu",
                 req->content_len, target->label, (unsigned long)target->size);
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE,
                                   "Firmware image is larger than the OTA partition");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, (size_t)req->content_len, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start update: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start update");
    }

    s_in_progress = true;
    ESP_LOGI(TAG, "Update started: %d bytes into partition '%s'", req->content_len, target->label);
    publish_status("starting");

    int remaining = req->content_len;
    int written = 0;
    int next_report_percent = 10;

    while (remaining > 0)
    {
        int to_read = remaining < (int)sizeof(s_chunk) ? remaining : (int)sizeof(s_chunk);
        int received = httpd_req_recv(req, s_chunk, to_read);

        if (received == HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue;
        }

        if (received <= 0)
        {
            ESP_LOGE(TAG, "Update interrupted after %d of %d bytes", written, req->content_len);
            esp_ota_abort(handle);
            s_in_progress = false;
            publish_status("failed: transfer interrupted");
            return ESP_FAIL;
        }

        err = esp_ota_write(handle, s_chunk, received);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Flash write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            s_in_progress = false;
            publish_status("failed: flash write error");
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
        }

        remaining -= received;
        written += received;

        int percent = (written * 100) / req->content_len;
        if (percent >= next_report_percent)
        {
            char progress[32];
            snprintf(progress, sizeof(progress), "updating %d%%", percent);
            ESP_LOGI(TAG, "Update progress: %d%%", percent);
            publish_status(progress);
            next_report_percent = ((percent / 10) + 1) * 10;
        }
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Image validation failed: %s", esp_err_to_name(err));
        s_in_progress = false;
        publish_status("failed: image validation error");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware image is not valid");
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to select boot partition: %s", esp_err_to_name(err));
        s_in_progress = false;
        publish_status("failed: could not select boot partition");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to select the new image");
    }

    s_in_progress = false;
    ESP_LOGI(TAG, "Update complete, rebooting into partition '%s'", target->label);
    publish_status("complete, rebooting");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Update complete. Rebooting.\n", HTTPD_RESP_USE_STRLEN);

    /* Let the response reach the client before the reboot drops the socket. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
    if (!upload_is_authorised(req))
    {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid OTA token");
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    char payload[288];
    snprintf(payload, sizeof(payload),
             "{\"project\":\"%s\",\"version\":\"%s\",\"idf\":\"%s\","
             "\"running_partition\":\"%s\",\"next_partition\":\"%s\","
             "\"update_in_progress\":%s,\"port\":%u}\n",
             app->project_name, app->version, app->idf_ver,
             running != NULL ? running->label : "unknown",
             next != NULL ? next->label : "none",
             s_in_progress ? "true" : "false",
             (unsigned)ota_port());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

void ota_manager_init(void)
{
    /* With rollback enabled an updated image boots in PENDING_VERIFY and must
       confirm itself, otherwise the bootloader reverts it on the next reset. */
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Running image confirmed, rollback cancelled");
    }
    else if (err != ESP_ERR_OTA_ROLLBACK_INVALID_STATE)
    {
        ESP_LOGW(TAG, "Could not confirm running image: %s", esp_err_to_name(err));
    }

    static const httpd_uri_t ota_post_uri = {
        .uri = OTA_URI_PATH,
        .method = HTTP_POST,
        .handler = ota_post_handler,
        .user_ctx = NULL,
    };

    static const httpd_uri_t ota_status_uri = {
        .uri = OTA_STATUS_URI_PATH,
        .method = HTTP_GET,
        .handler = ota_status_get_handler,
        .user_ctx = NULL,
    };

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = ota_port();
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    err = httpd_start(&s_server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start OTA server on port %u: %s", (unsigned)ota_port(), esp_err_to_name(err));
        s_server = NULL;
        return;
    }

    err = httpd_register_uri_handler(s_server, &ota_post_uri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register %s: %s", OTA_URI_PATH, esp_err_to_name(err));
    }

    err = httpd_register_uri_handler(s_server, &ota_status_uri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register %s: %s", OTA_STATUS_URI_PATH, esp_err_to_name(err));
    }

    if (g_ups_config.ota_bearer_token == NULL || g_ups_config.ota_bearer_token[0] == '\0')
    {
        ESP_LOGW(TAG, "OTA bearer token is not configured, uploads are unauthenticated");
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "OTA server listening on port %u, firmware %s on partition '%s'",
             (unsigned)ota_port(), app->version,
             running != NULL ? running->label : "unknown");
}
