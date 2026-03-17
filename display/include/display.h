/**
 * @file display.h
 * @brief ILI9341 TFT display driver — SPI2, landscape 320×240.
 *
 * Provides display initialisation, fill, and text rendering.
 * Two built-in 8×8 bitmap fonts are available:
 *   DISPLAY_FONT_MONO  — classic monospace (original)
 *   DISPLAY_FONT_SANS  — clean sans-serif
 *
 * Quick usage:
 *   display_text_ctx_t ctx = DISPLAY_CTX(DISPLAY_FONT_SANS, 2,
 *                                        DISPLAY_COLOR_WHITE,
 *                                        DISPLAY_COLOR_BLACK);
 *   display_print(&ctx, 10, 10, "Hello!");
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Display dimensions (landscape) ─────────────────────────────── */
#define DISPLAY_W  320
#define DISPLAY_H  240

/* ── Font cell size ──────────────────────────────────────────────── */
#define DISPLAY_FONT_W  8
#define DISPLAY_FONT_H  8

/* ── Colour helpers ──────────────────────────────────────────────── */
/** Pack r,g,b (0-255) into RGB565. */
#define DISPLAY_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

#define DISPLAY_COLOR_BLACK   ((uint16_t)0x0000)
#define DISPLAY_COLOR_WHITE   ((uint16_t)0xFFFF)
#define DISPLAY_COLOR_RED     DISPLAY_RGB565(255,   0,   0)
#define DISPLAY_COLOR_GREEN   DISPLAY_RGB565(  0, 255,   0)
#define DISPLAY_COLOR_BLUE    DISPLAY_RGB565(  0,   0, 255)
#define DISPLAY_COLOR_YELLOW  DISPLAY_RGB565(255, 255,   0)
#define DISPLAY_COLOR_CYAN    DISPLAY_RGB565(  0, 255, 255)

/* ── Font selector ───────────────────────────────────────────────── */
typedef enum {
    DISPLAY_FONT_MONO = 0,  /**< Classic monospace (8×8, LSB-left) */
    DISPLAY_FONT_SANS = 1,  /**< Clean sans-serif  (8×8, LSB-left) */
} display_font_t;

/* ── Text rendering context ──────────────────────────────────────── */
typedef struct {
    display_font_t font;   /**< Typeface selection */
    int            scale;  /**< Integer pixel scale (1=8px, 2=16px, …) */
    uint16_t       fg;     /**< Foreground RGB565 colour */
    uint16_t       bg;     /**< Background RGB565 colour */
} display_text_ctx_t;

/** Convenience initialiser macro. */
#define DISPLAY_CTX(font_, scale_, fg_, bg_) \
    ((display_text_ctx_t){ (font_), (scale_), (fg_), (bg_) })

/**
 * @brief Initialise SPI2 bus and ILI9341 display.
 * Must be called once before any other display function.
 */
void display_init(void);

/**
 * @brief Send a raw MADCTL value (0x36) to change scan direction / rotation.
 * @param madctl  e.g. 0x00, 0x60, 0xC0, 0xA0, 0x20, 0xE0 …
 */
void display_set_madctl(uint8_t madctl);

/**
 * @brief Fill the entire screen with a solid colour.
 * @param color  RGB565 colour value (host byte-order).
 */
void display_fill(uint16_t color);

/**
 * @brief Fill a rectangular region with a solid colour.
 * @param x, y   Top-left corner (pixels).
 * @param w, h   Width and height in pixels.
 * @param color  RGB565 colour value (host byte-order).
 */
void display_fill_rect(int x, int y, int w, int h, uint16_t color);

/**
 * @brief Draw a single ASCII character at pixel position (x, y).
 *
 * @param x      Left edge of the character cell.
 * @param y      Top edge of the character cell.
 * @param c      ASCII character (0x20–0x7F).
 * @param fg     Foreground RGB565 colour.
 * @param bg     Background RGB565 colour.
 * @param scale  Integer pixel scale (1 = 8×8, 2 = 16×16, 3 = 24×24 …).
 */
void display_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);

/**
 * @brief Draw a NUL-terminated ASCII string starting at (x, y).
 * Characters are placed left-to-right with no word-wrap.
 *
 * @param x      Left edge of the first character cell.
 * @param y      Top edge of the text row.
 * @param str    NUL-terminated ASCII string.
 * @param fg     Foreground RGB565 colour.
 * @param bg     Background RGB565 colour.
 * @param scale  Integer pixel scale.
 */
void display_draw_string(int x, int y, const char *str,
                         uint16_t fg, uint16_t bg, int scale);

/**
 * @brief Print a NUL-terminated string using a text context.
 *
 * This is the preferred high-level call. It respects the font, scale,
 * foreground, and background colour stored in @p ctx.
 *
 * @param ctx  Pointer to a display_text_ctx_t describing font/colours/scale.
 * @param x    Left edge in pixels.
 * @param y    Top edge in pixels.
 * @param str  NUL-terminated ASCII string.
 */
void display_print(const display_text_ctx_t *ctx, int x, int y, const char *str);

/**
 * @brief Draw a w×h bitmap at pixel position (x, y).
 *
 * Pixels must already be in ILI9341 wire byte-order (big-endian RGB565),
 * the format produced by png_to_icon.py.  The entire window is set once;
 * rows are transmitted sequentially — no runtime conversion is needed.
 *
 * @param x       Left edge in pixels.
 * @param y       Top edge in pixels.
 * @param w       Image width in pixels.
 * @param h       Image height in pixels.
 * @param pixels  Pointer to w×h uint16_t values in wire byte-order.
 */
void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *pixels);

/**
 * @brief Write one horizontal strip of pixels at position (x, y).
 *
 * Pixels must already be in ILI9341 wire byte-order (big-endian RGB565,
 * i.e. each uint16_t byte-swapped relative to host order).  This is the
 * format produced by png_to_rgb565.py and is suitable for blitting
 * pre-converted image data directly from flash to the display with no
 * runtime conversion.
 *
 * @param x       Left edge in pixels.
 * @param y       Row in pixels.
 * @param w       Number of pixels to write.
 * @param pixels  Pointer to w uint16_t values in wire byte-order.
 */
void display_draw_row_raw(int x, int y, int w, const uint16_t *pixels);

/**
 * @brief Encode @p text as a QR code and draw it centred at (@p cx, @p cy).
 *
 * @param cx          Centre x (pixels).
 * @param cy          Centre y (pixels).
 * @param text        NUL-terminated string to encode (byte mode, ECC M).
 * @param module_px   Pixels per QR module (3–4 recommended for 320×240).
 * @param fg          Dark-module colour (RGB565).
 * @param bg          Light-module / background colour (RGB565).
 * @return true if encoding succeeded and the code was drawn.
 */
bool display_draw_qr(int cx, int cy, const char *text,
                     int module_px, uint16_t fg, uint16_t bg);

#ifdef __cplusplus
}
#endif
