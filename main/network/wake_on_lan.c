#include "wake_on_lan.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TAG "WOL"

static bool parse_mac_address(const char *mac_text, uint8_t mac_bytes[6])
{
    unsigned int values[6];

    if (sscanf(mac_text,
               "%x:%x:%x:%x:%x:%x",
               &values[0],
               &values[1],
               &values[2],
               &values[3],
               &values[4],
               &values[5]) != 6)
    {
        return false;
    }

    for (size_t i = 0; i < 6; ++i)
    {
        mac_bytes[i] = (uint8_t)values[i];
    }

    return true;
}

esp_err_t wake_on_lan_send(const char *mac_address)
{
    uint8_t mac_bytes[6];
    uint8_t packet[102];

    if (!parse_mac_address(mac_address, mac_bytes))
    {
        ESP_LOGE(TAG, "Invalid MAC address: %s", mac_address);
        return ESP_ERR_INVALID_ARG;
    }

    memset(packet, 0xFF, 6);
    for (size_t i = 0; i < 16; ++i)
    {
        memcpy(&packet[6 + (i * 6)], mac_bytes, 6);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        return ESP_FAIL;
    }

    int broadcast_enabled = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enabled, sizeof(broadcast_enabled)) < 0)
    {
        ESP_LOGE(TAG, "Failed to enable broadcast: %d", errno);
        close(sock);
        return ESP_FAIL;
    }

    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(9),
        .sin_addr.s_addr = inet_addr("255.255.255.255"),
    };

    int sent = sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    close(sock);

    if (sent < 0)
    {
        ESP_LOGE(TAG, "Failed to send WOL packet: %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WOL packet sent to %s", mac_address);
    return ESP_OK;
}
