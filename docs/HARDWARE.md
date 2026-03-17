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


## BYUI eBadge V4.0 — Full Pinout

| GPIO | Primary Function | Notes |
|------|-----------------|-------|
| 0 | Display CS (SPI2) | |
| 1 | Display Reset | |
| 2 | RGB Red LED | |
| 3 | Display MOSI (SPI2) | |
| 4 | RGB Green LED | |
| 5 | RGB Blue LED | |
| 6 | Single Color LED | |
| 7 | Addressable LEDs (WS2813B) | |
| 8 | Joystick X | |
| 9 | Joystick Y | |
| 10 | Button Right | |
| 11 | Button Up | |
| 12 | Battery Voltage Indicator | |
| 13 | Minibadge A CLK | Expansion slot |
| 14 | Minibadge A IO2 | Expansion slot |
| 15 | Minibadge A IO3 | Expansion slot |
| 16 | Minibadge B CLK | Expansion slot |
| 17 | Minibadge B IO2 | Expansion slot |
| 18 | Minibadge B IO3 | Expansion slot |
| 21 | Button Left | |
| 33 | Button B | |
| 34 | Button A | |
| 35 | UART1 TX (exposed) | |
| 36 | UART1 RX (exposed) | |
| 37 | SD Card MISO (SPI3) / Exposed SPI MISO | |
| 38 | SD Card CLK (SPI3) / Exposed SPI CLK | |
| 39 | SD Card MOSI (SPI3) / Exposed SPI MOSI | |
| 40 | SD Card CS (SPI3) | |
| 41 | I2C SDA | |
| 42 | I2C SCL | |
| 43 | UART0 TX (USB bridge) | |
| 44 | UART0 RX (USB bridge) | |
| 45 | Display D/C (SPI2) | |
| 46 | Display CLK (SPI2) | |
| 47 | Button Down | |
| 48 | Buzzer | |

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


### Display — ILI9341 TFT LCD (SPI2)

| Signal | GPIO |
|--------|------|
| CS | 0 |
| DC (Data/Cmd) | 45 |
| RST | 1 |
| CLK (SPI2) | 46 |
| MOSI (SPI2) | 3 |


- Resolution: 240 × 320 pixels (native portrait), mounted landscape with FPC connector on the left
- Orientation: MADCTL register `0x36` = `0x40` (MX bit only) — produces correct landscape orientation with the physical FPC-left mounting
- Display inversion: send `0x21` (INVON) during init — this panel powers up inverted by default; INVON is required for correct colours
- Color format: RGB565 (16-bit), big-endian byte order (high byte first) over SPI; confirmed working: red `0xF800`, green `0x07E0`, blue `0x001F`
- SPI clock: up to 40 MHz, Mode 0 (CPOL=0, CPHA=0), MSB first
- **SPI2 bus is shared with SD card; manage CS lines carefully**


### SD Card — TF Push (SPI3, separate bus)

| Signal | GPIO |
|--------|------|
| CS | 40 |
| CLK (SPI3) | 38 |
| MOSI (SPI3) | 39 |
| MISO (SPI3) | 37 |

### Exposed SPI (shared with SD Card, SPI3)

| Signal | GPIO |
|--------|------|
| MISO | 37 |
| MOSI | 39 |
| CLK | 38 |



### Buttons

| Button | GPIO | Logic |
|--------|------|-------|
| Up | 11 | Active LOW (internal pull-up) |
| Down | 47 | Active LOW |
| Left | 21 | Active LOW |
| Right | 10 | Active LOW |
| A | 34 | Active LOW |
| B | 33 | Active LOW |



### RGB LED (RS-3535MWAM)

| Channel | GPIO |
|---------|------|
| Red | 2 |
| Green | 4 |
| Blue | 5 |

### Single Color LED

| Signal | GPIO |
|--------|------|
| LED | 6 |


> Check your board schematic for common-anode vs common-cathode wiring.


### Addressable LEDs — WS2813B-2121

| Signal | GPIO |
|--------|------|
| Data | 7 |


Use the ESP-IDF `led_strip` component (RMT-based). Compatible with WS2812/NeoPixel libraries.


### Buzzer — MLT-5020 Piezo

| Signal | GPIO |
|--------|------|
| Drive | 48 |


Drive with a LEDC PWM channel or GPIO toggle for simple tones. [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2410121451_Jiangsu-Huaneng-Elec-MLT-5020_C94598.pdf)


### Accelerometer — MMA8452Q

| Signal | GPIO |
|--------|------|
| SDA | 41 |
| SCL | 42 |

- I2C address: 0x1C
- [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2405281404_NXP-Semicon-MMA8452QR1_C11360.pdf)


### Joystick — Adafruit 2765

| Axis / Signal | GPIO |
|---------------|------|
| X-axis | 8 |
| Y-axis | 9 |



### UART0 (USB bridge, CP2102N)

| Signal | GPIO |
|--------|------|
| TX | 43 |
| RX | 44 |

### UART1 (Exposed UART)

| Signal | GPIO |
|--------|------|
| TX | 35 |
| RX | 36 |



### Charging — TP4056
Single-cell Li-ion/LiPo charger (4.2 V max). Charge current set by RPROG resistor. [Datasheet](https://www.lcsc.com/datasheet/lcsc_datasheet_1809261820_TOPPOWER-Nanjing-Extension-Microelectronics-TP4056-42-ESOP8_C16581.pdf)

### Battery Voltage Indicator

| Signal | GPIO |
|--------|------|
| Battery Voltage | 12 |



### Wi-Fi

- SoftAP SSID (provisioning): `BYUI_NameBadge`
- Channel: 6 (2.4 GHz only)
- Provisioning: open; client: WPA2-PSK


### Minibadge A (expansion slot)

| Signal | GPIO |
|--------|------|
| CLK | 13 |
| IO2 | 14 |
| IO3 | 15 |
| I2C | 41/42 |

### Minibadge B (expansion slot)

| Signal | GPIO |
|--------|------|
| CLK | 16 |
| IO2 | 17 |
| IO3 | 18 |
| I2C | 41/42 |


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
