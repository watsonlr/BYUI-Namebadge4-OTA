# CLAUDE.md — BYUI Namebadge OTA

Context file for Claude Code. Loaded automatically at the start of every session in this repo.

---

## Project Summary

Permanent, unbrickable OTA bootloader for the **BYUI eBadge V3.0/V4** (ESP32-S3-Mini-1-N8).
Stored in the **factory partition** so student OTA updates can never erase it.
Provides an on-device LCD menu for Wi-Fi config, OTA app download, and canvas reset.

Full spec: [OBJECTIVES.md](OBJECTIVES.md) | Architecture: [docs/NAMEBADGE_BOOTING.md](docs/NAMEBADGE_BOOTING.md)
Dev log + chat notes: [docs/DEV_CHAT.md](docs/DEV_CHAT.md)

---

## Hardware — eBadge V4 (ESP32-S3)

| MCU | ESP32-S3 QFN56 (dual-core Xtensa LX7, 240 MHz) |
|---|---|
| Flash | 4 MB embedded (XMC) |
| PSRAM | 2 MB octal PSRAM (available after bootloader) |
| Display | ILI9341 2.4" TFT, 240×320, SPI2 |
| Controls | Up/Down/Left/Right (D-pad) + A (GPIO 38) + B (GPIO 18) — active LOW |
| LEDs | RGB (GPIO 4/5/6) + WS2813B strip (GPIO 7, RMT DMA) |
| SPI2 | MOSI=11, MISO=10, CLK=12; Display DC=2, CS=13, RST=9; SD CS=3 |
| USB | J1=CP2102N UART (programming + charging); J2=ESP native USB, **device-mode only** |

USB host mode is **not possible** on V4 — CC pins are fixed pull-downs (5.1 kΩ to GND). No VBUS output path.

Full pinout: [docs/HARDWARE.md](docs/HARDWARE.md)

---

## Flash Partition Layout

```
0x0000   ROM bootloader (silicon — permanent)
0x1000   ESP-IDF 2nd-stage bootloader
0x8000   partition table
0x9000   NVS            (system config)
0xF000   otadata        (OTA slot selection)
0x11000  phy_init

0x20000  factory  ← THIS LOADER (1.25 MB, never touched by OTA)
0x160000 ota_0    (student app slot A, 1.25 MB)
0x2A0000 ota_1    (student app slot B, 1.25 MB)
0x3E0000 user_data (badge NVS: SSID, pass, nick, email, mfst — persists OTA)
```

`user_data` NVS keys: `ssid`, `pass`, `nick`, `email`, `mfst` (manifest URL).
NVS namespace: `wifi_cfg`; partition label: `user_data`.

---

## Boot Decision Flow

```
Power On → ROM → 2nd-stage bootloader → factory loader app
  buttons_init() + buttons_held(A+B, 150 ms)?
    YES → Factory Loader UI (display, portal, menu)
    NO  → valid OTA app? → esp_ota_set_boot_partition(ota_x) → restart
                           student app boots in <200 ms
        → no OTA app    → Factory Loader UI
```

**Factory Loader UI flow:**
1. Full peripheral init (display, LEDs, NVS)
2. Splash screen + BYUI-blue LED glow
3. `wifi_config_is_configured()`? — if NO → captive portal → then menu; if YES → welcome screen → A+B 5s reset window → menu

---

## Component Map

| Component | Path | Role |
|---|---|---|
| `buttons` | `buttons/` | GPIO driver, debounce, boot-time A+B check |
| `loader_menu` | `loader_menu/` | 5-item LCD menu, dispatches actions |
| `portal_mode` | `portal_mode/` | SoftAP captive portal + QR code |
| `ota_manager` | `ota_manager/` | Streaming HTTPS OTA download + SHA-256 |
| `display` | `display/` | ILI9341 SPI driver, fonts, QR rendering |
| `leds` | `leds/` | WS2813B RMT DMA driver |
| `splash_screen` | `splash_screen/` | BYUI logo animation |
| `wifi_config` | `wifi_config/` | NVS read/write for all badge settings |

---

## Development Environment

- **ESP-IDF:** v5.5, path `/home/lynn/esp/esp-idf/`
- **Activate:** `source /home/lynn/esp/esp-idf/export.sh`
- **Target:** `esp32s3`
- **Build:** `idf.py build` (output: `build/ebadge_app.bin`, ~1.1 MB)
- **Flash (Linux):** `idf.py -p /dev/ttyUSB0 flash monitor`
- **Flash (Windows):** `idf.py -p COM<X> flash` from PowerShell after IDF env
- **Clean build:** delete `build/` if CMake cache is stale

---

## Key Decisions & Gotchas

- **`CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048`** in `sdkconfig.defaults` — real browser headers exceed the 512-byte default; portal returns 431 without this.
- **OS captive-portal probe race:** iOS/Android fire `GET /` before a human opens the form. `s_form_served` is only set when `User-Agent` contains `Mozilla`. Accept both `ESP_OK` and `ESP_ERR_HTTPD_RESULT_TRUNC` from `httpd_req_get_hdr_value_str`.
- **PSRAM:** available only after 2nd-stage bootloader. Used for OTA buffers, JSON parsing, display framebuffers.
- **Factory partition:** `esp_https_ota` hardcodes writes to OTA slots — factory cannot be overwritten by OTA. Only `idf.py flash` over USB can overwrite it.
- **Rollback:** apps must call `esp_ota_mark_app_valid_cancel_rollback()` after successful startup; crash before that → automatic fallback to factory.

---

## Portal Display Phases

| Phase | Trigger | Display |
|---|---|---|
| 1 | Portal started | Blue header bar, Wi-Fi join QR, "Scan to Join Your Board's WiFi" |
| 2 | Phone joined AP | Green header, URL QR, "Device Connected! / Scan to open browser" |
| 3 | Form loaded (Mozilla UA) | White bg, blue text: "Fill out the form / on your device / and save" |

---

## OTA Tooling

- `tools/publish.sh` — builds catalog JSON, flash descriptor, copies bins, rebuilds index.html, pushes to GitHub Pages
- `tools/build_index.py` — generates index.html with OTA manifest URLs and USB flash sections
- Manifest URL stored in `user_data` NVS key `mfst`

---

## Current Status (as of 2026-03-15)

Recent commits indicate active work on **boot behavior** (`fixing the boot behavior still`).
Boot flow: A+B held → factory UI; else → launch OTA app if valid.
Portal, NVS persistence, welcome screen, and A+B factory reset window are implemented.
Loader menu, OTA download, SD card, and recovery (Tier 2) are partially stubbed.
