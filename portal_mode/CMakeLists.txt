idf_component_register(
    SRCS        "portal_mode.c"
    INCLUDE_DIRS "include"
    REQUIRES    wifi_config display buttons freertos log esp_system nvs_flash
)
