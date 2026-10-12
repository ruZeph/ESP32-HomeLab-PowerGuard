#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "ups.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "event_log.h"
#include "ota_manager.h"
#include "time_sync.h"

void app_main(void)
{
    event_log_init();
    wifi_manager_init();

    /* Needs the network stack. Gives the device a UTC wall clock, which stays
       around the epoch until the first sync, so anything emitting a timestamp
       has to check time_sync_is_valid() first. */
    time_sync_start();

    /* Must come after wifi_manager_init(): httpd_start() opens a listening
       socket, so the lwIP stack has to exist first. */
    ota_manager_init();

    mqtt_manager_init();

    ups_init();

    while (1)
    {
        ups_update();
        vTaskDelay(pdMS_TO_TICKS(g_ups_config.power_poll_interval_ms));
    }
}
