/**
 * @file factory_switch.c
 * @brief Bootloader hook: handles two independent duties on every reset.
 *
 * ── Duty 1a: Single-binary factory update (v1, software resets only) ─────────
 *
 * factory_self_update.c downloads a new loader binary to the inactive OTA
 * partition, writes UPDATE_MAGIC at 0x600FFFF0, then calls esp_restart().
 * On the resulting software reset we copy the staged binary into the factory
 * partition and fall through to normal boot with otadata intact.
 *
 * ── Duty 1b: Multi-binary full-flash (v2, software resets only) ──────────────
 *
 * ota_manager_flash_micropython() downloads up to 4 binaries (bootloader,
 * partition table, app, …) into the ota_0/ota_1 raw flash area, writes a
 * small manifest header there, then sets UPDATE_MAGIC_MULTI at 0x600FFFF0
 * pointing to that header.  On the next software reset we read the header
 * from flash (no RTC-memory size-limit issues), install each binary with
 * ROM SPI functions that bypass all flash-protection checks, then trigger a
 * second software reset so the new partition table takes effect.
 *
 * ── Duty 2: Factory-escape via BOOT button (hardware resets only) ────────────
 *
 * GPIO 0 has a 470 Ω pull-up to 3.3 V and a button to GND.
 * Press RESET → release RESET → press BOOT within ~500 ms → factory boots.
 */

#include "esp_rom_sys.h"        /* esp_rom_printf, esp_rom_delay_us  */
#include "esp_rom_spiflash.h"   /* esp_rom_spiflash_erase/read/write */
#include "soc/reset_reasons.h"  /* soc_reset_reason_t                */
#include "soc/gpio_reg.h"       /* GPIO_IN_REG — also pulls in soc.h */
#include "soc/io_mux_reg.h"     /* IO_MUX_GPIO0_REG                  */
#include "soc/rtc_cntl_reg.h"   /* RTC_CNTL_OPTIONS0_REG / SW_SYS_RST */

/* OTA data partition: two 4 KB sectors at 0xF000 (matches partitions.csv). */
#define OTADATA_SECTOR_0  (0x0F000u / 4096u)
#define OTADATA_SECTOR_1  (0x10000u / 4096u)

/* Factory partition: 1.25 MB at 0x20000 (matches partitions.csv). */
#define FACTORY_ADDR  0x20000u
#define FACTORY_SIZE  (1280u * 1024u)

#define GPIO_BOOT   0

/* ── v2: flash-based detection ───────────────────────────────────── *
 * The app writes a multi_flash_hdr_t to STAGE_HDR_ADDR before restart.*
 * factory_switch reads the magic directly from flash — no RTC memory.  */
#define STAGE_HDR_ADDR  0x160000u

/* ── v1: RTC update flag ─────────────────────────────────────────── *
 * Last 16 bytes of RTC fast memory.  Survives software reset.        */
#define RTC_FLAG_ADDR      0x600FFFF0u
#define UPDATE_MAGIC       0xFA510A0Bu   /* v1 — single binary → factory */

typedef struct {
    uint32_t magic;
    uint32_t staging_offset;
    uint32_t binary_size;
    uint32_t magic_inv;
} factory_update_flag_t;

/* ── Multi-binary flash header (lives in flash, not RTC memory) ───── *
 * Written to staging_offset (e.g. 0x160000) by ota_manager before    *
 * setting UPDATE_MAGIC_MULTI.  factory_switch reads it via ROM SPI.  */
#define MULTI_HDR_MAGIC  0xFA514D4Cu   /* "FA51ML" */
#define MULTI_HDR_MAX    4

typedef struct {
    uint32_t src;   /* source flash address (staged data)  */
    uint32_t dst;   /* destination flash address           */
    uint32_t size;  /* byte count                          */
} multi_bin_entry_t;

typedef struct {
    uint32_t          magic;          /* MULTI_HDR_MAGIC               */
    uint32_t          count;          /* number of entries (1..4)      */
    multi_bin_entry_t bins[MULTI_HDR_MAX]; /* 4 × 12 = 48 bytes        */
} multi_flash_hdr_t;  /* 4+4+48 = 56 bytes, fits in one flash read     */

/* ── Shared 4 KB sector buffer ───────────────────────────────────── *
 * Must be static — stack is too small in bootloader context.         */
static uint32_t s_sector_buf[1024];

/* ── Helpers ──────────────────────────────────────────────────────── */

static void copy_region(uint32_t src, uint32_t dst, uint32_t size)
{
    uint32_t sectors = (size + 4095u) / 4096u;
    for (uint32_t s = 0; s < sectors; s++) {
        esp_rom_spiflash_erase_sector((dst / 4096u) + s);
    }
    for (uint32_t off = 0; off < size; off += 4096u) {
        uint32_t chunk   = size - off;
        if (chunk > 4096u) chunk = 4096u;
        uint32_t aligned = (chunk + 3u) & ~3u;
        esp_rom_spiflash_read (src + off, s_sector_buf, (int32_t)aligned);
        esp_rom_spiflash_write(dst + off, s_sector_buf, (int32_t)aligned);
    }
}

/* ── Duty 1a: single-binary update ───────────────────────────────── */

static void apply_factory_update(uint32_t staging_offset, uint32_t binary_size)
{
    if (binary_size == 0 || binary_size > FACTORY_SIZE) {
        esp_rom_printf("[factory_switch] v1: invalid binary_size %u\n",
                       (unsigned)binary_size);
        return;
    }
    esp_rom_printf("[factory_switch] v1: copying %u B  0x%05x -> 0x%05x\n",
                   (unsigned)binary_size,
                   (unsigned)staging_offset,
                   (unsigned)FACTORY_ADDR);
    copy_region(staging_offset, FACTORY_ADDR, binary_size);
    esp_rom_printf("[factory_switch] v1: done\n");
}

/* ── Duty 1b: multi-binary full-flash ────────────────────────────── */

static void apply_multi_flash(uint32_t hdr_addr)
{
    /* Disable the RTC WDT (including flashboot mode) and the super-WDT so the
     * long flash copy (~20 s for a 1.6 MB binary) does not trigger a reset. */
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0x50D83AA1u);          /* unlock */
    REG_CLR_BIT(RTC_CNTL_WDTCONFIG0_REG, RTC_CNTL_WDT_FLASHBOOT_MOD_EN);
    REG_CLR_BIT(RTC_CNTL_WDTCONFIG0_REG, RTC_CNTL_WDT_EN);
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);                     /* re-lock */
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, RTC_CNTL_SWD_WKEY_VALUE); /* unlock */
    REG_SET_BIT(RTC_CNTL_SWD_CONF_REG, RTC_CNTL_SWD_DISABLE);
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0);                    /* re-lock */

    /* Read the header from flash. */
    multi_flash_hdr_t hdr;
    esp_rom_spiflash_read(hdr_addr, (uint32_t *)&hdr, sizeof(hdr));

    if (hdr.magic != MULTI_HDR_MAGIC || hdr.count == 0 || hdr.count > MULTI_HDR_MAX) {
        esp_rom_printf("[factory_switch] v2: bad header magic=%08x count=%u\n",
                       (unsigned)hdr.magic, (unsigned)hdr.count);
        return;
    }

    /* Clear the magic now (flip 1s→0s, no erase needed) so an unexpected
     * reset mid-copy won't re-run this operation on the next boot. */
    uint32_t zero = 0u;
    esp_rom_spiflash_write(hdr_addr, &zero, sizeof(zero));

    esp_rom_printf("[factory_switch] v2: %u binaries to install\n",
                   (unsigned)hdr.count);

    for (uint32_t b = 0; b < hdr.count; b++) {
        uint32_t src  = hdr.bins[b].src;
        uint32_t dst  = hdr.bins[b].dst;
        uint32_t size = hdr.bins[b].size;
        if (size == 0) continue;
        esp_rom_printf("[factory_switch] v2[%u]: 0x%05x -> 0x%05x  %u B\n",
                       (unsigned)b, (unsigned)src, (unsigned)dst, (unsigned)size);
        copy_region(src, dst, size);
        esp_rom_printf("[factory_switch] v2[%u]: done\n", (unsigned)b);
    }

    esp_rom_printf("[factory_switch] v2: all done — cleaning VFS area\n");

    /* Erase the flash area above the installed partitions so MicroPython
     * gets a clean filesystem on first boot.  The factory partition ends at
     * 0x200000; MicroPython uses the space from there to 0x3DFFFF as VFS.
     * Preserve user_data at 0x3E0000 (badge WiFi credentials).            */
    for (uint32_t addr = 0x200000u; addr < 0x3E0000u; addr += 4096u) {
        esp_rom_spiflash_erase_sector(addr / 4096u);
    }
    esp_rom_printf("[factory_switch] v2: VFS clean — second reboot\n");

    /* Trigger a second software reset so the new partition table takes effect. */
    REG_WRITE(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST_M);
    while (1) { ; }
}

/* ── Bootloader hook ──────────────────────────────────────────────── */

void bootloader_after_init(void)
{
    /* ── v2: multi-binary full-flash (flash-based, no RTC memory) ───── *
     * Read the magic word directly from the staging header sector.       *
     * apply_multi_flash clears it immediately after reading the header,  *
     * so an unexpected mid-copy reset will not re-trigger this path.    */
    uint32_t flash_magic;
    esp_rom_spiflash_read(STAGE_HDR_ADDR, &flash_magic, sizeof(flash_magic));
    if (flash_magic == MULTI_HDR_MAGIC) {
        esp_rom_printf("[factory_switch] v2 header found in flash\n");
        apply_multi_flash(STAGE_HDR_ADDR);
        while (1) { ; }  /* unreachable — apply_multi_flash resets chip */
    }

    /* ── v1: single-binary factory update (RTC-based) ───────────────── */
    volatile factory_update_flag_t *flag =
            (volatile factory_update_flag_t *)RTC_FLAG_ADDR;

    if (flag->magic == UPDATE_MAGIC &&
               flag->magic_inv == ~UPDATE_MAGIC) {

        esp_rom_printf("[factory_switch] v1 flag valid\n");
        uint32_t staging_offset = flag->staging_offset;
        uint32_t binary_size    = flag->binary_size;
        flag->magic     = 0u;
        flag->magic_inv = 0u;
        apply_factory_update(staging_offset, binary_size);
    }

    /* ── Check reset reason ───────────────────────────────────────── */
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    esp_rom_printf("[factory_switch] reset reason = %d\n", (int)reason);

    if (reason == RESET_REASON_CORE_SW || reason == RESET_REASON_CPU0_SW) {
        esp_rom_printf("[factory_switch] software reset — leaving otadata intact\n");
        return;
    }

    /* ── Hardware reset: poll BOOT button for ~500 ms ──────────────── *
     * GPIO 0 has a 470 Ω external pull-up — reads reliably without any
     * settle delay.  MCU_SEL=1 (GPIO matrix), FUN_IE=1 (input enable). *
     * A+B combo was removed: internal pull-ups on GPIO 33/34 are too   *
     * weak to overcome post-reset floating state, causing false trips.  */
    REG_WRITE(IO_MUX_GPIO0_REG, (1u << 12) | (1u << 9));

    bool boot_pressed = false;
    for (int i = 0; i < 50; i++) {        /* 50 × 10 ms = 500 ms */
        esp_rom_delay_us(10000);
        if (!(REG_READ(GPIO_IN_REG) & BIT(GPIO_BOOT))) {
            boot_pressed = true;
            break;
        }
    }

    if (boot_pressed) {
        esp_rom_spiflash_erase_sector(OTADATA_SECTOR_0);
        esp_rom_spiflash_erase_sector(OTADATA_SECTOR_1);
    }
}
