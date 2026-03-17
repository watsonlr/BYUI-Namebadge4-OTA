/**
 * @file portal_mode.h
 * @brief All-in-one Wi-Fi config portal: starts the AP, shows a QR code on
 *        the TFT, then blocks until the user submits credentials (or the
 *        optional timeout expires).
 *
 * Call portal_mode_run() from app_main() whenever you want to enter setup.
 * Credentials are written to NVS under the namespace defined in wifi_config.h.
 */

#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the configuration portal and block until done.
 *
 * 1. Calls wifi_config_start() to bring up the SoftAP + HTTP server.
 * 2. Draws a QR code on the TFT pointing at http://192.168.4.1/
 * 3. Draws the SSID and URL as text below the QR code.
 * 4. Polls wifi_config_done() every 500 ms.
 * 5. When credentials are saved (or timeout_s seconds elapse if > 0),
 *    calls wifi_config_stop(), clears the instruction overlay, and returns.
 *
 * @param timeout_s  Maximum seconds to wait (0 = wait forever).
 * @return true if credentials were submitted; false if timed out.
 */
bool portal_mode_run(int timeout_s);

#ifdef __cplusplus
}
#endif
