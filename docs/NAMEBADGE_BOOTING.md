# Namebadge Boot Architecture and Firmware Management

## Overview

This document describes the boot architecture, memory layout, OTA update
strategy, and recovery procedures for the Namebadge board based on the
**ESP32‑S3 with external PSRAM**.

Design goals:

-   Persistent loader that cannot be erased by student programs
-   Safe firmware updates via OTA
-   Simple classroom user interaction
-   Robust recovery mechanisms
-   Optional microSD-based full firmware restoration

The badge normally boots **directly into the student application**.\
The factory loader is entered by holding **A + B buttons during reset or
power‑up** (held simultaneously for ~150 ms).

------------------------------------------------------------------------

## Boot Process

### Stage 1 — ROM Bootloader

Location: **Inside the ESP32‑S3 silicon**

Responsibilities:

-   Runs immediately after reset
-   Initializes SPI flash interface
-   Loads the second‑stage bootloader from flash

This code is permanent and **cannot be erased or modified**.

------------------------------------------------------------------------

### Stage 2 — ESP‑IDF Second‑Stage Bootloader

Location:

    Flash address: 0x1000

Responsibilities:

-   Reads the partition table
-   Determines which application partition to boot
-   Loads selected firmware into memory

The second‑stage bootloader is intentionally small and contains **no
networking or UI logic**.

------------------------------------------------------------------------

### Stage 3 — Factory Loader Application

Location:

    factory partition

This is the **BYUI‑Namebadge‑OTA** firmware.

Responsibilities:

-   **Quick A+B detection** (~150 ms, before any display or network init)
-   Launch student applications (set OTA boot partition → restart)
-   Display the interactive loader menu
-   Configure WiFi via captive portal (QR code on phone)
-   Install firmware via OTA download
-   Execute bare‑metal / serial flash guidance
-   Stub placeholders for microSD app loading and SD recovery update

------------------------------------------------------------------------

## Boot Decision Logic

    Power On
       |
    ROM Bootloader
       |
    Second-Stage Bootloader  →  loads factory partition
       |
    Factory Loader App  (BYUI-Namebadge-OTA)
       |
    buttons_init()  +  buttons_held(A+B, 150 ms)
       |
       |── A+B held ──────────────────────────> Factory Loader UI
       |                                             |
       |── A+B not held                         wifi config portal
              |                                  (if not configured)
              |                                      |
              ├── valid OTA app found ──────>    Loader Menu
              |   esp_ota_set_boot_partition()
              |   esp_restart()
              |   [ second-stage bootloader ]
              |   [ loads OTA partition     ]
              |   [ student app runs        ]
              |
              └── no valid OTA app ──────────> Factory Loader UI
                                                (same path as A+B)

Startup latency before student app launch is **< 200 ms** when A+B are
not held and a valid OTA image is installed.

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
Re‑run the captive portal to change SSID, password, nickname, or
manifest URL.

**4. Bare‑metal / Flash**\
Display `idf.py flash` / `esptool.py` instructions.  The user manually
enters ROM download mode via IO0 + RESET.

**5. Update SD Recovery**\
*Coming soon* — stub screen is shown.

------------------------------------------------------------------------

## Button Assignments

| Button | GPIO | Loader Role                               |
|--------|------|-------------------------------------------|
| A      | 38   | Select / Confirm                          |
| B      | 18   | A+B combo enters loader; B alone reserved |
| Up     | 17   | Scroll menu up                            |
| Down   | 16   | Scroll menu down                          |
| Left   | 14   | Scroll up (alias for Up)                  |
| Right  | 15   | Select (alias for A)                      |
| BOOT   | 0    | ROM download mode (hold + RESET)          |

All buttons are **active LOW** with internal pull-ups.

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
| NVS        | System WiFi and ESP‑IDF config                        |
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

ESP‑IDF also supports **automatic rollback** if the new firmware crashes.

------------------------------------------------------------------------

## Student App / Factory Loader Relationship

Once a student app is installed the second-stage bootloader boots the
OTA slot directly on subsequent power-ups.  To re-enter the factory
loader the student app should call:

    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    esp_ota_set_boot_partition(factory);
    esp_restart();

Alternatively, the **"Reset to blank canvas"** workflow (erase otadata)
always forces the factory loader on next boot.

------------------------------------------------------------------------

## PSRAM Usage

The ESP32‑S3 module includes external PSRAM.

Important:

**PSRAM cannot store boot code.**\
It becomes available only **after the bootloader runs**.

PSRAM is used by the factory loader for:

-   OTA download buffers
-   JSON manifest parsing
-   Display framebuffers
-   Large FreeRTOS stacks

Example framebuffer size:

    320 × 240 × 2 bytes ≈ 150 KB

PSRAM is ideal for this.

------------------------------------------------------------------------

## Installing Applications via OTA

Steps (menu item 1):

1.  Connect to configured WiFi
2.  Download application manifest from the URL stored in `user_data`
3.  Select application
4.  Download firmware
5.  Write firmware to inactive OTA slot
6.  Reboot into new firmware

ESP‑IDF helper function:

    esp_ota_get_next_update_partition(NULL);

------------------------------------------------------------------------

## Reset to Blank Canvas

Erases student firmware while keeping loader intact.

Steps:

    erase ota_0
    erase ota_1
    erase otadata
    reboot

After reboot the badge returns to the loader (no OTA app → loader UI
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

-   bootloader
-   partition table
-   factory loader

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

Use ESP‑IDF and esptool:

    esptool.py merge_bin -o factory_bundle.bin \
        0x1000  bootloader.bin \
        0x8000  partition-table.bin \
        0x20000 factory.bin

Optional OTA apps may also be included.

------------------------------------------------------------------------

### Recovery Procedure (future)

User steps:

1.  Insert recovery microSD card
2.  Hold **A + B**
3.  Press reset
4.  Wait ~10 seconds
5.  Badge automatically restores firmware
6.  Device reboots

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
    Student program runs

Enter factory loader (from student app, or on a fresh badge):

    Hold A+B
    Press Reset
    Release Reset
    Factory loader appears

Install a new assignment:

    Enter factory loader (A+B at power-on)
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

| Component       | Location          | Purpose                                              |
|-----------------|-------------------|------------------------------------------------------|
| `buttons`       | `buttons/`        | 6-button GPIO driver; debounce; boot-time check      |
| `loader_menu`   | `loader_menu/`    | 5-item menu; dispatches OTA, portal, stubs           |
| `portal_mode`   | `portal_mode/`    | SoftAP captive portal with QR code for WiFi setup    |
| `ota_manager`   | `ota_manager/`    | Streaming OTA download and flash from manifest URL   |
| `display`       | `display/`        | ILI9341 SPI driver, fonts, QR rendering              |
| `leds`          | `leds/`           | WS2813B addressable LED chain (RMT DMA)              |
| `splash_screen` | `splash_screen/`  | BYUI logo scroll-up animation                        |
| `wifi_config`   | `wifi_config/`    | NVS: SSID, password, nickname, manifest URL          |

------------------------------------------------------------------------

## Summary

Key design decisions:

-   Loader stored in **factory partition** — cannot be overwritten by OTA
-   Student apps stored in **OTA slots** (ota_0 / ota_1)
-   Device normally boots student program directly (~150 ms overhead)
-   **A+B held ~150 ms at boot → factory loader UI**
-   No A+B + no student app → factory loader UI automatically
-   WiFi config done once via QR-code captive portal; stored in `user_data`
-   OTA install downloads from manifest URL stored during config
-   PSRAM used for buffers and graphics
-   microSD loading/recovery stubbed; reserved for future update
-   ROM bootloader guarantees last‑resort flashing

This architecture provides a **robust, classroom‑friendly firmware
system** for the Namebadge project.
