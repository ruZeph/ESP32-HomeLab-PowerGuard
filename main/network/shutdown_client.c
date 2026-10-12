#include "shutdown_client.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define TAG "SHUTDOWN"

static bool has_text(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static bool is_expected_disconnect(esp_err_t err)
{
    return err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_FETCH_HEADER;
}

static void apply_auth_header(esp_http_client_handle_t client)
{
    char auth_header[256];
    (void)snprintf(auth_header,
                   sizeof(auth_header),
                   "Bearer %s",
                   g_ups_config.shutdown_bearer_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
}

esp_err_t shutdown_client_send(void)
{
    if (!has_text(g_ups_config.shutdown_url))
    {
        ESP_LOGE(TAG, "Shutdown URL is not configured");
        return ESP_ERR_INVALID_ARG;
    }

    if (!has_text(g_ups_config.shutdown_bearer_token))
    {
        ESP_LOGE(TAG, "Shutdown bearer token is not configured");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = g_ups_config.shutdown_url,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    apply_auth_header(client);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300)
        {
            ESP_LOGI(TAG, "Shutdown request accepted, HTTP status = %d", status);
        }
        else
        {
            ESP_LOGE(TAG, "Shutdown request rejected, HTTP status = %d", status);
            err = ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

host_reachability_t shutdown_client_check_host_reachability(void)
{
    if (!has_text(g_ups_config.healthcheck_url))
    {
        ESP_LOGW(TAG, "Health check URL is not configured");
        return HOST_UNREACHABLE_ERROR;
    }

    esp_http_client_config_t cfg = {
        .url = g_ups_config.healthcheck_url,
        .timeout_ms = (int)g_ups_config.health_check_timeout_ms,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to create health check client");
        return HOST_UNREACHABLE_ERROR;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        if (is_expected_disconnect(err))
        {
            ESP_LOGI(TAG, "Health check unavailable: host is likely shutting down or still booting (%s)",
                     esp_err_to_name(err));
            return HOST_UNREACHABLE_EXPECTED;
        }

        ESP_LOGW(TAG, "Health check failed unexpectedly: %s", esp_err_to_name(err));
        return HOST_UNREACHABLE_ERROR;
    }

    if (status >= 200 && status < 300)
    {
        ESP_LOGI(TAG, "Health check responded with HTTP status = %d", status);
        return HOST_REACHABLE;
    }

    /* The service answered but says it is not healthy, which is not the same
       as the host being up. Treating any response as "reachable" leaves a
       failing health endpoint looking like a healthy host forever. */
    ESP_LOGW(TAG, "Health check returned HTTP status = %d, treating host as not healthy", status);
    return HOST_UNREACHABLE_ERROR;
}
