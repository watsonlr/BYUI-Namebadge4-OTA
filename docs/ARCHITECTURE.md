# System Architecture

## Boot Flow

```
Power On / Reset
      │
      ▼
ROM Bootloader  (permanent in ESP32-S3 silicon)
  • Checks strapping pins
  • Enters UART download mode if IO0 held LOW
  • Loads 2nd-stage bootloader from 0x1000
      │
      ▼
ESP-IDF Bootloader  (bootloader.bin @ 0x1000)
  • Reads partition table at 0x8000
  • Reads otadata to select boot partition
  • Verifies app image integrity
  • Jumps to selected partition
      │
      ▼
Your Application  (factory or OTA partition)
  • app_main() runs
  • Factory partition is never overwritten by OTA
```

## Flash Memory Map (8 MB)

```
Address       Size    Partition       Purpose
─────────────────────────────────────────────
0x0000          4 KB  (reserved)      Boot vectors
0x1000        ~40 KB  bootloader      ESP-IDF 2nd-stage bootloader
─────────────────────────────────────────────
0x8000          4 KB  partition table Partition layout
0x9000         24 KB  nvs             NVS: Wi-Fi credentials, settings
0xF000          8 KB  otadata         OTA boot state
0x11000         4 KB  phy_init        RF calibration data
─────────────────────────────────────────────
0x20000       960 KB  factory  (app)  Your permanent application
                                      ✓ Never overwritten by OTA
─────────────────────────────────────────────
0x110000      960 KB  ota_0    (app)  OTA download slot 0
0x200000      960 KB  ota_1    (app)  OTA download slot 1
0x2F0000      960 KB  ota_2    (app)  OTA download slot 2
─────────────────────────────────────────────
Total used: ~4 MB of 8 MB flash
```

> The OTA slots are optional. If you do not need OTA updates, the factory partition can be enlarged to fill more of the flash — adjust `partitions.csv` accordingly.

## OTA Update Flow (Optional)

If you add OTA capability to your application:

```
ESP32-S3 app_main()
      │
      ├─ Connect to Wi-Fi
      │
      ├─ HTTP GET  manifest.json
      │     {
      │       "apps": [
      │         { "name": "LED App", "version": "1.0", "url": "http://…/led.bin" }
      │       ]
      │     }
      │
      ├─ User selects an app
      │
      ├─ HTTP GET  led.bin  (binary stream)
      │     esp_https_ota_begin()
      │     Write chunks → ota_0 partition
      │     esp_https_ota_end()  (verifies image)
      │
      ├─ esp_ota_set_boot_partition(ota_0)
      │
      └─ esp_restart()  →  boots into downloaded app
```

To return to the factory partition from the OTA app:

```c
const esp_partition_t *factory =
    esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
esp_ota_set_boot_partition(factory);
esp_restart();
```

## Component Interaction

```
app_main()
  ├── NVS Flash init
  ├── GPIO init  (LEDs, buttons)
  ├── (optional) Wi-Fi Manager
  ├── (optional) Display driver  (SPI2)
  ├── (optional) LED strip  (RMT → GPIO7)
  ├── (optional) I2C  (accelerometer GPIO47/21)
  └── Main application task
```

## Typical New-Project Workflow

```
1. Use this template to create a new repo on GitHub
2. Clone, run `idf.py set-target esp32s3`
3. Run `idf.py menuconfig` to configure Wi-Fi / project settings
4. Edit main/main.c with your application logic
5. `idf.py build`
6. `idf.py -p <PORT> flash monitor`
```
