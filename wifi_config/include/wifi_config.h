/**
 * @file wifi_config.h
 * @brief SoftAP + HTTP captive-config portal.
 *
 * Starts a Wi-Fi access point and a tiny HTTP server so a phone can
 * browse to http://192.168.4.1/ and POST station credentials that are
 * saved to NVS.  Call wifi_config_start() once; it runs in the
 * background until wifi_config_stop() is called.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Dedicated NVS partition for user-entered badge configuration.
 *
 * This partition lives at the top of flash (0x3E0000) and is NEVER
 * overwritten by OTA.  Even if a user loads their own firmware, the
 * data here survives.  Any IDF application can read it with:
 *
 *   nvs_flash_init_partition(WIFI_CONFIG_NVS_PARTITION);
 *   nvs_open_from_partition(WIFI_CONFIG_NVS_PARTITION,
 *                           WIFI_CONFIG_NVS_NAMESPACE,
 *                           NVS_READONLY, &h);
 */
#define WIFI_CONFIG_NVS_PARTITION    "user_data"
#define WIFI_CONFIG_NVS_NAMESPACE    "badge_cfg"
#define WIFI_CONFIG_NVS_KEY_SSID     "ssid"
#define WIFI_CONFIG_NVS_KEY_PASS     "pass"
#define WIFI_CONFIG_NVS_KEY_NICK     "nick"
#define WIFI_CONFIG_NVS_KEY_EMAIL    "email"
#define WIFI_CONFIG_NVS_KEY_MANIFEST "mfst"

/**
 * @brief Start the SoftAP and HTTP configuration portal.
 *
 * Initialises NVS (if not already done), brings up the AP, starts the
 * HTTP server, and returns immediately.  The portal runs in the
 * background via ESP-IDF tasks.
 *
 * @return true on success, false if the AP or HTTP server failed to start.
 */
bool wifi_config_start(void);

/**
 * @brief Stop the portal and tear down the AP + HTTP server.
 */
void wifi_config_stop(void);

/**
 * @brief Check whether the user has submitted credentials via the portal.
 *
 * Credentials are written to NVS before this returns true.
 *
 * @return true once a valid SSID has been saved.
 */
bool wifi_config_done(void);

/**
 * @brief Returns true once at least one station has associated with the AP.
 *
 * Useful for updating the display to indicate the phone has connected,
 * before the captive-portal popup has been interacted with.
 */
bool wifi_config_sta_joined(void);

/**
 * @brief Returns true once the setup form has been served to a browser.
 *
 * Useful for updating the display with form-filling instructions as soon
 * as a phone opens the page.
 */
bool wifi_config_form_served(void);



/**
 * @brief Returns true if a badge nickname has already been saved to NVS.
 *
 * Call this at boot (before wifi_config_start) to decide whether to skip
 * the configuration portal entirely.
 */
bool wifi_config_is_configured(void);

/**
 * @brief Copy the saved badge nickname into @p out (NUL-terminated).
 *
 * @param out     Destination buffer.
 * @param outlen  Size of destination buffer (at least 33 bytes recommended).
 */
void wifi_config_get_nick(char *out, size_t outlen);

/**
 * @brief Return the device-unique AP SSID (e.g. "BYUI_NameBadge_F8").
 *        Valid after wifi_config_start() is called.
 */
const char *wifi_config_ssid(void);

/**
 * @brief Return the device-unique URL for the config portal
 *        (e.g. "http://192.168.60.8/"). Valid after wifi_config_start().
 */
const char *wifi_config_url(void);

#ifdef __cplusplus
}
#endif
