#pragma once

#include <stdint.h>

typedef enum
{
    EVT_ESP_BOOTED = 0,
    EVT_POWER_LOST,
    EVT_POWER_RESTORED,
    EVT_SHUTDOWN_TRIGGERED,
    EVT_SHUTDOWN_FAILED,
    EVT_SHUTDOWN_CANCELLED,
    EVT_HOST_DOWN_CONFIRMED,
    EVT_HOST_BACK_ONLINE,
    EVT_WOL_SENT,
    EVT_WOL_RETRY,
    EVT_WOL_FAILED,
} event_type_t;

/**
 * Initialise the NVS-backed event ring buffer.
 * Call once before any other event_log function.
 */
void event_log_init(void);

/**
 * Record a critical event with optional context byte.
 * Written to NVS flash immediately so it survives power loss.
 *
 * @param type  The event type.
 * @param extra Context value (e.g. WOL retry number, 0 if unused).
 */
void event_log_record(event_type_t type, uint8_t extra);

/**
 * Format the entire event log as a JSON array string.
 *
 * @param buf      Output buffer.
 * @param buf_size Size of the output buffer.
 * @return Number of bytes written (excluding null terminator), or 0 on error.
 */
int event_log_format_json(char *buf, int buf_size);

/**
 * Clear all recorded events from NVS.
 * Call after a successful publish to start fresh.
 */
void event_log_clear(void);
