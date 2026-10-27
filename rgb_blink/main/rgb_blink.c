/*
 * RGB LED blink for ESP-WROVER-KIT v4.1
 *
 * The on-board RGB LED is wired to three GPIOs. Each color is an
 * independent output, so we cycle through red -> green -> blue -> off.
 *
 * Wrover-Kit v4.1 LED pin mapping:
 *   Red   = GPIO0
 *   Green = GPIO2
 *   Blue  = GPIO4
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "rgb_blink";

#define LED_RED   GPIO_NUM_0
#define LED_GREEN GPIO_NUM_2
#define LED_BLUE  GPIO_NUM_4

// How long each color stays on, in milliseconds.
#define STEP_DELAY_MS 500

static void configure_leds(void)
{
    // Reset each pin, then set it as a plain push-pull output.
    const gpio_num_t leds[] = {LED_RED, LED_GREEN, LED_BLUE};
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(leds[i]);
        gpio_set_direction(leds[i], GPIO_MODE_OUTPUT);
        gpio_set_level(leds[i], 0); // start off
    }
}

static void set_rgb(int r, int g, int b)
{
    gpio_set_level(LED_RED, r);
    gpio_set_level(LED_GREEN, g);
    gpio_set_level(LED_BLUE, b);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Configuring RGB LED GPIOs");
    configure_leds();

    ESP_LOGI(TAG, "Starting color cycle");
    while (1) {
        set_rgb(1, 0, 0); // red
        vTaskDelay(STEP_DELAY_MS / portTICK_PERIOD_MS);

        set_rgb(0, 1, 0); // green
        vTaskDelay(STEP_DELAY_MS / portTICK_PERIOD_MS);

        set_rgb(0, 0, 1); // blue
        vTaskDelay(STEP_DELAY_MS / portTICK_PERIOD_MS);

        set_rgb(0, 0, 0); // off
        vTaskDelay(STEP_DELAY_MS / portTICK_PERIOD_MS);
    }
}
