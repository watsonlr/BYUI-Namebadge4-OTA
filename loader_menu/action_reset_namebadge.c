#include "loader_menu.h"
#include "display.h"
#include "buttons.h"
#include "leds.h"
#include "wifi_config.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "reset_action"

#define COLOR_HEADER_BG  DISPLAY_RGB565(  0,  36,  96)
#define COLOR_FOOTER_BG  DISPLAY_RGB565( 24,  24,  24)
#define COLOR_WARN       DISPLAY_RGB565(220,  80,   0)

/* ── Shared helpers ─────────────────────────────────────────────────── */

static void draw_confirm_screen(const char *title,
                                const char *lines[], int num_lines)
{
    display_fill(DISPLAY_COLOR_BLACK);

    /* Header */
    display_fill_rect(0, 0, DISPLAY_W, 30, COLOR_HEADER_BG);
    int tw = (int)strlen(title) * DISPLAY_FONT_W * 2;
    display_draw_string((DISPLAY_W - tw) / 2, 7, title,
                        DISPLAY_COLOR_WHITE, COLOR_HEADER_BG, 2);

    /* Body lines */
    for (int i = 0; i < num_lines; i++)
        display_draw_string(12, 44 + i * 20, lines[i],
                            DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);

    /* Footer */
    display_fill_rect(0, 212, DISPLAY_W, 28, COLOR_FOOTER_BG);
    const char *hint = "A:Confirm  B:Cancel";
    int hw = (int)strlen(hint) * DISPLAY_FONT_W;
    display_draw_string((DISPLAY_W - hw) / 2, 220, hint,
                        DISPLAY_COLOR_YELLOW, COLOR_FOOTER_BG, 1);
}

static bool wait_confirm(void)
{
    for (;;) {
        button_t btn = buttons_wait_event(0);
        if (btn & BTN_A) return true;
        if (btn & BTN_B) return false;
    }
}

/* Like wait_confirm() but blinks all LEDs dim red at ~1 Hz as a warning.
 * Turns LEDs off before returning regardless of which button was pressed. */
static bool wait_confirm_led_warning(void)
{
    bool led_on = true;
    leds_fill(8, 0, 0);
    leds_show();

    for (;;) {
        button_t btn = buttons_wait_event(500);

        if (btn & BTN_A) { leds_clear(); leds_show(); return true;  }
        if (btn & BTN_B) { leds_clear(); leds_show(); return false; }

        led_on = !led_on;
        if (led_on) { leds_fill(8, 0, 0); } else { leds_clear(); }
        leds_show();
    }
}

static void show_erasing(void)
{
    display_fill(DISPLAY_COLOR_BLACK);
    const char *msg = "Erasing...";
    int mw = (int)strlen(msg) * DISPLAY_FONT_W * 2;
    display_draw_string((DISPLAY_W - mw) / 2, 108,
                        msg, COLOR_WARN, DISPLAY_COLOR_BLACK, 2);
}

/* ── Action: Reset WiFi & Config ────────────────────────────────────── *
 * Erases user_data NVS (WiFi creds, nickname, manifest URL).
 * OTA apps are left intact — the badge reboots into OTA normally, but
 * factory_switch will route through the factory loader on next hardware
 * reset so the portal runs again.
 */
void action_reset_wifi_config(void)
{
    const char *lines[] = {
        "Erases your WiFi password,",
        "nickname, and manifest URL.",
        "",
        "Your installed app is kept.",
        "You will re-run WiFi setup",
        "on next factory boot.",
    };
    draw_confirm_screen("Reset Wifi/Config",
                        lines, sizeof(lines) / sizeof(lines[0]));

    if (!wait_confirm()) return;

    show_erasing();
    ESP_LOGI(TAG, "Erasing user_data NVS");
    nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── Action: Reset Board to Factory ─────────────────────────────────── *
 * Erases user_data NVS + ota_0 + ota_1 + otadata.
 * On reboot the board is in the same state as a freshly flashed board:
 * no student app, no WiFi config, portal runs automatically.
 */
void action_reset_board_factory(void)
{
    const char *lines[] = {
        "Erases ALL apps AND your",
        "WiFi / nickname settings.",
        "",
        "Board returns to factory",
        "state. WiFi setup will run",
        "on next boot.",
    };
    draw_confirm_screen("Full Factory Reset",
                        lines, sizeof(lines) / sizeof(lines[0]));

    if (!wait_confirm_led_warning()) return;

    show_erasing();
    ESP_LOGI(TAG, "Erasing user_data, ota_0, ota_1, otadata");

    /* Erase WiFi / badge config */
    nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);

    /* Erase student app slots */
    const esp_partition_t *p;
    p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (p) esp_partition_erase_range(p, 0, p->size);

    p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (p) esp_partition_erase_range(p, 0, p->size);

    /* Erase otadata so bootloader falls back to factory */
    p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (p) esp_partition_erase_range(p, 0, p->size);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
