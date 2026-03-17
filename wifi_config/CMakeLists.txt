idf_component_register(
    SRCS        "wifi_config.c"
    INCLUDE_DIRS "include"
    REQUIRES    esp_wifi esp_netif esp_http_server nvs_flash esp_event lwip
)
