# Hardware Configuration

## Target Device

| Item | Spec |
|------|------|
| MCU | ESP32-S3 QFN56 (revision v0.2) |
| Architecture | Xtensa dual-core LX7, 240 MHz |
| Flash | 4 MB embedded (XMC) |
| PSRAM | 2 MB octal PSRAM (AP_3v3) |
| RAM | 512 KB SRAM |
| Wi-Fi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | BLE 5.0 |

> **Note**: GPIO26 is reserved for the octal PSRAM interface — do not use it.

## BYUI eBadge V3.0 — Full Pinout

| GPIO | Primary Function | Notes |
|------|-----------------|-------|
| IO0 | Boot / Strapping | Hold LOW for download mode |
| IO1 | Joystick X-axis | ADC1_CH0 |
| IO2 | Joystick Y-axis | ADC1_CH1 |
| IO3 | SD Card CS | ADC1_CH2; strapping pin |
| IO4 | RGB Blue LED | PWM output |
| IO5 | RGB Green LED | PWM output |
| IO6 | RGB Red LED | PWM output |
| IO7 | Addressable LEDs (WS2813B) | RMT data line |
| IO8 | Minibadge CLK | Extension connector |
| IO9 | Display CS | SPI chip select |
| IO10 | SPI2 MISO | Shared: display + SD |
| IO11 | SPI2 MOSI | Shared: display + SD |
| IO12 | SPI2 CLK | Shared: display + SD |
| IO13 | Display DC | Data / Command select |
| IO14 | Button Left | Active LOW, pull-up |
| IO15 | Button Right | Active LOW, pull-up |
| IO16 | Button Down | Active LOW, pull-up |
| IO17 | Button Up | Active LOW, pull-up |
| IO18 | Button B | Active LOW, pull-up |
| IO19 | USB D- | Native USB |
| IO20 | USB D+ | Native USB |
| IO21 | I2C SCL | Accelerometer clock |
| IO26 | **PSRAM** | **Reserved on N4R2 — do not use** |
| IO33–37 | **SPI Flash** | **Internal — do not use** |
| IO38 | Button A | Active LOW, pull-up |
| IO39 | JTAG MTCK | Debug (exposed pad) |
| IO40 | JTAG MTDO | Debug (exposed pad) |
| IO41 | JTAG MTDI / C LED | Debug / indicator |
| IO42 | JTAG MTMS / Buzzer | Piezo PWM |
| IO43 | UART0 TX | Console output |
| IO44 | UART0 RX | Console input |
| IO45 | Strapping | Boot config |
| IO46 | Strapping | Boot config |
| IO47 | I2C SDA | Accelerometer data |
| IO48 | Display RST | Display reset |

### Strapping / Reserved Pins

| Pin | Function | Required State |
|-----|----------|---------------|
| IO0 | Boot mode | HIGH = normal boot, LOW = download mode |
| IO3 | SD CS / Strap | Has strapping function |
| IO45 | Boot config | Check module datasheet |
| IO46 | Boot config | Check module datasheet |
| IO26 | PSRAM (N4R2) | **Do not use on N8** |
| IO33–37 | Internal flash | **Never use** |

---

## Peripheral Reference

### Display — ILI9341 TFT LCD

| Signal | GPIO |
|--------|------|
| CS | 9 |
| DC (Data/Cmd) | 13 |
| RST | 48 |
| CLK (SPI2) | 12 |
| MOSI (SPI2) | 11 |
| MISO (SPI2) | 10 |

- Resolution: 240 × 320 pixels (native portrait), mounted landscape with FPC connector on the left
- Orientation: MADCTL register `0x36` = `0x40` (MX bit only) — produces correct landscape orientation with the physical FPC-left mounting
- Display inversion: send `0x21` (INVON) during init — this panel powers up inverted by default; INVON is required for correct colours
- Color format: RGB565 (16-bit), big-endian byte order (high byte first) over SPI; confirmed working: red `0xF800`, green `0x07E0`, blue `0x001F`
- SPI clock: up to 40 MHz, Mode 0 (CPOL=0, CPHA=0), MSB first
- **SPI2 bus is shared with SD card; manage CS lines carefully**

### SD Card — TF Push (SPI, shared bus)

| Signal | GPIO |
|--------|------|
| CS | 3 |
| CLK (SPI2) | 12 |
| MOSI (SPI2) | 11 |
| MISO (SPI2) | 10 |

### Buttons

| Button | GPIO | Logic |
|--------|------|-------|
| Up | 17 | Active LOW (internal pull-up) |
| Down | 16 | Active LOW |
| Left | 14 | Active LOW |
| Right | 15 | Active LOW |
| A | 38 | Active LOW |
| B | 18 | Active LOW |
| Boot / Reset | 0 | Active LOW (strapping) |

### RGB LED (RS-3535MWAM)

| Channel | GPIO |
|---------|------|
| Red | 6 |
| Green | 5 |
| Blue | 4 |

> Check your board schematic for common-anode vs common-cathode wiring.

### Addressable LEDs — WS2813B-2121

| Signal | GPIO |
|--------|------|
| Data | 7 |

Use the ESP-IDF `led_strip` component (RMT-based). Compatible with WS2812/NeoPixel libraries.

### Buzzer — MLT-5020 Piezo

| Signal | GPIO |
|--------|------|
| Drive | 42 |

Drive with a LEDC PWM channel or GPIO toggle for simple tones. [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2410121451_Jiangsu-Huaneng-Elec-MLT-5020_C94598.pdf)

### Accelerometer — MMA8452Q

| Signal | GPIO |
|--------|------|
| SDA | 47 |
| SCL | 21 |

- I2C address: 0x1C or 0x1D (SA0 pin selectable)
- [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2405281404_NXP-Semicon-MMA8452QR1_C11360.pdf)

### Joystick — Adafruit 2765

| Axis / Signal | GPIO | Note |
|---------------|------|------|
| X-axis | 1 | ADC1_CH0 (analog) |
| Y-axis | 2 | ADC1_CH1 (analog) |

### USB-Serial Bridge — CP2102N-A02-GQFN28R

| Signal | GPIO |
|--------|------|
| TX | 43 |
| RX | 44 |

Baud: 115200, 8N1. [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2304140030_SKYWORKS-SILICON-LABS-CP2102N-A02-GQFN28R_C964632.pdf)

### Charging — TP4056

Single-cell Li-ion/LiPo charger (4.2 V max). Charge current set by RPROG resistor. [Datasheet](https://www.lcsc.com/datasheet/lcsc_datasheet_1809261820_TOPPOWER-Nanjing-Extension-Microelectronics-TP4056-42-ESOP8_C16581.pdf)

### Wi-Fi

- SoftAP SSID (provisioning): `BYUI_NameBadge`
- Channel: 6 (2.4 GHz only)
- Provisioning: open; client: WPA2-PSK

---

## Power Requirements

| Mode | Typical Current |
|------|----------------|
| Active Wi-Fi TX | 120–140 mA |
| Active Wi-Fi RX | 80–95 mA |
| Modem sleep | 15–20 mA |
| Light sleep | ~0.8 mA |
| Deep sleep | ~5 µA |

Supply: 3.0–3.6 V, ≥500 mA recommended.

---

## Flash Layout

See `partitions.csv` for the full partition table.

| Partition | Type | Offset | Size |
|-----------|------|--------|------|
| nvs | data/nvs | 0x9000 | 24 KB |
| otadata | data/ota | 0xF000 | 8 KB |
| phy_init | data/phy | 0x11000 | 4 KB |
| factory | app/factory | 0x20000 | 960 KB |
| ota_0 | app/ota_0 | 0x110000 | 960 KB |
| ota_1 | app/ota_1 | 0x200000 | 960 KB |
| ota_2 | app/ota_2 | 0x2F0000 | 960 KB |

---

## Component Datasheets

| Component | Part | Link |
|-----------|------|------|
| MCU | ESP32-S3-Mini-1-N8 | [Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3-mini-1_mini-1u_datasheet_en.pdf) |
| Accelerometer | MMA8452QR1 | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2405281404_NXP-Semicon-MMA8452QR1_C11360.pdf) |
| Buzzer | MLT-5020 | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2410121451_Jiangsu-Huaneng-Elec-MLT-5020_C94598.pdf) |
| Charger | TP4056 | [Datasheet](https://www.lcsc.com/datasheet/lcsc_datasheet_1809261820_TOPPOWER-Nanjing-Extension-Microelectronics-TP4056-42-ESOP8_C16581.pdf) |
| Joystick | Adafruit 2765 | [Product page](https://www.adafruit.com/product/2765) |
| RGB LED | RS-3535MWAM | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2410121728_Foshan-NationStar-Optoelectronics-RS-3535MWAM_C842778.pdf) |
| SD Card socket | TF PUSH | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2504101957_SHOU-HAN-TF-PUSH_C393941.pdf) |
| USB-Serial | CP2102N-A02-GQFN28R | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2304140030_SKYWORKS-SILICON-LABS-CP2102N-A02-GQFN28R_C964632.pdf) |
| Addressable LEDs | WS2813B-2121 | [Product page](https://item.szlcsc.com/datasheet/WS2813B-2121/23859548.html) |

---

## Troubleshooting

| Symptom | Common Cause | Fix |
|---------|-------------|-----|
| Device not detected | Charge-only USB cable | Use a data USB cable |
| `python\r: No such…` | CRLF line endings in WSL | `.gitattributes` enforces LF; run `git reset --hard` |
| Flash failure | Baud rate too high | Add `-b 115200` to `idf.py flash` |
| Wi-Fi not working | 5 GHz network | Switch to 2.4 GHz |
| Brownout / resets | Weak power supply | Add bulk cap; check >500 mA supply |

---

**Last Updated**: March 2026
