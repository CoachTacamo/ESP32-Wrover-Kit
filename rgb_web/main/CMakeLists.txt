idf_component_register(SRCS "rgb_web.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_http_server nvs_flash esp_netif driver)
