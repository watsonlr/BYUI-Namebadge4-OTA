/**
 * @file factory_self_update.h
 * @brief Factory-loader self-update over WiFi.
 *
 * Call factory_self_update_begin() before splash_screen_run() and
 * factory_self_update_finish() after it.  The WiFi check runs silently
 * in the background during the splash so there is no added boot time.
 * A confirmation screen is shown only if a newer loader is found.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the background update check.
 *
 * Launches a FreeRTOS task that connects to WiFi and fetches the loader
 * manifest.  Does nothing if WiFi credentials are not yet configured
 * (safe to call unconditionally before the portal check).
 * No display I/O — safe to call while splash_screen_run() is active.
 */
void factory_self_update_begin(void);

/**
 * @brief Finish the update check and act on the result.
 *
 * Waits for the background task to complete (normally already done by
 * the time the splash finishes).  If a newer loader version was found,
 * shows a confirmation screen.
 *   - A pressed: downloads and applies the update (calls esp_restart —
 *                this function does NOT return).
 *   - B pressed: skips silently and returns.
 * If no update is available or begin() was never called: returns immediately.
 */
void factory_self_update_finish(void);

#ifdef __cplusplus
}
#endif
