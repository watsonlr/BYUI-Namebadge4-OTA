/**
 * @file loader_menu.h
 * @brief Factory-loader menu: renders the 5-item menu on the display and
 *        dispatches to the appropriate action when the user selects an item.
 *
 * Navigation:
 *   Up / Down       — move highlight
 *   A   / Right     — confirm / select
 *   B   / Left      — (reserved for future sub-menu back navigation)
 *
 * Call loader_menu_run() after WiFi has been configured.  The function loops
 * until the user selects an action that does not return (e.g. OTA reboot or
 * bare-metal mode), then returns so the caller can loop back if desired.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display the loader menu and handle user input indefinitely.
 *
 * Actions that trigger a system restart (OTA update, bare-metal mode) will
 * not return.  Actions that complete and stay in the loader (WiFi reconfig,
 * "coming soon" stubs) will redraw the menu and continue.
 */
void loader_menu_run(void);

#ifdef __cplusplus
}
#endif
