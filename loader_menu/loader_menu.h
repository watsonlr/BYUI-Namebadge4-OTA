#pragma once

/* Version defines — kept in sync with loader_menu/include/loader_menu.h */
#ifndef LOADER_HW_VERSION
#define LOADER_HW_VERSION   4
#define LOADER_SW_VERSION   6
#endif

/* NVS key for the splash-shown flag (user_data partition, badge_cfg namespace). */
#define LOADER_NVS_PARTITION   "user_data"
#define LOADER_NVS_NAMESPACE   "badge_cfg"
#define LOADER_NVS_KEY_SPLASH  "splash_done"

void action_reset_wifi_config(void);
void action_reset_board_factory(void);
void loader_menu_show_boot_hint(void);
