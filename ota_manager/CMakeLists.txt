idf_component_register(
    SRCS        "ota_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES    freertos log
                nvs_flash
                esp_wifi esp_event esp_netif
                esp_http_client esp-tls
                app_update
                esp_psram heap
                mbedtls json
                display
)
