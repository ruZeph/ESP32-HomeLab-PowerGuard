#pragma once

/**
 * Confirm the running image and start the LAN OTA HTTP server.
 *
 * Call once at the start of app_main() so that a freshly flashed image is
 * confirmed as valid before anything else can fail.
 */
void ota_manager_init(void);
