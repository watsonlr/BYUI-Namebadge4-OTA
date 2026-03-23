#!/usr/bin/env python3
"""
Capture the ESP32-S3 boot log by triggering a hardware reset via DTR/RTS
(pyserial only — no esptool port conflict).

Usage:
    python3 tools/capture_boot_log.py [/dev/ttyUSB0]
"""

import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
BAUD = 115200
CAPTURE_SECONDS = 5.0

print(f"Opening {PORT} at {BAUD} baud...", flush=True)

with serial.Serial(PORT, BAUD, timeout=0.05) as s:
    # Ensure GPIO0 (BOOT) stays HIGH — normal boot, not download mode.
    # DTR deasserted → GPIO0 stays pulled HIGH by the 470 Ω resistor.
    s.dtr = False
    s.rts = False
    time.sleep(0.05)
    s.reset_input_buffer()

    # Trigger hardware reset: assert RTS → EN goes LOW, then release.
    print("Resetting chip via RTS (EN pin)...", flush=True)
    s.rts = True
    time.sleep(0.1)
    s.rts = False   # EN rises → chip starts

    print("─" * 60)
    print("BOOT LOG:")
    print("─" * 60, flush=True)

    deadline = time.time() + CAPTURE_SECONDS
    while time.time() < deadline:
        chunk = s.read(256)
        if chunk:
            # Print raw bytes, replacing non-printable with '.'
            try:
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
            except Exception:
                sys.stdout.buffer.write(chunk)
            sys.stdout.flush()

print()
print("─" * 60)
print("Capture complete.")
