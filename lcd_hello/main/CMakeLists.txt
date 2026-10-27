idf_component_register(SRCS "lcd_hello.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_http_server nvs_flash esp_netif
                                esp_lcd driver)
