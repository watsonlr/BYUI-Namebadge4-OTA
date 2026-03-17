/**
 * @file splash_screen.c
 * @brief Scroll-up splash screen using the display component.
 *
 * Requires display_init() to have been called before splash_screen_run().
 * Pixels in image_rgb565.h are pre-byte-swapped by png_to_rgb565.py and
 * are sent directly to the ILI9341 via display_draw_row_raw().
 */

#include "splash_screen.h"
#include "display.h"
#include "buttons.h"
#include "image_rgb565.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* Delay between revealing each row (ms).  4 ms → ~1 s total scroll. */
#define SCROLL_ROW_DELAY_MS  4
/* How long to hold the completed image before returning (ms). */
#define SPLASH_HOLD_MS       2000

static const char *TAG = "splash";

void splash_screen_run(void)
{
    display_fill(DISPLAY_COLOR_BLACK);

    ESP_LOGI(TAG, "Scroll-up animation (%d rows, %d ms/row)...",
             SPLASH_IMAGE_H, SCROLL_ROW_DELAY_MS);

    /* Reveal from the bottom row upward — each row appears at its final
     * screen position so the image scrolls up into view. */
    for (int row = SPLASH_IMAGE_H - 1; row >= 0; row--) {
        display_draw_row_raw(0, row, SPLASH_IMAGE_W,
                             splash_image + (size_t)row * SPLASH_IMAGE_W);
        if (SCROLL_ROW_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(SCROLL_ROW_DELAY_MS));
        }
    }

    ESP_LOGI(TAG, "Splash complete, holding %d ms.", SPLASH_HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(SPLASH_HOLD_MS));

    /* Drain any buttons held during the animation so the next
     * buttons_wait_press() call starts from a clean released state.
     * Bounded to avoid blocking forever if a button is physically stuck. */
    for (int t = 0; t < 300 && buttons_read() != BTN_NONE; t += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
