/**
 * @file factory_switch.c
 * @brief Bootloader hook: on hardware reset, poll the BOOT button (GPIO 0)
 *        for 500 ms.  If pressed, erase otadata so the factory app boots.
 *        Otherwise leave otadata intact so the OTA app boots directly.
 *
 * Runs inside the second-stage bootloader via bootloader_after_init().
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

#include "esp_rom_sys.h"        /* esp_rom_printf, esp_rom_delay_us */
#include "esp_rom_spiflash.h"   /* esp_rom_spiflash_erase_sector    */
#include "soc/reset_reasons.h"  /* soc_reset_reason_t               */
#include "soc/gpio_reg.h"       /* GPIO_IN_REG                      */
#include "soc/io_mux_reg.h"     /* IO_MUX_GPIO0_REG                 */

/* OTA data partition: two 4 KB sectors at 0xF000 (matches partitions.csv). */
#define OTADATA_SECTOR_0  (0x0F000u / 4096u)   /* sector 15 */
#define OTADATA_SECTOR_1  (0x10000u / 4096u)   /* sector 16 */

/* BOOT button is on GPIO 0. */
#define GPIO_BOOT  0

/* Defined as weak in the bootloader — our strong definition overrides it. */
void bootloader_after_init(void)
{
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    esp_rom_printf("[factory_switch] reset reason = %d\n", (int)reason);

    /* Software reset: the calling app set otadata to point at the intended
     * partition.  Leave it alone. */
    if (reason == RESET_REASON_CORE_SW || reason == RESET_REASON_CPU0_SW) {
        esp_rom_printf("[factory_switch] software reset — leaving otadata intact\n");
        return;
    }

    /* Hardware reset: configure GPIO 0 as digital input.
     * MCU_SEL = 1 (GPIO function, bits 14:12), FUN_IE = 1 (input enable, bit 9).
     * External 470 Ω pull-up holds the pin HIGH; BOOT press pulls it LOW. */
    REG_WRITE(IO_MUX_GPIO0_REG, (1u << 12) | (1u << 9));

    /* Poll up to 500 ms.  ROM has already committed to normal boot (GPIO 0
     * was HIGH when EN rose).  The user can now press BOOT to request
     * factory escape. */
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
