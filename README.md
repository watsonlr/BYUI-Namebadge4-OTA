# BYUI Namebadge OTA

A **permanent, unbrickable OTA bootloader** for the BYUI eBadge V3.0 (ESP32-S3-Mini-1-N8). The loader lives in the factory partition and provides three top-level capabilities via an on-device LCD menu:

1. **Configure Device** — Wi-Fi SoftAP captive portal, accessible from any phone browser
2. **Download App** — browse and install apps from a GitHub Pages-hosted catalog over HTTPS
3. **Reset to Canvas** — wipe user apps and restore the badge to a clean, flash-ready state

See [OBJECTIVES.md](OBJECTIVES.md) for the full project specification and milestones.

## Hardware Overview

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3 QFN56 (dual-core Xtensa LX7, 240 MHz) |
| Flash | 4 MB embedded (XMC) |
| RAM | 512 KB SRAM + 2 MB octal PSRAM |
| Display | ILI9341 2.4" TFT LCD, 240×320, SPI2 |
| Controls | D-pad (4 buttons) + A + B buttons |
| LEDs | RGB LED (GPIO 4/5/6) + WS2813B addressable strip (GPIO 7) |
| Audio | Piezo buzzer (GPIO 42) |
| IMU | MMA8452Q accelerometer (I2C, SDA=47, SCL=21) |
| Storage | MicroSD card, SPI2 (shared with display) |
| Comms | Wi-Fi 2.4 GHz + BLE 5.0; USB-SERIAL CP2102N |

See [HARDWARE.md](HARDWARE.md) for the complete pinout and peripheral reference.

## Quick Start

### Prerequisites

- [ESP-IDF v5.3.1](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32s3/get-started/index.html) installed and sourced
- Python 3.11+
- WSL (Ubuntu) recommended on Windows — see [SETUP_GUIDE.md](SETUP_GUIDE.md)

### First Build

```bash
# Clone your new repo (after creating from template)
git clone https://github.com/<your-org>/<your-repo>.git
cd <your-repo>

# Set target and build
idf.py set-target esp32s3
idf.py build

# Flash (replace /dev/ttyUSB0 with your port)
idf.py -p /dev/ttyUSB0 flash monitor
```

On Windows with ESP-IDF PowerShell:
```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

### Configure Wi-Fi / Settings

```bash
idf.py menuconfig
# → "Project Configuration" to set Wi-Fi SSID/password and other options
```

## Project Structure

```
.
├── main/
│   ├── main.c              ← Your application entry point
│   ├── CMakeLists.txt      ← Component build config
│   └── Kconfig.projbuild   ← menuconfig options (Wi-Fi, etc.)
├── CMakeLists.txt          ← Top-level project build
├── partitions.csv          ← Flash partition layout (4 MB)
├── sdkconfig.defaults      ← Default ESP32-S3 configuration
├── HARDWARE.md             ← Complete pin reference
├── ARCHITECTURE.md         ← Boot flow and memory layout diagrams
├── SETUP_GUIDE.md          ← Environment setup instructions
├── BUILD_GUIDE.md          ← Build and flash instructions
└── QUICK_REFERENCE.md      ← Command cheat sheet
```

## Flash Partition Layout

| Partition | Type | Offset | Size | Purpose |
|-----------|------|--------|------|---------|
| nvs | data/nvs | 0x9000 | 24 KB | Wi-Fi credentials, settings |
| otadata | data/ota | 0xF000 | 8 KB | OTA boot state |
| phy_init | data/phy | 0x11000 | 4 KB | RF calibration |
| factory | app/factory | 0x20000 | 960 KB | Main application |
| ota_0 | app/ota_0 | 0x110000 | 960 KB | OTA slot 0 |
| ota_1 | app/ota_1 | 0x200000 | 960 KB | OTA slot 1 |
| ota_2 | app/ota_2 | 0x2F0000 | 960 KB | OTA slot 2 |

Total layout: 3.875 MB — fits within the 4 MB flash with 128 KB spare.

See [ARCHITECTURE.md](ARCHITECTURE.md) for boot flow diagrams.

## Key GPIO Reference

| Function | GPIO |
|----------|------|
| Display CS | 9 |
| Display DC | 13 |
| Display RST | 48 |
| SPI2 CLK | 12 |
| SPI2 MOSI | 11 |
| SPI2 MISO | 10 |
| SD Card CS | 3 |
| Button Up | 17 |
| Button Down | 16 |
| Button Left | 14 |
| Button Right | 15 |
| Button A | 38 |
| Button B | 18 |
| RGB Red | 6 |
| RGB Green | 5 |
| RGB Blue | 4 |
| Addressable LEDs | 7 |
| Buzzer | 42 |
| I2C SDA (IMU) | 47 |
| I2C SCL (IMU) | 21 |

## Documentation

| File | Contents |
|------|----------|
| [OBJECTIVES.md](OBJECTIVES.md) | Project goals, acceptance criteria, milestones |
| [HARDWARE.md](HARDWARE.md) | Full pinout, peripheral wiring, BOM, datasheets |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Boot flow, memory map, OTA flow diagrams |
| [SETUP_GUIDE.md](SETUP_GUIDE.md) | ESP-IDF install, WSL setup, serial port config |
| [BUILD_GUIDE.md](BUILD_GUIDE.md) | Build, flash, and OTA deployment steps |
| [QUICK_REFERENCE.md](QUICK_REFERENCE.md) | Essential commands at a glance |

## License

MIT — see [LICENSE](LICENSE) for details.
