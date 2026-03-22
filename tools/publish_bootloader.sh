#!/bin/bash
# publish_bootloader.sh
# Builds the factory loader, copies the binary to namebadge-apps, updates the
# manifest JSON, and pushes everything to GitHub so the OTA update URL goes live.

set -e

LOADER_REPO="$(cd "$(dirname "$0")/.." && pwd)"
APPS_REPO="/home/lynn/Documents/Repositories/namebadge-apps"
BOOTLOADER_DIR="${APPS_REPO}/bootloader_downloads"
MANIFEST="${BOOTLOADER_DIR}/loader_manifest.json"
HW_VERSION=4
GITHUB_PAGES_BASE="https://watsonlr.github.io/namebadge-apps/bootloader_downloads"

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
BIN_NAME="badge_bootloader_v${FULL_VERSION}.bin"
SHA_NAME="badge_bootloader_v${FULL_VERSION}.sha256"

echo ""
echo "Publishing loader v${FULL_VERSION}  →  ${BIN_NAME}"
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

source /home/lynn/esp/esp-idf/export.sh > /dev/null 2>&1
cd "$LOADER_REPO"
idf.py build 2>&1 | tail -5

BUILD_BIN="${LOADER_REPO}/build/ebadge_app.bin"
if [ ! -f "$BUILD_BIN" ]; then
    echo "Error: build output not found at ${BUILD_BIN}"
    exit 1
fi
echo "    Build OK"

# ── Step 4: Copy binary ───────────────────────────────────────────────────────
echo ""
echo "[ 3/6 ] Copying binary to ${BOOTLOADER_DIR}..."

mkdir -p "$BOOTLOADER_DIR"
cp "$BUILD_BIN" "${BOOTLOADER_DIR}/${BIN_NAME}"
echo "    Copied → ${BIN_NAME}"

# ── Step 5: Compute SHA-256 ───────────────────────────────────────────────────
echo ""
echo "[ 4/6 ] Computing SHA-256..."

SHA256=$(sha256sum "${BOOTLOADER_DIR}/${BIN_NAME}" | awk '{print $1}')
BIN_SIZE=$(stat -c%s "${BOOTLOADER_DIR}/${BIN_NAME}")

echo "$SHA256  ${BIN_NAME}" > "${BOOTLOADER_DIR}/${SHA_NAME}"
echo "    ${SHA256}"
echo "    Saved → ${SHA_NAME}"

# ── Step 6: Update loader_manifest.json ──────────────────────────────────────
echo ""
echo "[ 5/6 ] Updating loader_manifest.json..."

BINARY_URL="${GITHUB_PAGES_BASE}/${BIN_NAME}"

python3 - <<EOF
import json, sys

manifest_path = "${MANIFEST}"
try:
    with open(manifest_path) as f:
        data = json.load(f)
    # Handle legacy single-object format
    if isinstance(data, dict):
        data = [data]
    if not isinstance(data, list):
        data = []
except (FileNotFoundError, json.JSONDecodeError):
    data = []

new_entry = {
    "hw_version":     ${HW_VERSION},
    "loader_version": ${LOADER_VERSION},
    "binary_url":     "${BINARY_URL}",
    "size":           ${BIN_SIZE},
    "sha256":         "${SHA256}"
}

# Replace any existing entry for the same hw+loader version, else append
replaced = False
for i, entry in enumerate(data):
    if entry.get("hw_version") == ${HW_VERSION} and entry.get("loader_version") == ${LOADER_VERSION}:
        data[i] = new_entry
        replaced = True
        break
if not replaced:
    data.append(new_entry)

# Sort by hw_version then loader_version for readability
data.sort(key=lambda e: (e.get("hw_version", 0), e.get("loader_version", 0)))

with open(manifest_path, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")

print(f"    {'Updated' if replaced else 'Added'} entry for v${FULL_VERSION} in loader_manifest.json")
EOF

# ── Step 7: Git commit and push ───────────────────────────────────────────────
echo ""
echo "[ 6/6 ] Committing and pushing to GitHub..."

cd "$APPS_REPO"
git add bootloader_downloads/
git commit -m "Add factory loader v${FULL_VERSION}

Binary: ${BIN_NAME}
Size:   ${BIN_SIZE} bytes
SHA256: ${SHA256}"
git push

echo ""
echo "========================================"
echo "  Done! Loader v${FULL_VERSION} is live."
echo ""
echo "  Manifest URL:"
echo "  ${GITHUB_PAGES_BASE}/loader_manifest.json"
echo ""
echo "  Binary URL:"
echo "  ${BINARY_URL}"
echo "========================================"
echo ""
echo "NOTE: Remember to also commit the updated LOADER_SW_VERSION"
echo "      in the BYUI-Namebadge4-OTA repo when ready."
echo ""
