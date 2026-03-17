// Implements the Reset Namebadge action for the loader menu.
#include "loader_menu.h"
#include "display.h"
#include "buttons.h"
#include "wifi_config.h"
#include "portal_mode.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

void action_reset_namebadge(void) {
    // Confirmation prompt
    ESP_LOGI("reset_namebadge", "Prompting for confirmation");
    display_fill(DISPLAY_COLOR_BLACK);
    display_draw_string(20, 50, "Reset customization?", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    display_draw_string(20, 80, "This will erase your", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    display_draw_string(20, 105, "name and WiFi info,", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    display_draw_string(20, 130, "then re-run setup.", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    display_draw_string(20, 170, "A: Yes    B: No", DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_BLACK, 2);

    for (;;) {
        button_t btn = buttons_wait_event(0);
        ESP_LOGI("reset_namebadge", "Button pressed: 0x%02X", btn);
        if (btn & BTN_A) {
            ESP_LOGI("reset_namebadge", "BTN_A pressed: erasing customization");
            nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);
            nvs_flash_init_partition(WIFI_CONFIG_NVS_PARTITION);
            display_fill(DISPLAY_COLOR_BLACK);
            display_draw_string(40, 100, "Customization erased!", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
            display_draw_string(40, 140, "Starting setup...", DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
            vTaskDelay(pdMS_TO_TICKS(1500));
            portal_mode_run(0);
            break;
        } else if (btn & BTN_B) {
            ESP_LOGI("reset_namebadge", "BTN_B pressed: cancel");
            // Cancel and return to previous menu
            break;
        }
    }
}
