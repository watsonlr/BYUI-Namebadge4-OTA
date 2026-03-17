/**
 * @file leds.h
 * @brief WS2813B addressable LED driver for the BYUI eBadge V4.
 *
 * 24 WS2813B-2121 LEDs in a single chain, data on GPIO 7.
 * Uses the ESP-IDF RMT TX peripheral for bit-bang-free timing.
 *
 * Typical usage:
 *   leds_init();
 *   leds_fill(0, 0, 32);   // dim blue
 *   leds_show();
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** GPIO connected to the data-in of the first WS2813B. */
#define LEDS_GPIO        7

/** Number of addressable LEDs in the chain. */
#define LEDS_COUNT       24

/**
 * @brief Initialise the RMT TX channel and encoder.
 *
 * Must be called once before any other leds_* function.
 * Safe to call from app_main() before the scheduler starts.
 *
 * @return true on success, false on driver error.
 */
bool leds_init(void);

/**
 * @brief Set the RGB value for one LED (buffered, not yet visible).
 *
 * @param index  LED index 0 … LEDS_COUNT-1.
 * @param r      Red   0–255.
 * @param g      Green 0–255.
 * @param b      Blue  0–255.
 */
void leds_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set all LEDs to the same colour (buffered, not yet visible).
 */
void leds_fill(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Turn all LEDs off (buffered, not yet visible).
 */
void leds_clear(void);

/**
 * @brief Push the current buffer to the LEDs via RMT.
 *
 * Blocks until the transmission is complete (~750 µs for 24 LEDs).
 */
void leds_show(void);

#ifdef __cplusplus
}
#endif
