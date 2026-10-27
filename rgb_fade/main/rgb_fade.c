/*
 * HSV color-wheel sweep for ESP-WROVER-KIT v4.1
 *
 * We continuously rotate the hue through the full 0..360 degree color
 * wheel at full saturation and brightness. Each step we convert the
 * current HSV color to RGB, then drive each color channel's PWM duty
 * cycle accordingly. All three channels move together, producing a
 * smooth rainbow cycle.
 *
 * VERIFIED pin mapping on this board (determined empirically by
 * watching the earlier one-at-a-time fade -- the colors did NOT match
 * the common online pinouts, so trust the hardware, not the docs):
 *   GPIO0 = Blue
 *   GPIO2 = Red
 *   GPIO4 = Green
 *
 * LED polarity: active-high (pin high -> brighter). Full brightness is
 * duty = LEDC_DUTY_MAX, off is duty = 0. No inversion needed.
 *
 * Why PWM: switching the LED thousands of times per second and varying
 * the on-fraction (duty cycle) lets us set each color's brightness
 * anywhere from 0 to full, which is what color mixing requires.
 */

#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "rgb_fade";

/* ----- verified pin mapping ----- */
#define LED_BLUE_GPIO   0
#define LED_RED_GPIO    2
#define LED_GREEN_GPIO  4

/* ----- LEDC timer / resolution ----- */
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_FREQ_HZ     5000               // 5 kHz, flicker-free to the eye
#define LEDC_DUTY_RES    LEDC_TIMER_13_BIT  // 13-bit -> 0..8191 brightness steps
#define LEDC_DUTY_MAX    8191               // (2^13) - 1 = full brightness

/* ----- color-wheel pacing ----- */
#define HUE_STEP_DEG     2      // degrees of hue advanced per step (smaller = smoother)
#define STEP_DELAY_MS    30     // delay between steps (smaller = faster sweep)

/* One channel per color, each bound to the shared timer. */
#define CH_RED    LEDC_CHANNEL_0
#define CH_GREEN  LEDC_CHANNEL_1
#define CH_BLUE   LEDC_CHANNEL_2

/*
 * Convert HSV to 8-bit RGB.
 *   h: hue in degrees, 0..359
 *   s: saturation, 0..255
 *   v: value/brightness, 0..255
 * Standard sextant-based conversion.
 */
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
    uint8_t region = h / 60;            // which 60-degree sextant (0..5)
    uint8_t remainder = (h % 60) * 255 / 60; // position within the sextant, 0..255

    uint8_t p = (uint16_t)v * (255 - s) / 255;
    uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * remainder) / 255) / 255;
    uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - remainder)) / 255) / 255;

    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

static void configure_ledc(void)
{
    // Shared timer: sets PWM frequency and duty resolution.
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    // One channel per color, mapped to the verified GPIOs.
    const ledc_channel_t chans[3] = {CH_RED, CH_GREEN, CH_BLUE};
    const int gpios[3] = {LED_RED_GPIO, LED_GREEN_GPIO, LED_BLUE_GPIO};
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t ch_conf = {
            .speed_mode = LEDC_MODE,
            .channel    = chans[i],
            .timer_sel  = LEDC_TIMER,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = gpios[i],
            .duty       = 0,   // start off
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_conf));
    }
}

// Map an 8-bit color component (0..255) to the full PWM duty range.
static inline uint32_t to_duty(uint8_t component)
{
    return (uint32_t)component * LEDC_DUTY_MAX / 255;
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    ledc_set_duty(LEDC_MODE, CH_RED,   to_duty(r));
    ledc_set_duty(LEDC_MODE, CH_GREEN, to_duty(g));
    ledc_set_duty(LEDC_MODE, CH_BLUE,  to_duty(b));

    ledc_update_duty(LEDC_MODE, CH_RED);
    ledc_update_duty(LEDC_MODE, CH_GREEN);
    ledc_update_duty(LEDC_MODE, CH_BLUE);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Configuring LEDC for HSV color-wheel sweep");
    configure_ledc();

    ESP_LOGI(TAG, "Starting color-wheel loop");
    uint16_t hue = 0;
    while (1) {
        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, 255, &r, &g, &b); // full saturation + brightness
        set_rgb(r, g, b);

        hue = (hue + HUE_STEP_DEG) % 360;
        vTaskDelay(STEP_DELAY_MS / portTICK_PERIOD_MS);
    }
}
