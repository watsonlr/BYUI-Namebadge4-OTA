# Namebadge Boot Architecture and Firmware Management

## Overview

This document describes the boot architecture, memory layout, OTA update
strategy, and recovery procedures for the Namebadge board based on the
**ESP32-S3-Mini-1-N8**.

Design goals:

- Persistent loader that cannot be erased by student programs
- Safe firmware updates via OTA
- Simple classroom user interaction
- Robust recovery mechanisms
- Optional microSD-based full firmware restoration

The badge normally boots **directly into the student application**.\
The factory loader is entered by pressing the **BOOT button within
~500 ms of a hardware reset**.

------------------------------------------------------------------------

## Boot Process

### Stage 1 — ROM Bootloader

Location: **Inside the ESP32-S3 silicon**

Responsibilities:

- Runs immediately after reset
- Reads GPIO 0 level when EN rises (RESET released) to decide boot mode:
  HIGH → normal boot; LOW → ROM download mode
- Initializes SPI flash interface
- Loads the second-stage bootloader from flash

This code is permanent and **cannot be erased or modified**.

> **Note:** The ROM reads GPIO 0 at the exact moment RESET is released.
> Our BOOT button has a 470 Ω pull-up (no debounce capacitor), so the
> pin is always HIGH when RESET releases.  The factory escape is detected
> later, in the `factory_switch` hook, by polling GPIO 0 for 500 ms.

------------------------------------------------------------------------

### Stage 2 — ESP-IDF Second-Stage Bootloader + factory_switch hook

Location:

    Flash address: 0x1000

The second-stage bootloader is extended by the **`factory_switch`**
component (`bootloader_components/factory_switch/factory_switch.c`),
which runs via the `bootloader_after_init()` hook before any partition
is loaded.

`factory_switch` responsibilities (in order):

1. **Check for a pending factory self-update (RTC flag)**\
   Reads the last 16 bytes of RTC slow DRAM (`0x600FFFF0`).  If the
   magic word `0xFA510A0B` is present (written by `factory_self_update`
   before an `esp_restart()`), a new loader binary is staged in an
   inactive OTA slot.  The bootloader copies it sector-by-sector into
   the factory partition, then clears the flag.  otadata is left intact
   so the student app continues to run after the update.

2. **Check reset reason**\
   - **Software reset** (`RESET_REASON_CORE_SW` / `RESET_REASON_CPU0_SW`):
     Leave otadata intact (the calling app already set it) and return.
   - **Hardware reset** (power-on, RESET button, watchdog, …):
     Continue to step 3.

3. **Poll the BOOT button for ~500 ms**\
   Configures GPIO 0 as a digital input (`IO_MUX`, MCU_SEL=1,
   FUN_IE=1) and samples it every 10 ms for up to 50 iterations.
   GPIO 0 has a **470 Ω external pull-up** and **no debounce
   capacitor**, so it reads reliably within microseconds.
   - **BOOT pressed** (GPIO 0 LOW detected): erase both otadata
     sectors so the bootloader falls back to the factory partition.
   - **BOOT not pressed**: leave otadata intact so the OTA app boots
     directly.

After `factory_switch` returns, the standard bootloader reads otadata
and selects the boot partition normally.

------------------------------------------------------------------------

### Stage 3 — Factory Loader Application (when entered)

Location:

    factory partition (0x20000)

This is the **BYUI-Namebadge-OTA** firmware.  It runs only when:

- BOOT button was pressed within 500 ms of a hardware reset, **or**
- No valid OTA image exists (fresh badge or after Full Factory Reset)

Responsibilities:

- Start a background WiFi check for a loader self-update
- Display the splash screen while the background check runs
- Configure WiFi via captive portal (QR code on phone)
- Show confirmation screen if a newer loader is available (with blinking
  red LED warning strip)
- Display the interactive loader menu
- Install firmware via OTA download
- Provide reset and recovery options

------------------------------------------------------------------------

## Boot Decision Logic

    Power On / Hardware Reset
       |
    ROM Bootloader — reads GPIO 0 at EN↑
       | (GPIO 0 HIGH = normal boot path)
       |
    Second-Stage Bootloader
       |
    factory_switch hook  (bootloader_after_init)
       |
       |── [1] RTC flag valid? ──YES──> apply staged factory update
       |                               (erase + rewrite factory partition)
       |                               clear flag, fall through
       |
       |── [2] Software reset? ──YES──> return (otadata intact)
       |
       |── [3] Hardware reset: configure GPIO 0 input, poll 500 ms
       |
       |── BOOT pressed (GPIO 0 LOW) ──> erase otadata
       |
       |── BOOT not pressed ──────────> leave otadata intact
       |
    Bootloader reads otadata
       |
       |── otadata valid (OTA app installed) ──────> boot OTA app directly
       |
       └── otadata blank ───────────────────────> boot factory
                                                       |
                                                  buttons_init()
                                                  nvs_flash_init()
                                                  display_init()
                                                  leds_init()
                                                       |
                                                  factory_self_update_begin()
                                                  (background WiFi check starts)
                                                       |
                                                  splash_screen_run()
                                                  (WiFi + manifest check running
                                                   concurrently in background)
                                                       |
                                                  wifi_config_is_configured()?
                                                       |
                                                 NO ───┴─── YES
                                                 |           |
                                               portal   factory_self_update_finish()
                                                 |       (show confirm if update found)
                                                 |           |
                                                  loader menu

Startup latency on plain reset with a valid OTA app installed:
**< 1 ms** (no GPIO wait on software reset) + single normal boot.
On hardware reset with no BOOT press: **~500 ms** polling window, then
OTA app boots directly.

------------------------------------------------------------------------

## Factory Loader Self-Update

The factory loader can update itself over WiFi without affecting the
student app.  This allows BYUI to ship improvements to all badges
transparently.

### How It Works

1. `factory_self_update_begin()` launches a background FreeRTOS task
   that silently connects to WiFi and fetches the loader manifest from:

       https://watsonlr.github.io/namebadge-apps/bootloader_downloads/loader_manifest.json

2. The manifest is a JSON array.  Each entry describes one loader
   release:

       [
         {
           "hw_version":     4,
           "loader_version": 1,
           "binary_url":     "https://.../badge_bootloader_v4.1.bin",
           "size":           1139600,
           "sha256":         "73bc9ef8..."
         },
         ...
       ]

3. The task finds the entry with the highest `loader_version` that
   matches the current `hw_version`.  If that version is greater than
   `LOADER_SW_VERSION` compiled into the running firmware, the check
   reports an update is available.

4. `factory_self_update_finish()` (called after the splash screen)
   checks the result.  If an update is available, a confirmation screen
   appears with **blinking dim-red LED warning** at ~1 Hz:

       ┌──────────────────────────────────┐
       │      Loader Update Found         │
       ├──────────────────────────────────┤
       │ A new bootloader is available:   │
       │ Current:   v4.1                  │
       │ Available: v4.2                  │
       │ Update now? (~30 sec)            │
       ├──────────────────────────────────┤
       │     A:Update    B:Skip           │
       └──────────────────────────────────┘

5. **A pressed**: the binary is downloaded to the **inactive OTA slot**
   (used as a staging area — otadata is never touched), SHA-256 is
   verified, an RTC flag is written, and `esp_restart()` is called.

6. On the next boot the `factory_switch` bootloader hook detects the
   flag, copies the staged binary to the factory partition, clears the
   flag, and continues to normal boot.  The student app resumes running.

7. **B pressed**: silently disconnects WiFi and proceeds to the
   loader menu.

### Version Defines

Both version numbers are defined in `loader_menu/include/loader_menu.h`:

    #define LOADER_HW_VERSION   4   /* eBadge PCB revision   */
    #define LOADER_SW_VERSION   1   /* Loader software build */

`LOADER_SW_VERSION` is incremented when publishing a new loader release.

### Publishing a New Loader Release

Use the publish script in `tools/`:

    ./tools/publish_bootloader.sh

The script prompts for a loader version number, builds the firmware,
copies the binary to `namebadge-apps/bootloader_downloads/`, updates
`loader_manifest.json`, and pushes to GitHub.

------------------------------------------------------------------------

## Factory Loader UI Flow

When the factory loader UI is entered (BOOT held at reset, or no
student app):

    1. Full peripheral init (buttons, NVS, display, LEDs)
    2. Start background loader self-update check (silent WiFi connect)
    3. Splash screen animation (runs while WiFi + manifest check runs)
    4. If update found → confirmation screen with blinking red LEDs
    5. If not WiFi-configured → captive portal (QR code on phone)
    6. Loader menu (interactive)

------------------------------------------------------------------------

## Loader Menu

Navigation: **Up / Down** to move, **A or Right** to select.

    ┌───────────────────────────────────────┐
    │        BYU-I Loader (v4.1)            │  ← navy-blue header
    ├───────────────────────────────────────┤
    │  ▶  OTA App Download                  │  ← highlighted item (scale-2 text)
    │     SDCard Apps                       │
    │     Reset Wifi/Config                 │
    │     Full Factory Reset                │
    │     H/W Self-Tests                    │
    │     USB Program                       │
    ├───────────────────────────────────────┤
    │   Up/Dn:move   Right/A:select         │  ← hint bar
    └───────────────────────────────────────┘

Header shows the current loader version (`LOADER_HW_VERSION.LOADER_SW_VERSION`).

### Item Descriptions

**OTA App Download**\
Connect to WiFi, fetch the app catalog from the manifest URL stored in
`user_data` NVS, display icon tiles for each available app, download and
flash the selected firmware to the inactive OTA slot, then reboot.

**SDCard Apps**\
*Coming soon* — stub screen is shown with a brief explanation.

**Reset Wifi/Config**\
Erases the `user_data` NVS partition (WiFi credentials, nickname,
manifest URL).  Installed student app is preserved.  Badge reboots and
the portal runs on next factory-loader entry.

**Full Factory Reset**\
Erases everything: `user_data` NVS + `ota_0` + `ota_1` + `otadata`.
Badge returns to the same state as a freshly flashed board.
**Confirmation screen shows blinking dim-red LEDs as a warning.**

**H/W Self-Tests**\
*Coming soon* — stub screen is shown.

**USB Program**\
Displays `idf.py flash` / `esptool.py` instructions for entering ROM
download mode manually.

------------------------------------------------------------------------

## LED Warning Behaviour

Dangerous confirmation screens blink **all 24 addressable LEDs dim red
(intensity 8/255, ~3%) at 1 Hz** while waiting for user input.
The LEDs turn off immediately when any button is pressed.

Screens that trigger LED warnings:

- **Loader self-update confirmation** (A:Update / B:Skip)
- **Full Factory Reset confirmation** (A:Confirm / B:Cancel)

------------------------------------------------------------------------

## Button Assignments

| Button | GPIO | Role                                                    |
|--------|------|---------------------------------------------------------|
| BOOT   | 0    | Factory escape (press within 500 ms of hardware reset)  |
| A      | 34   | Select / Confirm in menus and confirmation screens      |
| B      | 33   | Cancel / Back; scroll menu down                         |
| Up     | 11   | Scroll menu up                                          |
| Down   | 47   | Scroll menu down                                        |
| Left   | 21   | Scroll up (alias for Up)                                |
| Right  | 10   | Select (alias for A)                                    |

All six nav buttons (A/B/Up/Down/Left/Right) are **active LOW** with
internal pull-ups and **~10 µF hardware debounce capacitors**.

> **LP pad note:** GPIOs 0–21 are LP I/O pads.  A spin-wait on these
> pins can block forever after software reset.  The button driver uses
> a time-based state machine (`esp_timer_get_time()`) — no spin-waits
> anywhere.  Task stack is 4096 bytes to accommodate the deeper call
> chain of `esp_timer_get_time()`.
>
> **BOOT button:** GPIO 0 has a **470 Ω external pull-up** and **no
> debounce capacitor**, making it read reliably within microseconds in
> bootloader context.  GPIO 0 is shared with Display CS but the display
> is not initialized in the bootloader, so reading it as a plain input
> is safe.

------------------------------------------------------------------------

## Flash Memory Layout

Current partition table (`partitions.csv`):

    Flash Memory Map
    -----------------------------------------------
    0x0000   ROM bootloader (internal)
    0x1000   second-stage bootloader
    0x8000   partition table
    0x9000   NVS            (system config)
    0xF000   otadata        (OTA boot selection — 2 × 4 KB sectors)
    0x11000  phy_init       (PHY calibration)

    0x20000  factory        (this loader — 1.25 MB)

    0x160000 ota_0          (student app slot A — 1.25 MB)
    0x2A0000 ota_1          (student app slot B — 1.25 MB)

    0x3E0000 user_data      (badge config NVS — never touched by OTA)
    -----------------------------------------------

### Partition Roles

| Partition  | Purpose                                                        |
|------------|----------------------------------------------------------------|
| NVS        | System WiFi and ESP-IDF config                                 |
| otadata    | Tracks active OTA slot; erasing forces factory boot            |
| phy_init   | RF calibration data                                            |
| factory    | This loader application (never overwritten by OTA)             |
| ota_0      | Student application slot A; also used as loader staging area   |
| ota_1      | Student application slot B; also used as loader staging area   |
| user_data  | Badge nickname, SSID, password, manifest URL (persists via OTA)|

### Factory Partition Space Usage (v4.1)

| Item | Bytes | KB | % |
| ---- | ----- | --- | --- |
| Binary | 1,139,600 | 1,113 | 87% |
| Partition | 1,310,720 | 1,280 | 100% |
| Free | 171,120 | 167 | 13% |

------------------------------------------------------------------------

## Why Two OTA Partitions Exist

OTA updates must **never overwrite the currently running firmware**.
Two slots allow safe updates and also serve a second purpose: when a
factory loader self-update is pending, the **inactive OTA slot is used
as a staging area** for the new loader binary.  otadata is never
modified during staging, so the student app is fully preserved.

### Example — Student App Update

    running → ota_0
    download new app → ota_1 (marked bootable, otadata updated)
    reboot → running ota_1
    next update → writes to ota_0

### Example — Factory Loader Self-Update

    running factory loader (student app in ota_0, marked bootable)
    download new loader binary → ota_1 (raw write, otadata NOT touched)
    write RTC flag → esp_restart()
    bootloader detects RTC flag → copies ota_1 → factory partition
    clears flag, continues boot
    otadata still points to ota_0 → student app resumes

------------------------------------------------------------------------

### What Happens If an Update Fails

If power is lost during a student-app download:

    ota_0 remains intact
    ota_1 incomplete / not marked bootable
    device continues booting from ota_0

If power is lost during a factory loader self-update (during the
bootloader copy step):

    factory partition partially written
    RTC flag was already cleared before copy started
    bootloader may not find a valid factory image
    recovery via USB serial flash required

ESP-IDF also supports **automatic rollback** if the new firmware crashes
before calling `esp_ota_mark_app_valid_cancel_rollback()`.

------------------------------------------------------------------------

## Student App / Factory Loader Relationship

Once a student app is installed, the bootloader boots the OTA slot
directly on every plain reset.  To re-enter the factory loader:

- **User gesture:** Press RESET, then press BOOT within 500 ms.
  `factory_switch` erases otadata; bootloader falls back to factory.

- **Programmatically** (from within the student app):

      const esp_partition_t *factory =
          esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                   ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
      esp_ota_set_boot_partition(factory);
      esp_restart();

------------------------------------------------------------------------

## PSRAM Usage

The ESP32-S3 module includes embedded PSRAM.

> **Hardware note (rev 0.2 silicon):** The current boards use ESP32-S3
> rev 0.2, which has a known errata where PSRAM initialisation fails
> (`PSRAM ID read error: 0x00000000`).  PSRAM is physically present but
> unavailable at runtime.  `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` allows the
> app to continue booting.  The factory loader and OTA manager operate
> entirely from internal RAM.

PSRAM cannot store boot code; it becomes available only after the
bootloader runs.

------------------------------------------------------------------------

## Installing Applications via OTA

Steps (menu item "OTA App Download"):

1. Connect to configured WiFi
2. Download application catalog from the manifest URL stored in `user_data`
3. Display icon tiles for available apps
4. User selects an app
5. Download firmware (streamed directly to flash in 8 KB chunks)
6. SHA-256 verified inline; abort on mismatch
7. Mark inactive OTA partition bootable; update otadata
8. Reboot into new firmware

------------------------------------------------------------------------

## Reset Options

### Reset Wifi/Config

Erases the `user_data` NVS partition only.  Student app is kept.

    erase user_data NVS
    reboot

Badge re-runs the WiFi captive portal on next factory-loader entry.

### Full Factory Reset

Erases everything student-related.  **Blinking red LED warning shown
during confirmation.**

    erase user_data NVS
    erase ota_0
    erase ota_1
    erase otadata
    reboot

After reboot the badge is in the same state as a freshly flashed board.

------------------------------------------------------------------------

## Serial Flash Mode (USB Program)

Allows full firmware flashing using USB or UART.

Enter mode (shown on-screen from menu item "USB Program"):

    Hold BOOT (IO0)
    Press RESET
    Release RESET
    Release BOOT

Flash using:

    idf.py -p <PORT> flash
    -- or --
    esptool.py write_flash

This can restore the bootloader, partition table, and factory loader.

------------------------------------------------------------------------

## microSD Recovery System

A microSD card can contain a **complete factory firmware bundle**.

> **Note:** microSD recovery is stubbed as "coming soon" in the current
> firmware.  The menu item is reserved for a future update.

### Recovery Card Layout

    /firmware/
        bootloader.bin
        partition-table.bin
        factory.bin
        ota0.bin
        ota1.bin

### Creating a Firmware Bundle

    esptool.py merge_bin -o factory_bundle.bin \
        0x1000  bootloader.bin \
        0x8000  partition-table.bin \
        0x20000 factory.bin

------------------------------------------------------------------------

## Recommended Recovery Hierarchy

    1 Factory loader self-update (automatic, over WiFi, on each boot)
    2 OTA app reinstall (menu: OTA App Download)
    3 microSD recovery (future — menu: SDCard Apps)
    4 USB serial flashing (menu: USB Program)
    5 ROM download mode (hold BOOT + RESET)

This ensures the badge can **always be restored**.

------------------------------------------------------------------------

## Typical Classroom Workflow

Normal use:

    Power on badge
    Student program runs directly (plain reset — factory_switch 500 ms window passes, OTA boots)

Enter factory loader (to install a new assignment):

    Press RESET on the badge
    While board is resetting, press BOOT within ~500 ms
    Factory loader appears (splash screen, then menu)

First-time setup (fresh badge):

    Power on — no student app installed
    Factory loader runs automatically
    WiFi configuration portal appears (QR code on phone)
    Student scans QR, enters SSID / password / nickname
    Loader menu appears — select "OTA App Download"

Loader self-update (automatic):

    Enter factory loader (BOOT at reset)
    Splash screen plays while WiFi connects in background
    If a newer loader is available, confirmation screen appears
    Press A to update; badge applies update and continues to menu

------------------------------------------------------------------------

## Component Map

| Component              | Location                                | Purpose                                                      |
|------------------------|-----------------------------------------|--------------------------------------------------------------|
| `factory_switch`       | `bootloader_components/factory_switch/` | Bootloader hook: RTC update apply, BOOT detect, otadata erase|
| `factory_self_update`  | `factory_self_update/`                  | Background WiFi check; loader self-update download + staging |
| `buttons`              | `buttons/`                              | 6-button GPIO driver; time-based debounce state machine      |
| `loader_menu`          | `loader_menu/`                          | 6-item menu; dispatches all actions                          |
| `portal_mode`          | `portal_mode/`                          | SoftAP captive portal with QR code for WiFi setup            |
| `ota_manager`          | `ota_manager/`                          | Streaming OTA download and flash from manifest URL           |
| `display`              | `display/`                              | ILI9341 SPI driver, fonts, QR rendering                      |
| `leds`                 | `leds/`                                 | WS2813B 24-LED addressable chain (RMT DMA); warning blink    |
| `splash_screen`        | `splash_screen/`                        | BYUI logo animation                                          |
| `wifi_config`          | `wifi_config/`                          | NVS: SSID, password, nickname, manifest URL                  |

### Tools

| Script                           | Purpose                                                       |
|----------------------------------|---------------------------------------------------------------|
| `tools/publish_bootloader.sh`    | Build, version-stamp, SHA-256, publish loader to GitHub Pages |
| `tools/publish.sh`               | Build and publish student app catalog to GitHub Pages         |
| `tools/build_index.py`           | Generate index.html for OTA app listing                       |

------------------------------------------------------------------------

## Summary

Key design decisions:

- Loader stored in **factory partition** — cannot be overwritten by OTA
- Student apps stored in **OTA slots** (ota_0 / ota_1)
- **`factory_switch` bootloader hook** handles all boot routing:
  - Check RTC flag → apply staged loader update if present
  - Software reset → otadata intact → boot wherever app pointed
  - Hardware reset, BOOT NOT pressed (500 ms window) → OTA app boots directly
  - Hardware reset, BOOT pressed within 500 ms → erase otadata → factory boots
- **BOOT button** (GPIO 0, 470 Ω pull-up, no debounce cap) used for
  factory escape — reliable in bootloader context unlike nav buttons
  which have 10 µF caps requiring long settle times
- **Factory loader self-update**: background WiFi check during splash;
  confirmation screen with blinking red LED warning; staged in inactive
  OTA slot; applied by bootloader on next restart; student app preserved
- **LED warnings**: dangerous confirmations (factory reset, loader update)
  blink all LEDs dim red at 1 Hz while waiting for user decision
- WiFi config done once via QR-code captive portal; stored in `user_data`
- OTA install streams directly to flash (8 KB chunks); no PSRAM required
- PSRAM unavailable on current rev 0.2 hardware (silicon errata)
- microSD loading/recovery stubbed; reserved for future update
- ROM bootloader guarantees last-resort flashing via BOOT + RESET

This architecture provides a **robust, classroom-friendly firmware
system** for the Namebadge project.
