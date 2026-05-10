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

/** Hardware revision of the eBadge PCB this loader was built for. */
#define LOADER_HW_VERSION   4

/** Factory-loader software release number (increment when updating). */
#define LOADER_SW_VERSION   6

/* NVS location for loader-specific flags (shared partition / namespace with
 * user wifi config — strings duplicated here to avoid a header dependency). */
#define LOADER_NVS_PARTITION   "user_data"
#define LOADER_NVS_NAMESPACE   "badge_cfg"
#define LOADER_NVS_KEY_SPLASH  "splash_done"

/**
 * @brief Display the loader menu and handle user input indefinitely.
 *
 * Actions that trigger a system restart (OTA update, bare-metal mode) will
 * not return.  Actions that complete and stay in the loader (WiFi reconfig,
 * "coming soon" stubs) will redraw the menu and continue.
 */
void loader_menu_run(void);

/**
 * @brief Show the one-time boot-hint info screen.
 *
 * Displays how to use RESET and the BOOT escape gesture.  Call this
 * from main before loader_menu_run() when the NVS boot_hint_done flag
 * is not set.  Blocks until the user presses any button.
 */
void loader_menu_show_boot_hint(void);

#ifdef __cplusplus
}
#endif
