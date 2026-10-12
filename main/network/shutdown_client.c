#include "shutdown_client.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/x509_crt.h"

#define TAG "SHUTDOWN"

/* The CA that signs the shutdown endpoint's certificate, embedded at build
   time so the device verifies its peer instead of trusting whatever answers.
   Regenerate with deploy/generate-certs.sh. */
extern const uint8_t poweroff_ca_pem_start[] asm("_binary_poweroff_ca_pem_start");
extern const uint8_t poweroff_ca_pem_end[] asm("_binary_poweroff_ca_pem_end");

#define CA_PLACEHOLDER_MARKER "REPLACE-ME"

static bool has_text(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static bool url_is_https(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

/* Pin the embedded CA for https endpoints. Without this the client would fail
   to verify, which is the intended behaviour: an unverified TLS connection is
   not better than plain HTTP for hiding a bearer token. */
static void apply_tls_config(esp_http_client_config_t *cfg, const char *url)
{
    if (url_is_https(url))
    {
        cfg->cert_pem = (const char *)poweroff_ca_pem_start;
    }
}

/**
 * True when the embedded CA is unusable and https therefore cannot verify.
 *
 * The placeholder CA committed in this repository is a valid certificate, so
 * the marker lives in its subject rather than in searchable PEM text: a PEM is
 * base64-encoded DER, so grepping the file for a marker can never match. The
 * subject has to be parsed out to tell the placeholder from a real CA.
 */
static bool embedded_ca_needs_replacing(void)
{
    size_t ca_len = (size_t)(poweroff_ca_pem_end - poweroff_ca_pem_start);
    if (ca_len <= 1)
    {
        return true;
    }

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);

    bool needs_replacing = true;

    if (mbedtls_x509_crt_parse(&crt, poweroff_ca_pem_start, ca_len) == 0)
    {
        char subject[256];
        int written = mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);
        needs_replacing = (written <= 0) || (strstr(subject, CA_PLACEHOLDER_MARKER) != NULL);
    }

    mbedtls_x509_crt_free(&crt);
    return needs_replacing;
}

void shutdown_client_init(void)
{
    bool shutdown_secure = url_is_https(g_ups_config.shutdown_url);
    bool health_secure = url_is_https(g_ups_config.healthcheck_url);

    ESP_LOGI(TAG, "Shutdown endpoint: %s, health endpoint: %s",
             shutdown_secure ? "https" : "http",
             health_secure ? "https" : "http");

    if (!shutdown_secure)
    {
        ESP_LOGW(TAG, "Shutdown endpoint is plain HTTP: the bearer token crosses the network in clear text");
    }

    if (embedded_ca_needs_replacing())
    {
        ESP_LOGW(TAG, "Embedded CA is not a usable CA, HTTPS would fail verification. "
                      "Replace main/certs/poweroff_ca.pem using deploy/generate-certs.sh");
    }
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
    apply_tls_config(&cfg, g_ups_config.shutdown_url);

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
    apply_tls_config(&cfg, g_ups_config.healthcheck_url);

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
