#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "buttons.h"
#include "display.h"
#include "splash_screen.h"
#include "portal_mode.h"
#include "wifi_config.h"
#include "loader_menu.h"
#include "factory_self_update.h"
#include "leds.h"

#define TAG "factory_loader"

/* ── Factory loader full initialisation ────────────────────────────── *
 *
 * This function only runs when the factory partition was selected:
 *   - BOOT button pressed within 500 ms of hardware reset, or
 *   - No valid OTA image installed (otadata blank).
 *
 * On a plain hardware reset with an OTA app installed, factory_switch
 * leaves otadata intact and the bootloader boots the OTA app directly —
 * this function is never called.
 */
static void run_factory_loader(void)
{
    buttons_init();
    nvs_flash_init();
    display_init();
    leds_init();

    leds_clear();
    leds_show();

    splash_screen_run();
    /* Flush any phantom button events queued during boot. */
    buttons_flush_events();

    /* Run WiFi configuration portal if the badge is not yet configured. */
    if (!wifi_config_is_configured()) {
        ESP_LOGI(TAG, "Not configured — launching portal");
        portal_mode_run(0);   /* blocks until user submits credentials */
    } else {
        ESP_LOGI(TAG, "Already configured — entering loader menu");
    }

    /* Check for a newer factory loader and apply it if available.
     * Silently returns if no update is found or WiFi fails.
     * Does NOT return if an update is downloaded (calls esp_restart). */
    factory_self_update_check();

    /* Hand off to the interactive loader menu. */
    loader_menu_run();

    /* loader_menu_run() loops forever; this point is never reached. */
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void)
{
    run_factory_loader();
}
