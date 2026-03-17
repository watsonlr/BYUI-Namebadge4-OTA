/**
 * @file ota_manager.h
 * @brief Multi-app OTA catalog manager.
 *
 * Typical call sequence:
 *
 *   ota_catalog_t cat;
 *   ota_result_t r = ota_manager_fetch_catalog(&cat);
 *   if (r == OTA_RESULT_OK) {
 *       // present cat.apps[0..cat.count-1] to user
 *       r = ota_manager_flash_app(&cat.apps[sel]);
 *       // flash_app only returns on error; success = device reboots
 *       ota_manager_wifi_disconnect();
 *   }
 *
 * Manifest JSON format — bare array hosted at the URL stored in NVS "mfst":
 *   [
 *     { "name":    "App Name",
 *       "version": 1,
 *       "url":     "https://raw.githubusercontent.com/.../apps/app.bin",
 *       "size":    123456,
 *       "sha256":  "64 lowercase hex chars",
 *       "icon":    "https://raw.githubusercontent.com/.../icons/app_icon.bin" },
 *     ...
 *   ]
 *
 * NVS partition: "user_data", namespace: "badge_cfg"
 * NVS keys read: "ssid", "pass", "mfst"
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Catalog limits ────────────────────────────────────────────────── */
#define OTA_APP_NAME_MAX   48    /**< Max chars in an app display name.  */
#define OTA_APP_URL_MAX   257    /**< Max chars in a firmware binary URL. */
#define OTA_APP_SHA_MAX    65    /**< SHA-256 hex string (64 chars + NUL).*/
#define OTA_CATALOG_MAX    10    /**< Max apps parsed from one manifest.  */

/* ── Icon dimensions (fixed contract, produced by png_to_icon.py) ──── */
#define OTA_ICON_W      308      /**< Icon width  in pixels.             */
#define OTA_ICON_H       72      /**< Icon height in pixels.             */
#define OTA_ICON_BYTES  (OTA_ICON_W * OTA_ICON_H * 2)  /**< 44 352 B.  */

/* ── Catalog types ─────────────────────────────────────────────────── */

/** One app entry parsed from the manifest array. */
typedef struct {
    char     name[OTA_APP_NAME_MAX];
    uint32_t version;
    char     url[OTA_APP_URL_MAX];
    int      size;                   /**< Expected binary size in bytes.       */
    char     sha256[OTA_APP_SHA_MAX];
    char     icon_url[OTA_APP_URL_MAX]; /**< Optional icon URL; "" if absent.  */
} ota_app_entry_t;

/** Parsed catalog returned by ota_manager_fetch_catalog(). */
typedef struct {
    ota_app_entry_t apps[OTA_CATALOG_MAX];
    int             count;       /**< Number of valid entries (0..OTA_CATALOG_MAX). */
} ota_catalog_t;

/* ── Result codes ──────────────────────────────────────────────────── */

typedef enum {
    OTA_RESULT_OK            = 0, /**< Success (catalog fetched, or flash done). */
    OTA_RESULT_NO_WIFI,           /**< No credentials or failed to connect.      */
    OTA_RESULT_NO_MANIFEST,       /**< Manifest URL missing, fetch, or parse.    */
    OTA_RESULT_EMPTY_CATALOG,     /**< Manifest parsed but contains no apps.     */
    OTA_RESULT_DOWNLOAD_FAIL,     /**< Firmware binary download failed.          */
    OTA_RESULT_VERIFY_FAIL,       /**< SHA-256 mismatch.                         */
    OTA_RESULT_FLASH_FAIL,        /**< OTA partition write failed.               */
} ota_result_t;

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * @brief Connect to WiFi and fetch + parse the app catalog.
 *
 * On success (OTA_RESULT_OK) WiFi remains connected so
 * ota_manager_flash_app() can download firmware immediately.
 * Call ota_manager_wifi_disconnect() when finished.
 *
 * On error WiFi is already disconnected — no cleanup needed.
 *
 * @param out  Filled on OTA_RESULT_OK; count >= 1.
 * @return     OTA_RESULT_OK, or an error code.
 */
ota_result_t ota_manager_fetch_catalog(ota_catalog_t *out);

/**
 * @brief Download and flash one app entry to the inactive OTA partition.
 *
 * Assumes WiFi is already connected (call after ota_manager_fetch_catalog).
 * On success: marks the partition bootable, then calls esp_restart() —
 * this function NEVER returns on success.
 *
 * On failure: returns the error code.  WiFi is left connected; call
 * ota_manager_wifi_disconnect() explicitly before returning to the menu.
 *
 * @param app  Selected entry from the catalog.
 * @return     Error code only (never OTA_RESULT_OK — device reboots).
 */
ota_result_t ota_manager_flash_app(const ota_app_entry_t *app);

/**
 * @brief Download one icon binary into a PSRAM (or heap) buffer.
 *
 * Assumes WiFi is already connected.  Allocates OTA_ICON_BYTES using
 * MALLOC_CAP_SPIRAM where possible, falling back to internal heap.
 *
 * @param url  URL of the raw 316×76 big-endian RGB565 binary.
 * @return     Pointer to OTA_ICON_BYTES of pixel data, or NULL on error.
 *             Caller is responsible for free()-ing the buffer.
 */
uint16_t *ota_manager_fetch_icon(const char *url);

/**
 * @brief Disconnect WiFi STA and release all resources.
 *
 * Call after ota_manager_fetch_catalog() returns OTA_RESULT_OK and you
 * are done (either flashing succeeded+rebooted, or the user cancelled,
 * or ota_manager_flash_app() returned an error).
 */
void ota_manager_wifi_disconnect(void);

#ifdef __cplusplus
}
#endif
