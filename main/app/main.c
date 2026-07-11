#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "ups.h"
#include "wifi_manager.h"

void app_main(void)
{
    wifi_manager_init();

    ups_init();

    while (1)
    {
        ups_update();
        vTaskDelay(pdMS_TO_TICKS(g_ups_config.power_poll_interval_ms));
    }
}
