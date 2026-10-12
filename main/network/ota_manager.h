#pragma once

/**
 * Confirm the running image and start the LAN OTA HTTP server.
 *
 * Call once from app_main() after the network stack exists, that is after
 * wifi_manager_init(). httpd_start() opens a listening socket, so calling this
 * before esp_netif_init() trips an lwIP "Invalid mbox" assert and boot-loops
 * the device.
 */
void ota_manager_init(void);
