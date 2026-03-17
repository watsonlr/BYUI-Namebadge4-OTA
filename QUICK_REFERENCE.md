# Quick Reference

## Environment Setup

```bash
# Activate ESP-IDF (each new shell, or add to ~/.bashrc)
source ~/esp/esp-idf/export.sh

# Attach badge USB to WSL (once per session, elevated PowerShell)
usbipd attach --wsl --busid <BUSID>

# Set serial port
export ESPPORT=/dev/ttyUSB0   # or /dev/ttyACM0, /dev/ttyS3, etc.
```

## Build & Flash

```bash
idf.py set-target esp32s3      # first time only
idf.py menuconfig              # configure Wi-Fi, project settings
idf.py build                   # compile
idf.py -p $ESPPORT erase-flash # wipe flash (first time or clean start)
idf.py -p $ESPPORT flash       # flash all: bootloader + table + app
idf.py -p $ESPPORT monitor     # open serial console (Ctrl+] to quit)
idf.py -p $ESPPORT flash monitor  # flash then monitor in one step
idf.py fullclean               # delete build/ entirely
```

## Windows (ESP-IDF PowerShell)

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

## Useful idf.py Commands

| Command | Description |
|---------|-------------|
| `idf.py size` | Show flash and RAM usage |
| `idf.py size-components` | Per-component size breakdown |
| `idf.py -p PORT monitor` | Open serial monitor only |
| `idf.py reconfigure` | Re-run CMake without full clean |
| `idf.py app-flash` | Flash app binary only (skip bootloader/table) |

## Key GPIO Summary

| Purpose | GPIO |
|---------|------|
| Button Up / Down / Left / Right | 17 / 16 / 14 / 15 |
| Button A / B | 38 / 18 |
| RGB LED R / G / B | 6 / 5 / 4 |
| WS2813B addressable LEDs | 7 |
| Buzzer | 42 |
| Display CS / DC / RST | 9 / 13 / 48 |
| SPI2 CLK / MOSI / MISO | 12 / 11 / 10 |
| SD Card CS | 3 |
| I2C SDA / SCL (IMU) | 47 / 21 |
| UART TX / RX | 43 / 44 |

## Serial Port Mapping (Windows ↔ WSL)

| Windows | WSL |
|---------|-----|
| COM3 | `/dev/ttyS3` |
| COM10 | `/dev/ttyS10` |
| (usbipd attached) | `/dev/ttyUSB0` |

## Common Fixes

| Problem | Fix |
|---------|-----|
| `python\r: No such file` | `git reset --hard` in WSL (ensures LF endings) |
| Flash permission denied | `sudo usermod -aG dialout $USER` → re-login |
| Brownout / resets | Add bulk cap to 3.3 V rail; use ≥500 mA PSU |
| Flash fails at high baud | `idf.py -p PORT -b 115200 flash` |
