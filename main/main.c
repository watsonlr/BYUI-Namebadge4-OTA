#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "buttons.h"
#include "display.h"
#include "splash_screen.h"
#include "portal_mode.h"
#include "wifi_config.h"
#include "loader_menu.h"
#include "leds.h"

#define TAG "factory_loader"

/* ── OTA redirect ───────────────────────────────────────────────────────
 * Called early in startup, after buttons_init() and display_init().
 *
 * The bootloader erased otadata on every hardware reset, so we always land
 * here first.  If ota_0 has a valid image AND A+B are not held, redirect
 * to the OTA app via a software restart.  The bootloader will honour the
 * otadata we write and boot ota_0 (software restart → no otadata erase).
 *
 * A+B held → fall through to the factory loader menu (escape hatch). */
static void maybe_redirect_to_ota(void)
{
    const esp_partition_t *ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!ota0) {
        ESP_LOGI(TAG, "No ota_0 partition found — staying in factory");
        return;
    }

    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(ota0, &app_desc) != ESP_OK) {
        ESP_LOGI(TAG, "ota_0 has no valid image — staying in factory");
        return;
    }

    ESP_LOGI(TAG, "ota_0 has valid image: %s %s", app_desc.project_name, app_desc.version);

    /* Give buttons time to settle, then check A+B (100 ms hold). */
    if (buttons_held(BTN_A | BTN_B, 100)) {
        ESP_LOGI(TAG, "A+B held — staying in factory loader");
        return;
    }

    /* No A+B → redirect to OTA app. */
    ESP_LOGI(TAG, "Redirecting to ota_0 via software restart");
    esp_ota_set_boot_partition(ota0);
    esp_restart();
    /* Not reached. */
}

/* ── Factory loader full initialisation ────────────────────────────── */

static void run_factory_loader(void)
{
    buttons_init();
    nvs_flash_init();
    display_init();
    leds_init();

    /* Redirect to OTA app on plain hardware reset; stay here if A+B held. */
    maybe_redirect_to_ota();

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

    /* Hand off to the interactive loader menu. */
    loader_menu_run();

    /* loader_menu_run() loops forever; this point is never reached. */
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void)
{
    run_factory_loader();
}
