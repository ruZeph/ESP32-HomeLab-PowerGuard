#include "leds.h"

#include "driver/gpio.h"
#include "esp_err.h"

#define GREEN_LED GPIO_NUM_25
#define RED_LED   GPIO_NUM_26

void leds_init(void)
{
    gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << GREEN_LED) | (1ULL << RED_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&output_cfg));

    gpio_set_level(GREEN_LED, 0);
    gpio_set_level(RED_LED, 0);
}

void leds_set_power_state(power_state_t state)
{
    switch (state)
    {
        case POWER_PRESENT:
            gpio_set_level(GREEN_LED, 1);
            gpio_set_level(RED_LED, 0);
            break;

        case POWER_LOST:
            gpio_set_level(GREEN_LED, 0);
            gpio_set_level(RED_LED, 1);
            break;

        default:
            gpio_set_level(GREEN_LED, 0);
            gpio_set_level(RED_LED, 0);
            break;
    }
}
