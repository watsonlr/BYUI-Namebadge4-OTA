idf_component_register(
    SRCS        "loader_menu.c" "action_reset_namebadge.c"
    INCLUDE_DIRS "include"
    REQUIRES    display buttons portal_mode ota_manager wifi_config nvs_flash log freertos esp_system
)
