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
 * Outcome of a shutdown request.
 *
 * UNCONFIRMED exists because a host that stops its services before replying
 * looks exactly like a timeout, and the request may well have been accepted.
 * Callers must not treat it as a failure; the host state is the real signal.
 */
typedef enum
{
    SHUTDOWN_REQUEST_ACCEPTED,    /* the host answered 2xx */
    SHUTDOWN_REQUEST_UNCONFIRMED, /* sent, but no usable response arrived */
    SHUTDOWN_REQUEST_FAILED       /* not delivered, or rejected by the host */
} shutdown_result_t;

shutdown_result_t shutdown_client_send(void);
host_reachability_t shutdown_client_check_host_reachability(void);
