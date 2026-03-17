idf_component_register(
    SRCS        "main.c"
    INCLUDE_DIRS "."
    REQUIRES    buttons loader_menu display splash_screen portal_mode wifi_config
                ota_manager leds log esp_psram app_update nvs_flash
)
