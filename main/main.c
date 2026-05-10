#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "buttons.h"
#include "display.h"
#include "splash_screen.h"
#include "portal_mode.h"
#include "wifi_config.h"
#include "loader_menu.h"
#include "factory_self_update.h"
#include "leds.h"

#define TAG "factory_loader"

/* ── Boot-gesture RTC flag ─────────────────────────────────────────── *
 * factory_switch writes BOOT_GESTURE_MAGIC at BOOT_GESTURE_FLAG_ADDR  *
 * when the user presses BOOT after RESET.  Read it at the very top of *
 * app_main() — before buttons_init() or nvs_flash_init() — so that   *
 * nothing in the ESP-IDF init path can zero-fill the RTC fast memory  *
 * region before we sample it.                                          */
#define BOOT_GESTURE_FLAG_ADDR  0x600FFFE8u
#define BOOT_GESTURE_MAGIC      0xB007B007u

static bool s_entered_via_boot_gesture = false;

bool loader_entered_via_boot_gesture(void)
{
    return s_entered_via_boot_gesture;
}

/* ── NVS flag helpers (user_data / badge_cfg) ──────────────────────── */

static bool nvs_read_flag(const char *key)
{
    nvs_flash_init_partition(LOADER_NVS_PARTITION); /* idempotent */
    nvs_handle_t h;
    if (nvs_open_from_partition(LOADER_NVS_PARTITION, LOADER_NVS_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v != 0;
}

static void nvs_write_flag(const char *key, bool val)
{
    nvs_flash_init_partition(LOADER_NVS_PARTITION); /* idempotent */
    nvs_handle_t h;
    if (nvs_open_from_partition(LOADER_NVS_PARTITION, LOADER_NVS_NAMESPACE,
                                NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, val ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

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

    /* Start the background loader-update check before the splash so the
     * WiFi connect + manifest fetch runs concurrently with the animation.
     * Does nothing if WiFi is not yet configured. */
    factory_self_update_begin();

    /* Show the splash only the first time (flag persists in NVS). */
    if (!nvs_read_flag(LOADER_NVS_KEY_SPLASH)) {
        splash_screen_run();
        nvs_write_flag(LOADER_NVS_KEY_SPLASH, true);
    }

    /* Flush any phantom button events queued during boot. */
    buttons_flush_events();

    /* Run WiFi configuration portal if the badge is not yet configured. */
    if (!wifi_config_is_configured()) {
        ESP_LOGI(TAG, "Not configured — launching portal");
        portal_mode_run(0);   /* blocks until user submits credentials */
    } else {
        ESP_LOGI(TAG, "Already configured — entering loader menu");
    }

    /* Check the background task result. Shows confirmation screen only if
     * a newer loader was found; otherwise returns immediately with no delay. */
    factory_self_update_finish();

    /* Hand off to the interactive loader menu. */
    loader_menu_run();

    /* loader_menu_run() loops forever; this point is never reached. */
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void)
{
    volatile uint32_t *rtc_flag = (volatile uint32_t *)BOOT_GESTURE_FLAG_ADDR;
    s_entered_via_boot_gesture = (*rtc_flag == BOOT_GESTURE_MAGIC);
    *rtc_flag = 0;

    run_factory_loader();
}
