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

/* ── Display helper ───────────────────────────────────────────────── */
static void show_status(const char *line1, const char *line2)
{
    display_fill(DISPLAY_COLOR_BLACK);
    display_text_ctx_t ctx = DISPLAY_CTX(DISPLAY_FONT_SANS, 1,
                                          DISPLAY_COLOR_WHITE,
                                          DISPLAY_COLOR_BLACK);
    if (line1) display_print(&ctx,  8, 108, line1);
    if (line2) display_print(&ctx,  8, 124, line2);
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
                                        const char *sha256_expected)
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
        if (pct / 5 != last_pct / 5) {
            last_pct = pct;
            char prog[32];
            snprintf(prog, sizeof(prog), "%d%%  (%d KB)", pct, total / 1024);
            show_status("Downloading...", prog);
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
        ESP_LOGW(TAG, "No manifest URL configured");
        return OTA_RESULT_NO_MANIFEST;
    }

    /* Connect to WiFi */
    show_status("Connecting to WiFi...", ssid);
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    if (!wifi_sta_connect(ssid, pass)) {
        ESP_LOGE(TAG, "WiFi connect failed");
        show_status("WiFi connect failed", ssid);
        wifi_sta_disconnect();
        return OTA_RESULT_NO_WIFI;
    }
    ESP_LOGI(TAG, "WiFi connected");

    /* Fetch manifest JSON */
    show_status("Fetching app catalog...", NULL);

    static char manifest_json[4096];
    int manifest_len = 0;
    if (http_get_text(mfst_url, manifest_json, sizeof(manifest_json),
                      &manifest_len) != ESP_OK) {
        ESP_LOGE(TAG, "Manifest fetch failed: %s", mfst_url);
        show_status("Manifest fetch failed", NULL);
        wifi_sta_disconnect();
        return OTA_RESULT_NO_MANIFEST;
    }
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
    show_status("Downloading...", app->name);
    esp_err_t err = http_stream_and_flash(app->url, app->size, app->sha256);
    if (err != ESP_OK) {
        return (err == ESP_ERR_OTA_VALIDATE_FAILED)
               ? OTA_RESULT_VERIFY_FAIL
               : OTA_RESULT_DOWNLOAD_FAIL;
    }

    /* Success — reboot into new partition */
    ESP_LOGI(TAG, "OTA complete — rebooting");
    show_status("Update complete!", "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    /* White fill lets TN pixels settle to a clean state so the student app
     * does not inherit the loader menu ghost on its first frame. */
    display_fill(DISPLAY_COLOR_WHITE);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();

    return OTA_RESULT_OK;  /* unreachable */
}
