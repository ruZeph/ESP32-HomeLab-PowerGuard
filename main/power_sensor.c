#include "power_sensor.h"

#include "driver/gpio.h"
#include "esp_err.h"

#define POWER_SENSOR_GPIO GPIO_NUM_34

void power_sensor_init(void)
{
    gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << POWER_SENSOR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&input_cfg));
}

power_state_t power_sensor_read(void)
{
    return gpio_get_level(POWER_SENSOR_GPIO) ? POWER_PRESENT : POWER_LOST;
}