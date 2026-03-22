
#include "loader_menu.h"

#include "display.h"
#include "buttons.h"
#include "portal_mode.h"
#include "ota_manager.h"
#include "wifi_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

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
#define COLOR_GREEN_OK    DISPLAY_RGB565(  0, 200,  80)
#define COLOR_WARN_BG     DISPLAY_RGB565( 80,   0,   0)

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
#define VISIBLE_APPS  4          /* max app rows visible at once        */

/* ── Main menu labels ──────────────────────────────────────────────── */
static const char *ITEM_LABELS[NUM_ITEMS] = {
    "OTA App Download",
    "SDCard Apps",
    "Reset Wifi/Config",
    "Full Factory Reset",
    "ESP-IDF Program",
    "Arduino Program",
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

static void draw_item(int idx, bool selected)
{
    int y = item_y(idx);

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

static void draw_menu(int selected)
{
    char title[32];
    snprintf(title, sizeof(title), "BYU-I Loader (v%d.%d)", HW_VERSION, LOADER_VERSION);
    draw_header_titled(title);
    /* Fill the 2px gap between header and first item (y=30-31). */
    display_fill_rect(0, HEADER_H, DISPLAY_W, ITEMS_START - HEADER_H, DISPLAY_COLOR_BLACK);
    for (int i = 0; i < NUM_ITEMS; i++) {
        draw_item(i, i == selected);
    }
    /* Fill any gap between last item and footer (appears when NUM_ITEMS < 5). */
    int gap_top = ITEMS_START + NUM_ITEMS * ITEM_H;
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

/* ── Icon tile app-selection menu ──────────────────────────────────── *
 *
 * Layout (320×240, icons 316×76):
 *   Tile 0  y =   0 .. 75   (76 px)
 *   Gap     y =  76          (1 px dark)
 *   Tile 1  y =  77 .. 152  (76 px)
 *   Gap     y = 153          (1 px dark)
 *   Tile 2  y = 154 .. 229  (76 px)
 *   Hint    y = 230 .. 239  (10 px)
 *
 * Icons are 308×72 px; 6 px L/R and 2 px T/B borders show selection colour.
 * ────────────────────────────────────────────────────────────────────── */

#define ICON_SLOT_H   76                               /* tile-slot height      */
#define ICON_TILE_H   OTA_ICON_H                       /* 72 — bitmap height    */
#define ICON_TILE_W   OTA_ICON_W                       /* 308 — bitmap width    */
#define ICON_X        ((DISPLAY_W - ICON_TILE_W) / 2)  /* 6 px L/R border      */
#define ICON_Y_OFF    ((ICON_SLOT_H - ICON_TILE_H) / 2) /* 2 px T/B border     */
#define ICON_GAP      1                                /* 1 px dark line between */
#define ICON_HINT_Y   230                              /* hint bar top          */
#define ICON_HINT_H   (DISPLAY_H - ICON_HINT_Y)       /* 10 px                 */
#define VISIBLE_ICONS 3

static int icon_tile_y(int row)
{
    return row * (ICON_SLOT_H + ICON_GAP);
}

/* Draw one icon tile.  icons[] may contain NULL (failed/no icon). */
static void draw_icon_tile(int row, const ota_app_entry_t *app,
                           const uint16_t *icon_pixels, bool selected)
{
    int ty = icon_tile_y(row);

    /* Background / selection frame colour fills the full 320-px width. */
    uint16_t frame = selected ? DISPLAY_COLOR_RED : DISPLAY_RGB565(20, 20, 20);
    display_fill_rect(0, ty, DISPLAY_W, ICON_SLOT_H, frame);

    if (icon_pixels) {
        /* Blit the icon; 6 px coloured borders remain L/R, 2 px T/B. */
        display_draw_bitmap(ICON_X, ty + ICON_Y_OFF, ICON_TILE_W, ICON_TILE_H, icon_pixels);
    } else {
        /* Fallback: name + version centred on a solid tile. */
        int name_w = (int)strlen(app->name) * DISPLAY_FONT_W * 2;
        int name_x = (DISPLAY_W - name_w) / 2;
        if (name_x < 4) name_x = 4;
        display_draw_string(name_x, ty + 22, app->name,
                            DISPLAY_COLOR_WHITE, frame, 2);

        char ver_buf[12];
        snprintf(ver_buf, sizeof(ver_buf), "v%" PRIu32, app->version);
        int ver_w = (int)strlen(ver_buf) * DISPLAY_FONT_W;
        display_draw_string((DISPLAY_W - ver_w) / 2, ty + 54,
                            ver_buf, COLOR_DIM_WHITE, frame, 1);
    }

    /* 1 px gap below (not for the last possible tile row) */
    if (row < VISIBLE_ICONS - 1) {
        display_fill_rect(0, ty + ICON_SLOT_H, DISPLAY_W, ICON_GAP,
                          DISPLAY_COLOR_BLACK);
    }
}

static void draw_icon_menu(const ota_catalog_t *catalog,
                           uint16_t * const icons[],
                           int selection, int scroll)
{
    for (int row = 0; row < VISIBLE_ICONS; row++) {
        int idx = scroll + row;
        if (idx >= catalog->count) {
            display_fill_rect(0, icon_tile_y(row), DISPLAY_W,
                              ICON_SLOT_H, DISPLAY_COLOR_WHITE);
        } else {
            draw_icon_tile(row, &catalog->apps[idx],
                           icons ? icons[idx] : NULL,
                           idx == selection);
        }
    }

    /* Hint bar */
    display_fill_rect(0, ICON_HINT_Y, DISPLAY_W, ICON_HINT_H, COLOR_FOOTER_BG);
    const char *hint = (catalog->count > 1)
                       ? "Up/Dn:move  Left:select  B:back"
                       : "Left:select  B:back";
    int hw = (int)strlen(hint) * DISPLAY_FONT_W;
    display_draw_string((DISPLAY_W - hw) / 2, ICON_HINT_Y + 1,
                        hint, DISPLAY_COLOR_WHITE, COLOR_FOOTER_BG, 1);
}

/**
 * Run the icon-tile app-selection menu.
 * @param icons  Array of OTA_CATALOG_MAX pointers (may be NULL entries).
 * @return selected index (0..count-1), or -1 if user pressed B (back).
 */
static int run_app_select_menu(const ota_catalog_t *catalog,
                               uint16_t * const icons[])
{
    int selection = 0;
    int scroll    = 0;

    draw_icon_menu(catalog, icons, selection, scroll);

    for (;;) {
        button_t btn = buttons_wait_event(0);

        if (btn & BTN_UP) {
            if (selection > 0) {
                selection--;
                if (selection < scroll) scroll = selection;
                draw_icon_menu(catalog, icons, selection, scroll);
            }

        } else if (btn & BTN_DOWN) {
            if (selection < catalog->count - 1) {
                selection++;
                if (selection >= scroll + VISIBLE_ICONS)
                    scroll = selection - VISIBLE_ICONS + 1;
                draw_icon_menu(catalog, icons, selection, scroll);
            }

        } else if (btn & (BTN_LEFT | BTN_A | BTN_RIGHT)) {
            return selection;

        } else if (btn & BTN_B) {
            return -1;
        }
    }
}

/* ── Action: OTA download ──────────────────────────────────────────── */

/* Static catalog + icon buffers — too large for the stack. */
static ota_catalog_t   s_catalog;
static uint16_t       *s_icons[OTA_CATALOG_MAX];

static void free_icons(int count)
{
    for (int i = 0; i < count; i++) {
        free(s_icons[i]);
        s_icons[i] = NULL;
    }
}

static void action_ota_download(void)
{
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

    /* ── Step 2: fetch icons while WiFi is up ──────────────────────── */
    for (int i = 0; i < s_catalog.count; i++) {
        s_icons[i] = NULL;
        if (s_catalog.apps[i].icon_url[0] == '\0') continue;

        /* Progress: "Loading icon 2/3..." */
        display_fill(DISPLAY_COLOR_BLACK);
        char prog[40];
        snprintf(prog, sizeof(prog), "Loading icon %d/%d...",
                 i + 1, s_catalog.count);
        int pw = (int)strlen(prog) * DISPLAY_FONT_W * 2;
        display_draw_string((DISPLAY_W - pw) / 2, 108,
                            prog, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);

        s_icons[i] = ota_manager_fetch_icon(s_catalog.apps[i].icon_url);
        ESP_LOGI(TAG, "icon[%d]: %s", i, s_icons[i] ? "loaded" : "NULL (will use text fallback)");
    }

    /* ── Step 3: present icon tile selection menu ──────────────────── */
    /* White fill drives all TN pixels to bright; hold for 600 ms so the
     * liquid crystals fully respond before dark tile backgrounds are drawn.
     * Without the delay the old menu ghost bleeds through. */
    display_fill(DISPLAY_COLOR_WHITE);
    vTaskDelay(pdMS_TO_TICKS(600));
    int sel = run_app_select_menu(&s_catalog, (uint16_t * const *)s_icons);

    if (sel < 0) {
        /* User pressed B — cancel */
        free_icons(s_catalog.count);
        ota_manager_wifi_disconnect();
        return;
    }

    /* ── Step 4: flash selected app ────────────────────────────────── */
    ESP_LOGI(TAG, "Selected: %s", s_catalog.apps[sel].name);
    free_icons(s_catalog.count);   /* free icons before the large flash download */

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

/* ── Action: USB Program ───────────────────────────────────────────── *
 * Shows flashing instructions with a clear warning to use the student
 * template project and idf.py app-flash (not idf.py flash).
 * Plain idf.py flash overwrites factory_switch + the partition table,
 * which bricks the badge.                                             */

static void action_usb_program(void)
{
    display_fill(DISPLAY_COLOR_BLACK);

    /* Header */
    display_fill_rect(0, 0, DISPLAY_W, 30, COLOR_HEADER_BG);
    const char *title = "ESP-IDF Program";
    int tw = (int)strlen(title) * DISPLAY_FONT_W * 2;
    display_draw_string((DISPLAY_W - tw) / 2, 7,
                        title, DISPLAY_COLOR_WHITE, COLOR_HEADER_BG, 2);

    /* Warning banner */
    display_fill_rect(0, 31, DISPLAY_W, 18, COLOR_WARN_BG);
    const char *warn = "! Use student template project !";
    int ww = (int)strlen(warn) * DISPLAY_FONT_W;
    display_draw_string((DISPLAY_W - ww) / 2, 35,
                        warn, COLOR_YELLOW, COLOR_WARN_BG, 1);

    /* NEVER / SAFE command lines */
    display_draw_string( 8, 60, "NEVER:", DISPLAY_COLOR_RED,   DISPLAY_COLOR_BLACK, 1);
    display_draw_string(57, 60, "idf.py flash",
                        DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);

    display_draw_string( 8, 78, "SAFE: ", COLOR_GREEN_OK,     DISPLAY_COLOR_BLACK, 1);
    display_draw_string(57, 78, "idf.py app-flash -p PORT",
                        DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);

    /* Short explanation */
    display_draw_string(8, 102,
                        "'idf.py flash' overwrites the",
                        COLOR_DIM_WHITE, DISPLAY_COLOR_BLACK, 1);
    display_draw_string(8, 118,
                        "loader -- this bricks the badge.",
                        COLOR_DIM_WHITE, DISPLAY_COLOR_BLACK, 1);

    /* Template URL */
    display_draw_string(8, 142,
                        "Student template project:",
                        COLOR_DIM_WHITE, DISPLAY_COLOR_BLACK, 1);
    display_draw_string(8, 158,
                        "github.com/watsonlr/",
                        DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    display_draw_string(8, 174,
                        "  namebadge-apps  (see README)",
                        DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);

    draw_footer("Press any button to return");
    wait_any_button();
}

/* ── Action: Arduino Program ───────────────────────────────────────── */

static void action_arduino_program(void)
{
    const char *lines[] = {
        "1. Install BYUI board package",
        "   in Arduino IDE (one-time):",
        "   github.com/watsonlr/",
        "   namebadge-apps (arduino/)",
        "",
        "2. Select: BYUI eBadge V4",
        "   in Tools > Board",
        "",
        "3. Write sketch, click Upload.",
    };
    show_info_screen("Arduino Program",
                     lines, sizeof(lines) / sizeof(lines[0]));
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

/* ── Public entry point ────────────────────────────────────────────── */

void loader_menu_run(void)
{
    int selection = 0;
    draw_menu(selection);
    ESP_LOGI(TAG, "loader_menu_run: entering menu loop");
    for (;;) {
        button_t btn = buttons_wait_event(0);
        ESP_LOGI(TAG, "loader_menu_run: got button event 0x%02X", btn);
        if (btn & (BTN_UP | BTN_LEFT)) {
            selection = (selection - 1 + NUM_ITEMS) % NUM_ITEMS;
            draw_menu(selection);
            continue;
        }
        if (btn & (BTN_DOWN | BTN_B)) {
            selection = (selection + 1) % NUM_ITEMS;
            draw_menu(selection);
            continue;
        }
        if (btn & (BTN_RIGHT | BTN_A)) {
            ESP_LOGI(TAG, "Selected item %d: %s",
                     selection + 1, item_label(selection));
            switch (selection) {
            case 0:  action_ota_download();        break;
            case 1:  action_sd_load();             break;
            case 2:  action_reset_wifi_config();   break;
            case 3:  action_reset_board_factory(); break;
            case 4:  action_usb_program();         break;
            case 5:  action_arduino_program();     break;
            default: break;
            }
            display_fill(DISPLAY_COLOR_BLACK);
            draw_menu(selection);
        }
    }
}
