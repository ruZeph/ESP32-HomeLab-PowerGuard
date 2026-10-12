#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum
{
    HOST_REACHABLE,
    HOST_UNREACHABLE_EXPECTED,
    HOST_UNREACHABLE_ERROR
} host_reachability_t;

/**
 * Log the configured endpoint schemes and warn when the shutdown endpoint is
 * plain HTTP or the pinned CA is still the placeholder.
 *
 * Call once at startup so a misconfigured TLS setup is obvious at boot rather
 * than during an outage.
 */
void shutdown_client_init(void);

esp_err_t shutdown_client_send(void);
host_reachability_t shutdown_client_check_host_reachability(void);
