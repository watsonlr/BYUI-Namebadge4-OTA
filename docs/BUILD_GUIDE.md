# Build Guide

## Prerequisites

- ESP-IDF v5.3.1 installed and sourced (see [SETUP_GUIDE.md](SETUP_GUIDE.md))
- Python 3.11+

---

## Build the Starter App

```bash
# Set the target chip (only needed once, result saved in sdkconfig)
idf.py set-target esp32s3

# (Optional) open configuration menu
idf.py menuconfig

# Build
idf.py build
```

Output files are in `build/`:

| File | Description |
|------|-------------|
| `build/bootloader/bootloader.bin` | 2nd-stage bootloader |
| `build/partition_table/partition-table.bin` | Partition table |
| `build/<project>.bin` | Your application binary |

---

## Flash the Badge

```bash
# First-time: erase flash to start clean
idf.py -p /dev/ttyUSB0 erase-flash

# Flash all components (bootloader + partition table + app)
idf.py -p /dev/ttyUSB0 flash

# Flash and open serial monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

On Windows (ESP-IDF PowerShell):

```powershell
idf.py -p COM3 erase-flash
idf.py -p COM3 flash monitor
```

> Press **Ctrl+]** to exit the monitor.

---

## Clean Build

```bash
idf.py fullclean
idf.py build
```

---

## Add Your Own Source Files

1. Add `.c` files to `main/`.
2. List them in `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS
        "main.c"
        "my_feature.c"      # ← add here
    INCLUDE_DIRS "."
    REQUIRES
        driver
        nvs_flash
        ...
)
```

3. Add any new ESP-IDF component dependencies to the `REQUIRES` list.

---

## Adding Additional Apps (OTA)

If you want to build separate apps that can be downloaded over-the-air:

1. Create an `Apps/<app_name>/` subdirectory.
2. Add a `CMakeLists.txt` and `main/` inside it (same structure as the root project).
3. Build each app:

```bash
cd Apps/my_app
idf.py set-target esp32s3
idf.py build
```

4. Copy the resulting `.bin` to `ota_files/apps/` and update `ota_files/manifest.json`.
5. Run the OTA server: `python3 simple_ota_server.py` (if added — see `manifest.json.example`).

---

## AP IP Address (MAC-Derived, Per-Badge Unique)

Each badge computes its own SoftAP IP address from the last two bytes of its factory eFuse MAC.

### Formula

Given MAC address `aa:bb:cc:dd:vv:ww` (6 bytes, `vv` = byte 4, `ww` = byte 5):

```
AP IP = 192.168.(vv % 240).(ww % 240)
```

- The last octet is clamped to a minimum of 1 to avoid using a network address (`x.x.x.0`).
- The resulting IP is used for the SoftAP gateway, the DHCP server, the HTTP config portal, and the DNS hijack response.
- All badges share the SSID `BYUI_NameBadge` but have different IPs, so they can operate in the same room without conflict.

### Example

| eFuse base MAC | vv (byte 4) | ww (byte 5) | AP IP |
|---|---|---|---|
| `a0:b7:65:12:3c:f8` | `0x3c` = 60 | `0xf8` = 248 | `192.168.60.8` (248 % 240 = 8) |
| `a0:b7:65:12:00:00` | 0 | 0 | `192.168.0.1` (last octet clamped) |

### Finding Your Badge's IP

After flashing, open a serial monitor — the IP is logged at startup:

```
I (xxx) wifi_config: AP IP derived from MAC aa:bb:cc:dd:vv:ww → 192.168.X.Y
I (xxx) wifi_config: SoftAP started: SSID="BYUI_NameBadge"  IP=192.168.X.Y
```

The badge also displays the URL on-screen below the QR code.

### Reading the MAC Before Flashing (to predict the IP)

```bash
# WSL / Linux
esptool.py --port /dev/ttyUSB0 read_mac
```

```powershell
# Windows ESP-IDF PowerShell
esptool.py --port COM3 read_mac
```

Output shows `MAC: aa:bb:cc:dd:vv:ww`. Apply the formula above.

