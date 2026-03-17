
# ESP32 Namebadge Bootloader Audit & Fix Instructions (Single Prompt for Claude-Code)

## Goal

Ensure the firmware architecture behaves as follows:

1. **Bootloader always runs first on reset**
2. **BTN_A + BTN_B held during reset → erase NVS → boot configuration loader**
3. **Otherwise boot the OTA student application**
4. **OTA-installed student apps run automatically on every reset**
5. **Students cannot brick the badge**
6. **Instructor always has a hardware escape hatch via BTN_A + BTN_B**

---

# Single Prompt to Give Claude-Code

Use the following prompt with the Claude-Code agent:

---

You are auditing and fixing an ESP32 firmware repository to enforce a reliable boot architecture for a classroom namebadge project.

Your task is to inspect the repository, determine how booting currently works, and modify it if necessary so that the system follows the required boot flow described below.

## Step 1 — Inspect Boot Architecture

Search the repository for:

- bootloader
- bootloader_main
- esp_ota_set_boot_partition
- esp_ota_get_boot_partition
- bootloader_hooks
- bootloader_start

Determine whether the project uses:

- the standard ESP-IDF bootloader
- or a custom bootloader

Document your findings.

---

## Step 2 — Inspect Partition Table

Locate and inspect:

    partitions.csv

Verify that partitions include:

- nvs
- otadata
- config_loader (factory app)
- ota_0
- ota_1

If missing, update the partition table to something equivalent to:

    nvs,          data, nvs,      0x9000,  0x5000
    otadata,      data, ota,      0xe000,  0x2000
    config,       app,  factory,  0x10000, 1M
    ota_0,        app,  ota_0,             1M
    ota_1,        app,  ota_1,             1M

Ensure:

- the configuration loader is the **factory app**
- OTA slots are used for student apps

---

## Step 3 — Locate Boot Decision Logic

Search the codebase for:

    esp_ota_get_boot_partition
    esp_ota_set_boot_partition

Determine:

- how the OTA slot is selected
- whether any code checks GPIO buttons during boot

If **no button check exists**, mark it as missing.

---

## Step 4 — Verify Button Definitions

Locate or define:

    BTN_A
    BTN_B

Confirm:

- GPIO numbers
- whether buttons are active-low

If missing, create definitions such as:

    #define BTN_A GPIO_NUM_X
    #define BTN_B GPIO_NUM_Y

---

## Step 5 — Implement Bootloader Button Detection

Modify the bootloader so that it performs:

1. Initialize BTN_A and BTN_B GPIOs as inputs with pullups
2. Wait ~300 ms after reset
3. Check if both buttons are pressed

Pseudo logic:

    if (BTN_A && BTN_B pressed)
        erase NVS
        boot config_loader
    else
        continue normal boot

---

## Step 6 — Implement NVS Reset

If the button combo is detected:

    nvs_flash_erase()
    nvs_flash_init()

This must clear:

- OTA selection
- WiFi credentials
- student state

---

## Step 7 — Ensure OTA Behavior Remains Correct

Confirm that OTA updates still function.

Student applications should call:

    esp_ota_set_boot_partition()

After installation so that:

    new OTA app runs automatically on every reset

---

## Step 8 — Confirm Boot Behavior

Final expected behavior:

| Condition | Result |
|----------|--------|
| Normal reset | run OTA student app |
| OTA installed | run OTA app |
| BTN_A + BTN_B held during reset | erase NVS + boot config loader |
| No OTA present | boot config loader |

---

## Step 9 — Add Boot Logging

Add serial debug messages such as:

    Bootloader start
    Buttons: A=%d B=%d
    Factory reset requested
    Booting OTA application
    Booting configuration loader

---

## Step 10 — Verify Bootloader Size

Ensure bootloader remains within ESP-IDF limits.

Check using:

    idf.py size

Bootloader should remain comfortably below ~48 KB.

---

# Required Final Boot Flow

    RESET
      ↓
    Bootloader executes
      ↓
    Initialize BTN_A + BTN_B GPIO
      ↓
    Delay ~300ms
      ↓
    Are BTN_A AND BTN_B pressed?
          │
          ├ YES → erase NVS
          │       boot CONFIG LOADER
          │
          └ NO
               ↓
        OTA app available?
               │
               ├ YES → boot OTA app
               │
               └ NO → boot CONFIG LOADER

---

# Outcome

After these changes:

- Student firmware runs automatically after installation
- Power cycling always launches the student firmware
- Instructors can recover badges using BTN_A + BTN_B
- The system is resilient against students bricking devices

