/*
 * LCD "Hello" status display for ESP-WROVER-KIT v4.1  (LVGL version)
 *
 * The ESP32 creates an open Wi-Fi network (SoftAP) "ESP32-RGB" and
 * serves a tiny web page with a text box. Type your name, submit, and
 * the on-board 320x240 ILI9341 LCD shows:
 *
 *     ESP32 LCD Hello           (title)
 *     SSID:    ESP32-RGB
 *     Web UI:  http://192.168.4.1/
 *     Clients: N
 *     Hello, my name is <name>
 *
 * The client count updates automatically as devices join/leave the AP.
 *
 * Rendering is handled by LVGL via Espressif's esp_lvgl_port, which
 * owns the draw buffers, the flush to the panel (including the PSRAM
 * path), a background LVGL task, and professional anti-aliased fonts.
 * We just create labels and update their text.
 *
 * ---------------------------------------------------------------------
 * Pins (Espressif esp-bsp / esp_wrover_kit, verified):
 *   LCD (ILI9341, SPI): MOSI=23 MISO=25 CLK=19 CS=22 DC=21 RST=18 BL=5
 *   Backlight on GPIO5 is ACTIVE-LOW. Landscape 320x240.
 * ---------------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "lcd_hello";

/* ---------- SoftAP ---------- */
#define AP_SSID     "ESP32-RGB"
#define AP_CHANNEL  1
#define AP_MAX_CONN 4
#define AP_URL      "http://192.168.4.1/"

/* ---------- LCD pins ---------- */
#define LCD_HOST        SPI2_HOST
#define PIN_LCD_MOSI    23
#define PIN_LCD_MISO    25
#define PIN_LCD_CLK     19
#define PIN_LCD_CS      22
#define PIN_LCD_DC      21
#define PIN_LCD_RST     18
#define PIN_LCD_BL      5
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)

#define LCD_H_RES 320   // landscape width
#define LCD_V_RES 240   // landscape height

/* ---------- shared state ---------- */
static char s_name[32] = "";
static volatile int s_clients = 0;

/* LVGL label widgets we update at runtime. */
static lv_obj_t *s_lbl_clients = NULL;
static lv_obj_t *s_lbl_hello   = NULL;

/* =====================================================================
 * LCD + LVGL bring-up
 * ===================================================================== */

static void display_init(void)
{
    // Backlight: GPIO5 is ACTIVE-LOW, so drive LOW to turn it ON.
    gpio_config_t bk = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bk));
    gpio_set_level(PIN_LCD_BL, 0);

    // SPI bus.
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_CLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Panel IO.
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    // ILI9341 panel.
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // Initialize the LVGL port (creates LVGL task, tick, etc.).
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    // Register the display. The port owns the buffers + flush. We set
    // the landscape orientation here (swap_xy + mirror_x) rather than on
    // the panel, so LVGL knows the real resolution.
    // Single partial draw buffer. Double-buffering on this slow SPI bus
    // gives little benefit and was causing a periodic full-screen white
    // flash as LVGL swapped between the two partial buffers.
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 40,   // 40-line partial draw buffer
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,   // ILI9341 wants big-endian RGB565
        },
    };
    lvgl_port_add_disp(&disp_cfg);

    ESP_LOGI(TAG, "LVGL display ready (ILI9341 320x240 landscape)");
}

/* =====================================================================
 * Build the UI (must hold the LVGL lock)
 * ===================================================================== */

static void ui_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), 0);

    // Title.
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32 LCD Hello");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00B0FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    // SSID.
    lv_obj_t *ssid = lv_label_create(scr);
    lv_label_set_text(ssid, "SSID: " AP_SSID);
    lv_obj_set_style_text_color(ssid, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ssid, LV_ALIGN_TOP_LEFT, 8, 44);

    // Web UI URL.
    lv_obj_t *url = lv_label_create(scr);
    lv_label_set_text(url, "Web UI: " AP_URL);
    lv_obj_set_style_text_color(url, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(url, LV_ALIGN_TOP_LEFT, 8, 70);

    // Clients (updated at runtime).
    s_lbl_clients = lv_label_create(scr);
    lv_label_set_text(s_lbl_clients, "Clients: 0");
    lv_obj_set_style_text_color(s_lbl_clients, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_lbl_clients, LV_ALIGN_TOP_LEFT, 8, 96);

    // Hello line (updated at runtime).
    s_lbl_hello = lv_label_create(scr);
    lv_label_set_long_mode(s_lbl_hello, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_hello, LCD_H_RES - 16);
    lv_label_set_text(s_lbl_hello, "Type a name in the Web UI");
    lv_obj_set_style_text_color(s_lbl_hello, lv_color_hex(0x30E060), 0);
    lv_obj_set_style_text_font(s_lbl_hello, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_hello, LV_ALIGN_TOP_LEFT, 8, 140);

    lvgl_port_unlock();
}

/* Update the clients label (called from the Wi-Fi event task). */
static void ui_update_clients(void)
{
    if (!s_lbl_clients) return;
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_lbl_clients, "Clients: %d", s_clients);
    lvgl_port_unlock();
}

/* Update the hello label (called from the HTTP task). */
static void ui_update_name(void)
{
    if (!s_lbl_hello) return;
    lvgl_port_lock(0);
    if (s_name[0]) {
        lv_label_set_text_fmt(s_lbl_hello, "Hello, my name is\n%s", s_name);
    } else {
        lv_label_set_text(s_lbl_hello, "Type a name in the Web UI");
    }
    lvgl_port_unlock();
}

/* =====================================================================
 * Web UI
 * ===================================================================== */

static const char PAGE_FMT[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 LCD Hello</title>"
"<style>body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:24px}"
"input{font-size:1.1rem;padding:8px;border-radius:6px;border:1px solid #444;background:#222;color:#eee}"
"button{font-size:1.1rem;padding:8px 16px;margin-top:12px;border-radius:6px;border:0;background:#06f;color:#fff}"
"</style></head><body>"
"<h1>ESP32 LCD Hello</h1>"
"<p>Type your name and send it to the screen.</p>"
"<form method='POST' action='/name'>"
"<input name='name' maxlength='24' placeholder='your name' value='%s'><br>"
"<button type='submit'>Send to LCD</button>"
"</form>"
"<p style='color:#888'>Clients connected: %d</p>"
"</body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    char *page = malloc(sizeof(PAGE_FMT) + 64);
    if (!page) return ESP_ERR_NO_MEM;
    snprintf(page, sizeof(PAGE_FMT) + 64, PAGE_FMT, s_name, s_clients);
    httpd_resp_set_type(req, "text/html");
    esp_err_t r = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return r;
}

static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dst_len; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

static esp_err_t name_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int total = req->content_len;
    if (total > (int)sizeof(body) - 1) total = sizeof(body) - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) break;
        received += r;
    }
    body[received] = '\0';

    char *p = strstr(body, "name=");
    if (p) {
        p += 5;
        char *amp = strchr(p, '&');
        if (amp) *amp = '\0';
        url_decode(p, s_name, sizeof(s_name));
        ESP_LOGI(TAG, "name set to '%s'", s_name);
        ui_update_name();
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, "", 0);
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri="/", .method=HTTP_GET, .handler=root_handler };
        httpd_uri_t name = { .uri="/name", .method=HTTP_POST, .handler=name_handler };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &name);
        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

/* =====================================================================
 * Wi-Fi SoftAP + client tracking
 * ===================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_clients++;
        ESP_LOGI(TAG, "client connected (now %d)", s_clients);
        ui_update_clients();
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_clients > 0) s_clients--;
        ESP_LOGI(TAG, "client disconnected (now %d)", s_clients);
        ui_update_clients();
    }
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP '%s' up -> %s", AP_SSID, AP_URL);
}

/* =====================================================================
 * app_main
 * ===================================================================== */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    display_init();
    ui_create();

    wifi_init_softap();
    start_webserver();

    ESP_LOGI(TAG, "Ready");
}
