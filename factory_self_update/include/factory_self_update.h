/**
 * @file factory_self_update.h
 * @brief Automatic factory-loader self-update over WiFi.
 *
 * Call factory_self_update_check() once at startup (after WiFi credentials
 * are confirmed to be configured) and before loader_menu_run().
 *
 * Behaviour:
 *   1. Connects to the saved WiFi AP.
 *   2. Fetches the loader manifest from a hardcoded URL.
 *   3. If the manifest describes a newer loader for the same HW revision:
 *        a. Downloads the binary to the inactive OTA slot (raw staging).
 *        b. Verifies SHA-256.
 *        c. Writes an RTC-memory flag so factory_switch can apply the update.
 *        d. Calls esp_restart() — this function does NOT return.
 *   4. If no update is needed (or any step fails): disconnects WiFi silently
 *      and returns.  The loader menu then runs as usual.
 *
 * The update is applied by factory_switch.c in the bootloader on the next
 * (software) reset: the staged binary is copied sector-by-sector into the
 * factory partition.  otadata is left intact so the student app is preserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check for and (if available) download a factory-loader update.
 *
 * Silently returns on failure or when no update is available.
 * Does NOT return if an update is downloaded and applied (esp_restart called).
 */
void factory_self_update_check(void);

#ifdef __cplusplus
}
#endif
