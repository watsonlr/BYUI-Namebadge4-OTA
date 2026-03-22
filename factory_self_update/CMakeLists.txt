idf_component_register(
    SRCS        "factory_self_update.c"
    INCLUDE_DIRS "include"
    REQUIRES    freertos log
                nvs_flash
                esp_wifi esp_event esp_netif
                esp_http_client esp-tls
                app_update esp_partition
                mbedtls json
                display loader_menu wifi_config
                esp_system
)
