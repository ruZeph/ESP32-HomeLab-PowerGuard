#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum
{
    HOST_REACHABLE,
    HOST_UNREACHABLE_EXPECTED,
    HOST_UNREACHABLE_ERROR
} host_reachability_t;

esp_err_t shutdown_client_send(void);
host_reachability_t shutdown_client_check_host_reachability(void);
