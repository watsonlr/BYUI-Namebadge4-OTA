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
The factory loader is entered by holding **A + B buttons during reset or
power-up** (held for ~550 ms while the bootloader reads them).

------------------------------------------------------------------------

## Boot Process

### Stage 1 — ROM Bootloader

Location: **Inside the ESP32-S3 silicon**

Responsibilities:

- Runs immediately after reset
- Initializes SPI flash interface
- Loads the second-stage bootloader from flash

This code is permanent and **cannot be erased or modified**.

------------------------------------------------------------------------

### Stage 2 — ESP-IDF Second-Stage Bootloader + factory_switch hook

Location:

    Flash address: 0x1000

The second-stage bootloader is extended by the **`factory_switch`**
component (`bootloader_components/factory_switch/factory_switch.c`),
which runs via the `bootloader_after_init()` hook before any partition
is loaded.

`factory_switch` responsibilities:

- On **software reset**: leave otadata intact (the app set it before
  restarting) and return immediately — no GPIO reads, no delay.
- On **hardware reset** (power-on, RESET button, watchdog, …):
  - Configure GPIO 34 (A) and GPIO 33 (B) as inputs with pull-ups.
  - Wait **550 ms** for the on-board debounce capacitors (~10 µF)
    to charge through the internal pull-ups.
  - Read both pins.  If **both LOW** (A+B held): erase otadata so
    the bootloader falls back to the factory partition.
  - Otherwise: leave otadata intact so the bootloader boots the OTA
    app directly.

After `factory_switch` returns, the standard bootloader reads otadata
and selects the boot partition normally.

------------------------------------------------------------------------

### Stage 3 — Factory Loader Application (when entered)

Location:

    factory partition (0x20000)

This is the **BYUI-Namebadge-OTA** firmware.  It runs only when:

- A+B was held at reset (A+B escape), **or**
- No valid OTA image exists (fresh badge or after "Reset to Blank Canvas")

Responsibilities:

- Display the interactive loader menu
- Configure WiFi via captive portal (QR code on phone)
- Install firmware via OTA download
- Execute bare-metal / serial flash guidance
- Stub placeholders for microSD app loading and SD recovery update

------------------------------------------------------------------------

## Boot Decision Logic

    Power On / Hardware Reset
       |
    ROM Bootloader
       |
    Second-Stage Bootloader
       |
    factory_switch hook  (bootloader_after_init)
       |
       |── Configure GPIO34(A) + GPIO33(B): input + pull-up
       |── Wait 550 ms (debounce cap settle)
       |── Read A and B
       |
       |── A+B both LOW (held) ──────> erase otadata
       |                                    |
       |── A+B not both held ──────> leave otadata intact
       |
    Bootloader reads otadata
       |
       |── otadata valid (OTA app installed) ──────> boot OTA app directly
       |
       └── otadata blank (no OTA app, or A+B erased it) ──> boot factory
                                                                  |
                                                             buttons_init()
                                                             display_init()
                                                             leds_init()
                                                             splash_screen()
                                                                  |
                                                             wifi_config_is_configured()?
                                                                  |
                                                           NO ────┴──── YES
                                                           |             |
                                                       portal        loader menu
                                                           \             /
                                                            loader menu

Startup latency on plain reset with a valid OTA app installed:
**~550 ms** (bootloader cap-settle wait) + normal single boot time.

------------------------------------------------------------------------

## Factory Loader UI Flow

When the factory loader UI is entered (A+B held, or no student app):

    1. Full peripheral init (display, LEDs, NVS)
    2. Splash screen animation + BYUI-blue LED glow
    3. If not WiFi-configured → captive portal (QR code on phone)
    4. Loader menu (interactive)

------------------------------------------------------------------------

## Loader Menu

Navigation: **Up / Down** to move, **A or Right** to select.

    ┌─────────────────────────────────────┐
    │         BYUI Badge Loader           │  ← navy-blue header
    ├─────────────────────────────────────┤
    │ ▶  1. OTA App Download              │  ← highlighted item
    │    2. Load from SD Card             │
    │    3. Configure WiFi                │
    │    4. Bare-metal / Flash            │
    │    5. Update SD Recovery            │
    ├─────────────────────────────────────┤
    │    Up/Dn:move   A/Rt:select         │  ← hint bar
    └─────────────────────────────────────┘

### Item Descriptions

**1. OTA App Download**\
Connect to WiFi, fetch the manifest from the configured URL, download
and flash student firmware to the inactive OTA slot, then reboot.

**2. Load from SD Card**\
*Coming soon* — stub screen is shown with a brief explanation.

**3. Configure WiFi**\
Re-run the captive portal to change SSID, password, nickname, or
manifest URL.

**4. Bare-metal / Flash**\
Display `idf.py flash` / `esptool.py` instructions.  The user manually
enters ROM download mode via IO0 + RESET.

**5. Update SD Recovery**\
*Coming soon* — stub screen is shown.

------------------------------------------------------------------------

## Button Assignments

| Button | GPIO | Loader Role                                  |
|--------|------|----------------------------------------------|
| A      | 34   | Select / Confirm; A+B combo = factory escape |
| B      | 33   | A+B combo = factory escape; B alone reserved |
| Up     | 11   | Scroll menu up                               |
| Down   | TBD  | Scroll menu down                             |
| Left   | 21   | Scroll up (alias for Up)                     |
| Right  | TBD  | Select (alias for A)                         |
| BOOT   | 0    | ROM download mode (hold + RESET)             |

All buttons are **active LOW** with internal pull-ups and ~10 µF
hardware debounce capacitors.

> **Note:** GPIO 11 (Up) and GPIO 21 (Left) are LP I/O pads.  A
> spin-wait on these pins can block forever after software reset.  The
> button driver uses a time-based state machine (`esp_timer_get_time()`)
> — no spin-waits anywhere.

------------------------------------------------------------------------

## Flash Memory Layout

Current partition table (`partitions.csv`):

    Flash Memory Map
    -----------------------------------------------
    0x0000   ROM bootloader (internal)
    0x1000   second-stage bootloader
    0x8000   partition table
    0x9000   NVS            (system config)
    0xF000   otadata        (OTA boot selection)
    0x11000  phy_init       (PHY calibration)

    0x20000  factory        (this loader — 1.25 MB)

    0x160000 ota_0          (student app slot A — 1.25 MB)
    0x2A0000 ota_1          (student app slot B — 1.25 MB)

    0x3E0000 user_data      (badge config NVS — never touched by OTA)
    -----------------------------------------------

### Partition Roles

| Partition  | Purpose                                               |
|------------|-------------------------------------------------------|
| NVS        | System WiFi and ESP-IDF config                        |
| otadata    | Tracks active OTA slot                                |
| phy_init   | RF calibration data                                   |
| factory    | This loader application                               |
| ota_0      | Student application slot A                            |
| ota_1      | Student application slot B                            |
| user_data  | Badge nickname, SSID, manifest URL (persists via OTA) |

The **factory loader is never overwritten by OTA updates**.

------------------------------------------------------------------------

## Why Two OTA Partitions Exist

OTA updates must **never overwrite the currently running firmware**.

Two slots allow safe updates.

### Example

Current firmware:

    running → ota_0

New firmware download:

    write new firmware → ota_1

After reboot:

    running → ota_1

Next update writes back to:

    ota_0

This alternates forever.

------------------------------------------------------------------------

### What Happens If an Update Fails

If power is lost during download:

    ota_0 remains intact
    ota_1 incomplete

Device can still boot from the previous firmware.

ESP-IDF also supports **automatic rollback** if the new firmware crashes
before calling `esp_ota_mark_app_valid_cancel_rollback()`.

------------------------------------------------------------------------

## Student App / Factory Loader Relationship

Once a student app is installed, the bootloader boots the OTA slot
directly on every plain reset.  To re-enter the factory loader the
student app can call:

    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    esp_ota_set_boot_partition(factory);
    esp_restart();

Alternatively, the user can hold **A+B at reset** — `factory_switch`
erases otadata, causing the bootloader to fall back to factory.

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

Steps (menu item 1):

1. Connect to configured WiFi
2. Download application manifest from the URL stored in `user_data`
3. Select application
4. Download firmware (streamed directly to flash in 8 KB chunks)
5. SHA-256 verified inline; abort on mismatch
6. Write firmware to inactive OTA slot
7. Reboot into new firmware

ESP-IDF helper function:

    esp_ota_get_next_update_partition(NULL);

------------------------------------------------------------------------

## Reset to Blank Canvas

Erases student firmware while keeping loader intact.

Steps:

    erase ota_0
    erase ota_1
    erase otadata
    reboot

After reboot the badge returns to the loader (no OTA app → factory UI
is entered automatically even without A+B).

------------------------------------------------------------------------

## Serial Flash Mode (Bare-metal)

Allows full firmware flashing using USB or UART.

Enter mode (shown on-screen from menu item 4):

    Hold BOOT (IO0)
    Press RESET
    Release RESET
    Release BOOT

Flash using:

    idf.py -p <PORT> flash
    -- or --
    esptool.py write_flash

This restores:

- bootloader
- partition table
- factory loader

------------------------------------------------------------------------

## microSD Recovery System

A microSD card can contain a **complete factory firmware bundle**.

When the badge boots with **A+B held and a recovery card inserted**, the
loader can restore firmware automatically.

### Recovery Card Layout

    /firmware/
        bootloader.bin
        partition-table.bin
        factory.bin
        ota0.bin
        ota1.bin

or

    /firmware/
        factory_bundle.bin

> **Note:** microSD recovery is stubbed as "coming soon" in the current
> firmware.  The menu item is reserved for a future update.

------------------------------------------------------------------------

### Creating a Firmware Bundle

Use ESP-IDF and esptool:

    esptool.py merge_bin -o factory_bundle.bin \
        0x1000  bootloader.bin \
        0x8000  partition-table.bin \
        0x20000 factory.bin

Optional OTA apps may also be included.

------------------------------------------------------------------------

### Recovery Procedure (future)

User steps:

1. Insert recovery microSD card
2. Hold **A + B**
3. Press reset
4. Wait ~10 seconds
5. Badge automatically restores firmware
6. Device reboots

------------------------------------------------------------------------

## Recommended Recovery Hierarchy

    1 OTA reinstall (menu item 1)
    2 microSD recovery (future — menu item 2)
    3 USB serial flashing (menu item 4)
    4 ROM download mode

This ensures the badge can **always be restored**.

------------------------------------------------------------------------

## Typical Classroom Workflow

Normal use:

    Power on badge
    Student program runs directly (~550 ms bootloader wait)

Enter factory loader (from student app, or on a fresh badge):

    Hold A+B
    Press Reset
    Release Reset (keep holding A+B for ~550 ms)
    Factory loader appears

Install a new assignment:

    Enter factory loader (A+B at reset)
    Select "OTA App Download"
    Badge connects to WiFi, downloads, reboots into new app

First-time setup (fresh badge):

    Power on — no student app installed
    Factory loader runs automatically
    WiFi configuration portal appears (QR code on phone)
    Student scans QR, enters SSID / password / nickname
    Loader menu appears — select "OTA App Download"

Recover a badge (future):

    Insert microSD recovery card
    Hold A+B
    Reset board
    Badge restores firmware

------------------------------------------------------------------------

## Component Map

| Component        | Location                                | Purpose                                               |
|------------------|-----------------------------------------|-------------------------------------------------------|
| `factory_switch` | `bootloader_components/factory_switch/` | Bootloader hook: A+B detect, otadata erase/preserve   |
| `buttons`        | `buttons/`                              | 6-button GPIO driver; debounce state machine          |
| `loader_menu`    | `loader_menu/`                          | 5-item menu; dispatches OTA, portal, stubs            |
| `portal_mode`    | `portal_mode/`                          | SoftAP captive portal with QR code for WiFi setup     |
| `ota_manager`    | `ota_manager/`                          | Streaming OTA download and flash from manifest URL    |
| `display`        | `display/`                              | ILI9341 SPI driver, fonts, QR rendering               |
| `leds`           | `leds/`                                 | WS2813B addressable LED chain (RMT DMA)               |
| `splash_screen`  | `splash_screen/`                        | BYUI logo scroll-up animation                         |
| `wifi_config`    | `wifi_config/`                          | NVS: SSID, password, nickname, manifest URL           |

------------------------------------------------------------------------

## Summary

Key design decisions:

- Loader stored in **factory partition** — cannot be overwritten by OTA
- Student apps stored in **OTA slots** (ota_0 / ota_1)
- **`factory_switch` bootloader hook** handles all boot routing:
  - Software reset → otadata intact → boot wherever app pointed
  - Hardware reset, A+B NOT held → otadata intact → OTA app boots directly
  - Hardware reset, A+B held → erase otadata → factory loader boots
- **550 ms bootloader wait** on hardware reset (required for debounce cap settle)
- No A+B + no student app → factory loader UI automatically
- WiFi config done once via QR-code captive portal; stored in `user_data`
- OTA install streams directly to flash (8 KB chunks); no PSRAM required
- PSRAM unavailable on current rev 0.2 hardware (silicon errata)
- microSD loading/recovery stubbed; reserved for future update
- ROM bootloader guarantees last-resort flashing

This architecture provides a **robust, classroom-friendly firmware
system** for the Namebadge project.
