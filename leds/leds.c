/**
 * @file leds.c
 * @brief WS2813B addressable LED driver using ESP-IDF RMT TX.
 *
 * WS2813B timing spec (from datasheet):
 *   T0H  300 ns (±150 ns)    T0L  900 ns (±150 ns)
 *   T1H  750 ns (±150 ns)    T1L  600 ns (±150 ns)
 *   RES  >300 µs
 *
 * RMT clock: 10 MHz → 1 tick = 100 ns
 *   T0H = 3 ticks (300 ns)   T0L = 9 ticks  (900 ns)
 *   T1H = 8 ticks (800 ns)   T1L = 6 ticks  (600 ns)
 *   RES = 3500 ticks via loop_count idle flush + 350 µs delay
 *
 * Pixel order transmitted to WS2813B: GRB (MSB first).
 */

#include "leds.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "leds"

/* RMT clock and derived tick durations. */
#define RMT_RESOLUTION_HZ   (10 * 1000 * 1000)   /* 10 MHz → 100 ns/tick */

/* Pixel buffer: GRB order, 3 bytes per LED. */
static uint8_t s_grb[LEDS_COUNT * 3];

static rmt_channel_handle_t  s_tx_chan    = NULL;
static rmt_encoder_handle_t  s_encoder   = NULL;
static bool                  s_ready     = false;

/* ── Public API ─────────────────────────────────────────────────── */

bool leds_init(void)
{
    if (s_ready) return true;

    /* ── RMT TX channel ──────────────────────────────────────────── */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num            = LEDS_GPIO,
        .clk_src             = RMT_CLK_SRC_DEFAULT,
        .resolution_hz       = RMT_RESOLUTION_HZ,
        /* 24 LEDs × 24 bits/LED = 576 symbols + reset → 1024 rounded up */
        .mem_block_symbols   = 1024,
        .trans_queue_depth   = 4,
        .flags.invert_out    = false,
        .flags.with_dma      = true,   /* DMA avoids ISR latency issues */
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return false;
    }

    /* ── Bytes encoder (WS2813B bit pattern) ─────────────────────── */
    rmt_bytes_encoder_config_t enc_cfg = {
        /* '0' bit: 300 ns high, 900 ns low */
        .bit0 = {
            .level0   = 1, .duration0 = 3,
            .level1   = 0, .duration1 = 9,
        },
        /* '1' bit: 800 ns high, 600 ns low */
        .bit1 = {
            .level0   = 1, .duration0 = 8,
            .level1   = 0, .duration1 = 6,
        },
        .flags.msb_first = true,
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return false;
    }

    err = rmt_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        s_encoder = NULL;
        return false;
    }

    memset(s_grb, 0, sizeof(s_grb));
    s_ready = true;
    ESP_LOGI(TAG, "Initialised: %d LEDs on GPIO %d", LEDS_COUNT, LEDS_GPIO);
    return true;
}

void leds_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= LEDS_COUNT) return;
    s_grb[index * 3 + 0] = g;
    s_grb[index * 3 + 1] = r;
    s_grb[index * 3 + 2] = b;
}

void leds_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LEDS_COUNT; i++) {
        s_grb[i * 3 + 0] = g;
        s_grb[i * 3 + 1] = r;
        s_grb[i * 3 + 2] = b;
    }
}

void leds_clear(void)
{
    memset(s_grb, 0, sizeof(s_grb));
}

void leds_show(void)
{
    if (!s_ready) return;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,   /* transmit once */
    };
    esp_err_t err = rmt_transmit(s_tx_chan, s_encoder,
                                  s_grb, sizeof(s_grb),
                                  &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
        return;
    }
    /* Wait for completion, then hold low ≥ 300 µs (WS2813B reset). */
    rmt_tx_wait_all_done(s_tx_chan, pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(1));   /* ≥ 300 µs reset gap */
}
