/**
 * @file portal_mode.c
 * @brief Wi-Fi config portal orchestrator.
 *
 * Combines wifi_config (AP + HTTP) with the display to show a QR code and
 * instructions.  Blocks until credentials are submitted or timeout expires.
 */

#include "portal_mode.h"
#include "wifi_config.h"
#include "display.h"
#include "buttons.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>

#define TAG "portal_mode"

#define BTN_A_GPIO  38   /* Left  / Back    */
#define BTN_B_GPIO  18   /* Right / Forward */

/* ── WiFi test helpers (used by Step 4 welcome screen) ──────────── */
#define WIFI_TEST_CONNECTED_BIT  BIT0
#define WIFI_TEST_FAIL_BIT       BIT1
#define WIFI_TEST_MAX_RETRIES    3
#define WIFI_TEST_TIMEOUT_MS     10000

static EventGroupHandle_t  s_test_eg        = NULL;
static esp_netif_t        *s_test_netif     = NULL;
static int                 s_test_retries   = 0;
static uint8_t             s_disconnect_reason = 0;

/* Map ESP-IDF WiFi disconnect reason codes to a short human-readable string. */
static const char *wifi_reason_str(uint8_t reason)
{
    switch (reason) {
    case 201: return "SSID not found";          /* WIFI_REASON_NO_AP_FOUND       */
    case 2:   /* AUTH_EXPIRE */
    case 202: return "Wrong password";          /* WIFI_REASON_AUTH_FAIL         */
    case 15:  return "Wrong password";          /* WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT */
    case 203: return "Association failed";      /* WIFI_REASON_ASSOC_FAIL        */
    case 200: return "Beacon timeout";          /* WIFI_REASON_BEACON_TIMEOUT    */
    case 67:  return "Connection failed";       /* WIFI_REASON_CONNECTION_FAIL   */
    case 204: return "Handshake timeout";       /* WIFI_REASON_HANDSHAKE_TIMEOUT */
    default:  return NULL;                      /* show raw code instead         */
    }
}

static void _test_wifi_event_handler(void *arg, esp_event_base_t base,
                                     int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        s_disconnect_reason = ev->reason;
        ESP_LOGW(TAG, "WiFi disconnect reason: %u", s_disconnect_reason);
        if (s_test_retries < WIFI_TEST_MAX_RETRIES) {
            s_test_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_test_eg, WIFI_TEST_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_test_eg, WIFI_TEST_CONNECTED_BIT);
    }
}

/* Try connecting to the saved network; fill ip_out on success. */
static bool _portal_wifi_test(const char *ssid, const char *pass,
                               char *ip_out, size_t ip_out_len)
{
    s_test_eg = xEventGroupCreate();
    s_test_retries = 0;
    s_disconnect_reason = 0;

    s_test_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t inst_wifi, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, _test_wifi_event_handler, NULL, &inst_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, _test_wifi_event_handler, NULL, &inst_ip));

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_test_eg,
                                           WIFI_TEST_CONNECTED_BIT | WIFI_TEST_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_TEST_TIMEOUT_MS));

    bool connected = (bits & WIFI_TEST_CONNECTED_BIT) != 0;
    if (connected && ip_out) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(s_test_netif, &ip_info);
        snprintf(ip_out, ip_out_len, IPSTR, IP2STR(&ip_info.ip));
    }

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_wifi);
    vEventGroupDelete(s_test_eg);
    s_test_eg = NULL;

    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_test_netif) {
        esp_netif_destroy(s_test_netif);
        s_test_netif = NULL;
    }
    return connected;
}

/* ── Layout constants (320×240 landscape) ───────────────────────── */

/* QR code: "WIFI:T:nopass;S:BYUI_NameBadge_XX;;" — built at runtime.
 * At 4 px/module + 4-module quiet zone → footprint 164 px square.
 *
 * All text scale 2 (16px tall, 16px wide per char, 20 chars max per line).
 *
 * Phase 1 layout (WiFi join QR):
 *   y=2      "Welcome to your"   scale 2 (16px) BLUE, centred
 *   y=20     "NameBadge!"        scale 2 (16px) BLUE, centred
 *   QR centre (160, 120) → footprint y=38..202
 *   y=206    "Scan to Join Your" scale 2 (16px) centred
 *   y=224    "Board's WiFi"      scale 2 (16px) centred
 *
 * Phase 2 layout (URL QR, after phone joined):
 *   y=20     "Device Connected!" scale 2 (16px) GREEN, centred
 *   QR same centre, footer text changes.                              */
#define QR_MODULE_PX   4
#define QR_CX          (DISPLAY_W / 2)     /* 160 */
#define QR_CY          120                 /* 38 top margin + 82 half-size */

#define HDR_Y1         2
#define HDR_Y2         20
#define TEXT_Y1        188
#define TEXT_Y2        206
#define HDR_SCALE      2    /* scale 2 = 16px — matches footer size */
#define TEXT_SCALE     2    /* scale 2 = 16px */
/* Coloured bar: covers top of screen down to 4px below last header line */
#define HDR_BAR_H      (HDR_Y2 + HDR_SCALE * DISPLAY_FONT_H + 4)  /* = 40 */


/* Centre a string horizontally for a given font scale. */
static int centre_x(const char *s, int scale)
{
    int px = (int)strlen(s) * DISPLAY_FONT_W * scale;
    int x  = (DISPLAY_W - px) / 2;
    return x < 0 ? 0 : x;
}

/* ── Helper: fill screen and draw QR + header + footer lines ─────── */
static void draw_portal_screen(const char *qr_payload,
                                uint16_t hdr_color,
                                const char *hdr1, const char *hdr2,
                                const char *foot1, const char *foot2)
{
    display_fill(DISPLAY_COLOR_WHITE);

    /* Coloured bar behind the header lines */
    display_fill_rect(0, 0, DISPLAY_W, HDR_BAR_H, hdr_color);

    display_draw_qr(QR_CX, QR_CY, qr_payload,
                    QR_MODULE_PX,
                    DISPLAY_COLOR_BLACK,
                    DISPLAY_COLOR_WHITE);

    /* Header lines — white text on the coloured bar */
    display_text_ctx_t hctx = DISPLAY_CTX(DISPLAY_FONT_SANS, HDR_SCALE,
                                           DISPLAY_COLOR_WHITE,
                                           hdr_color);
    if (hdr1) display_print(&hctx, centre_x(hdr1, HDR_SCALE),  HDR_Y1, hdr1);
    if (hdr2) display_print(&hctx, centre_x(hdr2, HDR_SCALE),  HDR_Y2, hdr2);

    /* Footer lines — black, scale 2, horizontally centred */
    display_text_ctx_t fctx = DISPLAY_CTX(DISPLAY_FONT_SANS, TEXT_SCALE,
                                           DISPLAY_COLOR_BLACK,
                                           DISPLAY_COLOR_WHITE);
    if (foot1) display_print(&fctx, centre_x(foot1, TEXT_SCALE), TEXT_Y1, foot1);
    if (foot2) display_print(&fctx, centre_x(foot2, TEXT_SCALE), TEXT_Y2, foot2);
}

bool portal_mode_run(int timeout_s)
{
    /* ── Start portal ── */
    if (!wifi_config_start()) {
        ESP_LOGE(TAG, "Failed to start config portal");
        return false;
    }
    const char *url  = wifi_config_url();
    const char *ssid = wifi_config_ssid();
    ESP_LOGI(TAG, "Portal active — SSID=\"%s\"  URL=%s", ssid, url);

    /* Build WiFi QR payload with the unique SSID at runtime. */
    char wifi_qr[48];
    snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:nopass;S:%s;;", ssid);

    /* ── Phase 1: invite user to scan and join WiFi ── */
    draw_portal_screen(wifi_qr, DISPLAY_COLOR_BLUE,
                       "Welcome to your",
                       "ECEN NameBadge!",
                       "Scan to Join Your",
                       "Board's WiFi");

    /* ── Poll until done or timeout ── */
    int elapsed = 0;
    const int poll_ms = 500;
    bool phase2_shown = false;
    bool phase3_shown = false;

    while (!wifi_config_done()) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        elapsed += poll_ms;

        /* ── Phase 2: once phone has joined, swap QR to URL ── */
        if (!phase2_shown && wifi_config_sta_joined()) {
            phase2_shown = true;

            draw_portal_screen(url, DISPLAY_COLOR_GREEN,
                               "Device Connected!",
                               NULL,
                               "Scan to open browser",
                               "If not open yet.");



        /* ── Phase 3: browser has loaded the form — drop QR, show text ──
         * Requires Phase 2 has been visible for PHASE2_MIN_HOLD_MS so
         * the user has time to scan the URL QR if the OS probe beat
         * them to it.                                                  */
        } else if (!phase3_shown && phase2_shown && wifi_config_form_served()) {
            phase3_shown = true;

            /* White background, blue text — matches the web form style.
             * No QR needed — the browser is already open on their phone. */
            display_fill(DISPLAY_COLOR_WHITE);

            display_text_ctx_t ctx = DISPLAY_CTX(DISPLAY_FONT_SANS, 2,
                                                   DISPLAY_COLOR_BLUE,
                                                   DISPLAY_COLOR_WHITE);
            display_print(&ctx, centre_x("Fill out the form", 2),  88,
                          "Fill out the form");
            display_print(&ctx, centre_x("on your device", 2),    112,
                          "on your device");
            display_print(&ctx, centre_x("and save", 2),          136,
                          "and save");
        }

        if (timeout_s > 0 && elapsed >= timeout_s * 1000) {
            ESP_LOGW(TAG, "Portal timed out after %d s", timeout_s);
            wifi_config_stop();
            return false;
        }
    }

    ESP_LOGI(TAG, "Credentials saved — stopping portal");
    wifi_config_stop();

    /* ── Step 4: Setup Complete screen ─────────────────────────────
     * Read the saved nickname and credentials, show the screen, then
     * do a quick WiFi connection test to tell the user whether their
     * credentials work before OTA takes over.                        */
    char nick[33]      = {0};
    char saved_ssid[33] = {0};
    char saved_pass[65] = {0};
    wifi_config_get_nick(nick, sizeof(nick));

    nvs_handle_t nh;
    if (nvs_open_from_partition(WIFI_CONFIG_NVS_PARTITION, WIFI_CONFIG_NVS_NAMESPACE,
                                NVS_READONLY, &nh) == ESP_OK) {
        size_t n;
        n = sizeof(saved_ssid); nvs_get_str(nh, WIFI_CONFIG_NVS_KEY_SSID, saved_ssid, &n);
        n = sizeof(saved_pass); nvs_get_str(nh, WIFI_CONFIG_NVS_KEY_PASS, saved_pass, &n);
        nvs_close(nh);
    }

    /* Helper: draw the static portions of the Setup Complete screen */
    int name_scale = ((int)strlen(nick) * DISPLAY_FONT_W * 3 <= DISPLAY_W) ? 3 : 2;

#define _DRAW_SETUP_BG() do { \
        display_fill(DISPLAY_COLOR_WHITE); \
        display_text_ctx_t _hdr = DISPLAY_CTX(DISPLAY_FONT_SANS, 2, \
                                               DISPLAY_COLOR_BLACK, \
                                               DISPLAY_COLOR_WHITE); \
        display_print(&_hdr, centre_x("Setup Complete", 2), 20, "Setup Complete"); \
        display_text_ctx_t _nm = DISPLAY_CTX(DISPLAY_FONT_SANS, name_scale, \
                                              DISPLAY_COLOR_BLACK, \
                                              DISPLAY_COLOR_WHITE); \
        display_print(&_nm, centre_x(nick, name_scale), 60, nick); \
    } while (0)

    /* Configure Back/Forward buttons (low-active with internal pull-up). */
    {
        gpio_config_t bc = {
            .pin_bit_mask = (1ULL << BTN_A_GPIO) | (1ULL << BTN_B_GPIO),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&bc);
    }

    /* Test WiFi; loop so the user can retry with Back or skip with Forward. */
    char ip_str[20] = {0};
    do {
        /* Show "Connecting to WiFi..." while the test runs */
        ip_str[0] = '\0';
        _DRAW_SETUP_BG();
        {
            display_text_ctx_t wc = DISPLAY_CTX(DISPLAY_FONT_SANS, 1,
                                                 DISPLAY_COLOR_BLACK,
                                                 DISPLAY_COLOR_WHITE);
            display_print(&wc, centre_x("Connecting to WiFi...", 1), 130,
                          "Connecting to WiFi...");
        }

        bool connected = (saved_ssid[0] != '\0') &&
                         _portal_wifi_test(saved_ssid, saved_pass,
                                           ip_str, sizeof(ip_str));
        _DRAW_SETUP_BG();

        if (connected) {
            display_text_ctx_t ok = DISPLAY_CTX(DISPLAY_FONT_SANS, 2,
                                                 DISPLAY_COLOR_GREEN,
                                                 DISPLAY_COLOR_WHITE);
            display_print(&ok, centre_x("WiFi Connected", 2), 100,
                          "WiFi Connected");
            display_text_ctx_t ip_ctx = DISPLAY_CTX(DISPLAY_FONT_SANS, 1,
                                                     DISPLAY_COLOR_GREEN,
                                                     DISPLAY_COLOR_WHITE);
            display_print(&ip_ctx, centre_x(ip_str, 1), 122, ip_str);

            display_text_ctx_t a_ctx = DISPLAY_CTX(DISPLAY_FONT_SANS, 2,
                                                    DISPLAY_COLOR_BLACK,
                                                    DISPLAY_COLOR_WHITE);
            display_print(&a_ctx, centre_x("A: Continue", 2), 152,
                          "A: Continue");
            display_text_ctx_t b_ctx = DISPLAY_CTX(DISPLAY_FONT_SANS, 2,
                                                    DISPLAY_COLOR_BLACK,
                                                    DISPLAY_COLOR_WHITE);
            display_print(&b_ctx, centre_x("B: Start Over", 2), 178,
                          "B: Start Over");

            ESP_LOGI(TAG, "WiFi test: connected (%s)", ip_str);

            button_t choice;
            int portal_btn_iter = 0;
            do {
                ESP_LOGI(TAG, "portal_mode: waiting for A/B, iter %d", portal_btn_iter++);
                choice = buttons_wait_event(0);
                ESP_LOGI(TAG, "portal_mode: got button event 0x%02X", choice);
            } while (!(choice & (BTN_A | BTN_B)));

            if (choice & BTN_B) {
                ESP_LOGI(TAG, "B pressed — erasing config and restarting");
                nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);
                esp_restart();
            }
            break;  /* A pressed — continue to loader menu */
        }

        /* ── Failure screen ─────────────────────────────────────── */
        const char *reason = wifi_reason_str(s_disconnect_reason);
        char reason_buf[24];
        if (!reason) {
            snprintf(reason_buf, sizeof(reason_buf), "Error code: %u",
                     s_disconnect_reason);
            reason = reason_buf;
        }
        ESP_LOGW(TAG, "WiFi test failed: %s", reason);

        /* "WiFi NOT Connected" — scale 2 (large, red) */
        display_text_ctx_t err = DISPLAY_CTX(DISPLAY_FONT_SANS, 2,
                                              DISPLAY_COLOR_RED,
                                              DISPLAY_COLOR_WHITE);
        display_print(&err, centre_x("WiFi NOT Connected", 2), 100,
                      "WiFi NOT Connected");

        /* Reason — scale 1, red, below heading */
        display_text_ctx_t rsn = DISPLAY_CTX(DISPLAY_FONT_SANS, 1,
                                              DISPLAY_COLOR_RED,
                                              DISPLAY_COLOR_WHITE);
        display_print(&rsn, centre_x(reason, 1), 122, reason);

        /* Button hints */
        display_text_ctx_t hint = DISPLAY_CTX(DISPLAY_FONT_SANS, 1,
                                               DISPLAY_COLOR_BLACK,
                                               DISPLAY_COLOR_WHITE);
        display_print(&hint, 4, 165, "<- Back button to re-try");
        display_print(&hint, 4, 179, "-> FWD button to continue");

        /* Wait for Back (A) or Forward (B) */
        while (gpio_get_level(BTN_A_GPIO) != 0 && gpio_get_level(BTN_B_GPIO) != 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        bool retry = (gpio_get_level(BTN_A_GPIO) == 0);
        /* Wait for release (debounce) */
        while (gpio_get_level(BTN_A_GPIO) == 0 || gpio_get_level(BTN_B_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!retry) {
            ESP_LOGI(TAG, "Forward pressed — continuing without WiFi");
            break;   /* user chose to skip */
        }
        ESP_LOGI(TAG, "Back pressed — retrying WiFi");
    } while (true);

#undef _DRAW_SETUP_BG

    return true;
}
