/**
 * @file factory_self_update.c
 * @brief Automatic factory-loader self-update over WiFi.
 *
 * Flow:
 *   1. Init netif + event loop (idempotent — tolerates already-created state).
 *   2. Read SSID/PASS from user_data NVS.
 *   3. Connect to WiFi STA.
 *   4. GET manifest JSON from LOADER_MANIFEST_URL.
 *   5. Parse: hw_version, loader_version, binary_url, size, sha256.
 *   6. If hw_version == LOADER_HW_VERSION && loader_version > LOADER_SW_VERSION:
 *        a. Find inactive OTA partition for staging.
 *        b. Erase and stream-download binary into staging partition (raw write,
 *           no esp_ota_end — otadata is never touched).
 *        c. Verify SHA-256.
 *        d. Write RTC flag at RTC_FLAG_ADDR (magic + staging_offset + size + ~magic).
 *        e. Disconnect WiFi, display update notice, esp_restart().
 *   7. Otherwise: disconnect WiFi silently and return.
 *
 * The bootloader (factory_switch.c) checks the RTC flag on the next software
 * reset, copies the staged binary to the factory partition, and clears the flag.
 */

#include "factory_self_update.h"
#include "loader_menu.h"    /* LOADER_HW_VERSION, LOADER_SW_VERSION */
#include "wifi_config.h"    /* NVS partition / namespace / key constants */
#include "display.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "mbedtls/sha256.h"
#include "cJSON.h"

#define TAG  "factory_self_update"

/* ── Manifest URL (placeholder — replace with actual hosting URL) ─── */
#define LOADER_MANIFEST_URL  "https://TODO/loader_manifest.json"

/* ── RTC slow memory flag — shared between app and bootloader ──────── *
 * Address 0x600FFFF0: last 16 bytes of RTC slow DRAM (8 KB region that
 * starts at 0x600FE000).  This area is outside the FreeRTOS heap and
 * retains its value through a software reset (cleared on hardware reset). */
#define RTC_FLAG_ADDR   0x600FFFF0u
#define UPDATE_MAGIC    0xFA510A0Bu

typedef struct {
    uint32_t magic;           /* UPDATE_MAGIC                      */
    uint32_t staging_offset;  /* flash byte address of staging slot */
    uint32_t binary_size;     /* byte count of staged binary        */
    uint32_t magic_inv;       /* ~magic, used as second check word  */
} factory_update_flag_t;

/* ── WiFi STA ─────────────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRIES    3
#define WIFI_TIMEOUT_MS     20000

static EventGroupHandle_t s_wifi_eg   = NULL;
static esp_netif_t       *s_sta_netif = NULL;
static int                s_retries   = 0;

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
        s_retries = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(const char *ssid, const char *pass)
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

static void wifi_disconnect(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
}

/* ── HTTP: fetch manifest JSON (small text) ──────────────────────── */
#define MANIFEST_BUF_SIZE  1024
static char s_manifest_json[MANIFEST_BUF_SIZE];

static bool fetch_manifest(void)
{
    esp_http_client_config_t cfg = {
        .url                   = LOADER_MANIFEST_URL,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 8000,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return false; }

    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int total = 0;
    while (total < (int)sizeof(s_manifest_json) - 1) {
        int n = esp_http_client_read(client,
                                     s_manifest_json + total,
                                     (int)sizeof(s_manifest_json) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    s_manifest_json[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total > 0;
}

/* ── HTTP: stream binary to OTA staging partition ─────────────────── */
#define CHUNK_SIZE  8192
static uint8_t s_chunk[CHUNK_SIZE];   /* static — too large for stack */

static bool download_to_staging(const char *url, int expected_size,
                                  const char *sha256_expected,
                                  const esp_partition_t *staging)
{
    /* Erase only the sectors we need */
    uint32_t erase_size = ((uint32_t)expected_size + 4095u) & ~4095u;
    if (erase_size > staging->size) {
        ESP_LOGE(TAG, "Binary too large for staging partition");
        return false;
    }
    esp_err_t err = esp_partition_erase_range(staging, 0, erase_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Staging erase failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_http_client_config_t cfg = {
        .url                   = url,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 120000,
        .buffer_size           = CHUNK_SIZE,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return false; }

    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    int total = 0;
    bool ok   = true;

    while (total < expected_size) {
        int want = expected_size - total;
        if (want > CHUNK_SIZE) want = CHUNK_SIZE;

        int n = esp_http_client_read(client, (char *)s_chunk, want);
        if (n < 0) { ok = false; break; }
        if (n == 0) break;

        mbedtls_sha256_update(&sha_ctx, s_chunk, n);

        err = esp_partition_write(staging, (uint32_t)total, s_chunk, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Partition write @%d: %s", total, esp_err_to_name(err));
            ok = false;
            break;
        }

        total += n;

        /* Simple progress log every 64 KB */
        if ((total % (64 * 1024)) < CHUNK_SIZE)
            ESP_LOGI(TAG, "Downloaded %d / %d KB", total / 1024, expected_size / 1024);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok || total != expected_size) {
        ESP_LOGE(TAG, "Download incomplete: %d / %d B", total, expected_size);
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }

    /* Verify SHA-256 */
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    char computed[65] = {0};
    for (int i = 0; i < 32; i++) snprintf(computed + 2 * i, 3, "%02x", hash[i]);

    if (strcmp(computed, sha256_expected) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch\n  expected: %s\n  computed: %s",
                 sha256_expected, computed);
        return false;
    }

    ESP_LOGI(TAG, "SHA-256 OK");
    return true;
}

/* ── Display helpers ──────────────────────────────────────────────── */
static void show_msg(const char *line1, const char *line2)
{
    display_fill(DISPLAY_COLOR_BLACK);
    if (line1) {
        int w = (int)strlen(line1) * DISPLAY_FONT_W * 2;
        display_draw_string((DISPLAY_W - w) / 2, 100,
                            line1, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK, 2);
    }
    if (line2) {
        int w = (int)strlen(line2) * DISPLAY_FONT_W;
        display_draw_string((DISPLAY_W - w) / 2, 132,
                            line2, DISPLAY_COLOR_YELLOW, DISPLAY_COLOR_BLACK, 1);
    }
}

/* ── Public entry point ────────────────────────────────────────────── */

void factory_self_update_check(void)
{
    /* Init network stack — both calls tolerate already-initialised state. */
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_netif_init: %s — skipping update check", esp_err_to_name(e));
        return;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_event_loop_create_default: %s — skipping update check",
                 esp_err_to_name(e));
        return;
    }

    /* Read WiFi credentials from NVS */
    esp_err_t ue = nvs_flash_init_partition(WIFI_CONFIG_NVS_PARTITION);
    if (ue == ESP_ERR_NVS_NO_FREE_PAGES || ue == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION);
        nvs_flash_init_partition(WIFI_CONFIG_NVS_PARTITION);
    }

    char ssid[33] = {0};
    char pass[65] = {0};

    nvs_handle_t h;
    if (nvs_open_from_partition(WIFI_CONFIG_NVS_PARTITION,
                                WIFI_CONFIG_NVS_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed — skipping update check");
        return;
    }
    size_t n;
    n = sizeof(ssid); nvs_get_str(h, WIFI_CONFIG_NVS_KEY_SSID, ssid, &n);
    n = sizeof(pass); nvs_get_str(h, WIFI_CONFIG_NVS_KEY_PASS, pass, &n);
    nvs_close(h);

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "No SSID configured — skipping update check");
        return;
    }

    /* Connect to WiFi */
    show_msg("Checking for", "loader update...");
    ESP_LOGI(TAG, "Connecting to '%s' to check for loader update", ssid);

    if (!wifi_connect(ssid, pass)) {
        ESP_LOGW(TAG, "WiFi connect failed — skipping update check");
        wifi_disconnect();
        return;
    }

    /* Fetch manifest */
    if (!fetch_manifest()) {
        ESP_LOGW(TAG, "Manifest fetch failed — skipping update check");
        wifi_disconnect();
        return;
    }

    /* Parse manifest JSON */
    cJSON *root = cJSON_Parse(s_manifest_json);
    if (!root) {
        ESP_LOGW(TAG, "Manifest JSON parse error");
        wifi_disconnect();
        return;
    }

    int  mfst_hw      = cJSON_GetObjectItem(root, "hw_version")     ? cJSON_GetObjectItem(root, "hw_version")->valueint     : -1;
    int  mfst_loader  = cJSON_GetObjectItem(root, "loader_version") ? cJSON_GetObjectItem(root, "loader_version")->valueint : -1;
    const char *bin_url = cJSON_GetObjectItem(root, "binary_url") && cJSON_IsString(cJSON_GetObjectItem(root, "binary_url"))
                            ? cJSON_GetObjectItem(root, "binary_url")->valuestring : NULL;
    int  bin_size     = cJSON_GetObjectItem(root, "size")           ? cJSON_GetObjectItem(root, "size")->valueint           : 0;
    const char *sha256  = cJSON_GetObjectItem(root, "sha256") && cJSON_IsString(cJSON_GetObjectItem(root, "sha256"))
                            ? cJSON_GetObjectItem(root, "sha256")->valuestring : NULL;

    ESP_LOGI(TAG, "Manifest: hw=%d loader=%d (current: hw=%d loader=%d)",
             mfst_hw, mfst_loader, LOADER_HW_VERSION, LOADER_SW_VERSION);

    bool update_needed = (mfst_hw == LOADER_HW_VERSION)
                      && (mfst_loader > LOADER_SW_VERSION)
                      && (bin_url != NULL)
                      && (bin_size > 0)
                      && (sha256 != NULL);

    if (!update_needed) {
        ESP_LOGI(TAG, "No loader update needed");
        cJSON_Delete(root);
        wifi_disconnect();
        return;
    }

    /* Copy strings out of cJSON before we do the download (cJSON_Delete later) */
    static char s_bin_url[257];
    static char s_sha256[65];
    strlcpy(s_bin_url, bin_url, sizeof(s_bin_url));
    strlcpy(s_sha256,  sha256,  sizeof(s_sha256));
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loader update available: v%d.%d → v%d.%d",
             LOADER_HW_VERSION, LOADER_SW_VERSION, mfst_hw, mfst_loader);

    /* Find the inactive OTA partition for staging */
    const esp_partition_t *staging = esp_ota_get_next_update_partition(NULL);
    if (!staging) {
        ESP_LOGE(TAG, "No inactive OTA partition available for staging");
        wifi_disconnect();
        return;
    }
    ESP_LOGI(TAG, "Staging to partition '%s' @ 0x%08" PRIx32 " (%"PRIu32" B available)",
             staging->label, staging->address, staging->size);

    /* Download and verify */
    show_msg("Updating loader...", "Do not power off");
    bool dl_ok = download_to_staging(s_bin_url, bin_size, s_sha256, staging);
    wifi_disconnect();

    if (!dl_ok) {
        ESP_LOGE(TAG, "Download/verify failed — update aborted");
        show_msg("Update failed", "Continuing...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    /* Write RTC flag for factory_switch to pick up on next boot.
     * Write magic_inv first, then staging_offset, then binary_size,
     * and magic last — so an incomplete write leaves an invalid flag. */
    volatile factory_update_flag_t *flag =
            (volatile factory_update_flag_t *)RTC_FLAG_ADDR;
    flag->magic_inv      = ~UPDATE_MAGIC;
    flag->staging_offset = staging->address;
    flag->binary_size    = (uint32_t)bin_size;
    flag->magic          = UPDATE_MAGIC;   /* arm the flag last */

    ESP_LOGI(TAG, "RTC flag set — restarting to apply update");

    /* Brief on-screen notice before restart */
    show_msg("Applying update...", "Restarting now");
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_restart();  /* software restart → factory_switch picks up the flag */
}
