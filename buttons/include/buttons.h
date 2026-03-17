void buttons_flush_events(void);
/**
 * @file buttons.h
 * @brief Polled GPIO driver for all six badge buttons.
 *
 * All buttons are active-LOW with internal pull-ups.
 *
 *   GPIO 17 — Up       GPIO 16 — Down
 *   GPIO 14 — Left     GPIO 15 — Right
 *   GPIO 38 — A        GPIO 18 — B
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bitmask constants — may be OR-ed together. */
#define BTN_NONE   0x00u
#define BTN_UP     0x01u
#define BTN_DOWN   0x02u
#define BTN_LEFT   0x04u
#define BTN_RIGHT  0x08u
#define BTN_A      0x10u
#define BTN_B      0x20u

typedef uint8_t button_t;

/**
 * @brief Configure all six button GPIOs as inputs with pull-ups.
 * Must be called once before any other buttons_* function.
 */
void buttons_init(void);

/**
 * @brief Return a bitmask of buttons currently pressed (no debounce).
 */
button_t buttons_read(void);

/**
 * @brief Block until all buttons in @p mask have been held simultaneously
 *        for @p duration_ms, polling every 20 ms.
 *
 * Returns true if the combo was held for the full duration.
 * Returns false immediately if any button in the mask is released.
 * Intended for the boot-time A+B check before the display is initialised.
 */
bool buttons_held(button_t mask, uint32_t duration_ms);

/**
 * @brief Block until a button press event is detected, then return it.
 *
 * A "press event" is a button that transitions from released → pressed and
 * stays pressed for the debounce period.  The function waits for the button
 * to be released before returning so that the caller does not see the same
 * press twice.
 *
 * @param timeout_ms  Maximum wait in ms.  Pass 0 to wait indefinitely.
 * @return Bitmask of the button(s) that triggered the event, or BTN_NONE on
 *         timeout.
 */

/**
 * @brief Wait for a specific button to be pressed (semaphore-based).
 * @param btn         Button bitmask (BTN_UP, BTN_DOWN, etc.)
 * @param timeout_ms  Maximum wait in ms.  Pass 0 to wait indefinitely.
 * @return true if pressed, false on timeout.
 */
bool buttons_wait_button(button_t btn, uint32_t timeout_ms);

/**
 * @brief Wait for any button event (queue-based).
 * @param timeout_ms  Maximum wait in ms.  Pass 0 to wait indefinitely.
 * @return Bitmask of the button pressed, or BTN_NONE on timeout.
 */
button_t buttons_wait_event(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
