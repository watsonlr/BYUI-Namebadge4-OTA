/**
 * @file factory_switch.c
 * @brief Bootloader hook: hold A for 2 s at power-on to force factory boot.
 *
 * Runs inside the second-stage bootloader via bootloader_after_init().
 * Only GPIO 38 (Button A) is checked — it is a standard digital pad with
 * no LP/sleep-mode routing complications.  GPIOs 0-21 are LP I/O pads;
 * after esp_restart() they may have SLP_SEL set in IO_MUX, causing
 * GPIO_IN_REG to read 0 regardless of the actual pad state.  Avoiding
 * those pads entirely makes this hook reliable across both cold power-on
 * and warm software resets.
 *
 * If A is held LOW for HOLD_MS the OTA data partition is erased so the
 * ESP-IDF bootloader falls back to the factory app — until a new OTA
 * download re-writes the OTA data.
 */

#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_rom_spiflash.h"
#include "soc/gpio_reg.h"

#define BTN_A_GPIO   38
#define HOLD_MS      2000   /* 2 s deliberate hold required */

/* OTA data partition: two 4 KB sectors at 0xF000 (matches partitions.csv). */
#define OTADATA_SECTOR_0  (0x0F000u / 4096u)   /* sector 15 */
#define OTADATA_SECTOR_1  (0x10000u / 4096u)   /* sector 16 */

static inline int read_gpio_a(void)
{
    /* GPIO 38 is above 31, so it lives in GPIO_IN1_REG. */
    return (int)((REG_READ(GPIO_IN1_REG) >> (BTN_A_GPIO - 32)) & 1u);
}

/* Defined as weak in the bootloader — our strong definition overrides it. */
void bootloader_after_init(void)
{
    /* Release any pad hold, route through digital GPIO matrix, enable pull-up. */
    esp_rom_gpio_pad_unhold(BTN_A_GPIO);
    esp_rom_gpio_pad_select_gpio(BTN_A_GPIO);
    esp_rom_gpio_pad_pullup_only(BTN_A_GPIO);

    /* Allow pull-up to charge the line before sampling. */
    esp_rom_delay_us(10000);   /* 10 ms */

    /* Quick pre-check: if A is not held at all, return immediately. */
    if (read_gpio_a() != 0)
        return;

    for (int ms = 0; ms < HOLD_MS; ms++) {
        if (read_gpio_a() != 0)
            return;   /* A released — boot normally */
        esp_rom_delay_us(1000);
    }

    /* A held the full 2 s window — erase OTA data → factory boot. */
    esp_rom_spiflash_erase_sector(OTADATA_SECTOR_0);
    esp_rom_spiflash_erase_sector(OTADATA_SECTOR_1);
}
