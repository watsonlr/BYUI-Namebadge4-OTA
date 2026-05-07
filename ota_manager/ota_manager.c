/**
 * @file ota_manager.c
 * @brief Multi-app OTA catalog manager.
 *
 * Flow:
 *   1. ota_manager_fetch_catalog()
 *      a. Read WiFi credentials + manifest URL from NVS.
 *      b. Connect to the saved AP (STA mode).
 *      c. Fetch the JSON manifest (bare array of app objects).
 *      d. Parse up to OTA_CATALOG_MAX entries.
 *      e. Return OTA_RESULT_OK with catalog populated; WiFi stays up.
 *
 *   2. Caller presents the catalog to the user (loader_menu).
 *
 *   3. ota_manager_flash_app(selected_entry)
 *      a. Stream firmware binary in 8 KB chunks to inactive OTA partition.
 *      b. Verify SHA-256 incrementally.
 *      c. Mark partition bootable, reboot — never returns on success.
 *
 *   4. ota_manager_wifi_disconnect()  — call when done (or on cancel).
 */

#include "ota_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"

#include "display.h"

#define TAG "ota_manager"

/* ── NVS ──────────────────────────────────────────────────────────── */
#define NVS_PARTITION  "user_data"
#define NVS_NAMESPACE  "badge_cfg"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "pass"
#define NVS_KEY_MFST   "mfst"

/* ── WiFi STA ─────────────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRIES    5
#define WIFI_TIMEOUT_MS     30000

static EventGroupHandle_t s_wifi_eg   = NULL;
static esp_netif_t       *s_sta_netif = NULL;
static int                s_retries   = 0;

/* ── Display helpers ──────────────────────────────────────────────── */
static void show_status(const char *line1, const char *line2)
{
    display_fill(DISPLAY_COLOR_BLACK);
    if (line1) {
        int w = (int)strlen(line1) * DISPLAY_FONT_W * 2;
        int x = (DISPLAY_W - w) / 2;
        if (x < 0) x = 0;
        display_draw_string(x, line2 ? 96 : 104,
                            line1, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    }
    if (line2) {
        int w = (int)strlen(line2) * DISPLAY_FONT_W * 2;
        int x = (DISPLAY_W - w) / 2;
        if (x < 0) x = 0;
        display_draw_string(x, 120,
                            line2, DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_BLACK, 2);
    }
}

/* ── Animated indeterminate bar (used while fetching catalog) ─────── */
#define FETCH_BAR_X      20
#define FETCH_BAR_Y      148
#define FETCH_BAR_W      280
#define FETCH_BAR_H      22
#define FETCH_BLOCK_W    70

static volatile bool  s_fetch_anim_run = false;
static TaskHandle_t   s_fetch_anim_task = NULL;

static void fetch_anim_task(void *arg)
{
    int pos = 0, dir = 1;
    while (s_fetch_anim_run) {
        display_fill_rect(FETCH_BAR_X, FETCH_BAR_Y,
                          FETCH_BAR_W, FETCH_BAR_H, DISPLAY_RGB565(40, 40, 40));
        display_fill_rect(FETCH_BAR_X + pos, FETCH_BAR_Y,
                          FETCH_BLOCK_W, FETCH_BAR_H, DISPLAY_COLOR_BLUE);
        pos += dir * 10;
        if (pos + FETCH_BLOCK_W >= FETCH_BAR_W) { pos = FETCH_BAR_W - FETCH_BLOCK_W; dir = -1; }
        if (pos <= 0)                            { pos = 0;                            dir =  1; }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    vTaskDelete(NULL);
}

static void show_fetching(const char *msg)
{
    display_fill(DISPLAY_COLOR_BLACK);
    int tw = (int)strlen(msg) * DISPLAY_FONT_W * 2;
    display_draw_string((DISPLAY_W - tw) / 2, 96,
                        msg, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    /* Bar outline */
    display_fill_rect(FETCH_BAR_X - 2, FETCH_BAR_Y - 2, FETCH_BAR_W + 4, 2,  DISPLAY_COLOR_WHITE);
    display_fill_rect(FETCH_BAR_X - 2, FETCH_BAR_Y + FETCH_BAR_H, FETCH_BAR_W + 4, 2, DISPLAY_COLOR_WHITE);
    display_fill_rect(FETCH_BAR_X - 2, FETCH_BAR_Y - 2, 2, FETCH_BAR_H + 4,  DISPLAY_COLOR_WHITE);
    display_fill_rect(FETCH_BAR_X + FETCH_BAR_W, FETCH_BAR_Y - 2, 2, FETCH_BAR_H + 4, DISPLAY_COLOR_WHITE);
    display_fill_rect(FETCH_BAR_X, FETCH_BAR_Y, FETCH_BAR_W, FETCH_BAR_H, DISPLAY_RGB565(40, 40, 40));
    s_fetch_anim_run = true;
    xTaskCreate(fetch_anim_task, "fetch_anim", 2048, NULL, 5, &s_fetch_anim_task);
}

static void stop_fetching(void)
{
    s_fetch_anim_run = false;
    vTaskDelay(pdMS_TO_TICKS(100));
    s_fetch_anim_task = NULL;
}

static void show_wifi_status(const char *label, const char *ssid)
{
    display_fill(DISPLAY_COLOR_BLACK);

    /* Label at scale 2, centred (clamped to x=0 if too wide) */
    int lw = (int)strlen(label) * DISPLAY_FONT_W * 2;
    int lx = (DISPLAY_W - lw) / 2;
    if (lx < 0) lx = 0;
    display_draw_string(lx, 72, label,
                        DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);

    /* SSID at scale 2, centred (clamped) */
    if (ssid && ssid[0]) {
        int nw = (int)strlen(ssid) * DISPLAY_FONT_W * 2;
        int nx = (DISPLAY_W - nw) / 2;
        if (nx < 0) nx = 0;
        display_draw_string(nx, 104, ssid,
                            DISPLAY_COLOR_CYAN, DISPLAY_COLOR_BLACK, 2);
    }
}

#define PROG_BAR_X  20
#define PROG_BAR_Y  130
#define PROG_BAR_W  280
#define PROG_BAR_H  24

static void show_download_progress(const char *name, uint32_t version, int pct,
                                   int kb_done, int kb_total)
{
    /* On the first call (pct == 0) draw the static background elements:
     * title, app name, version, bar border, and initial black fill.
     * On subsequent calls only the bar fill and percentage text are
     * redrawn — no full-screen clear, so there is no flash. */
    static bool s_bg_drawn = false;
    if (pct == 0) s_bg_drawn = false;   /* reset for each new download */

    if (!s_bg_drawn) {
        s_bg_drawn = true;
        display_fill(DISPLAY_COLOR_BLACK);

        /* "Downloading {name}" at scale 2, centred */
        char title[OTA_APP_NAME_MAX + 16];
        snprintf(title, sizeof(title), "Downloading %s", name ? name : "");
        int tw = (int)strlen(title) * DISPLAY_FONT_W * 2;
        display_draw_string((DISPLAY_W - tw) / 2, 72,
                            title, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);

        /* "Version xx" on the next line */
        char ver_buf[24];
        snprintf(ver_buf, sizeof(ver_buf), "Version %" PRIu32, version);
        int vw = (int)strlen(ver_buf) * DISPLAY_FONT_W * 2;
        display_draw_string((DISPLAY_W - vw) / 2, 98,
                            ver_buf, DISPLAY_COLOR_CYAN, DISPLAY_COLOR_BLACK, 2);

        /* White border (4 strips) — drawn once, never touched again */
        display_fill_rect(PROG_BAR_X - 2, PROG_BAR_Y - 2, PROG_BAR_W + 4, 2,
                          DISPLAY_COLOR_WHITE);
        display_fill_rect(PROG_BAR_X - 2, PROG_BAR_Y + PROG_BAR_H, PROG_BAR_W + 4, 2,
                          DISPLAY_COLOR_WHITE);
        display_fill_rect(PROG_BAR_X - 2, PROG_BAR_Y - 2, 2, PROG_BAR_H + 4,
                          DISPLAY_COLOR_WHITE);
        display_fill_rect(PROG_BAR_X + PROG_BAR_W, PROG_BAR_Y - 2, 2, PROG_BAR_H + 4,
                          DISPLAY_COLOR_WHITE);
    }

    /* Update only the progress bar fill — no border, no title, no flash. */
    int filled = (pct * PROG_BAR_W) / 100;
    display_fill_rect(PROG_BAR_X, PROG_BAR_Y,
                      filled, PROG_BAR_H, DISPLAY_COLOR_GREEN);
    display_fill_rect(PROG_BAR_X + filled, PROG_BAR_Y,
                      PROG_BAR_W - filled, PROG_BAR_H,
                      DISPLAY_RGB565(40, 40, 40));

    /* Overwrite the percentage line in-place (black bg clears old text). */
    char pct_buf[40];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%  (%d / %d KB)", pct, kb_done, kb_total);
    int pw = (int)strlen(pct_buf) * DISPLAY_FONT_W;
    /* Clear the text row before writing (handles shorter strings). */
    display_fill_rect(0, 164, DISPLAY_W, DISPLAY_FONT_H, DISPLAY_COLOR_BLACK);
    display_draw_string((DISPLAY_W - pw) / 2, 164,
                        pct_buf, DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_BLACK, 1);
}

/* ── WiFi event handler ───────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < WIFI_MAX_RETRIES) {
            s_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* ── WiFi STA connect / disconnect ───────────────────────────────── */
static bool wifi_sta_connect(const char *ssid, const char *pass)
{
    s_wifi_eg = xEventGroupCreate();
    s_retries = 0;

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t inst_wifi, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &inst_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &inst_ip));

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_wifi);
    vEventGroupDelete(s_wifi_eg);
    s_wifi_eg = NULL;

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static void wifi_sta_disconnect(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
}

/* ── HTTP: fetch small text resource (manifest JSON) ─────────────── */
static esp_err_t http_get_text(const char *url, char *out, size_t out_max,
                                int *out_len)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);

    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int total = 0;
    while (total < (int)out_max - 1) {
        int n = esp_http_client_read(client, out + total,
                                     (int)out_max - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    out[total] = '\0';
    *out_len = total;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total > 0 ? ESP_OK : ESP_FAIL;
}

/* ── HTTP: fetch binary resource (icon data) ─────────────────────── */
static esp_err_t http_get_binary(const char *url, uint8_t *out, size_t out_max,
                                  int *out_len)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int total = 0;
    while (total < (int)out_max) {
        int n = esp_http_client_read(client, (char *)out + total,
                                     (int)out_max - total);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        total += n;
    }
    *out_len = total;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total > 0 ? ESP_OK : ESP_FAIL;
}

/* ── Stream firmware from HTTP directly into OTA flash partition ──── *
 *
 * Downloads in 8 KB chunks, hashes incrementally with SHA-256, and
 * writes each chunk to the inactive OTA partition as it arrives.    */
#define OTA_CHUNK_SIZE 8192
static uint8_t s_ota_chunk[OTA_CHUNK_SIZE];  /* static → BSS, not stack */

static esp_err_t http_stream_and_flash(const char *url, int expected_size,
                                        const char *sha256_expected,
                                        const char *name)
{
    /* Open OTA partition for writing */
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "No OTA partition available");
        return ESP_FAIL;
    }
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 120000,
        .buffer_size       = OTA_CHUNK_SIZE,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { esp_ota_abort(ota_handle); return ESP_FAIL; }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return err;
    }
    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    /* SHA-256 hashed incrementally as chunks arrive */
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    int total = 0, last_pct = -1;
    err = ESP_OK;

    while (total < expected_size) {
        int want = expected_size - total;
        if (want > OTA_CHUNK_SIZE) want = OTA_CHUNK_SIZE;
        int n = esp_http_client_read(client, (char *)s_ota_chunk, want);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0)  break;

        mbedtls_sha256_update(&sha_ctx, s_ota_chunk, n);

        esp_err_t we = esp_ota_write(ota_handle, s_ota_chunk, n);
        if (we != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(we));
            err = we;
            break;
        }
        total += n;

        int pct = (int)(100LL * total / expected_size);
        if (pct / 2 != last_pct / 2) {
            last_pct = pct;
            show_download_progress(name, 0, pct,
                                   total / 1024, expected_size / 1024);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || total != expected_size) {
        ESP_LOGE(TAG, "Download incomplete: %d of %d bytes", total, expected_size);
        mbedtls_sha256_free(&sha_ctx);
        esp_ota_abort(ota_handle);
        show_status("Download failed", NULL);
        return ESP_FAIL;
    }

    /* Verify SHA-256 */
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);
    char computed[65] = {0};
    for (int i = 0; i < 32; i++) snprintf(computed + 2*i, 3, "%02x", hash[i]);
    if (strcmp(computed, sha256_expected) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch\n  expected: %s\n  computed: %s",
                 sha256_expected, computed);
        esp_ota_abort(ota_handle);
        show_status("SHA-256 mismatch!", "Update aborted");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SHA-256 verified OK");

    /* Finalise and mark partition bootable */
    show_status("Installing update...", "Do not power off");
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        show_status("Flash write failed", NULL);
        return err;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
    }
    return err;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════ */

void ota_manager_wifi_disconnect(void)
{
    wifi_sta_disconnect();
}

/* ── Fetch icon ───────────────────────────────────────────────────── */

uint16_t *ota_manager_fetch_icon(const char *url)
{
    if (!url || url[0] == '\0') return NULL;

    size_t nbytes = OTA_ICON_BYTES;

    /* Prefer PSRAM; fall back to internal heap if PSRAM is unavailable. */
    uint16_t *buf = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(nbytes);
    }
    if (!buf) {
        ESP_LOGE(TAG, "Icon alloc failed (%zu B)", nbytes);
        return NULL;
    }

    int received = 0;
    esp_err_t err = http_get_binary(url, (uint8_t *)buf, nbytes, &received);
    if (err != ESP_OK || received != (int)nbytes) {
        ESP_LOGE(TAG, "Icon fetch: got %d of %d bytes from %s",
                 received, (int)nbytes, url);
        free(buf);
        return NULL;
    }

    ESP_LOGI(TAG, "Icon fetched: %d B", received);
    return buf;
}

/* ── Fetch catalog ────────────────────────────────────────────────── */

ota_result_t ota_manager_fetch_catalog(ota_catalog_t *out)
{
    out->count = 0;

    /* Init netif / event loop — safe to call even if already done. */
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(e));
        return OTA_RESULT_NO_WIFI;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(e));
        return OTA_RESULT_NO_WIFI;
    }

    /* Mount user_data NVS partition (idempotent). */
    esp_err_t ue = nvs_flash_init_partition(NVS_PARTITION);
    if (ue == ESP_ERR_NVS_NO_FREE_PAGES || ue == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(NVS_PARTITION);
        nvs_flash_init_partition(NVS_PARTITION);
    }

    char ssid[33]      = {0};
    char pass[65]      = {0};
    char mfst_url[129] = {0};

    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGE(TAG, "user_data NVS open failed");
        return OTA_RESULT_NO_WIFI;
    }
    size_t n;
    n = sizeof(ssid);     nvs_get_str(h, NVS_KEY_SSID, ssid,     &n);
    n = sizeof(pass);     nvs_get_str(h, NVS_KEY_PASS, pass,     &n);
    n = sizeof(mfst_url); nvs_get_str(h, NVS_KEY_MFST, mfst_url, &n);
    nvs_close(h);

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "No WiFi SSID configured");
        return OTA_RESULT_NO_WIFI;
    }
    if (mfst_url[0] == '\0') {
        strlcpy(mfst_url, "https://byu-i-ebadge.github.io/apps/manifest.json",
                sizeof(mfst_url));
    }

    /* Connect to WiFi */
    show_wifi_status("Connecting to WiFi...", ssid);
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    if (!wifi_sta_connect(ssid, pass)) {
        ESP_LOGE(TAG, "WiFi connect failed");
        show_wifi_status("WiFi connect failed", ssid);
        wifi_sta_disconnect();
        return OTA_RESULT_NO_WIFI;
    }
    ESP_LOGI(TAG, "WiFi connected");

    /* Fetch manifest JSON — show animated bar while waiting */
    show_fetching("Fetching App Catalog");

    static char manifest_json[4096];
    int manifest_len = 0;
    if (http_get_text(mfst_url, manifest_json, sizeof(manifest_json),
                      &manifest_len) != ESP_OK) {
        stop_fetching();
        ESP_LOGE(TAG, "Manifest fetch failed: %s", mfst_url);
        show_status("Manifest fetch failed", NULL);
        wifi_sta_disconnect();
        return OTA_RESULT_NO_MANIFEST;
    }
    stop_fetching();
    ESP_LOGI(TAG, "Manifest (%d B) fetched", manifest_len);

    /* Parse manifest — expected format: bare JSON array */
    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) {
        ESP_LOGE(TAG, "Manifest JSON parse error");
        show_status("Manifest parse error", NULL);
        wifi_sta_disconnect();
        return OTA_RESULT_NO_MANIFEST;
    }

    /* Support both bare array [ {...}, ... ] and object { "apps": [...] } */
    cJSON *arr = root;
    if (cJSON_IsObject(root)) {
        arr = cJSON_GetObjectItem(root, "apps");
    }
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "Manifest is not an array (and has no 'apps' array)");
        show_status("Manifest format error", NULL);
        cJSON_Delete(root);
        wifi_sta_disconnect();
        return OTA_RESULT_NO_MANIFEST;
    }

    int parsed = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (parsed >= OTA_CATALOG_MAX) break;

        cJSON *j_name = cJSON_GetObjectItem(item, "name");
        cJSON *j_ver  = cJSON_GetObjectItem(item, "version");
        cJSON *j_url  = cJSON_GetObjectItem(item, "url");
        cJSON *j_size = cJSON_GetObjectItem(item, "size");
        cJSON *j_sha  = cJSON_GetObjectItem(item, "sha256");

        /* name is optional; others are required */
        if (!cJSON_IsString(j_url) ||
            !cJSON_IsNumber(j_size) ||
            !cJSON_IsString(j_sha)) {
            ESP_LOGW(TAG, "Skipping malformed entry %d", parsed);
            continue;
        }

        ota_app_entry_t *entry = &out->apps[parsed];

        if (cJSON_IsString(j_name)) {
            strlcpy(entry->name, j_name->valuestring, sizeof(entry->name));
        } else {
            snprintf(entry->name, sizeof(entry->name), "App %d", parsed + 1);
        }
        entry->version = cJSON_IsNumber(j_ver) ? (uint32_t)j_ver->valuedouble : 0;
        strlcpy(entry->url,    j_url->valuestring, sizeof(entry->url));
        entry->size = (int)j_size->valuedouble;
        strlcpy(entry->sha256, j_sha->valuestring, sizeof(entry->sha256));

        cJSON *j_icon = cJSON_GetObjectItem(item, "icon");
        if (cJSON_IsString(j_icon)) {
            strlcpy(entry->icon_url, j_icon->valuestring, sizeof(entry->icon_url));
        } else {
            entry->icon_url[0] = '\0';
        }

        ESP_LOGI(TAG, "Catalog[%d]: \"%s\" v%" PRIu32 " (%d B)",
                 parsed, entry->name, entry->version, entry->size);
        parsed++;
    }
    cJSON_Delete(root);

    if (parsed == 0) {
        ESP_LOGW(TAG, "Manifest contains no valid app entries");
        show_status("No apps in catalog", NULL);
        wifi_sta_disconnect();
        return OTA_RESULT_EMPTY_CATALOG;
    }

    out->count = parsed;
    ESP_LOGI(TAG, "Catalog ready: %d app(s)", parsed);
    /* WiFi remains connected for subsequent flash call */
    return OTA_RESULT_OK;
}

/* ── Flash selected app ───────────────────────────────────────────── */

ota_result_t ota_manager_flash_app(const ota_app_entry_t *app)
{
    ESP_LOGI(TAG, "Flashing \"%s\" v%" PRIu32 " (%d B)",
             app->name, app->version, app->size);

    /* Verify the image fits */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No OTA partition available");
        return OTA_RESULT_FLASH_FAIL;
    }
    if (app->size <= 0 || (uint32_t)app->size > update_part->size) {
        ESP_LOGE(TAG, "App size %d exceeds partition size %" PRIu32,
                 app->size, update_part->size);
        show_status("Image too large", NULL);
        return OTA_RESULT_FLASH_FAIL;
    }

    /* Stream + hash + flash */
    show_download_progress(app->name, app->version, 0, 0, app->size / 1024);
    esp_err_t err = http_stream_and_flash(app->url, app->size, app->sha256,
                                          app->name);
    if (err != ESP_OK) {
        return (err == ESP_ERR_OTA_VALIDATE_FAILED)
               ? OTA_RESULT_VERIFY_FAIL
               : OTA_RESULT_DOWNLOAD_FAIL;
    }

    /* Success — reboot into new partition */
    ESP_LOGI(TAG, "OTA complete — rebooting");
    show_status("Download Complete", "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    /* White fill lets TN pixels settle to a clean state so the student app
     * does not inherit the loader menu ghost on its first frame. */
    display_fill(DISPLAY_COLOR_WHITE);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();

    return OTA_RESULT_OK;  /* unreachable */
}

/* ── MicroPython full-flash path ──────────────────────────────────── *
 * Stages all three MicroPython binaries (bootloader, partition table, *
 * app) into the raw ota_0/ota_1 flash area using esp_flash_write      *
 * (no partition-size or protection checks for those addresses).       *
 * Writes a multi_flash_hdr_t into the first sector (0x160000), arms  *
 * the v1-format RTC flag at 0x600FFFF0 with UPDATE_MAGIC_MULTI, then *
 * calls esp_restart(). factory_switch reads the header from flash,    *
 * copies each binary to its target via ROM SPI, then triggers a       *
 * second software reset so the new partition table takes effect.      *
 *                                                                      *
 * Staging layout (raw flash, spans ota_0 + part of ota_1):            *
 *   0x160000  multi_flash_hdr_t (56 bytes, 1 sector reserved)         *
 *   0x161000  bootloader  (~19 KB, 5 sectors, ends at 0x165FFF)       *
 *   0x166000  partition table (~3 KB, 1 sector)                       *
 *   0x167000  app binary  (~1.6 MB, ends ~0x303020 < user_data)       *
 * ──────────────────────────────────────────────────────────────────── */

#define UPYTHON_NAME    "MicroPython"
#define UPYTHON_BIN_MAX 4
#define FLASH_SECTOR_SZ 4096

/* Staging layout — all within ota_0/ota_1 raw area (0x160000-0x3DFFFF).
 *
 * IMPORTANT: copy_region erases the full destination before copying.
 * The APP destination is 0x10000..0x1ABFFF (412 sectors for a 1.6 MB app).
 * STAGE_APP_ADDR must be >= 0x1AC000 so the erase never touches the source.
 * 0x1C0000 gives ~80 KB of headroom for app size variation.               */
#define STAGE_HDR_ADDR  0x160000u   /* multi_flash_hdr_t header (1 sector) */
#define STAGE_BL_ADDR   0x161000u   /* bootloader staging  (~19 KB)        */
#define STAGE_PT_ADDR   0x166000u   /* partition table staging (~3 KB)     */
#define STAGE_APP_ADDR  0x1C0000u   /* app staging (~1.6 MB, ends ~0x35C000) */

/* RTC flag — reuses the proven v1 16-byte slot at 0x600FFFF0.
 * factory_switch distinguishes v1 from v2 by the magic value.
 * For v2, staging_offset points to multi_flash_hdr_t in flash. */
#define RTC_FLAG_ADDR       0x600FFFF0u
#define UPDATE_MAGIC_MULTI  0xFA510A0Cu  /* must match factory_switch.c */
#define MULTI_HDR_MAGIC     0xFA514D4Cu  /* must match factory_switch.c */
#define MULTI_HDR_MAX       4

typedef struct { uint32_t src; uint32_t dst; uint32_t size; } multi_bin_entry_t;
typedef struct {
    uint32_t          magic;
    uint32_t          count;
    multi_bin_entry_t bins[MULTI_HDR_MAX];
} multi_flash_hdr_t;  /* 4+4+48 = 56 bytes */

typedef struct {
    uint32_t magic;
    uint32_t staging_offset;
    uint32_t binary_size;
    uint32_t magic_inv;
} rtc_update_flag_t;

typedef struct { char url[OTA_APP_URL_MAX]; uint32_t address; } upython_bin_t;
typedef struct { upython_bin_t bins[UPYTHON_BIN_MAX]; int count; } upython_entry_t;

static esp_err_t parse_upython_entry(const char *json, upython_entry_t *out)
{
    out->count = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_FAIL;

    cJSON *apps = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "apps") : root;
    if (!cJSON_IsArray(apps)) { cJSON_Delete(root); return ESP_FAIL; }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, apps) {
        cJSON *j_name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(j_name) ||
            strcmp(j_name->valuestring, UPYTHON_NAME) != 0) continue;

        cJSON *j_bins = cJSON_GetObjectItem(item, "binaries");
        if (!cJSON_IsArray(j_bins)) break;

        cJSON *bin = NULL;
        cJSON_ArrayForEach(bin, j_bins) {
            if (out->count >= UPYTHON_BIN_MAX) break;
            cJSON *j_url  = cJSON_GetObjectItem(bin, "url");
            cJSON *j_addr = cJSON_GetObjectItem(bin, "address");
            if (!cJSON_IsString(j_url) || !cJSON_IsNumber(j_addr)) continue;
            strlcpy(out->bins[out->count].url,  j_url->valuestring, OTA_APP_URL_MAX);
            out->bins[out->count].address = (uint32_t)j_addr->valuedouble;
            out->count++;
        }
        break;
    }
    cJSON_Delete(root);
    return (out->count > 0) ? ESP_OK : ESP_FAIL;
}

/* Download a binary directly to a raw flash address via esp_flash_write.
 * Uses esp_flash_erase_region (no partition-boundary size limit) so the
 * write can span the ota_0/ota_1 boundary for large binaries. */
static esp_err_t download_and_stage(const char *url, uint32_t flash_addr,
                                     const char *name, uint32_t *out_size)
{
    esp_http_client_config_t cfg = {
        .url                   = url,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 120000,
        .buffer_size           = OTA_CHUNK_SIZE,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }

    int content_len = (int)esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 || content_len <= 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint32_t erase_sz = ((uint32_t)content_len + FLASH_SECTOR_SZ - 1)
                        & ~(uint32_t)(FLASH_SECTOR_SZ - 1);
    display_fill(DISPLAY_COLOR_BLACK);
    {
        const char *prep = "Preparing";
        int pw = (int)strlen(prep) * DISPLAY_FONT_W * 2;
        display_draw_string((DISPLAY_W - pw) / 2, 8, prep,
                            DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
        if (name && name[0]) {
            int nw = (int)strlen(name) * DISPLAY_FONT_W * 2;
            display_draw_string((DISPLAY_W - nw) / 2, 32, name,
                                DISPLAY_COLOR_CYAN, DISPLAY_COLOR_BLACK, 2);
        }
    }
    err = esp_flash_erase_region(NULL, flash_addr, erase_sz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stage erase @ 0x%08" PRIx32 " size %" PRIu32 ": %s",
                 flash_addr, erase_sz, esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    int total = 0, last_pct = -1;
    while (total < content_len) {
        int want = content_len - total;
        if (want > OTA_CHUNK_SIZE) want = OTA_CHUNK_SIZE;
        int n = esp_http_client_read(client, (char *)s_ota_chunk, want);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;

        err = esp_flash_write(NULL, s_ota_chunk,
                              flash_addr + (uint32_t)total, (uint32_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Stage write @ 0x%08" PRIx32 ": %s",
                     flash_addr + (uint32_t)total, esp_err_to_name(err));
            break;
        }
        total += n;

        int pct = (int)(100LL * total / content_len);
        if (pct / 2 != last_pct / 2) {
            last_pct = pct;
            show_download_progress(name, 0, pct, total / 1024, content_len / 1024);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || total != content_len) {
        ESP_LOGE(TAG, "Stage incomplete: %d of %d", total, content_len);
        return ESP_FAIL;
    }
    *out_size = (uint32_t)total;
    ESP_LOGI(TAG, "Staged %d B at 0x%" PRIx32, total, flash_addr);
    return ESP_OK;
}

ota_result_t ota_manager_flash_micropython(void)
{
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return OTA_RESULT_NO_WIFI;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return OTA_RESULT_NO_WIFI;

    esp_err_t ue = nvs_flash_init_partition(NVS_PARTITION);
    if (ue == ESP_ERR_NVS_NO_FREE_PAGES || ue == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(NVS_PARTITION);
        nvs_flash_init_partition(NVS_PARTITION);
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return OTA_RESULT_NO_WIFI;
    size_t n;
    n = sizeof(ssid); nvs_get_str(h, NVS_KEY_SSID, ssid, &n);
    n = sizeof(pass); nvs_get_str(h, NVS_KEY_PASS, pass, &n);
    nvs_close(h);

    if (ssid[0] == '\0') return OTA_RESULT_NO_WIFI;

    const char *mfst_url = "https://byu-i-ebadge.github.io/apps/manifest.json";

    show_wifi_status("Connecting to WiFi...", ssid);
    if (!wifi_sta_connect(ssid, pass)) {
        wifi_sta_disconnect();
        return OTA_RESULT_NO_WIFI;
    }

    show_status("Fetching manifest...", NULL);
    static char s_mpy_manifest[6144];
    int mlen = 0;
    if (http_get_text(mfst_url, s_mpy_manifest, sizeof(s_mpy_manifest), &mlen) != ESP_OK) {
        wifi_sta_disconnect();
        return OTA_RESULT_NO_MANIFEST;
    }

    upython_entry_t entry;
    if (parse_upython_entry(s_mpy_manifest, &entry) != ESP_OK) {
        ESP_LOGE(TAG, "MicroPython entry not found in manifest");
        wifi_sta_disconnect();
        return OTA_RESULT_NO_MANIFEST;
    }

    /* Locate the three required binaries by manifest address. */
    const char *bl_url = NULL, *pt_url = NULL, *app_url = NULL;
    for (int i = 0; i < entry.count; i++) {
        uint32_t addr = entry.bins[i].address;
        if (addr == 0x0u)     bl_url  = entry.bins[i].url;
        if (addr == 0x8000u)  pt_url  = entry.bins[i].url;
        if (addr == 0x10000u) app_url = entry.bins[i].url;
    }
    if (!bl_url || !pt_url || !app_url) {
        ESP_LOGE(TAG, "MicroPython manifest missing required binary "
                 "(bl=%s pt=%s app=%s)",
                 bl_url ? "ok" : "MISSING",
                 pt_url ? "ok" : "MISSING",
                 app_url ? "ok" : "MISSING");
        wifi_sta_disconnect();
        return OTA_RESULT_NO_MANIFEST;
    }

    /* Stage all three binaries into raw flash (ota_0/ota_1 area).
     * esp_flash_erase_region/write operate on absolute addresses with no
     * partition-size limit, so the 1.6 MB app can span ota_0 into ota_1. */
    uint32_t bl_size = 0, pt_size = 0, app_size = 0;

    if (download_and_stage(bl_url,  STAGE_BL_ADDR,  "Bootloader",      &bl_size)  != ESP_OK ||
        download_and_stage(pt_url,  STAGE_PT_ADDR,  "Partition Table", &pt_size)  != ESP_OK ||
        download_and_stage(app_url, STAGE_APP_ADDR, "MicroPython",     &app_size) != ESP_OK) {
        show_status("Download failed!", NULL);
        vTaskDelay(pdMS_TO_TICKS(3000));
        wifi_sta_disconnect();
        return OTA_RESULT_DOWNLOAD_FAIL;
    }

    wifi_sta_disconnect();

    /* Write multi_flash_hdr_t into the first sector of the staging area.
     * factory_switch reads this from flash — no RTC memory size limit. */
    multi_flash_hdr_t hdr = {
        .magic = MULTI_HDR_MAGIC,
        .count = 3,
        .bins  = {
            { .src = STAGE_BL_ADDR,  .dst = 0x000000u, .size = bl_size  },
            { .src = STAGE_PT_ADDR,  .dst = 0x008000u, .size = pt_size  },
            { .src = STAGE_APP_ADDR, .dst = 0x010000u, .size = app_size },
        },
    };
    show_status("Installing...", "Do not power off");
    esp_err_t herr = esp_flash_erase_region(NULL, STAGE_HDR_ADDR, FLASH_SECTOR_SZ);
    if (herr == ESP_OK) {
        herr = esp_flash_write(NULL, &hdr, STAGE_HDR_ADDR, sizeof(hdr));
    }
    if (herr != ESP_OK) {
        ESP_LOGE(TAG, "Header write failed: %s", esp_err_to_name(herr));
        show_status("Flash error!", NULL);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return OTA_RESULT_FLASH_FAIL;
    }
    ESP_LOGI(TAG, "Flash hdr @ 0x%05x: bl=%uB pt=%uB app=%uB",
             (unsigned)STAGE_HDR_ADDR,
             (unsigned)bl_size, (unsigned)pt_size, (unsigned)app_size);
    /* factory_switch detects the operation by reading the header magic
     * directly from flash — no RTC memory flag needed. */

    /* Show success screen before triggering the install reboot. */
    display_fill(DISPLAY_COLOR_BLACK);
    {
        const char *hd = "MicroPython";
        int hw = (int)strlen(hd) * DISPLAY_FONT_W * 2;
        display_draw_string((DISPLAY_W - hw) / 2, 72, hd,
                            DISPLAY_COLOR_GREEN, DISPLAY_COLOR_BLACK, 2);
    }
    display_draw_string(8, 116, "REPL is now running.",
                        DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 1);
    display_draw_string(8, 132, "Connect USB: 115200 baud",
                        DISPLAY_COLOR_CYAN, DISPLAY_COLOR_BLACK, 1);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    return OTA_RESULT_OK;  /* unreachable */
}
