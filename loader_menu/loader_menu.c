
#include "loader_menu.h"

#include "display.h"
#include "buttons.h"
#include "portal_mode.h"
#include "ota_manager.h"
#include "wifi_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* Provided by main.c — sampled at the very top of app_main() before any init. */
extern bool loader_entered_via_boot_gesture(void);

#define TAG              "loader_menu"
/* Version defines live in loader_menu.h so factory_self_update can read them. */
#define HW_VERSION       LOADER_HW_VERSION
#define LOADER_VERSION   LOADER_SW_VERSION

/* ── Palette ───────────────────────────────────────────────────────── */
#define COLOR_BYUI_BLUE   DISPLAY_RGB565(  0,  46, 120)
#define COLOR_HEADER_BG   DISPLAY_RGB565(  0,  36,  96)
#define COLOR_DARK        DISPLAY_RGB565( 30,  30,  30)
#define COLOR_YELLOW      DISPLAY_RGB565(255, 200,   0)
#define COLOR_FOOTER_BG   DISPLAY_RGB565( 24,  24,  24)
#define COLOR_DIM_WHITE   DISPLAY_RGB565(180, 180, 180)

/* ── Layout constants (320 × 240 landscape) ────────────────────────── */
#define HEADER_Y      0
#define HEADER_H     30
#define ITEMS_START  32          /* first pixel below header            */
#define ITEM_H       30          /* 6 × 30 = 180 px                     */
#define FOOTER_Y    212
#define FOOTER_H     28

#define ITEM_TEXT_SCALE  2
#define ITEM_CHAR_W     (DISPLAY_FONT_W * ITEM_TEXT_SCALE)
#define ITEM_ARROW_X     8
#define ITEM_TEXT_X     32

#define NUM_ITEMS     6
#define VISIBLE_ITEMS 6          /* item rows visible at once           */
#define VISIBLE_APPS  4          /* max app rows visible at once        */

/* ── Main menu labels ──────────────────────────────────────────────── */
static const char *ITEM_LABELS[NUM_ITEMS] = {
    "OTA App Download",
    "Return to Last App",
    "SDCard Apps",
    "Reset Wifi/Config",
    "Full Factory Reset",
    "Load MicroPython",
};

static const char *item_label(int idx)
{
    return ITEM_LABELS[idx];
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static int item_y(int idx)
{
    return ITEMS_START + idx * ITEM_H;
}

static void draw_header_titled(const char *title)
{
    display_fill_rect(0, HEADER_Y, DISPLAY_W, HEADER_H, COLOR_HEADER_BG);
    int tw = (int)strlen(title) * DISPLAY_FONT_W * 2;
    int tx = (DISPLAY_W - tw) / 2;
    if (tx < 4) tx = 4;
    display_draw_string(tx, HEADER_Y + 7, title,
                        DISPLAY_COLOR_WHITE, COLOR_HEADER_BG, 2);
}

static void draw_footer(const char *hint)
{
    display_fill_rect(0, FOOTER_Y, DISPLAY_W, FOOTER_H, COLOR_FOOTER_BG);
    int hw = (int)strlen(hint) * DISPLAY_FONT_W;
    int hx = (DISPLAY_W - hw) / 2;
    if (hx < 4) hx = 4;
    display_draw_string(hx, FOOTER_Y + 8, hint,
                        DISPLAY_COLOR_WHITE, COLOR_FOOTER_BG, 1);
}

static void draw_item(int row, int idx, bool selected)
{
    int y = item_y(row);

    uint16_t bg = selected ? DISPLAY_COLOR_RED : DISPLAY_COLOR_WHITE;
    uint16_t fg = selected ? DISPLAY_COLOR_WHITE : COLOR_DARK;

    display_fill_rect(0, y, DISPLAY_W, ITEM_H, bg);

    if (selected) {
        /* 3 px yellow border top and bottom for extra visibility */
        display_fill_rect(0, y,                  DISPLAY_W, 3, COLOR_YELLOW);
        display_fill_rect(0, y + ITEM_H - 3,     DISPLAY_W, 3, COLOR_YELLOW);
        display_draw_string(ITEM_ARROW_X, y + 7, ">",
                            COLOR_YELLOW, bg, ITEM_TEXT_SCALE);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%s", item_label(idx));
    display_draw_string(ITEM_TEXT_X, y + 7, buf, fg, bg, ITEM_TEXT_SCALE);
}

static void draw_menu(int selected, int scroll)
{
    char title[32];
    snprintf(title, sizeof(title), "BYU-I Loader (v%d.%d)", HW_VERSION, LOADER_VERSION);
    draw_header_titled(title);
    /* Fill the 2px gap between header and first item (y=30-31). */
    display_fill_rect(0, HEADER_H, DISPLAY_W, ITEMS_START - HEADER_H, DISPLAY_COLOR_BLACK);
    for (int row = 0; row < VISIBLE_ITEMS; row++) {
        int idx = scroll + row;
        if (idx < NUM_ITEMS)
            draw_item(row, idx, idx == selected);
        else
            display_fill_rect(0, item_y(row), DISPLAY_W, ITEM_H, DISPLAY_COLOR_BLACK);
    }
    /* Fill any gap between last item and footer. */
    int gap_top = ITEMS_START + VISIBLE_ITEMS * ITEM_H;
    if (gap_top < FOOTER_Y)
        display_fill_rect(0, gap_top, DISPLAY_W, FOOTER_Y - gap_top, DISPLAY_COLOR_BLACK);

    draw_footer("Up/Dn:move  Right/A:select");
}

/* ── Info / stub screens ───────────────────────────────────────────── */

static void wait_any_button(void)
{
    buttons_wait_event(0);
}

static void show_info_screen(const char *title,
                             const char *lines[],
                             int         num_lines)
{
    display_fill(DISPLAY_COLOR_BLACK);
    display_fill_rect(0, 0, DISPLAY_W, 28, COLOR_BYUI_BLUE);
    int tw = (int)strlen(title) * DISPLAY_FONT_W * 2;
    display_draw_string((DISPLAY_W - tw) / 2, 6, title,
                        DISPLAY_COLOR_WHITE, COLOR_BYUI_BLUE, 2);

    for (int i = 0; i < num_lines; i++) {
        display_draw_string(12, 44 + i * 22, lines[i],
                            DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    }

    draw_footer("Press any button to return");
    wait_any_button();
}

/* ── Text-only app-selection menu ─────────────────────────────────────── *
 *
 * Layout (320×240, text listing):
 *   Header  y =   0 .. 29   (30 px)
 *   Gap     y =  30 .. 31   (2 px)
 *   Apps    y =  32 .. 211  (6 × 30px rows = 180 px)
 *   Footer  y = 212 .. 239  (28 px)
 *
 * Shows app name + version in text format, no icon loading required.
 * ────────────────────────────────────────────────────────────────────── */

#define APP_ITEM_H      30
#define APP_ITEMS_START 32
#define APP_VISIBLE     6

/* Draw one app item row in text format. */
static void draw_app_item(int row, const ota_app_entry_t *app, bool selected)
{
    int y = APP_ITEMS_START + row * APP_ITEM_H;

    uint16_t bg = selected ? DISPLAY_COLOR_RED : DISPLAY_COLOR_WHITE;
    uint16_t fg = selected ? DISPLAY_COLOR_WHITE : COLOR_DARK;

    display_fill_rect(0, y, DISPLAY_W, APP_ITEM_H, bg);

    if (selected) {
        /* 3 px yellow border top and bottom for extra visibility */
        display_fill_rect(0, y,                      DISPLAY_W, 3, COLOR_YELLOW);
        display_fill_rect(0, y + APP_ITEM_H - 3,     DISPLAY_W, 3, COLOR_YELLOW);
        display_draw_string(ITEM_ARROW_X, y + 7, ">",
                            COLOR_YELLOW, bg, ITEM_TEXT_SCALE);
    }

    /* App name at scale 2 */
    char name_buf[32];
    snprintf(name_buf, sizeof(name_buf), "%s", app->name);
    display_draw_string(ITEM_TEXT_X, y + 3, name_buf, fg, bg, 2);

    /* Version at scale 1, right side */
    char ver_buf[16];
    snprintf(ver_buf, sizeof(ver_buf), "v%" PRIu32, app->version);
    int ver_w = (int)strlen(ver_buf) * DISPLAY_FONT_W;
    display_draw_string(DISPLAY_W - ver_w - 8, y + 16,
                        ver_buf, selected ? COLOR_DIM_WHITE : COLOR_DARK, bg, 1);
}

static void draw_app_menu(const ota_catalog_t *catalog, int selection, int scroll)
{
    draw_header_titled("Select App");
    
    /* Fill the 2px gap between header and first item */
    display_fill_rect(0, HEADER_H, DISPLAY_W, APP_ITEMS_START - HEADER_H, DISPLAY_COLOR_BLACK);
    
    for (int row = 0; row < APP_VISIBLE; row++) {
        int idx = scroll + row;
        if (idx < catalog->count) {
            draw_app_item(row, &catalog->apps[idx], idx == selection);
        } else {
            display_fill_rect(0, APP_ITEMS_START + row * APP_ITEM_H, 
                              DISPLAY_W, APP_ITEM_H, DISPLAY_COLOR_BLACK);
        }
    }
    
    /* Fill any gap between last item and footer */
    int gap_top = APP_ITEMS_START + APP_VISIBLE * APP_ITEM_H;
    if (gap_top < FOOTER_Y)
        display_fill_rect(0, gap_top, DISPLAY_W, FOOTER_Y - gap_top, DISPLAY_COLOR_BLACK);

    const char *hint = (catalog->count > 1)
                       ? "Up/Dn:move  Right/A:select  B:back"
                       : "Right/A:select  B:back";
    draw_footer(hint);
}

/**
 * Run the text-based app-selection menu.
 * @return selected index (0..count-1), or -1 if user pressed B (back).
 */
static int run_app_select_menu(const ota_catalog_t *catalog)
{
    int selection = 0;
    int scroll    = 0;

    draw_app_menu(catalog, selection, scroll);

    for (;;) {
        button_t btn = buttons_wait_event(0);

        if (btn & BTN_UP) {
            if (selection > 0) {
                selection--;
                if (selection < scroll) scroll = selection;
                draw_app_menu(catalog, selection, scroll);
            }

        } else if (btn & BTN_DOWN) {
            if (selection < catalog->count - 1) {
                selection++;
                if (selection >= scroll + APP_VISIBLE)
                    scroll = selection - APP_VISIBLE + 1;
                draw_app_menu(catalog, selection, scroll);
            }

        } else if (btn & (BTN_LEFT | BTN_A | BTN_RIGHT)) {
            return selection;

        } else if (btn & BTN_B) {
            return -1;
        }
    }
}

static bool s_hint_shown = false;  /* suppress repeat within one session */

/* ── Action: OTA download ──────────────────────────────────────────── */

/* Static catalog buffer — too large for the stack. */
static ota_catalog_t   s_catalog;

static void action_ota_download(void)
{
    /* Show the RESET / BOOT-escape hint unless the user already proved they
     * know the gesture (factory_switch set the RTC flag on BOOT press), or
     * we already showed it this session. */
    if (!s_hint_shown && !loader_entered_via_boot_gesture()) {
        loader_menu_show_boot_hint();
    }
    s_hint_shown = true;

    /* ── Step 1: fetch catalog ─────────────────────────────────────── */
    ota_result_t r = ota_manager_fetch_catalog(&s_catalog);

    if (r == OTA_RESULT_NO_WIFI) {
        display_fill(DISPLAY_COLOR_BLACK);
        draw_header_titled("OTA App Download");
        /* Each line ≤ 19 chars so scale-2 text (16 px/char) fits 320 px. */
        const char *l1 = "WiFi Connect Failed";   /* 19 × 16 = 304 px */
        const char *l2 = "A: Reconfigure WiFi";   /* 19 × 16 = 304 px */
        display_draw_string((DISPLAY_W - (int)strlen(l1) * DISPLAY_FONT_W * 2) / 2,
                            95,  l1, DISPLAY_COLOR_RED,   DISPLAY_COLOR_BLACK, 2);
        display_draw_string((DISPLAY_W - (int)strlen(l2) * DISPLAY_FONT_W * 2) / 2,
                            135, l2, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
        button_t btn;
        do { btn = buttons_wait_event(0); } while (!(btn & BTN_A));
        /* Erase credentials so factory loader re-runs the portal on reboot.
         * Calling portal_mode_run() here would fail — the OTA manager left
         * esp_netif and the event loop initialised, and wifi_config_start()
         * uses ESP_ERROR_CHECK which aborts on ESP_ERR_INVALID_STATE. */
        nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);
        esp_restart();
        return; /* unreachable — silences compiler warning */
    }

    if (r != OTA_RESULT_OK) {
        const char *msg;
        switch (r) {
        case OTA_RESULT_NO_MANIFEST:   msg = "Manifest not found.";   break;
        case OTA_RESULT_EMPTY_CATALOG: msg = "No apps in catalog.";   break;
        default:                       msg = "Catalog fetch failed."; break;
        }
        display_fill(DISPLAY_COLOR_BLACK);
        draw_header_titled("OTA App Download");
        int mw = (int)strlen(msg) * DISPLAY_FONT_W * 2;
        display_draw_string((DISPLAY_W - mw) / 2, 110,
                            msg, DISPLAY_COLOR_RED, DISPLAY_COLOR_BLACK, 2);
        draw_footer("Press any button to return");
        wait_any_button();
        return;
    }

    /* ── Step 2: present text-based selection menu ─────────────────── */
    /* White fill drives all TN pixels to bright; hold for 600 ms so the
     * liquid crystals fully respond before dark backgrounds are drawn.
     * Without the delay the old menu ghost bleeds through. */
    display_fill(DISPLAY_COLOR_WHITE);
    vTaskDelay(pdMS_TO_TICKS(600));
    int sel = run_app_select_menu(&s_catalog);

    if (sel < 0) {
        /* User pressed B — cancel */
        ota_manager_wifi_disconnect();
        return;
    }

    /* ── Step 3: flash selected app ────────────────────────────────── */
    ESP_LOGI(TAG, "Selected: %s", s_catalog.apps[sel].name);

    r = ota_manager_flash_app(&s_catalog.apps[sel]);
    /* On success the device reboots — only error paths reach here. */
    ota_manager_wifi_disconnect();

    const char *err_msg;
    switch (r) {
    case OTA_RESULT_DOWNLOAD_FAIL: err_msg = "Download failed.";    break;
    case OTA_RESULT_VERIFY_FAIL:   err_msg = "Verify failed.";      break;
    case OTA_RESULT_FLASH_FAIL:    err_msg = "Flash write failed."; break;
    default:                       err_msg = "Update failed.";      break;
    }
    display_fill(DISPLAY_COLOR_BLACK);
    draw_header_titled("OTA App Download");
    int ew = (int)strlen(err_msg) * DISPLAY_FONT_W * 2;
    display_draw_string((DISPLAY_W - ew) / 2, 110,
                        err_msg, DISPLAY_COLOR_RED, DISPLAY_COLOR_BLACK, 2);
    draw_footer("Press any button to return");
    wait_any_button();
}

/* ── Action: Return to Last App ────────────────────────────────────── *
 * Finds the first OTA partition containing a valid image (magic 0xE9), *
 * sets it as the boot target, and restarts.  Shows an error screen if  *
 * no valid app is found (e.g. badge has never received an OTA update). */

static void action_return_to_app(void)
{
    const esp_partition_t *ota0 = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota1 = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);

    const esp_partition_t *target = NULL;
    uint8_t magic = 0;
    if (ota0 && esp_partition_read(ota0, 0, &magic, 1) == ESP_OK && magic == 0xE9)
        target = ota0;
    else if (ota1 && esp_partition_read(ota1, 0, &magic, 1) == ESP_OK && magic == 0xE9)
        target = ota1;

    if (target == NULL) {
        display_fill(DISPLAY_COLOR_BLACK);
        draw_header_titled("Return to Last App");
        const char *msg = "No app installed";
        int mw = (int)strlen(msg) * DISPLAY_FONT_W * 2;
        display_draw_string((DISPLAY_W - mw) / 2, 110,
                            msg, DISPLAY_COLOR_RED, DISPLAY_COLOR_BLACK, 2);
        draw_footer("Press any button to return");
        wait_any_button();
        return;
    }

    esp_ota_set_boot_partition(target);
    esp_restart();
}

/* ── Action: Load from SD Card (stub) ─────────────────────────────── */

static void action_sd_load(void)
{
    const char *lines[] = {
        "SD card app loading is",
        "coming in a future update.",
        "",
        "To install apps now, use",
        "option 1 (OTA Download).",
    };
    show_info_screen("Load from SD Card",
                     lines, sizeof(lines) / sizeof(lines[0]));
}

/* ── Action: Configure WiFi ────────────────────────────────────────── */

static void action_configure_wifi(void)
{
    portal_mode_run(0);
}

/* ── Action: Update SD recovery (stub) ────────────────────────────── */

static void action_sd_recovery(void)
{
    const char *lines[] = {
        "SD recovery image creation",
        "is coming in a future update.",
        "",
        "For now, flash the recovery",
        "image manually via USB using",
        "esptool.py merge_bin.",
    };
    show_info_screen("Update SD Recovery",
                     lines, sizeof(lines) / sizeof(lines[0]));
}

/* ── Action: Load MicroPython ──────────────────────────────────────── */

static void action_load_micropython(void)
{
    /* Warning screen — this overwrites the badge OS */
    display_fill(DISPLAY_COLOR_BLACK);
    draw_header_titled("Load MicroPython");

    const char *lines[] = {
        "This will REPLACE",
        "the badge OS",
        "with MicroPython.",
        "",
        "To restore, use:",
        "the webflash site.",
        "",
        "A: Continue",
        "B: Cancel",
    };
    for (int i = 0; i < 9; i++) {
        display_draw_string(8, 34 + i * 20, lines[i],
                            i < 2 ? DISPLAY_COLOR_RED : DISPLAY_COLOR_WHITE,
                            DISPLAY_COLOR_BLACK, 2);
    }

    button_t btn = buttons_wait_event(0);
    if (!(btn & BTN_A)) return;

    ota_result_t r = ota_manager_flash_micropython();
    /* Only reaches here on error — success reboots inside the function. */

    const char *msg;
    switch (r) {
    case OTA_RESULT_NO_WIFI:
        display_fill(DISPLAY_COLOR_BLACK);
        draw_header_titled("Load MicroPython");
        display_draw_string(
            (DISPLAY_W - 19 * DISPLAY_FONT_W * 2) / 2, 95,
            "WiFi Connect Failed",
            DISPLAY_COLOR_RED, DISPLAY_COLOR_BLACK, 2);
        display_draw_string(
            (DISPLAY_W - 19 * DISPLAY_FONT_W * 2) / 2, 135,
            "A: Reconfigure WiFi",
            DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
        {
            button_t b;
            do { b = buttons_wait_event(0); } while (!(b & BTN_A));
        }
        nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);
        esp_restart();
        return;
    case OTA_RESULT_NO_MANIFEST: msg = "Manifest not found.";  break;
    case OTA_RESULT_FLASH_FAIL:  msg = "Flash write failed.";  break;
    default:                     msg = "Install failed.";      break;
    }
    display_fill(DISPLAY_COLOR_BLACK);
    draw_header_titled("Load MicroPython");
    int mw = (int)strlen(msg) * DISPLAY_FONT_W * 2;
    display_draw_string((DISPLAY_W - mw) / 2, 110,
                        msg, DISPLAY_COLOR_RED, DISPLAY_COLOR_BLACK, 2);
    draw_footer("Press any button to return");
    wait_any_button();
}

/* ── Boot-hint info screen (shown once after first install / loader update) *
 *                                                                            *
 * Light yellow background, black text at scale 2.  The word "RESET" is      *
 * drawn twice (offset 1 px right) in dark red for a faux-bold effect.       *
 * ──────────────────────────────────────────────────────────────────────── */

#define HINT_BG     DISPLAY_RGB565(255, 255, 180)   /* light yellow */
#define HINT_FG     DISPLAY_COLOR_BLACK
#define HINT_RED    DISPLAY_RGB565(180, 0, 0)        /* dark red for RESET */

static void hint_draw_reset(int x, int y)
{
    display_draw_string(x,   y, "RESET", HINT_RED, HINT_BG, 2);
    display_draw_string(x+1, y, "RESET", HINT_RED, HINT_BG, 2); /* faux bold */
}

void loader_menu_show_boot_hint(void)
{
    char title[32];
    snprintf(title, sizeof(title), "BYU-I Loader v%d.%d", HW_VERSION, LOADER_VERSION);

    display_fill(DISPLAY_COLOR_BLACK);

    /* Blue header */
    display_fill_rect(0, 0, DISPLAY_W, 28, COLOR_BYUI_BLUE);
    int tw = (int)strlen(title) * DISPLAY_FONT_W * 2;
    int tx = (DISPLAY_W - tw) / 2;
    if (tx < 4) tx = 4;
    display_draw_string(tx, 6, title, DISPLAY_COLOR_WHITE, COLOR_BYUI_BLUE, 2);

    /* Light yellow content area */
    display_fill_rect(0, 28, DISPLAY_W, FOOTER_Y - 28, HINT_BG);

    /* "RESET Button:"  — RESET in faux-bold dark red */
    hint_draw_reset(8, 40);
    display_draw_string(8 + 5 * DISPLAY_FONT_W * 2, 40,
                        " Button:", HINT_FG, HINT_BG, 2);

    /* "  Restarts App"  (indented) */
    display_draw_string(24, 64, "Restarts App", HINT_FG, HINT_BG, 2);

    /* "Return to Loader:"  (section break above) */
    display_draw_string(8, 100, "Return to Loader:", HINT_FG, HINT_BG, 2);

    /* "Press/Release RESET"  — RESET in faux-bold dark red */
    display_draw_string(8, 124, "Press/Release ", HINT_FG, HINT_BG, 2);
    hint_draw_reset(8 + 14 * DISPLAY_FONT_W * 2, 124);

    /* "  Press/Hold BOOT"  (indented) */
    display_draw_string(24, 148, "Press/Hold BOOT", HINT_FG, HINT_BG, 2);

    draw_footer("Press any button to continue");
    wait_any_button();
}

/* ── Public entry point ────────────────────────────────────────────── */

void loader_menu_run(void)
{
    int selection = 0;
    draw_menu(selection, 0);
    ESP_LOGI(TAG, "loader_menu_run: entering menu loop");
    for (;;) {
        button_t btn = buttons_wait_event(0);
        ESP_LOGI(TAG, "loader_menu_run: got button event 0x%02X", btn);
        if (btn & (BTN_UP | BTN_LEFT)) {
            selection = (selection - 1 + NUM_ITEMS) % NUM_ITEMS;
            draw_menu(selection, 0);
            continue;
        }
        if (btn & (BTN_DOWN | BTN_B)) {
            selection = (selection + 1) % NUM_ITEMS;
            draw_menu(selection, 0);
            continue;
        }
        if (btn & (BTN_RIGHT | BTN_A)) {
            ESP_LOGI(TAG, "Selected item %d: %s",
                     selection + 1, item_label(selection));
            switch (selection) {
            case 0:  action_ota_download();        break;
            case 1:  action_return_to_app();       break;
            case 2:  action_sd_load();             break;
            case 3:  action_reset_wifi_config();   break;
            case 4:  action_reset_board_factory(); break;
            case 5:  action_load_micropython();    break;
            default: break;
            }
            display_fill(DISPLAY_COLOR_BLACK);
            draw_menu(selection, 0);
        }
    }
}
