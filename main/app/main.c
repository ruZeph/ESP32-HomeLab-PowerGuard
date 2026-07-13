#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "ups.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "event_log.h"

void app_main(void)
{
    event_log_init();
    wifi_manager_init();
    mqtt_manager_init();

    ups_init();

    while (1)
    {
        ups_update();
        vTaskDelay(pdMS_TO_TICKS(g_ups_config.power_poll_interval_ms));
    }
}
