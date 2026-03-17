# Project Objectives — BYUI Namebadge OTA

## Vision

Build a **permanent, student-proof bootloader** for the BYUI eBadge V3.0 that:

- Lives in the ESP32-S3 **factory partition** and can never be accidentally erased by OTA
- Runs student apps in **OTA partition slots** so a bad app never destroys the loader
- Lets students configure, update, and reset their badge entirely from their phone — no laptop, no programmer, no USB cable needed for normal use
- Has a clear, documented path to restore the bootloader itself if it somehow gets overwritten

---

## Partition Architecture

Understanding the flash layout is essential to understanding why the loader is durable:

```
ROM Bootloader (in ESP32-S3 silicon — truly permanent, cannot be changed)
  └─► ESP-IDF Bootloader @ 0x1000 (can be re-flashed but rarely touched)
        └─► factory partition @ 0x20000  ← THIS LOADER LIVES HERE
              |
              ├── ota_0  ← student app slot
              ├── ota_1  ← student app slot
              └── ota_2  ← student app slot
```

**Why apps can't destroy the loader under normal operation:**
- The ESP-IDF `esp_https_ota` API is physically incapable of writing to the factory partition — it is hard-coded to target OTA slots only.
- The loader menu will never offer an option that calls `esp_partition_write()` on the factory region.
- `idf.py flash` (USB/UART) *can* overwrite the factory partition — this is the one way a student can break the invariant, and Objective 4 covers recovery from exactly that.

**What happens when an OTA app crashes:**
- The ESP-IDF bootloader's app rollback detects a failed or unconfirmed app and automatically falls back to the factory partition. No student action needed.

---

The loader presents a simple on-device menu (D-pad navigation, LCD display) and offers three top-level modes plus a hardware recovery path:

1. **Configure Device** — Wi-Fi SoftAP captive portal, phone-accessible
2. **Download App** — browse and install from a GitHub Pages catalog
3. **Reset to Canvas** — wipe user apps, return to clean state
4. **Recovery** — restore the loader itself if it gets overwritten

---

## Objective 1 — Board Configuration Portal

**Goal**: Let a student configure the badge from any phone or laptop, without needing a pre-existing Wi-Fi network.

### How it works
1. User selects **"Configure Device"** from the main menu (or holds **Button B** during power-on).
2. The badge starts a **Wi-Fi SoftAP** (SSID: `BYUI_NameBadge`, no password).
3. The badge displays a **QR code** on the LCD pointing to `http://192.168.4.1`.
4. A **captive portal** HTTP server runs on the badge (port 80) — iOS and Android will auto-redirect when the phone joins the AP; the student can also scan the QR code.
5. The portal page (served directly from the badge) provides:
   - **Wi-Fi credentials** — SSID + password for the network the badge should join for OTA
   - **Device nickname** — a friendly name stored in NVS (shown on the badge LCD)
   - **Manifest URL** — the GitHub Pages URL to fetch the app catalog from
   - **Factory reset** — erase all stored settings and return to defaults
6. On form submit, settings are written to NVS and the badge reboots to the main menu.

### Acceptance criteria
- [ ] SoftAP starts and QR code appears on LCD within 3 seconds
- [ ] Captive portal auto-redirects on iOS and Android (no manual URL entry)
- [ ] QR code scannable on the badge LCD from 30 cm away
- [ ] All settings persist across power cycles (NVS)
- [ ] Invalid / empty Wi-Fi SSID shows inline error — nothing written to NVS
- [ ] Button B cancels and returns to main menu without saving

---

## Objective 2 — GitHub Pages App Catalog (OTA Download)

**Goal**: Let a student browse and install any app from a centrally-maintained catalog hosted on GitHub Pages — no instructor laptop required once the badge has Wi-Fi credentials.

### How it works
1. User selects **"Download App"** from the main menu.
2. The badge connects to the stored Wi-Fi network.
3. An HTTPS GET fetches `manifest.json` from the configured GitHub Pages URL
   (default: `https://<org>.github.io/<catalog-repo>/manifest.json`).
4. The badge displays the app list on the LCD (name, version, short description).
5. User navigates with D-pad, selects an app with button A.
6. The binary is downloaded in chunks via `esp_https_ota` and written to the next available OTA slot (`ota_0`, `ota_1`, or `ota_2`).
7. On success, `esp_ota_set_boot_partition()` is called and the badge reboots into the new app.
8. The installed app can call `esp_ota_set_boot_partition(factory)` + `esp_restart()` to return to the loader at any time.

### Manifest format (`manifest.json`)
```json
{
  "version": 1,
  "apps": [
    {
      "name": "Game Launcher",
      "version": "2.1.0",
      "description": "Browse and launch arcade games",
      "url": "https://<org>.github.io/<catalog-repo>/apps/launcher.bin",
      "sha256": "<hex-digest>",
      "size_bytes": 524288
    }
  ]
}
```

### Acceptance criteria
- [ ] Manifest fetched over HTTPS (certificate bundle included in build)
- [ ] SHA-256 of downloaded binary verified before `esp_ota_set_boot_partition()` is called; mismatch aborts and shows error
- [ ] Progress bar shown on LCD during download
- [ ] Download can be cancelled mid-flight with button B (returns to menu, does not change boot partition)
- [ ] If Wi-Fi connection fails, a clear error message is shown with a retry option
- [ ] Installed app version is stored in NVS and shown in the menu ("currently installed: v2.1.0")

---

## Objective 3 — Bare-Metal Canvas Mode

**Goal**: Let a student wipe the current user app and put the badge into a clean state ready to accept completely new code — either from a USB cable or a future OTA push — as if it just came out of the box.

### How it works
1. User selects **"Reset to Canvas"** from the main menu (confirmation prompt required).
2. The loader:
   a. Erases all OTA app partitions (`ota_0`, `ota_1`, `ota_2`).
   b. Clears the `otadata` partition so the next boot falls back to the factory loader.
   c. Optionally erases user NVS keys (app-specific data only; Wi-Fi credentials are preserved unless "Full Reset" is chosen).
3. The badge reboots back into the factory loader with a blank slate.
4. **USB sub-mode (optional)**: if the user holds the Boot button during step 1, the badge reboots directly into **ESP32-S3 ROM download mode** (equivalent to `esptool.py chip_id` target), allowing a connected laptop to flash arbitrary firmware without any button gymnastics.

### Why "Canvas"?
Students in a class may want to start a brand-new experiment from scratch without carrying their laptop. After a Canvas reset, the badge is at exactly the same state as when the instructor handed it out — ready to receive any code via USB or OTA.

### Acceptance criteria
- [ ] Confirmation dialog ("Erase all apps? A=Yes, B=Cancel") prevents accidental wipes
- [ ] After reset, boot sequence correctly selects factory partition (verified via `esp_ota_get_boot_partition()`)
- [ ] "Canvas + USB" sub-mode restarts into ROM download mode (GPIO0 held LOW before `esp_restart()`)
- [ ] LCD shows clear status messages during erase ("Erasing app partitions… Done")
- [ ] Operation completes in under 5 seconds

---

---

## Objective 4 — Bootloader Self-Recovery

**Goal**: Give students and instructors a clear, minimal-friction path to re-install the factory loader if it gets overwritten — without any software installation, and without needing the badge to be in a fully bootable state.

### Why this is needed

The one scenario where durability breaks down: a student connects a USB cable and runs `idf.py flash` (the default full-flash command), which overwrites the factory partition with their own code. After this the badge may boot into something with no loader menu at all.

### Recovery — three tiers

Ordered from "no hardware needed" to "always works even if badge is dead":

---

#### Tier 1 — Automatic Rollback (transparent, no student action)

If a downloaded OTA app fails to boot (crash, watchdog, or invalid image), the ESP-IDF 2nd-stage bootloader detects an unconfirmed app and automatically falls back to the factory partition on the next power cycle.

Apps should call `esp_ota_mark_app_valid_cancel_rollback()` after a successful startup; if they crash before that call, rollback is silent and automatic.

> **Protects against bad OTA app code. Does not protect against `idf.py flash` overwriting factory.**

---

#### Tier 2 — SoftAP Recovery Flash (phone only — no USB, no laptop)

**Trigger**: Hold **Button A + Button B** simultaneously for 3 seconds during power-on.

Detected in the **custom 2nd-stage bootloader** (not in the application), so it works even if the factory partition contains garbage:

1. Custom bootloader samples GPIO38 (A) and GPIO18 (B) immediately after startup.
2. Both held → bootloader launches a minimal SoftAP + HTTP recovery environment instead of jumping to any app partition.
3. LCD shows **"Recovery Mode"** and a large **QR code**.
4. QR code encodes the GitHub Pages recovery URL:
   `https://watsonlr.github.io/BYUI-Namebadge-OTA/recovery`

From the recovery web page the student has two sub-options:

**Option A — Phone flash (zero hardware needed besides the badge itself):**
- The badge recovery AP exposes `POST http://192.168.4.1/flash`
- The recovery web page (running on the phone browser) fetches the latest `factory_loader.bin` from GitHub Pages and POSTs it to the badge
- Badge verifies SHA-256, writes to the factory partition, reboots

**Option B — USB flash via ESP Web Tools (laptop with Chrome or Edge, no install):**
- Recovery web page also links to an [ESP Web Tools](https://esphome.github.io/esp-web-tools/) manifest
- Student connects USB to laptop → click Connect → click Flash → done
- Uses the browser's built-in WebSerial API — no Python, no drivers, no `idf.py`

> **Why QR code on screen?** It always points to the live GitHub Pages URL where the recovery page and latest binary live. No printed labels to go out of date. URL also printed on the badge back as a fallback.

---

#### Tier 3 — ROM Download Mode (hardware-guaranteed, cannot be blocked by software)

The ESP32-S3 ROM bootloader lives **in chip silicon — it can never be erased or overwritten by any software, ever.**

Procedure:
1. Hold **BOOT (GPIO0)**, press and release **RESET (EN)**, then release BOOT.
2. Badge enumerates as a USB CDC device in ROM UART download mode.
3. Flash using any of:
   - **ESP Web Tools** (Chrome/Edge) — scan QR on badge label or visit recovery URL
   - `esptool.py --port COMx write_flash 0x20000 factory_loader.bin`
   - `idf.py -p COMx flash` from the project checkout

---

### Recovery QR / URL placement

| Location | Format |
|----------|--------|
| Badge LCD (Tier 2 mode) | Large QR + `byui.me/badge-recover` |
| Printed label on badge back | Short URL + BOOT+RST procedure |
| Class getting-started card | QR + recovery URL |

### Acceptance criteria

- [ ] Custom bootloader detects A+B hold on startup without jumping to any app partition
- [ ] QR code on LCD encodes correct recovery URL and is scan-readable at 30 cm
- [ ] Option A: phone POST completes; SHA-256 verified before writing factory partition; badge reboots into restored loader
- [ ] Option B: `manifest.json` on GitHub Pages always references the latest `factory_loader.bin` with correct SHA-256
- [ ] Tier 3: ROM download mode reachable via BOOT+RST (hardware — no code change needed)
- [ ] Recovery page clearly labels all paths and lists required equipment per path
- [ ] After any recovery path, badge boots normally into the restored loader

---

## Main Menu Layout (reference)

```
┌─────────────────────────────┐
│  BYUI eBadge  v1.0          │
│  My Badge                   │
├─────────────────────────────┤
│  ▶ 1. Download App          │
│    2. Configure Device      │
│    3. Reset to Canvas       │
├─────────────────────────────┤
│  Installed: Game Launcher   │
│             v2.1.0          │
└─────────────────────────────┘
    U/D = navigate   A = select
```

Recovery mode screen (Tier 2):

```
┌─────────────────────────────┐
│  !! RECOVERY MODE !!        │
│                             │
│  Scan to restore loader:    │
│                             │
│  [  QR CODE (large)  ]      │
│                             │
│  or: byui.me/badge-recover  │
└─────────────────────────────┘
```

---

## Cross-Cutting Concern — Hardware Revision Identification

### Problem

The eBadge hardware will evolve across board revisions. Firmware binaries compiled for V3.0 may not be compatible with V3.1 (different peripherals, pin changes, etc.). The bootloader must know which hardware revision it is running on so it can request — and validate — compatible firmware from the catalog.

### Constraints

- **No eFuse burns** during manufacturing (no provisioning step available)
- **No ADC version resistor** (PCB change required per revision, undesirable)
- Each ESP32-S3 chip has a **globally unique, factory-programmed 48-bit MAC address** (burned by Espressif into eFuse at chip manufacture — not by the badge assembler) readable via `esp_efuse_mac_get_default()`

### Chosen approach — MAC address registry

The MAC address is used as a stable, unique key. A static `registry.json` file maintained on the same GitHub Pages catalog site maps each badge's MAC to its hardware revision string. The bootloader resolves its own hardware revision at runtime by looking itself up in this file.

#### Runtime flow

```
Boot → read own MAC (esp_efuse_mac_get_default())
     → connect Wi-Fi
     → GET https://<org>.github.io/<catalog>/registry.json
     → look up MAC in registry
         found   → hw_rev = "v3.1"
         not found → hw_rev = "unknown"; show enrollment prompt on LCD
     → GET manifest.json
     → filter app list to entries compatible with hw_rev
     → display filtered list to student
```

#### `registry.json` format

```json
{
  "schema_version": 1,
  "default_hw_rev": "v3.0",
  "badges": [
    { "mac": "AA:BB:CC:DD:EE:FF", "hw_rev": "v3.0", "label": "Batch 2025A" },
    { "mac": "AA:BB:CC:DD:EE:00", "hw_rev": "v3.1", "label": "Batch 2026A" }
  ]
}
```

`default_hw_rev` is used when a MAC is not in the list (see enrollment below).

#### Updated `manifest.json` format

Each app entry gains a `hw_rev` compatibility field:

```json
{
  "schema_version": 2,
  "apps": [
    {
      "name": "Game Launcher",
      "version": "2.1.0",
      "description": "Browse and launch arcade games",
      "hw_rev": ["v3.0", "v3.1"],
      "url": "https://.../apps/launcher-v3.bin",
      "sha256": "<hex>",
      "size_bytes": 524288
    },
    {
      "name": "Game Launcher",
      "version": "2.2.0",
      "description": "Browse and launch arcade games (v3.1 hardware)",
      "hw_rev": ["v3.1"],
      "url": "https://.../apps/launcher-v31.bin",
      "sha256": "<hex>",
      "size_bytes": 531000
    }
  ]
}
```

The badge filters the app list client-side to only show entries where its `hw_rev` appears in the app's `hw_rev` array.

### Badge enrollment (MAC not in registry)

When a badge's MAC is not found in `registry.json`:

1. The badge falls back to `default_hw_rev` and continues normally.
2. The LCD home screen shows a subtle indicator: **"⚠ Board not enrolled"**.
3. The configuration portal page (Obj 1) displays the badge's MAC address so the instructor can copy it into `registry.json` via a pull request or the GitHub web editor.
4. Once the registry is updated and re-fetched, the indicator clears.

> The MAC is also displayed on the "About" screen (long-press A on home menu) for easy identification without entering config mode.

### MAC as device identity (secondary benefit)

The MAC can serve double-duty beyond hw_rev lookup:

- **Attendance / inventory**: instructor can cross-reference MAC → student name in a separate class roster
- **Per-device config**: future `registry.json` extensions could carry per-badge flags (e.g. `"beta": true` to show pre-release apps)
- **Support**: when a student reports a problem, the MAC on-screen uniquely identifies the exact unit

### Acceptance criteria

- [ ] `esp_efuse_mac_get_default()` called once at boot; MAC stored in RAM for the session lifetime
- [ ] MAC displayed as `AA:BB:CC:DD:EE:FF` on config portal page and About screen
- [ ] `registry.json` fetched before (or alongside) `manifest.json`; a fetch failure falls back to `default_hw_rev` with a visible warning
- [ ] App list silently filters to `hw_rev`-compatible entries only; incompatible entries are never shown
- [ ] "Board not enrolled" indicator shown when MAC absent from registry; clears after successful enrollment
- [ ] `registry.json` and `manifest.json` schema versions checked; mismatched schema logs a warning and falls back gracefully

---

## Non-Goals (out of scope for v1)

- No Bluetooth app transfer
- No app code-signing beyond SHA-256 checksum (v2 consideration)
- No multi-user NVS profiles
- No OTA self-update of the loader via the normal download menu (factory partition is intentionally fixed; Tier 2 Option A is the deliberate exception)
- No 5 GHz Wi-Fi (ESP32-S3 is 2.4 GHz only)
- No eFuse burning during manufacturing
- No hardware version resistor / ADC strapping; MAC registry is the sole hw_rev mechanism

---

## Milestones

| # | Milestone | Objectives |
|---|-----------|------------|
| M1 | Hardware bringup | LCD on, buttons read, RGB LED, loader skeleton boots |
| M2 | Configuration portal | Obj 1 — SoftAP, captive portal, NVS settings, QR code, MAC display |
| M3 | OTA catalog | Obj 2 — manifest fetch, HTTPS download, SHA-256, progress UI |
| M4 | MAC registry + hw_rev filtering | Cross-cutting — registry.json fetch, hw_rev lookup, filtered app list, enrollment indicator |
| M5 | Canvas mode | Obj 3 — OTA erase, rollback confirmation, USB sub-mode |
| M6 | Custom bootloader hook | Obj 4 — A+B detection, Tier 2 recovery AP, recovery LCD screen |
| M7 | GitHub Pages recovery site | Obj 4 — ESP Web Tools manifest, Option A POST endpoint, recovery page |
| M8 | Integration + polish | All menus connected, full error handling, all acceptance criteria green |
| M9 | App catalog + registry repo | GitHub Pages site: manifest.json, registry.json, initial app set |
