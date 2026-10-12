#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Start SNTP so the device has a wall clock.
 *
 * Call once after the network stack exists, that is after wifi_manager_init().
 * The device has no RTC, so the clock sits near the epoch until the first sync
 * completes and time_sync_is_valid() reports false.
 */
void time_sync_start(void);

/**
 * True once the clock has been corrected to a plausible real time.
 *
 * Anything that emits a timestamp must check this first. A timestamp taken
 * before the first sync is wrong, and emitting it is worse than emitting none.
 */
bool time_sync_is_valid(void);

/**
 * Format the current time as ISO-8601 UTC, for example "2026-10-12T13:20:34Z".
 *
 * Returns false and leaves the buffer untouched when the clock is not yet
 * valid, so callers can omit the field rather than publish a bogus time.
 */
bool time_sync_format_utc(char *buf, size_t buf_len);
