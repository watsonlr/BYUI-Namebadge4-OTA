# Setup Guide

## Recommended Environment: WSL (Ubuntu)

All commands below target a WSL Ubuntu shell. This avoids PowerShell/bash conflicts, enforces LF line endings, and simplifies serial port handling.

---

## 1. Install Prerequisites (WSL)

```bash
sudo apt update
sudo apt install -y git wget flex bison gperf python3 python3-pip \
    cmake ninja-build ccache libffi-dev libssl-dev dfu-util
```

## 2. Install ESP-IDF v5.3.1

```bash
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.3.1
cd esp-idf
./install.sh esp32s3
```

Add to `~/.bashrc` for auto-activation (optional):

```bash
echo 'source $HOME/esp/esp-idf/export.sh' >> ~/.bashrc
source ~/.bashrc
```

Or activate manually in each new shell:

```bash
source ~/esp/esp-idf/export.sh
```

Verify:

```bash
idf.py --version   # should print v5.3.1
```

## 3. Create Your Project from This Template

1. On GitHub, click **"Use this template"** → **"Create a new repository"**.
2. Clone your new repo:

```bash
cd ~/projects
git clone https://github.com/<your-org>/<your-repo>.git
cd <your-repo>

# Ensure LF line endings
git config core.autocrlf false
git reset --hard
```

## 4. Configure Serial Port Access (WSL)

The badge appears as a COM port on Windows (e.g., COM10). Access it from WSL:

- Simple: `/dev/ttyS10`  (COM10 → `/dev/ttyS10`)
- Better: attach via `usbipd-win` for a native `/dev/ttyUSB0`:

```powershell
# In elevated Windows PowerShell (one-time setup)
winget install --id=dorssel.usbipd-win
usbipd list
usbipd bind -b <BUSID>
usbipd attach --wsl --busid <BUSID>
```

```bash
# Back in WSL
ls /dev/ttyUSB* /dev/ttyACM*
export ESPPORT=/dev/ttyUSB0
```

## 5. First Build

```bash
idf.py set-target esp32s3
idf.py menuconfig    # set Wi-Fi SSID/password under "Project Configuration"
idf.py build
```

## 6. Flash and Monitor

```bash
idf.py -p $ESPPORT erase-flash   # first-time only
idf.py -p $ESPPORT flash monitor
# Press Ctrl+] to exit monitor
```

---

## Windows (ESP-IDF PowerShell / CMD)

If you prefer native Windows tools, open the **ESP-IDF PowerShell** shortcut:

```powershell
cd C:\path\to\your-repo
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor   # replace COM3 with your port
```

> Find your COM port in **Device Manager → Ports (COM & LPT)**.
> Look for "Silicon Labs CP210x" or "USB-SERIAL CH340".

---

## Common Issues

| Error | Fix |
|-------|-----|
| `/usr/bin/env: 'python\r'` | CRLF line endings — run `git reset --hard` inside WSL |
| `No module named serial` | `pip install pyserial` |
| Permission denied `/dev/ttyS*` | `sudo usermod -aG dialout $USER` then re-login |
| Flash fails at 921600 baud | Add `-b 115200` to the flash command |
