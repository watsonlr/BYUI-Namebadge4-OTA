/**
 * @file factory_switch.c
 * @brief Bootloader hook: handles two independent duties on every reset.
 *
 * ── Duty 1: Apply a staged factory-loader update (software resets only) ──
 *
 * factory_self_update.c (app context) downloads a new loader binary to the
 * inactive OTA partition, writes a magic flag to the last 16 bytes of RTC
 * slow memory (0x600FFFF0), then calls esp_restart().  On the resulting
 * software reset we detect the flag here and copy the staged binary
 * sector-by-sector into the factory partition, then clear the flag.
 * otadata is left intact so the student app is preserved.
 *
 * ── Duty 2: Factory-escape via BOOT button (hardware resets only) ────────
 *
 * GPIO 0 is shared with Display CS but also has a 470 Ω pull-up to 3.3 V
 * and a button to GND.  No debounce capacitor — reads are instant and
 * reliable in bootloader context without any settling delay.
 * display_init() has not run yet, so reading GPIO 0 as a plain input is safe.
 *
 * User gesture for factory escape:
 *   Press RESET → release RESET (ROM reads GPIO 0 HIGH → normal boot) →
 *   press BOOT within ~500 ms → factory app boots.
 *
 * Plain reset (no BOOT press): OTA app boots directly — single boot, fast.
 * Software reset: otadata left intact (the calling app already set it).
 */

#include "esp_rom_sys.h"        /* esp_rom_printf, esp_rom_delay_us  */
#include "esp_rom_spiflash.h"   /* esp_rom_spiflash_erase/read/write */
#include "soc/reset_reasons.h"  /* soc_reset_reason_t                */
#include "soc/gpio_reg.h"       /* GPIO_IN_REG                       */
#include "soc/io_mux_reg.h"     /* IO_MUX_GPIO0_REG                  */

/* OTA data partition: two 4 KB sectors at 0xF000 (matches partitions.csv). */
#define OTADATA_SECTOR_0  (0x0F000u / 4096u)   /* sector 15 */
#define OTADATA_SECTOR_1  (0x10000u / 4096u)   /* sector 16 */

/* Factory partition: 1.25 MB at 0x20000 (matches partitions.csv). */
#define FACTORY_ADDR      0x20000u
#define FACTORY_SIZE      (1280u * 1024u)       /* 1 310 720 bytes = 320 sectors */

/* BOOT button is on GPIO 0. */
#define GPIO_BOOT  0

/* ── RTC update flag ─────────────────────────────────────────────── *
 * Occupies the last 16 bytes of RTC slow DRAM (0x600FE000, 8 KB).
 * Survives software reset; cleared on hardware reset.
 * Written by factory_self_update.c in app context.              */
#define RTC_FLAG_ADDR    0x600FFFF0u
#define UPDATE_MAGIC     0xFA510A0Bu

typedef struct {
    uint32_t magic;           /* UPDATE_MAGIC                      */
    uint32_t staging_offset;  /* flash byte address of staging data */
    uint32_t binary_size;     /* byte count of staged loader binary */
    uint32_t magic_inv;       /* ~magic, second validity check      */
} factory_update_flag_t;

/* ── Apply staged factory update ──────────────────────────────────── *
 * Runs in bootloader context (IRAM + ROM only).
 * Uses a 4 KB static buffer — cannot be on the stack.            */
static uint32_t s_sector_buf[1024];   /* 4 096 bytes, word-aligned */

static void apply_factory_update(uint32_t staging_offset, uint32_t binary_size)
{
    if (binary_size == 0 || binary_size > FACTORY_SIZE) {
        esp_rom_printf("[factory_switch] invalid binary_size %u — aborting update\n",
                       (unsigned)binary_size);
        return;
    }

    uint32_t sectors = (binary_size + 4095u) / 4096u;
    esp_rom_printf("[factory_switch] erasing %u factory sectors (@ 0x%05x)...\n",
                   (unsigned)sectors, (unsigned)FACTORY_ADDR);

    for (uint32_t s = 0; s < sectors; s++) {
        esp_rom_spiflash_erase_sector((FACTORY_ADDR / 4096u) + s);
    }

    esp_rom_printf("[factory_switch] copying %u B from 0x%08x to 0x%08x...\n",
                   (unsigned)binary_size,
                   (unsigned)staging_offset,
                   (unsigned)FACTORY_ADDR);

    for (uint32_t off = 0; off < binary_size; off += 4096u) {
        /* Round chunk up to a word boundary for ROM SPI functions. */
        uint32_t chunk = binary_size - off;
        if (chunk > 4096u) chunk = 4096u;
        uint32_t aligned = (chunk + 3u) & ~3u;

        esp_rom_spiflash_read(staging_offset + off, s_sector_buf, (int32_t)aligned);
        esp_rom_spiflash_write(FACTORY_ADDR  + off, s_sector_buf, (int32_t)aligned);
    }

    esp_rom_printf("[factory_switch] factory update applied successfully\n");
}

/* ── Bootloader hook (overrides weak symbol in ESP-IDF bootloader) ─── */

void bootloader_after_init(void)
{
    /* ── Check for a pending factory update ───────────────────────── */
    volatile factory_update_flag_t *flag =
            (volatile factory_update_flag_t *)RTC_FLAG_ADDR;

    if (flag->magic == UPDATE_MAGIC && flag->magic_inv == ~UPDATE_MAGIC) {
        esp_rom_printf("[factory_switch] RTC update flag valid — applying factory update\n");

        uint32_t staging_offset = flag->staging_offset;
        uint32_t binary_size    = flag->binary_size;

        /* Disarm the flag immediately so a crash mid-update doesn't loop. */
        flag->magic     = 0u;
        flag->magic_inv = 0u;

        apply_factory_update(staging_offset, binary_size);
        /* Fall through: check reset reason and let boot proceed normally.
         * The calling app used esp_restart() (software reset), so the
         * reset-reason check below will leave otadata intact and the
         * student app will continue to run. */
    }

    /* ── Check reset reason ───────────────────────────────────────── */
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    esp_rom_printf("[factory_switch] reset reason = %d\n", (int)reason);

    /* Software reset: the calling app set otadata to point at the intended
     * partition (or left it alone).  Leave otadata intact. */
    if (reason == RESET_REASON_CORE_SW || reason == RESET_REASON_CPU0_SW) {
        esp_rom_printf("[factory_switch] software reset — leaving otadata intact\n");
        return;
    }

    /* ── Hardware reset: poll BOOT button for ~500 ms ─────────────── *
     * Configure GPIO 0: MCU_SEL=1 (GPIO function, bits 14:12),
     * FUN_IE=1 (input enable, bit 9).
     * External 470 Ω pull-up holds the pin HIGH; BOOT press pulls LOW. */
    REG_WRITE(IO_MUX_GPIO0_REG, (1u << 12) | (1u << 9));

    bool boot_pressed = false;
    for (int i = 0; i < 50; i++) {
        esp_rom_delay_us(10000);                        /* 10 ms per poll */
        if (!(REG_READ(GPIO_IN_REG) & BIT(GPIO_BOOT))) {
            boot_pressed = true;
            break;
        }
    }

    if (boot_pressed) {
        esp_rom_printf("[factory_switch] BOOT pressed — erasing otadata\n");
        esp_rom_spiflash_erase_sector(OTADATA_SECTOR_0);
        esp_rom_spiflash_erase_sector(OTADATA_SECTOR_1);
    } else {
        esp_rom_printf("[factory_switch] BOOT not pressed — leaving otadata intact\n");
    }
}
