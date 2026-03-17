# splash_screen — ESP-IDF Component

Scroll-up splash screen for the BYUI eBadge V3.0 ILI9341 display.

## What it does

`splash_screen_run()` initialises SPI2 and the ILI9341 in landscape mode
(320×240), then reveals your image one row at a time from the bottom of the
screen upward — a smooth "scroll up" effect — then holds the completed image
for two seconds before returning.

## Step 1 — Convert your PNG

Install Pillow if you haven't already:

```
pip install Pillow
```

Run the converter from **this directory**, pointing it at your PNG:

```
python png_to_rgb565.py ../byui_splash_320x240.png
```

This writes `image_rgb565.h` into the `splash_screen/` folder.  The image is
automatically resized to 320×240 if it isn't already.

## Step 2 — Register the component

Add `splash_screen` to the `REQUIRES` list in `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES splash_screen nvs_flash driver esp_driver_spi
)
```

Also register the component directory in the top-level `CMakeLists.txt` if it
isn't already discovered automatically:

```cmake
set(EXTRA_COMPONENT_DIRS "splash_screen")
```

## Step 3 — Call it from app_main

```c
#include "splash_screen.h"

void app_main(void)
{
    splash_screen_run();   /* runs animation, then returns */

    /* ... rest of your application ... */
}
```

## Tuning

Two constants at the top of `splash_screen.c` control timing:

| Constant | Default | Effect |
|---|---|---|
| `SCROLL_ROW_DELAY_MS` | 4 | ms between each revealed row (~1 s total) |
| `SPLASH_HOLD_MS` | 2000 | ms to hold the completed image |

## Orientation

The display is initialised with `MADCTL = 0x68` (landscape, BGR).  If the
image appears mirrored or rotated, try `0x28`, `0x48`, `0xA8`, or `0xE8` in
the `disp_data` call for command `0x36` in `splash_screen.c`.

## SPI bus sharing

`splash_screen_run()` initialises SPI2_HOST and leaves it running.  To add
the SD-card device afterwards, call `spi_bus_add_device()` with CS pin 3 —
there is no need to reinitialise the bus.
