#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "buttons.h"
#include "display.h"
#include "splash_screen.h"
#include "portal_mode.h"
#include "wifi_config.h"
#include "loader_menu.h"
#include "leds.h"

#define TAG "factory_loader"

/* ── Factory loader full initialisation ────────────────────────────── */

static void run_factory_loader(void)
{
    /* Init buttons FIRST — before display_init/leds_init/nvs_flash_init —
     * to isolate whether PSRAM probe (pre-app_main) or a later peripheral
     * init is holding the pads at 0V. */
    buttons_init();

    nvs_flash_init();
    display_init();
    leds_init();

    leds_clear();
    leds_show();


    splash_screen_run();
    // Flush any phantom button events queued during boot
    buttons_flush_events();

    /* Run WiFi configuration portal if the badge is not yet configured. */
    if (!wifi_config_is_configured()) {
        ESP_LOGI(TAG, "Not configured — launching portal");
        portal_mode_run(0);   /* blocks until user submits credentials */
    } else {
        ESP_LOGI(TAG, "Already configured — entering loader menu");
    }

    /* Hand off to the interactive loader menu. */
    loader_menu_run();

    /* loader_menu_run() loops forever; this point is never reached. */
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void)
{
    run_factory_loader();
}
