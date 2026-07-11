#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

static const char *TAG = "UPS";

void app_main(void)
{
    ESP_LOGI(TAG, "==================================");
    ESP_LOGI(TAG, "UPS Controller Started");
    ESP_LOGI(TAG, "ESP-IDF Version");
    ESP_LOGI(TAG, "==================================");

    while (1)
    {
        ESP_LOGI(TAG, "Still running...");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}