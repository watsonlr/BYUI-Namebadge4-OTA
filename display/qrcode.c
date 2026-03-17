/**
 * @file qrcode.c
 * @brief Minimal QR Code encoder — byte mode, ECC level M, versions 1-10.
 *
 * Based on the algorithm from Project Nayuki's QR Code generator (MIT).
 * https://www.nayuki.io/page/qr-code-generator-library
 *
 * Simplifications vs. the full library:
 *   - Byte encoding only (covers any URL / ASCII text)
 *   - ECC level M only
 *   - Mask patterns tried: all 8, best chosen by penalty score
 *   - Versions 1-10 (max data ~174 bytes at level M)
 */

#include "qrcode.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

/* ── Constants ───────────────────────────────────────────────────── */

/* ECC level M data / ecc codeword counts per version (v1-v10). */
static const uint8_t ECC_M_DATA_CODEWORDS[11] = {
    0, 16, 28, 44, 64, 86, 108, 124, 154, 182, 216
};
static const uint8_t ECC_M_ECC_PER_BLOCK[11] = {
    0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26
};
static const uint8_t ECC_M_BLOCKS[11] = {
    0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5
};

/* Reed-Solomon GF(256) log/antilog tables, polynomial x^8+x^4+x^3+x^2+1 */
static uint8_t s_gf_exp[512];
static uint8_t s_gf_log[256];
static bool    s_gf_init = false;

static void gf_init(void)
{
    if (s_gf_init) return;
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        s_gf_exp[i] = (uint8_t)x;
        s_gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) s_gf_exp[i] = s_gf_exp[i - 255];
    s_gf_init = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return s_gf_exp[(int)s_gf_log[a] + (int)s_gf_log[b]];
}

/* ── Bit buffer ──────────────────────────────────────────────────── */

typedef struct { uint8_t *data; int bit_len; int cap_bits; } BitBuf;

static void bb_append(BitBuf *bb, uint32_t val, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        int bi = bb->bit_len;
        if (bi >= bb->cap_bits) return;
        uint8_t bit = (val >> i) & 1;
        bb->data[bi / 8] |= bit << (7 - (bi % 8));
        bb->bit_len++;
    }
}

/* ── Reed-Solomon ────────────────────────────────────────────────── */

static void rs_generate_poly(int degree, uint8_t *poly)
{
    memset(poly, 0, (size_t)degree);
    poly[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree - 1; j++)
            poly[j] = gf_mul(poly[j], root) ^ poly[j + 1];
        poly[degree - 1] = gf_mul(poly[degree - 1], root);
        root = s_gf_exp[(int)s_gf_log[root] + 1];
    }
}

static void rs_remainder(const uint8_t *data, int data_len,
                          const uint8_t *poly, int degree,
                          uint8_t *result)
{
    memset(result, 0, (size_t)degree);
    for (int i = 0; i < data_len; i++) {
        uint8_t factor = data[i] ^ result[0];
        memmove(result, result + 1, (size_t)(degree - 1));
        result[degree - 1] = 0;
        for (int j = 0; j < degree; j++)
            result[j] ^= gf_mul(poly[j], factor);
    }
}

/* ── Module grid helpers ─────────────────────────────────────────── */

#define QR_SIZE(ver) ((ver)*4 + 17)

static inline void grid_set(uint8_t *grid, int sz, int x, int y, bool dark)
{
    int i = y * sz + x;
    if (dark) grid[i / 8] |=  (uint8_t)(0x80 >> (i % 8));
    else      grid[i / 8] &= ~(uint8_t)(0x80 >> (i % 8));
}

static inline bool grid_get(const uint8_t *grid, int sz, int x, int y)
{
    int i = y * sz + x;
    return (grid[i / 8] >> (7 - (i % 8))) & 1;
}

/* Function-module mask: 1 bit per module, same indexing */
static inline void mask_set(uint8_t *mask, int sz, int x, int y)
{
    int i = y * sz + x; mask[i / 8] |= (0x80 >> (i % 8));
}
static inline bool mask_get(const uint8_t *mask, int sz, int x, int y)
{
    int i = y * sz + x; return (mask[i / 8] >> (7 - (i % 8))) & 1;
}

/* ── Finder pattern ──────────────────────────────────────────────── */

static void draw_finder(uint8_t *grid, uint8_t *fm, int sz, int x, int y)
{
    for (int dy = -1; dy <= 7; dy++) {
        for (int dx = -1; dx <= 7; dx++) {
            int gx = x + dx, gy = y + dy;
            if (gx < 0 || gx >= sz || gy < 0 || gy >= sz) continue;
            /* Border separator always light */
            bool dark = false;
            if (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6) {
                if (dx == 0 || dx == 6 || dy == 0 || dy == 6)
                    dark = true; /* outer ring */
                else if (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4)
                    dark = true; /* inner 3×3 */
            }
            grid_set(grid, sz, gx, gy, dark);
            mask_set(fm, sz, gx, gy);
        }
    }
}

/* ── Alignment patterns (v2+) ────────────────────────────────────── */

/* Alignment pattern centres for versions 1-10 */
static const uint8_t ALIGN_POS[11][7] = {
    {0},
    {0},                          /* v1: none */
    {6, 18},                      /* v2 */
    {6, 22},                      /* v3 */
    {6, 26},                      /* v4 */
    {6, 30},                      /* v5 */
    {6, 34},                      /* v6 */
    {6, 22, 38},                  /* v7 */
    {6, 24, 42},                  /* v8 */
    {6, 26, 46},                  /* v9 */
    {6, 28, 50},                  /* v10 */
};
static const uint8_t ALIGN_COUNT[11] = {0,0,2,2,2,2,2,3,3,3,3};

static void draw_alignment(uint8_t *grid, uint8_t *fm, int sz, int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            bool dark = (abs(dx) == 2 || abs(dy) == 2 ||
                         (dx == 0 && dy == 0));
            grid_set(grid, sz, cx + dx, cy + dy, dark);
            mask_set(fm, sz, cx + dx, cy + dy);
        }
    }
}

/* ── Timing strips ───────────────────────────────────────────────── */

static void draw_timing(uint8_t *grid, uint8_t *fm, int sz)
{
    for (int i = 8; i < sz - 8; i++) {
        grid_set(grid, sz, i, 6, (i % 2) == 0);
        grid_set(grid, sz, 6, i, (i % 2) == 0);
        mask_set(fm, sz, i, 6);
        mask_set(fm, sz, 6, i);
    }
}

/* ── Dark module ─────────────────────────────────────────────────── */

static void draw_dark_module(uint8_t *grid, uint8_t *fm, int sz, int ver)
{
    (void)ver;
    grid_set(grid, sz, 8, sz - 8, true);
    mask_set(fm, sz, 8, sz - 8);
}

/* ── Format information (ECC level M = 00, mask pattern p) ───────── */

/* Format string for ECC=M (bits 14-13 = 01) and mask 0-7 */
static const uint16_t FORMAT_BITS_M[8] = {
    0x5412, 0x5125, 0x5E7C, 0x5B4B,
    0x45F9, 0x40CE, 0x4F97, 0x4AA0
};

static void draw_format(uint8_t *grid, uint8_t *fm, int sz, int mask_id)
{
    uint16_t fmt = FORMAT_BITS_M[mask_id];

    /* Primary copy: around top-left finder.
     * Bits 14(MSB)..0(LSB) placed at the positions below (per ISO 18004). */
    static const int PX[] = {0,1,2,3,4,5,7,8, 8,8,8,8,8,8,8};
    static const int PY[] = {8,8,8,8,8,8,8,8, 7,5,4,3,2,1,0};

    for (int i = 0; i < 15; i++) {
        bool bit = (fmt >> (14 - i)) & 1;

        /* Primary (top-left area) */
        grid_set(grid, sz, PX[i], PY[i], bit);
        mask_set(fm,   sz, PX[i], PY[i]);

        /* Secondary copy (ISO 18004 Table 25):
         *  bits 0-6  → column 8, rows (sz-1) down to (sz-7)  [near BL finder]
         *  bits 7-14 → row 8,    cols (sz-8)  up  to (sz-1)  [near TR finder] */
        int sx, sy;
        if (i < 7) {
            sx = 8;
            sy = sz - 1 - i;
        } else {
            sx = sz - 8 + (i - 7);
            sy = 8;
        }
        grid_set(grid, sz, sx, sy, bit);
        mask_set(fm,   sz, sx, sy);
    }
}

/* ── Data placement ──────────────────────────────────────────────── */

static void place_data(uint8_t *grid, uint8_t *fm, int sz,
                        const uint8_t *data, int data_bits)
{
    int bi = 0;
    int right = sz - 1;
    bool going_up = true;

    while (right >= 1) {
        if (right == 6) { right--; } /* skip timing col */
        for (int count = 0; count < sz; count++) {
            int y = going_up ? (sz - 1 - count) : count;
            for (int col = 0; col < 2; col++) {
                int x = right - col;
                if (mask_get(fm, sz, x, y)) continue;
                bool bit = false;
                if (bi < data_bits) {
                    bit = (data[bi / 8] >> (7 - (bi % 8))) & 1;
                    bi++;
                }
                grid_set(grid, sz, x, y, bit);
            }
        }
        going_up = !going_up;
        right -= 2;
    }
}

/* ── Mask application ────────────────────────────────────────────── */

static bool apply_mask_pattern(int pattern, int x, int y)
{
    switch (pattern) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return (x * y) % 2 + (x * y) % 3 == 0;
        case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
        case 7: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
        default: return false;
    }
}

static void do_mask(uint8_t *grid, const uint8_t *fm, int sz, int pattern)
{
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            if (mask_get(fm, sz, x, y)) continue;
            if (apply_mask_pattern(pattern, x, y))
                grid_set(grid, sz, x, y, !grid_get(grid, sz, x, y));
        }
    }
}

/* ── Penalty score ───────────────────────────────────────────────── */

static int penalty_score(const uint8_t *grid, int sz)
{
    int score = 0;
    /* Rule 1: runs of 5+ same colour in row/col */
    for (int y = 0; y < sz; y++) {
        int run = 1;
        for (int x = 1; x < sz; x++) {
            if (grid_get(grid, sz, x, y) == grid_get(grid, sz, x-1, y))
                run++;
            else { if (run >= 5) score += 3 + (run - 5); run = 1; }
        }
        if (run >= 5) score += 3 + (run - 5);
    }
    for (int x = 0; x < sz; x++) {
        int run = 1;
        for (int y = 1; y < sz; y++) {
            if (grid_get(grid, sz, x, y) == grid_get(grid, sz, x, y-1))
                run++;
            else { if (run >= 5) score += 3 + (run - 5); run = 1; }
        }
        if (run >= 5) score += 3 + (run - 5);
    }
    /* Rule 2: 2×2 blocks */
    for (int y = 0; y < sz - 1; y++)
        for (int x = 0; x < sz - 1; x++) {
            bool c = grid_get(grid, sz, x, y);
            if (c == grid_get(grid, sz, x+1, y) &&
                c == grid_get(grid, sz, x, y+1) &&
                c == grid_get(grid, sz, x+1, y+1))
                score += 3;
        }
    return score;
}

/* ── Public API ──────────────────────────────────────────────────── */

bool qr_encode(const char *text, uint8_t *qr_buf, int *out_size)
{
    gf_init();

    size_t text_len = strlen(text);

    /* Pick the smallest version that fits */
    int version = 0;
    for (int v = 1; v <= QR_MAX_VERSION; v++) {
        /* Byte mode header: 4 (mode) + 8 (char count) + 8*len data bits,
         * plus 4-bit terminator, rounded up to codeword boundary */
        int data_bits = 4 + 8 + (int)text_len * 8;
        int capacity  = ECC_M_DATA_CODEWORDS[v] * 8;
        if (data_bits + 4 <= capacity) { version = v; break; }
    }
    if (version == 0) return false; /* too long */

    int sz = QR_SIZE(version);
    int grid_bytes = (sz * sz + 7) / 8;

    /* We reuse qr_buf for both grid and function-module mask.
     * Layout: [0 .. grid_bytes-1] = grid, [grid_bytes .. 2*grid_bytes-1] = fm
     * Caller must provide QR_BUFFER_LEN_MAX bytes minimum.
     * (57*57 = 3249 bits = 407 bytes, ×2 = 814 — fits easily.) */
    uint8_t *grid = qr_buf;
    /* Allocate function-module mask from stack (v10 worst = 407 bytes) */
    uint8_t fm[((57 * 57 + 7) / 8) + 1];
    memset(grid, 0, (size_t)grid_bytes);
    memset(fm,   0, sizeof(fm));

    /* ── Draw fixed patterns ── */
    draw_finder(grid, fm, sz, 0, 0);       /* TL */
    draw_finder(grid, fm, sz, sz - 7, 0);  /* TR */
    draw_finder(grid, fm, sz, 0, sz - 7);  /* BL */
    draw_timing(grid, fm, sz);
    draw_dark_module(grid, fm, sz, version);

    for (int i = 0; i < ALIGN_COUNT[version]; i++) {
        for (int j = 0; j < ALIGN_COUNT[version]; j++) {
            int cx = ALIGN_POS[version][i];
            int cy = ALIGN_POS[version][j];
            /* Skip if centre is on timing strip row/col (always 6) */
            if (cx == 6 || cy == 6) continue;
            /* Skip if centre overlaps a finder pattern */
            if (mask_get(fm, sz, cx, cy)) continue;
            draw_alignment(grid, fm, sz, cx, cy);
        }
    }

    /* Reserve format info areas so data placement skips them.
     * Must match the exact positions written by draw_format().        */
    static const int FX[] = {0,1,2,3,4,5,7,8, 8,8,8,8,8,8,8};
    static const int FY[] = {8,8,8,8,8,8,8,8, 7,5,4,3,2,1,0};
    for (int i = 0; i < 15; i++) {
        mask_set(fm, sz, FX[i], FY[i]);          /* primary  */
        /* secondary */
        if (i < 7) mask_set(fm, sz, 8, sz - 1 - i);
        else       mask_set(fm, sz, sz - 8 + (i - 7), 8);
    }

    /* ── Build data codewords ── */
    int total_data = ECC_M_DATA_CODEWORDS[version];
    int num_blocks = ECC_M_BLOCKS[version];
    int ecc_per    = ECC_M_ECC_PER_BLOCK[version];

    uint8_t raw[256] = {0};
    BitBuf bb = { raw, 0, (int)sizeof(raw) * 8 };

    bb_append(&bb, 0x4, 4);           /* byte mode */
    bb_append(&bb, (uint32_t)text_len, 8);
    for (size_t i = 0; i < text_len; i++)
        bb_append(&bb, (uint8_t)text[i], 8);
    bb_append(&bb, 0, 4);             /* terminator */
    /* Pad to byte boundary */
    while (bb.bit_len % 8) bb_append(&bb, 0, 1);
    /* Pad codewords */
    int target_bits = total_data * 8;
    uint8_t pad_bytes[] = {0xEC, 0x11};
    int pi = 0;
    while (bb.bit_len < target_bits)
        bb_append(&bb, pad_bytes[pi++ % 2], 8);

    /* ── Reed-Solomon ── */
    int short_blocks = num_blocks - (total_data % num_blocks);
    int short_data   = total_data / num_blocks;
    /* All blocks same size for v1-10 at M level (confirm: no remainders) */
    (void)short_blocks;

    uint8_t poly[30];
    rs_generate_poly(ecc_per, poly);

    /* Interleave data */
    uint8_t final_data[400] = {0};
    int fd_len = 0;

    /* Collect block data codewords interleaved */
    for (int col = 0; col < short_data + 1; col++) {
        for (int blk = 0; blk < num_blocks; blk++) {
            int blk_len = (blk < (num_blocks - (total_data % num_blocks == 0 ? num_blocks : total_data % num_blocks)))
                          ? short_data : short_data + (total_data % num_blocks != 0 ? 1 : 0);
            /* Simpler: equal-size blocks for these versions */
            blk_len = total_data / num_blocks +
                      (blk < (total_data % num_blocks) ? 1 : 0);
            if (col < blk_len) {
                /* Offset of this block in raw[] */
                int offset = 0;
                for (int b2 = 0; b2 < blk; b2++)
                    offset += total_data / num_blocks +
                              (b2 < (total_data % num_blocks) ? 1 : 0);
                final_data[fd_len++] = raw[offset + col];
            }
        }
    }

    /* Interleave ECC */
    uint8_t ecc_all[num_blocks * ecc_per];
    int offset = 0;
    for (int blk = 0; blk < num_blocks; blk++) {
        int blk_len = total_data / num_blocks +
                      (blk < (total_data % num_blocks) ? 1 : 0);
        rs_remainder(raw + offset, blk_len, poly, ecc_per,
                     ecc_all + blk * ecc_per);
        offset += blk_len;
    }
    for (int col = 0; col < ecc_per; col++)
        for (int blk = 0; blk < num_blocks; blk++)
            final_data[fd_len++] = ecc_all[blk * ecc_per + col];

    /* ── Place data, try all masks, pick best ── */
    uint8_t best_grid[((57 * 57 + 7) / 8) + 1];
    int best_score = INT_MAX;
    int best_mask  = 0;

    for (int m = 0; m < 8; m++) {
        uint8_t tmp[((57 * 57 + 7) / 8) + 1];
        memcpy(tmp, grid, (size_t)grid_bytes);
        place_data(tmp, fm, sz, final_data, fd_len * 8);
        do_mask(tmp, fm, sz, m);
        draw_format(tmp, fm, sz, m);
        int sc = penalty_score(tmp, sz);
        if (sc < best_score) {
            best_score = sc;
            best_mask  = m;
            memcpy(best_grid, tmp, (size_t)grid_bytes);
        }
    }
    (void)best_mask;

    memcpy(qr_buf, best_grid, (size_t)grid_bytes);
    *out_size = sz;
    return true;
}

bool qr_get_module(const uint8_t *qr_buf, int size, int x, int y)
{
    int i = y * size + x;
    return (qr_buf[i / 8] >> (7 - (i % 8))) & 1;
}
