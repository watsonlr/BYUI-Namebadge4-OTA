/**
 * @file splash_screen.h
 * @brief Scroll-up splash screen for the BYUI eBadge ILI9341 display.
 *
 * Call splash_screen_run() once near the start of app_main().
 * It initialises SPI2 + the ILI9341, plays the scroll-up animation,
 * holds the image briefly, then returns.
 *
 * Before building, run png_to_rgb565.py to generate image_rgb565.h
 * and place that file in this component directory.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the splash-screen animation to completion.
 *
 * Requires display_init() to have been called first.
 * Clears the screen to black, then reveals the image row-by-row from
 * the bottom upward (scroll-up effect).  Holds the completed image
 * for SPLASH_HOLD_MS then returns.  The display remains active.
 */
void splash_screen_run(void);

#ifdef __cplusplus
}
#endif
