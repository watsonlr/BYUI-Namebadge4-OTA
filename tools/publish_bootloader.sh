#!/bin/bash
# publish_bootloader.sh
# Builds the factory loader, copies all flash binaries to the org Pages repo,
# updates loader_manifest.json in the new multi-binary format, and pushes.

set -e

LOADER_REPO="$(cd "$(dirname "$0")/.." && pwd)"
HW_VERSION=4
GITHUB_PAGES_BASE="https://byu-i-ebadge.github.io/bootloader_downloads"

# Locate the Pages repo — set NAMEBADGE_PAGES_REPO to override
if [[ -z "${NAMEBADGE_PAGES_REPO:-}" ]]; then
    # Try sibling of the parent of this repo
    SIBLING="$(cd "${LOADER_REPO}/.." && pwd)/byu-i-ebadge.github.io"
    if [[ -d "${SIBLING}/.git" ]]; then
        NAMEBADGE_PAGES_REPO="${SIBLING}"
    fi
fi
if [[ -z "${NAMEBADGE_PAGES_REPO:-}" ]] || [[ ! -d "${NAMEBADGE_PAGES_REPO}/.git" ]]; then
    echo "ERROR: Cannot find the byu-i-ebadge.github.io Pages repo."
    echo "       Clone it, then set the env var:"
    echo "       git clone git@github.com:BYU-I-eBadge/byu-i-ebadge.github.io.git"
    echo "       export NAMEBADGE_PAGES_REPO=/path/to/byu-i-ebadge.github.io"
    exit 1
fi
PAGES_REPO="${NAMEBADGE_PAGES_REPO}"
BOOTLOADER_DIR="${PAGES_REPO}/bootloader_downloads"
MANIFEST="${BOOTLOADER_DIR}/loader_manifest.json"

# ── Step 1: Prompt for loader version number ──────────────────────────────────
echo ""
echo "========================================"
echo "  BYU-I Badge Bootloader Publisher"
echo "========================================"
echo ""
read -p "Enter new loader version number (integer, e.g. 1 for v${HW_VERSION}.1): " LOADER_VERSION

if ! [[ "$LOADER_VERSION" =~ ^[0-9]+$ ]]; then
    echo "Error: version must be a non-negative integer."
    exit 1
fi

FULL_VERSION="${HW_VERSION}.${LOADER_VERSION}"
APP_BIN_NAME="badge_bootloader_v${FULL_VERSION}.bin"
BL_BIN_NAME="badge_bootloader_v${FULL_VERSION}_bl.bin"
PT_BIN_NAME="badge_bootloader_v${FULL_VERSION}_pt.bin"
SHA_NAME="badge_bootloader_v${FULL_VERSION}.sha256"

echo ""
echo "Publishing loader v${FULL_VERSION}"
echo ""

# ── Step 2: Update LOADER_SW_VERSION in source headers ───────────────────────
echo "[ 1/6 ] Updating LOADER_SW_VERSION to ${LOADER_VERSION} in source..."

PUBLIC_HEADER="${LOADER_REPO}/loader_menu/include/loader_menu.h"
INTERNAL_HEADER="${LOADER_REPO}/loader_menu/loader_menu.h"

sed -i "s/^#define LOADER_SW_VERSION.*/#define LOADER_SW_VERSION   ${LOADER_VERSION}/" "$PUBLIC_HEADER"
sed -i "s/^#define LOADER_SW_VERSION.*/#define LOADER_SW_VERSION   ${LOADER_VERSION}/" "$INTERNAL_HEADER"

echo "    Updated ${PUBLIC_HEADER}"
echo "    Updated ${INTERNAL_HEADER}"

# ── Step 3: Build ─────────────────────────────────────────────────────────────
echo ""
echo "[ 2/6 ] Building (this takes ~30 s)..."

if [[ -n "${IDF_PATH:-}" ]]; then
    source "${IDF_PATH}/export.sh" > /dev/null 2>&1
elif [[ -f "${HOME}/esp/esp-idf/export.sh" ]]; then
    source "${HOME}/esp/esp-idf/export.sh" > /dev/null 2>&1
else
    echo "ERROR: ESP-IDF not found. Set IDF_PATH or install to ~/esp/esp-idf."
    exit 1
fi
cd "$LOADER_REPO"
idf.py build 2>&1 | tail -5

BUILD_APP="${LOADER_REPO}/build/ebadge_app.bin"
BUILD_BL="${LOADER_REPO}/build/bootloader/bootloader.bin"
BUILD_PT="${LOADER_REPO}/build/partition_table/partition-table.bin"

for f in "$BUILD_APP" "$BUILD_BL" "$BUILD_PT"; do
    if [ ! -f "$f" ]; then
        echo "Error: build output not found at ${f}"
        exit 1
    fi
done
echo "    Build OK"

# ── Step 4: Copy binaries ─────────────────────────────────────────────────────
echo ""
echo "[ 3/6 ] Copying binaries to ${BOOTLOADER_DIR}..."

mkdir -p "$BOOTLOADER_DIR"
cp "$BUILD_APP" "${BOOTLOADER_DIR}/${APP_BIN_NAME}"
cp "$BUILD_BL"  "${BOOTLOADER_DIR}/${BL_BIN_NAME}"
cp "$BUILD_PT"  "${BOOTLOADER_DIR}/${PT_BIN_NAME}"

echo "    Copied → ${APP_BIN_NAME}  (factory app,     0x20000)"
echo "    Copied → ${BL_BIN_NAME}   (ESP-IDF bootloader, 0x0)"
echo "    Copied → ${PT_BIN_NAME}   (partition table,    0x8000)"

# ── Step 5: Compute SHA-256 for factory app ───────────────────────────────────
echo ""
echo "[ 4/6 ] Computing SHA-256..."

SHA256=$(sha256sum "${BOOTLOADER_DIR}/${APP_BIN_NAME}" | awk '{print $1}')
APP_SIZE=$(stat -c%s "${BOOTLOADER_DIR}/${APP_BIN_NAME}")

echo "$SHA256  ${APP_BIN_NAME}" > "${BOOTLOADER_DIR}/${SHA_NAME}"
echo "    ${SHA256}"
echo "    Saved → ${SHA_NAME}"

# ── Step 6: Update loader_manifest.json ──────────────────────────────────────
echo ""
echo "[ 5/6 ] Updating loader_manifest.json..."

APP_URL="${GITHUB_PAGES_BASE}/${APP_BIN_NAME}"
BL_URL="${GITHUB_PAGES_BASE}/${BL_BIN_NAME}"
PT_URL="${GITHUB_PAGES_BASE}/${PT_BIN_NAME}"

python3 - <<EOF
import json

manifest_path = "${MANIFEST}"
try:
    with open(manifest_path) as f:
        data = json.load(f)
    if isinstance(data, dict):
        data = [data]
    if not isinstance(data, list):
        data = []
except (FileNotFoundError, json.JSONDecodeError):
    data = []

new_entry = {
    "hw_version":     ${HW_VERSION},
    "loader_version": ${LOADER_VERSION},
    "binaries": [
        {"url": "${BL_URL}",  "address": 0x0},
        {"url": "${PT_URL}",  "address": 0x8000},
        {"url": "${APP_URL}", "address": 0x20000},
    ],
    "size":   ${APP_SIZE},
    "sha256": "${SHA256}",
}

replaced = False
for i, entry in enumerate(data):
    if entry.get("hw_version") == ${HW_VERSION} and entry.get("loader_version") == ${LOADER_VERSION}:
        data[i] = new_entry
        replaced = True
        break
if not replaced:
    data.append(new_entry)

data.sort(key=lambda e: (e.get("hw_version", 0), e.get("loader_version", 0)))

with open(manifest_path, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")

print(f"    {'Updated' if replaced else 'Added'} entry for v${FULL_VERSION} in loader_manifest.json")
EOF

# ── Step 7: Git commit and push ───────────────────────────────────────────────
echo ""
echo "[ 6/6 ] Committing and pushing to GitHub..."

cd "$PAGES_REPO"
git pull
git add bootloader_downloads/
git commit -m "Add factory loader v${FULL_VERSION}

Factory app: ${APP_BIN_NAME} (${APP_SIZE} bytes)
Bootloader:  ${BL_BIN_NAME}
Partitions:  ${PT_BIN_NAME}
SHA256:      ${SHA256}"
git push

echo ""
echo "========================================"
echo "  Done! Loader v${FULL_VERSION} is live."
echo ""
echo "  Manifest: ${GITHUB_PAGES_BASE}/loader_manifest.json"
echo "========================================"
echo ""
echo "NOTE: Remember to also commit the updated LOADER_SW_VERSION"
echo "      in the BYUI-Namebadge4-OTA repo when ready."
echo ""
