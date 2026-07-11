#include "config.h"

const ups_config_t g_ups_config = {
    .power_poll_interval_ms = 100U,
    .shutdown_delay_seconds = 10U,   // test value; change later to 120/180
};