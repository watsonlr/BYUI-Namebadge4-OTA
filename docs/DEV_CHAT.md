# DEV_CHAT — BYUI eBadge Firmware Development

Combined development log and chat notes from development sessions.

---

# Development Log

Chronological record of design decisions and implementation notes from development sessions.

---

## 2026-03-07

### Build system
- ESP-IDF 5.5 toolchain used; stale CMake cache from a prior incomplete configure was removed (`build/` deleted) and a clean build performed.
- Binary: `build/ebadge_app.bin`, ~1.1 MB, 15% free on the app partition.

---

### Portal — SSID pre-fill
The Wi-Fi SSID input on the captive-portal form (`GET /`) was already pre-filled
with the static default `BYUI_Visitor` via a hardcoded `value=` attribute in the HTML string in `wifi_config/wifi_config.c`.

---

### Portal — detecting when a phone reads the page
Several detection points exist / were discussed:

| Event | How detected |
|-------|-------------|
| Phone joins the AP | `WIFI_EVENT_AP_STACONNECTED` → `s_sta_joined = true` |
| OS captive-portal probe (auto-popup) | `redirect_handler` — 302 redirect to `/` |
| Form page actually loaded | `get_root_handler` — new `s_form_served` flag set here |
| Form submitted | `post_save_handler` |

---

### Portal — Phase 3 display (form loaded)
When `GET /` is served the display now clears and shows plain text instructions instead of the QR code:

```
Fill out the form
on your phone
to continue
```

**Files changed:**
- `wifi_config/wifi_config.c` — added `s_form_served` flag; set in `get_root_handler`; reset in `wifi_config_start()`; exposed via `wifi_config_form_served()`.
- `wifi_config/include/wifi_config.h` — declared `wifi_config_form_served()`; added `<stddef.h>`.
- `portal_mode/portal_mode.c` — added Phase 3 poll branch; calls `display_fill` + three centred `display_print` lines when `wifi_config_form_served()` first returns true.

Portal display phases summary:

| Phase | Trigger | Display content |
|-------|---------|----------------|
| 1 | Portal started | Wi-Fi join QR + "Scan to Join Your Board's WiFi" |
| 2 | Phone joined AP | URL QR + "Device Connected!" + "Scan to open browser" |
| 3 | Form loaded in browser | Plain text: "Fill out the form / on your phone / to continue" |

---

### Persistent storage — form data survives reboots
All five form fields (SSID, password, nickname, email, manifest URL) are written to the dedicated `user_data` NVS partition on submit:

```c
nvs_open_from_partition(WIFI_CONFIG_NVS_PARTITION, WIFI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &h);
nvs_set_str(h, "ssid", ssid);
nvs_set_str(h, "pass", pass);
nvs_set_str(h, "nick", nick);
nvs_set_str(h, "email", email);
nvs_set_str(h, "mfst", manifest);
nvs_commit(h);
```

The `user_data` partition sits at the top of flash (0x3E0000) and is **never overwritten by OTA**. The OTA manager reads `ssid`/`pass`/`mfst` back from this partition on every boot.

---

### Boot branching — skip portal if already configured
On every boot, `wifi_config_is_configured()` is called before the portal. It opens the `user_data` NVS partition read-only and checks whether a non-empty `nick` key exists. If it does, the portal is skipped entirely.

**New API added to `wifi_config`:**

| Function | Description |
|----------|-------------|
| `wifi_config_is_configured()` | Returns `true` if a nickname is stored in NVS |
| `wifi_config_get_nick(out, outlen)` | Copies the stored nickname into `out` |

**Boot flow:**

```
boot
 └─ wifi_config_is_configured()?
     ├─ YES → show_welcome(nick) → [A+B reset window] → ota_manager_run()
     └─ NO  → portal_mode_run(0) → ota_manager_run()
```

**Files changed:** `main/main.c`, `wifi_config/wifi_config.c`, `wifi_config/include/wifi_config.h`.

---

### Welcome screen — A+B factory reset
After the welcome screen is drawn, a 5-second window opens during which holding **Button A (GPIO 38)** and **Button B (GPIO 18)** simultaneously for 2 continuous seconds triggers a factory reset:

1. Screen shows `"Config erased. Rebooting..."` (red on black).
2. `nvs_flash_erase_partition(WIFI_CONFIG_NVS_PARTITION)` wipes all user data.
3. `esp_restart()` reboots into the portal flow.

If neither button is held the window expires silently and normal operation continues.

A small hint line is shown at the bottom of the welcome screen: **"Press A & B to reconfig"**.

**Files changed:** `main/main.c`.

---

# Chat Notes

Key decisions, hardware findings, and implementation notes from development sessions.

---

## 2026-03-07 — USB OTG / VBUS Hardware Analysis

### Background
Investigated whether the BYUI eBadge V4 hardware supports USB Host mode (e.g., reading a firmware image from a USB flash drive or connecting a keyboard).

### Findings from Netlist (`docs/badge-esp32-s3.net`)

**ESP USB connector (J2): Device-mode only**
- CC1 and CC2 pins (J2) connect only to R5 and R6, both **5.1 kΩ pull-downs to GND**
- 5.1 kΩ on CC is the USB-C spec for UFP (Upstream Facing Port = device). Hardwired — cannot advertise host role.
- VBUS net on J2 (`/VBUSB`): connects only to J2 pins and D8 (ESD protection diode to GND). **Nothing drives it — input-only.**

**5V power rail exists but is not routed to J2 VBUS**
- U2 (TPS630701RNMR) is a buck-boost converter producing `/5V_stable` from the battery
- `/5V_stable` feeds the LED array (AL1–AL24 VDD), J3, J11 — **never routed to J2 VBUS**
- Q3 (HL2301A P-MOSFET): gate on `/USB_5V`, drain on `/VBATT`, source on `Net-(Q3-S)` which connects only to a `Power1` symbol — this is the charger input power path, not a VBUS output switch

**Power switch IC**
- U4 = TP4056 (LiPo charger), VCC/CE both on `/USB_5V` — this is the charge input, not an output path

### Conclusion
USB Host mode requires:
1. VBUS output to power the connected device (5V, up to 500 mA for FS HID)
2. CC pins pulled **up** to 3.3V through 56 kΩ (host) rather than pulled down to GND (device)

V4 has neither. The 5V boost rail exists on the board but is not switched to J2 VBUS, and the CC resistors are fixed pull-downs.

### What J2 CAN do (device mode only)
| Use case | Works? | Notes |
|---|---|---|
| USB serial console (`idf.py monitor`) | ✅ | Built into ESP-IDF |
| USB DFU firmware flash | ✅ | ESP-IDF `usb_dfu` component |
| Badge acts as HID keyboard/mouse | ✅ | TinyUSB HID device |
| Badge acts as USB mass storage | ✅ | TinyUSB MSC |
| Reading a USB keyboard or flash drive | ❌ | Requires host mode — needs VBUS output + CC pull-ups |

### V5 board recommendation
To support USB Host on a future revision:
- Route `/5V_stable` through a P-channel MOSFET high-side switch to J2 VBUS
- Connect the MOSFET gate to a spare GPIO (e.g., GPIO45 or GPIO46)
- Change CC resistors to 56 kΩ pull-ups (or use a USB-C PD controller that supports role switching)

---

## 2026-03-07 — OTA & Update Strategy

### Wireless OTA (implemented)
- `ota_manager` component: downloads firmware binary into PSRAM, verifies SHA-256, writes to inactive OTA partition, reboots
- Manifest JSON format: `{ "version": N, "url": "...", "size": N, "sha256": "..." }`
- NVS namespace `wifi_cfg`, key `mfst` stores the manifest URL (set via portal)

### OTA Site Tooling (implemented)
- `tools/publish.sh` — builds catalog JSON, flash descriptor JSON, copies binaries, rebuilds index.html, git pushes
- `tools/build_index.py` — generates index.html with OTA manifest URLs and full USB flash sections per variant
- `tools/catalog_base.example.json` — template for catalog JSON

### SD Card OTA (not yet implemented — pending decision)
- Hardware: SD card wired on SPI2, GPIO3=CS, GPIO10=MISO, GPIO11=MOSI, GPIO12=CLK (shared with display)
- Plan: mount SD on boot, look for `ebadge_app.bin`, verify SHA-256 sidecar, flash via `esp_ota_ops`, rename to `.done`

### USB serial flash (for instructors)
- `idf.py -p COM<X> flash` from PowerShell (Windows) after running ESP-IDF env setup
- In WSL: `source /home/lynn/esp-idf/export.sh` then flash via WSL only if usbipd is configured
- Bootloader can only be updated via USB — never OTA

---

## 2026-03-07 — Portal Display

### Current layout (portal_mode/portal_mode.c)
- All text at scale 2 (16 px per char, 8 px wide)
- `centre_x()` helper auto-centers strings
- Phase 1 (before device joins): Blue bar header, "Welcome to your" / "ECEN NameBadge!" white on blue, WiFi QR at y=120, "Scan to Join Your" / "Board's WiFi" footer
- Phase 2 (after device joins): Green bar header, "Device Connected!" white on green, URL QR, "Scan to open browser" / "If not open yet." footer
- `display_fill_rect()` added to display component for colored header bars

### Key defines
```c
#define QR_CY     120
#define HDR_Y1    2
#define HDR_Y2    20
#define TEXT_Y1   188
#define TEXT_Y2   206
#define HDR_BAR_H 40   // HDR_Y2 + HDR_SCALE*DISPLAY_FONT_H + 4
```

### Web form
- SSID field pre-filled with `BYUI_Visitor` via `value='BYUI_Visitor'`

---

## 2026-03-07 — Hardware Reference (V4)

### Pinout (confirmed from netlist)
| GPIO | Function | Net name |
|------|----------|----------|
| IO19 | USB D- | `/ESP_USB-` |
| IO20 | USB D+ | `/ESP_USB+` |
| IO3  | SD Card CS | `/GPIO3` |
| IO10 | SPI2 MISO | `/GPIO10` (shared display + SD) |
| IO11 | SPI2 MOSI | `/GPIO11` (shared display + SD) |
| IO12 | SPI2 CLK  | `/GPIO12` (shared display + SD) |

### Power architecture (V4)
| Component | Part | Function |
|-----------|------|----------|
| U2 | TPS630701RNMR | Buck-boost, battery → 5V (`/5V_stable`) for LEDs |
| U4 | TP4056 | LiPo charger, powered from USB-UART VBUS |
| Q3 | HL2301A (P-MOS) | Battery power path switch |
| Q4 | HL2301A (P-MOS) | Secondary power switch |
| U3 | CP2102N | USB-UART bridge (J1 connector) |
| J1 | USB-C 16P | USB-UART port (programming + charging) |
| J2 | USB-C 16P | ESP32-S3 native USB (device mode only) |

---

## ESP-IDF Environment

- IDF path: `/home/lynn/esp-idf/`
- Activate: `source /home/lynn/esp-idf/export.sh`
- Target: `esp32s3`
- Flash: `idf.py -p COM<X> flash` (PowerShell) or `idf.py -p /dev/ttyUSB0 flash` (Linux)
- Build confirmed clean as of 2026-03-07

---

## 2026-03-08

### Portal — Phase 3 screen redesign
The "Form is open" screen was redesigned to be cleaner and more consistent with the web form styling:

- **Before:** Black background, green header bar with "Form is open!" / "Fill it out on", white body text, cyan small hint lines ("Enter your name, Wi-Fi, …")
- **After:** Plain white background, blue text (matching the web form's `#006eb8`), three centered lines only — no header bar, no hints

```
Fill out the form
 on your device
   and save
```

The word "save" was chosen to match the "Save Settings" button label on the web form (was "and submit").

**Files changed:** `portal_mode/portal_mode.c`

---

### Portal — HTTP header buffer enlarged
Form submissions from real browsers were failing with a 431 "Header fields too long" error.

**Root cause:** `CONFIG_HTTPD_MAX_REQ_HDR_LEN` was 512 bytes — too small for browser `Accept`, `Accept-Language`, and other headers.

**Fix:** Set `CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048` in both `sdkconfig` and `sdkconfig.defaults`. This is a Kconfig-only setting in ESP-IDF 5.3; there is no runtime struct field for it.

POST body buffer also enlarged from 512 → 1024 bytes in `wifi_config.c` to give headroom for URL-encoded special characters in passwords.

**Files changed:** `sdkconfig`, `sdkconfig.defaults`, `wifi_config/wifi_config.c`

---

### Portal — Phase 2→3 transition fix (OS probe race)
When a phone joins the badge's AP, iOS/Android/Windows automatically fire a `GET /` probe to detect captive portals. This was setting `s_form_served = true` before Phase 2 had even drawn, causing an immediate skip to Phase 3.

**Fix:** Only set `s_form_served` when the request carries a `Mozilla` User-Agent — something all real browsers include but OS probes do not.

`httpd_req_get_hdr_value_str` returns `ESP_ERR_HTTPD_RESULT_TRUNC` when the UA is longer than the buffer (real UAs are 100+ chars). Code accepts both `ESP_OK` and `ESP_ERR_HTTPD_RESULT_TRUNC`; "Mozilla" is always the first token so a 64-byte truncated read still catches it.

An `else if` guard on the polling loop prevents Phase 3 from firing on the same tick as Phase 2.

**Files changed:** `wifi_config/wifi_config.c`, `portal_mode/portal_mode.c`

---

### Portal — SSID field replaced with discovered-network dropdown
Replaced the static `value='BYUI_Visitor'` text input with a dynamic `<input list='nets'>` + `<datalist>` built from a real-time WiFi scan.

**Implementation:**
1. After `esp_wifi_start()`, `start_softap()` calls `scan_nearby_networks()`.
2. `scan_nearby_networks()` briefly switches to `WIFI_MODE_APSTA`, runs a blocking active scan (100–250 ms per channel), collects up to 20 AP records, then reverts to `WIFI_MODE_AP`.
3. `get_root_handler()` switched from `httpd_resp_send` to chunked transfer (`httpd_resp_sendstr_chunk`) and builds the SSID field on the fly: `<input name='ssid' list='nets'>` followed by `<datalist>` options for each discovered SSID (hidden SSIDs and duplicates skipped; names HTML-escaped via `html_esc()` helper to prevent injection).
4. Label changed to **"SSID — Select or Enter"**.
5. Fallback: if scan yields 0 results, a plain `<input>` is rendered with a placeholder.

The user can both pick from the dropdown list **and** type any custom value — standard HTML `<datalist>` behaviour, no JavaScript needed.

**Files changed:** `wifi_config/wifi_config.c`

---

### VS Code — CMake Tools popup suppressed
"Bad CMake executable" banner was appearing every time the workspace opened.

**Fix:** Added to `.vscode/settings.json`:
```json
"cmake.enabled": false
```
This completely disables the CMake Tools extension for the workspace (ESP-IDF has its own CMake integration; CMake Tools is not needed here).

Earlier mitigations (`configureOnOpen: false`, `automaticReconfigure: false`, `configureOnEdit: false`) were insufficient — the extension still activated. `cmake.enabled: false` is the definitive fix.

When the error persisted even with `cmake.enabled: false`, the Espressif-bundled cmake was found at `C:\Espressif\tools\cmake\3.24.0\bin\cmake.exe` and set explicitly:
```json
"cmake.cmakePath": "C:/Espressif/tools/cmake/3.24.0/bin/cmake.exe"
```

**Files changed:** `.vscode/settings.json`

---

### OTA — PSRAM failure diagnosis
After submitting the portal form the display showed **"OTA: no PSRAM"** on every attempt.

**Root cause:** ESP32-S3 rev 0.2 silicon errata. The boot log showed:

```
E (344) octal_psram: PSRAM ID read error: 0x00000000
```

The chip reports 0 bytes of PSRAM from `esp_psram_get_size()`. `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` allows the app to continue booting. Changing `CONFIG_SPIRAM_SPEED` from 80 MHz → 40 MHz (built and flashed) made no difference — the core issue is not clock speed but a hardware initialisation failure in this silicon revision.

**Result:** PSRAM is physically present (esptool identifies "Embedded PSRAM 2MB (AP_3v3)") but is never usable at runtime on this particular board.

---

### OTA — Rewrite: PSRAM-buffered → streaming-to-flash

The original architecture allocated the full firmware image in PSRAM before flashing. Since PSRAM is unavailable on this board, the OTA manager was redesigned to stream directly to flash with no large RAM buffer.

**New architecture:**

1. Connect to WiFi STA.
2. Fetch JSON manifest → parse `version`, `url`, `size`, `sha256`.
3. Compare `version` against stored `ota_ver` in NVS; skip if up-to-date.
4. Verify image fits in the inactive OTA partition.
5. Open OTA partition with `esp_ota_begin()`.
6. Open HTTP connection; read in 8 KB chunks (`s_ota_chunk[8192]`, static / BSS).
7. Each chunk: `mbedtls_sha256_update()` + `esp_ota_write()`.
8. After final chunk: `mbedtls_sha256_finish()` → compare hex string.
9. On SHA mismatch: `esp_ota_abort()`; display "SHA-256 mismatch!".
10. On success: `esp_ota_end()` → `esp_ota_set_boot_partition()` → commit new version to NVS → reboot.

Display shows `"Downloading..."` with `"%% (KB)"` progress updated every 5%.

**Key changes in `ota_manager.c`:**
- Removed `#include "esp_psram.h"` and `#include "esp_heap_caps.h"`
- Removed functions `http_download_firmware()`, `sha256_verify()`, `flash_from_psram()`
- Added `#define OTA_CHUNK_SIZE 8192`, `static uint8_t s_ota_chunk[OTA_CHUNK_SIZE]`
- New function `http_stream_and_flash(url, expected_size, sha256_expected)`
- `ota_manager_run()`: removed PSRAM check, PSRAM size check, malloc block; replaced three-step download/verify/flash sequence with single `http_stream_and_flash()` call

**Key changes in `ota_manager.h`:**
- Description updated: "PSRAM-buffered OTA" → "Streaming OTA update manager"
- `OTA_RESULT_NO_PSRAM` removed from `ota_result_t` enum

**Files changed:** `ota_manager/ota_manager.c`, `ota_manager/include/ota_manager.h`

---

### Portal — Setup Complete screen redesign

The Step 4 "welcome" screen after form submission was redesigned:

**Before:** Black background, green header bar ("Setup Complete!" / "Welcome to your"), yellow "eBadge!" text, white nickname, cyan info lines ("Connecting to Wi-Fi…" etc.), 3-second static delay.

**After:** White background, clean layout, live WiFi connection test with status feedback:
- "Setup Complete" — black text, scale 2, centred near top
- Badge nickname — black, scale 2 or 3, centred
- "Connecting to WiFi..." shown while connection is attempted (up to 10 s)
- On success: **"WiFi Connected"** (green, scale 2) + IP address below (green, scale 1)
- On failure: **"WiFi NOT Connected"** (red, scale 2) + failure reason (red, scale 1) + button prompts (black, scale 1):
  - `"<- Back button to re-try"`
  - `"-> FWD button to continue"`

**Button behaviour on failure:**
- **Button A (GPIO 38 / Back / Left):** re-runs the WiFi test with saved credentials — can retry indefinitely
- **Button B (GPIO 18 / Forward / Right):** skips WiFi check and continues; OTA will later return "no WiFi" gracefully

**WiFi failure reasons mapped to human-readable strings:**

| Reason code | Message |
|------------|---------|
| 201 | SSID not found |
| 2 / 202 | Wrong password |
| 15 | Wrong password |
| 203 | Association failed |
| 200 | Beacon timeout |
| 67 | Connection failed |
| 204 | Handshake timeout |
| other | Error code: N |

The disconnect reason code is captured from `WIFI_EVENT_STA_DISCONNECTED` event data (`wifi_event_sta_disconnected_t.reason`) in the test event handler. The last reason seen before giving up is stored in `s_disconnect_reason`.

The WiFi test (connect → get IP → disconnect) runs fully inline in `portal_mode_run()` after `wifi_config_stop()`. After the screen delay (or button press), `ota_manager_run()` reconnects independently.

**Files changed:** `portal_mode/portal_mode.c`

