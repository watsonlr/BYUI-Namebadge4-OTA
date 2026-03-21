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
| Controls | Up/Down/Left/Right (D-pad) + A (GPIO 34) + B (GPIO 33) — active LOW |
| LEDs | RGB (GPIO 2/4/5) + WS2813B strip (GPIO 7, RMT DMA) |
| SPI2 | MOSI=3, CLK=46; Display DC=45, CS=0, RST=1 (write-only, no MISO) |
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

The A+B escape-to-factory logic lives in the **custom 2nd-stage bootloader**
(`bootloader_components/factory_switch/`), not in the factory app itself.

```
Power On → ROM → 2nd-stage bootloader (factory_switch hook)
  A+B held 150 ms? → erase otadata sectors (0xF000–0x10FFF)
  otadata valid?   → boot ota_0 / ota_1   (student app)
  otadata blank?   → boot factory          (this loader)
```

**Factory Loader always runs the UI** — it never redirects to an OTA app.
The bootloader handles that redirect before the factory app even starts.

**Factory Loader UI flow:**
1. Full peripheral init (display, LEDs, NVS)
2. Splash screen + BYUI-blue LED glow
3. `wifi_config_is_configured()`? — if NO → captive portal → then menu; if YES → menu

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
- **A+B factory escape:** implemented in `bootloader_components/factory_switch/factory_switch.c` via `bootloader_after_init()` hook. Checks GPIO 33 (B) + GPIO 34 (A) held 150 ms; if so, erases otadata sectors so bootloader falls back to factory. Both pins are above GPIO 21 (non-LP) and read reliably in bootloader context. Previous hook used wrong GPIO 38.
- **Button debounce — LP pad issue:** GPIOs 0–21 are LP I/O pads. A `while (gpio_get_level() == 0)` spin-wait inside the button task can block forever on these pads after a software reset, even after `gpio_config()`. UP=GPIO 11 and LEFT=GPIO 21 are affected. Solution: time-based state machine using `esp_timer_get_time()` — no spin-waits anywhere. Task stack must be **4096 bytes** (not 2048) because `esp_timer_get_time()` has a deeper call chain than expected.
- **Button debounce — state machine:** `STATE_IDLE → STATE_DEBOUNCING → STATE_FIRED`. Both press (30 ms continuous LOW) and release (30 ms continuous HIGH) are debounced independently. One physical press = exactly one queue event, regardless of contact bounce.
- **WiFi reconfigure after failed OTA:** `ota_manager_fetch_catalog()` initialises `esp_netif` and the default event loop but does not deinit them on failure. Calling `portal_mode_run()` afterward causes `wifi_config_start()` → `start_softap()` → `ESP_ERROR_CHECK(esp_netif_init())` to abort with `ESP_ERR_INVALID_STATE`. Fix: erase `user_data` NVS partition then `esp_restart()` — the factory loader reboots clean and auto-runs the portal since `wifi_config_is_configured()` returns false (nick key gone).
- **Display text width at scale 2:** `DISPLAY_FONT_W = 8`, scale 2 = 16 px/char, display width = 320 px → **max 20 chars per line** at scale 2. Strings longer than 20 chars overflow the display. The centering formula `(DISPLAY_W - len*FONT_W*scale)/2` goes negative if string is too wide — clamp or shorten the string.
- **NVS namespace:** `wifi_config_is_configured()` checks the `nick` key in partition `user_data`, namespace `badge_cfg` (not `wifi_cfg` as previously noted).

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

## Current Status (as of 2026-03-20)

- **Boot flow complete:** Custom bootloader (`factory_switch`) erases otadata when A+B held 150 ms at reset; factory loader always shows the UI. Students can always escape a broken OTA app by holding A+B on reset — no code required in OTA apps.
- **Button driver rewritten:** Time-based state machine in `buttons/buttons.c`. Press debounce + release debounce both 30 ms. Task stack 4096 bytes. Reliable on all 6 GPIOs including LP-range pads.
- **Loader menu working:** Up/Down navigation, A/Right to select, B to move down. All four items functional (OTA download, SD stub, bare-metal info, reset namebadge).
- **OTA download working:** Fetches catalog, shows icon tiles, streams firmware, reboots into student app.
- **WiFi fail → reconfigure:** If WiFi connect fails during OTA download, screen shows "WiFi Connect Failed / A: Reconfigure WiFi". Pressing A erases user_data NVS and restarts — factory loader re-runs the full QR-code portal.
- **SD card load and SD recovery:** stubbed ("coming soon" screens).
- **Welcome screen / A+B 5 s reset window:** previously described but not currently in main.c — factory loader goes directly to portal (if unconfigured) or menu (if configured).
