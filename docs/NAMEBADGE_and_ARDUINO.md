# BYUI eBadge V4 — Arduino Support

This document explains how students can write and upload Arduino sketches to
the BYUI eBadge V4 without destroying the permanent BYU-I factory loader.

---

## The Problem

The eBadge V4 ships with a permanent factory loader stored in the **factory
partition** (`0x20000`).  This loader provides WiFi setup, OTA app download,
and recovery tools that must survive any student code upload.

When a student opens the Arduino IDE and clicks **Upload**, the IDE uses
`esptool` under the hood and flashes **three separate regions** of flash:

| What Arduino flashes | Flash address | Effect on badge |
|---|---|---|
| Arduino's 2nd-stage bootloader | `0x1000` | **Overwrites `factory_switch`** — BOOT-button escape is gone |
| Arduino's partition table | `0x8000` | **Overwrites the badge layout** — factory partition is lost |
| The compiled sketch | `0x10000` (default) | Lands before the factory partition; badge is already broken |

A student using a generic **ESP32-S3 Dev Module** board profile and clicking
Upload once will brick the badge.  The factory loader binary at `0x20000` is
physically untouched, but it is unreachable because the partition table that
describes it no longer exists and the bootloader that could jump to it has
been replaced.

> **There is no software lock that prevents this.**  The ROM bootloader
> handles download mode directly; no application code runs during a USB
> flash.  Protection must come from the tools used on the student's PC.

---

## The Two-Component Solution

Two components work together to make Arduino safe on the badge.

### Component 1 — BYUI Arduino Board Package

A custom Arduino board package installed on the **student's PC** replaces
the generic ESP32-S3 board profile.  It bundles the badge-specific binaries
and configures Arduino to flash only the correct regions.

**What the board package contains:**

```
byui-badge-arduino/
├── boards.txt                    ← board definition and upload parameters
├── platform.txt                  ← toolchain and esptool settings
├── bootloader/
│   └── factory_switch.bin        ← built from bootloader_components/factory_switch/
├── partitions/
│   └── byui_badge.csv            ← copy of this repo's partitions.csv
├── ota_data/
│   └── ota_data_initial.bin      ← pre-built otadata pointing to ota_0
└── README.md                     ← setup steps for students
```

**What it enforces:**

| Parameter | Value | Why |
|---|---|---|
| Bootloader binary | `factory_switch.bin` | Preserves BOOT-button factory escape |
| Partition table | `byui_badge.csv` | Preserves factory partition layout |
| App flash offset | `0x160000` (ota_0) | Sketch lands in student slot, not factory |
| otadata | `ota_data_initial.bin` pointing to ota_0 | Badge boots sketch after upload |

**Key `boards.txt` entries (excerpt):**

```ini
byui_badge.name=BYUI eBadge V4
byui_badge.build.mcu=esp32s3
byui_badge.build.flash_freq=80m
byui_badge.build.flash_mode=dio
byui_badge.build.flash_size=4MB
byui_badge.build.partitions=byui_badge
byui_badge.upload.maximum_size=1310720
byui_badge.upload.tool=esptool_py
byui_badge.bootloader.tool=esptool_py
byui_badge.bootloader.bin={runtime.platform.path}/bootloader/factory_switch.bin
```

With this profile selected, Arduino Upload flashes:
- `factory_switch.bin` → `0x1000` (bootloader preserved)
- `byui_badge.csv` compiled table → `0x8000` (partition layout preserved)
- `ota_data_initial.bin` → `0xF000` (badge boots sketch on next reset)
- Student's sketch → `0x160000` (ota_0, student slot)

The factory partition at `0x20000` is **never touched**.

---

### Component 2 — "Arduino Starter" OTA App

The board package protects the badge during upload, but students still need
to know it exists and have it installed before they ever open Arduino IDE.
The **Arduino Starter OTA app** is the on-ramp that enforces this order.

**What it is:**

A minimal ESP-IDF application published to the OTA catalog alongside regular
student apps.  Students download it through the normal BYU-I Loader menu
(WiFi setup → OTA App Download) before doing anything with Arduino.

**What it does:**

1. Downloads and runs from `ota_0` — so `otadata` already points to `ota_0`
   before the student ever opens Arduino IDE.
2. Calls `esp_ota_mark_app_valid_cancel_rollback()` so it stays running and
   does not roll back to the factory loader on the next reset.
3. Displays step-by-step Arduino setup instructions on the badge screen,
   including the URL of the board package.
4. Serves as proof that the OTA pipeline works before the student touches
   a USB cable for programming.

**What it shows on screen:**

```
┌──────────────────────────────────────┐
│         Arduino Ready!               │
├──────────────────────────────────────┤
│  Before using Arduino IDE:           │
│                                      │
│  1. Install the BYUI board package   │
│     (one-time setup on your PC):     │
│                                      │
│  github.com/watsonlr/                │
│    namebadge-apps                    │
│    (see arduino/ folder)             │
│                                      │
│  2. In Arduino IDE, select:          │
│     Tools > Board > BYUI eBadge V4   │
│                                      │
│  3. Connect USB, write your sketch,  │
│     and click Upload.                │
└──────────────────────────────────────┘
```

---

## Combined Student Workflow

```
Badge                                    Student's PC
─────                                    ────────────
Step 1 — On-badge setup
  Connect to badge WiFi (captive portal)
  Enter SSID / password / nickname
  Badge connects to WiFi

Step 2 — Download Arduino Starter (OTA)
  BYU-I Loader → OTA App Download
  Select "Arduino Starter"              ← published in OTA catalog
  Badge flashes to ota_0, reboots
  Screen shows setup instructions

Step 3 — Install board package (one-time, PC)
                                         Visit github.com/watsonlr/namebadge-apps
                                         Follow arduino/ README
                                         Add board URL to Arduino IDE
                                         Install "BYUI eBadge V4" board

Step 4 — Upload a sketch
                                         Open Arduino IDE
                                         Select Tools > Board > BYUI eBadge V4
                                         Select the correct COM/tty port
                                         Write or open a sketch
                                         Click Upload
  Badge receives sketch → ota_0
  factory_switch.bin + byui_badge.csv
  are written, keeping loader safe
  Badge reboots into student's sketch

Step 5 — Return to factory loader (any time)
  Press RESET on badge
  Within 500 ms, press BOOT button
  otadata erased → factory loader boots
  BYU-I Loader menu appears
```

---

## Recovery: What to Do If a Student Bricks the Badge

If a student uploads with the wrong board profile (e.g. generic ESP32-S3),
the `factory_switch` bootloader and partition table are overwritten.  The
BOOT-button escape no longer works because `factory_switch` is gone.

**Recovery requires an instructor with the factory project:**

```bash
# From the BYUI-Namebadge4-OTA repo root
source /path/to/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 flash
```

This restores:
- `factory_switch` bootloader
- Badge partition table
- Factory loader app

The student's NVS credentials (`user_data`) are also erased; the badge will
re-run the WiFi captive portal on next boot.

> Recovery takes under two minutes and requires a data-capable USB cable.
> An instructor can keep a flashed badge as a "known-good" reference image
> using `esptool.py read_flash` for even faster recovery.

---

## Why the "Arduino Starter First" Requirement Helps

Requiring students to download the Arduino Starter app before touching
Arduino IDE creates a natural gate:

| Without this gate | With this gate |
|---|---|
| Student installs Arduino, picks ESP32-S3 Dev Module, clicks Upload | Student must complete WiFi setup + OTA download first |
| Badge bricked on first upload | Student is in a guided flow with instructions on screen |
| Instructor intervention required | Board package install is already prompted before any USB action |

The OTA download also validates the full WiFi + download pipeline.  If
something is wrong with the student's network or badge config, it surfaces
here rather than mid-Arduino-session.

---

## Build Checklist

Both components still need to be built.  Neither exists yet.

### Board Package

- [ ] Create `byui-badge-arduino/` folder in the `namebadge-apps` repo
- [ ] Build `factory_switch.bin` from current bootloader source and copy in
- [ ] Copy `partitions.csv` from this repo as `byui_badge.csv`
- [ ] Generate `ota_data_initial.bin` pointing to `ota_0`
  - Use `gen_esp32part.py` or a small Python script with the ESP-IDF OTA data format
- [ ] Write `boards.txt` with correct offsets and binary paths
- [ ] Write `platform.txt`
- [ ] Write `README.md` with Arduino IDE install instructions
- [ ] Test: Upload a blink sketch — verify factory loader survives

### Arduino Starter OTA App

- [ ] Create `arduino_starter/` ESP-IDF project
- [ ] `app_main`: init display, call `esp_ota_mark_app_valid_cancel_rollback()`,
  show instruction screen, loop on button input
- [ ] Add to OTA catalog via `tools/publish.sh`
- [ ] Test: Download via BYU-I Loader, verify screen content

---

## Notes on `ota_data_initial.bin` Format

The otadata partition is 8 KB (`0xF000`–`0x10FFF`), two identical 4 KB
sectors.  Each sector holds an `esp_ota_select_entry_t` struct:

```c
typedef struct {
    uint32_t ota_seq;        /* sequence number; higher = preferred boot */
    uint8_t  seq_label[20];  /* unused padding                           */
    uint32_t ota_state;      /* ESP_OTA_IMG_VALID = 0xFFFFFFFF           */
    uint32_t crc;            /* CRC32 of the first 28 bytes              */
} esp_ota_select_entry_t;   /* 32 bytes total                            */
```

To boot `ota_0`: `ota_seq = 1`, `ota_state = 0xFFFFFFFF`, correct CRC.
To boot `ota_1`: `ota_seq = 2`.

The ESP-IDF tool `gen_esp32part.py` does not generate this file; it must be
generated with a small script or extracted from a known-good flash.
`esptool.py read_flash 0xF000 0x2000 ota_data_initial.bin` from a badge that
has successfully run an OTA app is the simplest approach.

---

*Last updated: 2026-03-21*
