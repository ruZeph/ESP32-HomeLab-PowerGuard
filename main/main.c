#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POWER_GPIO GPIO_NUM_34

#define GREEN_LED GPIO_NUM_25
#define RED_LED GPIO_NUM_26

static const char *TAG = "UPS";

/*----------------------------------------------------------
 * Types
 *---------------------------------------------------------*/

typedef enum
{
    POWER_UNKNOWN = -1,
    POWER_LOST = 0,
    POWER_PRESENT = 1
} power_state_t;

/*----------------------------------------------------------
 * Function Prototypes
 *---------------------------------------------------------*/

static void configure_gpio(void);

static power_state_t read_power_state(void);

static void update_leds(power_state_t state);

static void on_power_lost(void);
static void on_power_restored(void);

static void handle_power_change(power_state_t state);

/*----------------------------------------------------------
 * GPIO Configuration
 *---------------------------------------------------------*/

static void configure_gpio(void)
{
    gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << POWER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&input_cfg));

    gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << GREEN_LED) |
                        (1ULL << RED_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&output_cfg));

    ESP_LOGI(TAG, "GPIO initialized");
}

/*----------------------------------------------------------
 * Read Power State
 *---------------------------------------------------------*/

static power_state_t read_power_state(void)
{
    return gpio_get_level(POWER_GPIO)
               ? POWER_PRESENT
               : POWER_LOST;
}

/*----------------------------------------------------------
 * LED Control
 *---------------------------------------------------------*/

static void update_leds(power_state_t state)
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

/*----------------------------------------------------------
 * Power Lost Event
 *---------------------------------------------------------*/

static void on_power_lost(void)
{
    update_leds(POWER_LOST);

    ESP_LOGW(TAG, "Power LOST");

    /*
        Future:

        - Start shutdown timer
        - Sound buzzer
        - Notify Home Assistant
    */
}

/*----------------------------------------------------------
 * Power Restored Event
 *---------------------------------------------------------*/

static void on_power_restored(void)
{
    update_leds(POWER_PRESENT);

    ESP_LOGI(TAG, "Power RESTORED");

    /*
        Future:

        - Cancel shutdown timer
        - Stop buzzer
        - Send notification
    */
}

/*----------------------------------------------------------
 * State Change Dispatcher
 *---------------------------------------------------------*/

static void handle_power_change(power_state_t state)
{
    switch (state)
    {
    case POWER_PRESENT:
        on_power_restored();
        break;

    case POWER_LOST:
        on_power_lost();
        break;

    default:
        break;
    }
}

/*----------------------------------------------------------
 * Main Application
 *---------------------------------------------------------*/

void app_main(void)
{
    ESP_LOGI(TAG, "UPS Controller Starting...");

    configure_gpio();

    power_state_t previousState = POWER_UNKNOWN;

    while (1)
    {
        power_state_t currentState = read_power_state();

        if (currentState != previousState)
        {
            handle_power_change(currentState);

            previousState = currentState;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}