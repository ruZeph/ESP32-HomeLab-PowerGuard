#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ups.h"

void app_main(void)
{
    ups_init();

    while (1)
    {
        ups_update();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}