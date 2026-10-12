#pragma once

#include <stdbool.h>

void ups_init(void);
void ups_update(void);
void ups_cancel_countdown(void);
void ups_force_shutdown(void);
void ups_publish_current_state(void);

/**
 * True only when the controller is idle, meaning no shutdown countdown or
 * wake sequence would be lost by rebooting the ESP32.
 */
bool ups_is_reboot_safe(void);
