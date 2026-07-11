#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "ups.h"

void app_main(void)
{
    ups_init();

    while (1)
    {
        ups_update();
        vTaskDelay(pdMS_TO_TICKS(g_ups_config.power_poll_interval_ms));
    }
}