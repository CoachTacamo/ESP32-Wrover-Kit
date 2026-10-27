/*
 * Wi-Fi controlled RGB LED for ESP-WROVER-KIT v4.1
 *
 * The ESP32 creates its own open Wi-Fi network (SoftAP) called
 * "ESP32-RGB". Connect a phone/laptop to it, open http://192.168.4.1/
 * in a browser, and use the color picker + sliders to drive the LED
 * live. No home router or credentials involved.
 *
 * Pieces:
 *   - LEDC PWM: three channels drive the RGB LED brightness (reused
 *     from the rgb_fade project).
 *   - Wi-Fi SoftAP: the ESP32 is its own access point.
 *   - HTTP server: GET "/" serves the UI, GET "/set?r=..&g=..&b=.."
 *     applies a color.
 *
 * VERIFIED pin mapping on this board (determined empirically via the
 * web UI, driving one pure color channel at a time and comparing to a
 * known on-screen color -- more reliable than the earlier eyeball
 * blink test, which turned out to be wrong):
 *   GPIO0 = Red
 *   GPIO2 = Green
 *   GPIO4 = Blue
 * LED polarity is active-high (pin high -> brighter).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/ledc.h"

static const char *TAG = "rgb_web";

/* ---------- SoftAP settings ---------- */
#define AP_SSID        "ESP32-RGB"
#define AP_CHANNEL     1
#define AP_MAX_CONN    4

/* ---------- verified pin mapping ---------- */
#define LED_RED_GPIO    0
#define LED_GREEN_GPIO  2
#define LED_BLUE_GPIO   4

/* ---------- LEDC config ---------- */
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_FREQ_HZ     5000
#define LEDC_DUTY_RES    LEDC_TIMER_13_BIT
#define LEDC_DUTY_MAX    8191

#define CH_RED    LEDC_CHANNEL_0
#define CH_GREEN  LEDC_CHANNEL_1
#define CH_BLUE   LEDC_CHANNEL_2

/* =====================================================================
 * LED control
 * ===================================================================== */

static void configure_ledc(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    const ledc_channel_t chans[3] = {CH_RED, CH_GREEN, CH_BLUE};
    const int gpios[3] = {LED_RED_GPIO, LED_GREEN_GPIO, LED_BLUE_GPIO};
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t ch_conf = {
            .speed_mode = LEDC_MODE,
            .channel    = chans[i],
            .timer_sel  = LEDC_TIMER,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = gpios[i],
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_conf));
    }
}

static inline uint32_t to_duty(int component)   // component 0..255
{
    if (component < 0)   component = 0;
    if (component > 255) component = 255;
    return (uint32_t)component * LEDC_DUTY_MAX / 255;
}

static void set_rgb(int r, int g, int b)
{
    ledc_set_duty(LEDC_MODE, CH_RED,   to_duty(r));
    ledc_set_duty(LEDC_MODE, CH_GREEN, to_duty(g));
    ledc_set_duty(LEDC_MODE, CH_BLUE,  to_duty(b));
    ledc_update_duty(LEDC_MODE, CH_RED);
    ledc_update_duty(LEDC_MODE, CH_GREEN);
    ledc_update_duty(LEDC_MODE, CH_BLUE);
}

/* =====================================================================
 * Web UI
 * ===================================================================== */

/* A single self-contained page: color picker + three sliders. The JS
 * sends GET /set?r=..&g=..&b=.. on every change. Kept deliberately
 * small so it fits comfortably in a string literal. */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 RGB</title>"
"<style>"
"body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:24px}"
"h1{font-weight:400;font-size:1.3rem}"
"input[type=range]{width:80%;max-width:360px}"
".row{margin:14px 0}"
"#swatch{width:120px;height:120px;border-radius:12px;margin:16px auto;border:1px solid #333}"
"label{display:inline-block;width:1.2em}"
"</style></head><body>"
"<h1>ESP32 RGB LED</h1>"
"<div id='swatch'></div>"
"<div class='row'><input type='color' id='pick' value='#ff0000'></div>"
"<div class='row'><label>R</label><input type='range' id='r' min='0' max='255' value='255'></div>"
"<div class='row'><label>G</label><input type='range' id='g' min='0' max='255' value='0'></div>"
"<div class='row'><label>B</label><input type='range' id='b' min='0' max='255' value='0'></div>"
"<script>"
"var r=document.getElementById('r'),g=document.getElementById('g'),b=document.getElementById('b');"
"var pick=document.getElementById('pick'),sw=document.getElementById('swatch');"
"var t=null;"
"function hex(n){return ('0'+(n|0).toString(16)).slice(-2);}"
"function send(){"
"  var rv=+r.value,gv=+g.value,bv=+b.value;"
"  sw.style.background='#'+hex(rv)+hex(gv)+hex(bv);"
"  pick.value='#'+hex(rv)+hex(gv)+hex(bv);"
"  if(t)clearTimeout(t);"
"  t=setTimeout(function(){fetch('/set?r='+rv+'&g='+gv+'&b='+bv);},30);"
"}"
"[r,g,b].forEach(function(e){e.addEventListener('input',send);});"
"pick.addEventListener('input',function(){"
"  var v=pick.value;"
"  r.value=parseInt(v.substr(1,2),16);"
"  g.value=parseInt(v.substr(3,2),16);"
"  b.value=parseInt(v.substr(5,2),16);"
"  send();"
"});"
"send();"
"</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Pull one integer query param (0..255) out of the query string. */
static int param_value(const char *query, const char *key, int fallback)
{
    char buf[8];
    if (httpd_query_key_value(query, key, buf, sizeof(buf)) == ESP_OK) {
        return atoi(buf);
    }
    return fallback;
}

static esp_err_t set_handler(httpd_req_t *req)
{
    char query[64];
    int r = 0, g = 0, b = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        r = param_value(query, "r", 0);
        g = param_value(query, "g", 0);
        b = param_value(query, "b", 0);
        set_rgb(r, g, b);
        ESP_LOGI(TAG, "set color r=%d g=%d b=%d", r, g, b);
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL,
        };
        httpd_uri_t set = {
            .uri = "/set", .method = HTTP_GET, .handler = set_handler, .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &set);
        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
    return server;
}

/* =====================================================================
 * Wi-Fi SoftAP
 * ===================================================================== */

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,   // open network, no password
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started. SSID:'%s'  ->  http://192.168.4.1/", AP_SSID);
}

/* =====================================================================
 * app_main
 * ===================================================================== */

void app_main(void)
{
    // NVS is required by the Wi-Fi driver to store calibration data.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    configure_ledc();
    set_rgb(255, 0, 0);   // start on red so you can see it's alive

    wifi_init_softap();
    start_webserver();

    ESP_LOGI(TAG, "Ready. Connect to Wi-Fi '%s' and open http://192.168.4.1/", AP_SSID);
}
