/**
 * @file factory_switch.c
 * @brief Bootloader hook: erase otadata on hardware reset so the factory app
 *        always runs first; let the factory app decide whether to redirect to
 *        the OTA partition.
 *
 * Runs inside the second-stage bootloader via bootloader_after_init().
 *
 * Strategy (GPIO reading is unreliable in bootloader context on hardware
 * reset — GPIO_IN1_REG returns 0 regardless of physical pad state):
 *
 *   • Software reset (esp_restart()): skip entirely.  The factory app or OTA
 *     app triggered this restart intentionally (e.g. after OTA download or
 *     WiFi-fail reconfigure); let the bootloader use whatever otadata is set.
 *
 *   • Hardware reset (power-on, RESET button, watchdog): unconditionally erase
 *     otadata.  This forces the bootloader to fall back to the factory
 *     partition.  The factory app then checks:
 *       – Is there a valid image in ota_0?
 *       – Are A+B held?
 *     If a valid OTA image exists and A+B are NOT held, the factory app calls
 *     esp_ota_set_boot_partition(ota_0) + esp_restart() (software reset) to
 *     redirect execution to the OTA app.  A+B held → stay in factory menu.
 */

#include "esp_rom_sys.h"        /* esp_rom_printf, esp_rom_delay_us, esp_rom_get_reset_reason */
#include "esp_rom_spiflash.h"
#include "soc/reset_reasons.h"  /* soc_reset_reason_t, RESET_REASON_CORE_SW, etc. */

/* OTA data partition: two 4 KB sectors at 0xF000 (matches partitions.csv). */
#define OTADATA_SECTOR_0  (0x0F000u / 4096u)   /* sector 15 */
#define OTADATA_SECTOR_1  (0x10000u / 4096u)   /* sector 16 */

/* Defined as weak in the bootloader — our strong definition overrides it. */
void bootloader_after_init(void)
{
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    esp_rom_printf("[factory_switch] reset reason = %d\n", (int)reason);

    /* Software reset: the factory app or OTA app called esp_restart() with
     * otadata already pointed at the intended partition.  Leave it alone. */
    if (reason == RESET_REASON_CORE_SW || reason == RESET_REASON_CPU0_SW) {
        esp_rom_printf("[factory_switch] software reset — leaving otadata intact\n");
        return;
    }

    /* Hardware reset (power-on, RESET button, watchdog, etc.):
     * Erase otadata unconditionally so the bootloader boots factory.
     * The factory app will redirect to the OTA partition if appropriate. */
    esp_rom_printf("[factory_switch] hardware reset — erasing otadata\n");
    esp_rom_spiflash_erase_sector(OTADATA_SECTOR_0);
    esp_rom_spiflash_erase_sector(OTADATA_SECTOR_1);
}
