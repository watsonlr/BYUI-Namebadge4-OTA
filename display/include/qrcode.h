/**
 * @file qrcode.h
 * @brief Minimal QR Code encoder (Version 1–10, byte mode).
 *
 * Derived from Project Nayuki's QR Code generator, MIT License.
 * https://github.com/nayuki/QR-Code-generator
 *
 * Stripped to the minimum needed for short URLs on an embedded system.
 * Only byte-mode encoding, error correction level M.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum version supported (version 10 → 57×57 modules). */
#define QR_MAX_VERSION 10

/** Buffer size needed to hold one QR bitmap (worst case v10). */
#define QR_BUFFER_LEN_MAX  ((((QR_MAX_VERSION * 4 + 17) * (QR_MAX_VERSION * 4 + 17) + 7) / 8) + 1)

/**
 * @brief Encode a NUL-terminated byte string into a QR Code bitmap.
 *
 * @param text      Input string (treated as raw bytes).
 * @param qr_buf    Output buffer, at least QR_BUFFER_LEN_MAX bytes.
 * @param[out] size Module count per side (e.g. 21 for version 1).
 * @return true on success.
 */
bool qr_encode(const char *text, uint8_t *qr_buf, int *size);

/**
 * @brief Query a module (pixel) in the QR bitmap.
 *
 * @param qr_buf  Buffer filled by qr_encode().
 * @param size    Module count from qr_encode().
 * @param x       Column (0 = left).
 * @param y       Row    (0 = top).
 * @return true if the module is dark.
 */
bool qr_get_module(const uint8_t *qr_buf, int size, int x, int y);

#ifdef __cplusplus
}
#endif
