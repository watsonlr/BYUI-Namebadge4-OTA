#!/usr/bin/env bash
# ==========================================================================
#  publish.sh  —  Publish a firmware build to the ECEN NameBadge OTA site
#
#  Usage:
#    ./tools/publish.sh <variant-name> <path-to-ota-site-repo>
#
#  Examples:
#    ./tools/publish.sh base      ~/Repositories/ecen-badge-ota
#    ./tools/publish.sh snake     ~/Repositories/ecen-badge-ota
#    ./tools/publish.sh pong      ~/Repositories/ecen-badge-ota
#
#  What it does:
#    1. Reads build/ebadge_app.bin  → OTA wireless update catalog
#    2. Reads build/bootloader/bootloader.bin,
#            build/partition_table/partition-table.bin,
#            build/ota_data_initial.bin
#       → full USB flash package (bootloader can only be updated via USB)
#    3. Bumps the version in catalog_<variant>.json
#    4. Writes flash_<variant>.json  (addresses for esptool full flash)
#    5. Rebuilds index.html
#    6. git commit + push
# ==========================================================================
set -euo pipefail

VARIANT="${1:-}"
OTA_REPO="${2:-}"

if [[ -z "$VARIANT" || -z "$OTA_REPO" ]]; then
    echo "Usage: $0 <variant-name> <path-to-ota-site-repo>"
    echo "  variant-name: base | snake | pong | (any name)"
    exit 1
fi

APP_BIN="build/ebadge_app.bin"
BOOT_BIN="build/bootloader/bootloader.bin"
PART_BIN="build/partition_table/partition-table.bin"
OTADATA_BIN="build/ota_data_initial.bin"

for f in "$APP_BIN" "$BOOT_BIN" "$PART_BIN" "$OTADATA_BIN"; do
    [[ -f "$f" ]] || { echo "ERROR: $f not found. Run 'idf.py build' first."; exit 1; }
done

FIRMWARE_DIR="$OTA_REPO/firmware"
FLASH_DIR="$OTA_REPO/flash/${VARIANT}"
CATALOG="$OTA_REPO/catalog_${VARIANT}.json"
FLASH_JSON="$OTA_REPO/flash_${VARIANT}.json"
mkdir -p "$FIRMWARE_DIR" "$FLASH_DIR"

# ── Helpers ───────────────────────────────────────────────────────────────
filesize() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }
sha256()   { sha256sum "$1" 2>/dev/null | awk '{print $1}' || shasum -a 256 "$1" | awk '{print $1}'; }

# ── Compute app OTA values ────────────────────────────────────────────────
APP_SIZE=$(filesize "$APP_BIN")
APP_SHA=$(sha256 "$APP_BIN")

# ── Bump OTA version ──────────────────────────────────────────────────────
if [[ -f "$CATALOG" ]]; then
    OLD_VER=$(python3 -c "import json; print(json.load(open('$CATALOG'))['version'])" 2>/dev/null || echo 0)
else
    OLD_VER=0
fi
NEW_VER=$(( OLD_VER + 1 ))

# ── Derive GitHub Pages base URL ──────────────────────────────────────────
REMOTE=$(git -C "$OTA_REPO" remote get-url origin 2>/dev/null || true)
if [[ "$REMOTE" =~ github\.com[:/]([^/]+)/([^/.]+) ]]; then
    GH_USER="${BASH_REMATCH[1]}"
    GH_REPO="${BASH_REMATCH[2]}"
    BASE_URL="https://${GH_USER}.github.io/${GH_REPO}"
else
    BASE_URL="https://YOUR_USERNAME.github.io/ecen-badge-ota"
    echo "WARN: Could not detect GitHub remote. Edit BASE_URL manually."
fi

# ── Copy OTA app binary ────────────────────────────────────────────────────
cp "$APP_BIN" "$FIRMWARE_DIR/${VARIANT}.bin"
echo "Copied $APP_BIN → firmware/${VARIANT}.bin"

# ── Copy full-flash binaries ───────────────────────────────────────────────
cp "$BOOT_BIN"     "$FLASH_DIR/bootloader.bin"
cp "$PART_BIN"     "$FLASH_DIR/partition-table.bin"
cp "$OTADATA_BIN"  "$FLASH_DIR/ota_data_initial.bin"
cp "$APP_BIN"      "$FLASH_DIR/ebadge_app.bin"
echo "Copied full-flash binaries → flash/${VARIANT}/"

# ── Write OTA catalog JSON ─────────────────────────────────────────────────
python3 - <<EOF
import json, datetime
d = {
    "variant":  "$VARIANT",
    "version":  $NEW_VER,
    "url":      "${BASE_URL}/firmware/${VARIANT}.bin",
    "size":     $APP_SIZE,
    "sha256":   "$APP_SHA",
    "built":    "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
with open("$CATALOG", "w") as f:
    json.dump(d, f, indent=2); f.write("\n")
print(f"Wrote $CATALOG  (v{d['version']}, {d['size']} bytes)")
EOF

# ── Write full-flash descriptor JSON ──────────────────────────────────────
# esptool offsets match partitions.csv: bootloader=0x0, part-table=0x8000,
# ota_data=0xf000, factory/app=0x20000
python3 - <<EOF
import json, os
bflash = "${BASE_URL}/flash/${VARIANT}"
d = {
    "variant":   "$VARIANT",
    "version":   $NEW_VER,
    "chip":      "esp32s3",
    "flash_size": "4MB",
    "flash_mode": "dio",
    "flash_freq": "80m",
    "baud":       460800,
    "parts": [
        {"addr": "0x0000",  "file": "bootloader.bin",       "url": f"{bflash}/bootloader.bin",
         "note": "Second-stage bootloader — USB flash only, never OTA"},
        {"addr": "0x8000",  "file": "partition-table.bin",  "url": f"{bflash}/partition-table.bin"},
        {"addr": "0xf000",  "file": "ota_data_initial.bin", "url": f"{bflash}/ota_data_initial.bin"},
        {"addr": "0x20000", "file": "ebadge_app.bin",       "url": f"{bflash}/ebadge_app.bin"}
    ],
    "esptool_cmd": (
        f"python -m esptool --chip esp32s3 -b 460800 "
        f"--before default_reset --after hard_reset write_flash "
        f"--flash_mode dio --flash_size 4MB --flash_freq 80m "
        f"0x0 bootloader.bin 0x8000 partition-table.bin "
        f"0xf000 ota_data_initial.bin 0x20000 ebadge_app.bin"
    )
}
with open("$FLASH_JSON", "w") as f:
    json.dump(d, f, indent=2); f.write("\n")
print(f"Wrote $FLASH_JSON")
EOF

# ── Rebuild index.html ─────────────────────────────────────────────────────
python3 "$OTA_REPO/tools/build_index.py" "$OTA_REPO"

# ── Commit and push ────────────────────────────────────────────────────────
cd "$OTA_REPO"
git add "catalog_${VARIANT}.json" "flash_${VARIANT}.json" \
        "firmware/${VARIANT}.bin" \
        "flash/${VARIANT}/" \
        "index.html"
git commit -m "OTA+flash ${VARIANT} v${NEW_VER}"
git push

echo ""
echo "✓ Published ${VARIANT} v${NEW_VER}"
echo "  OTA manifest : ${BASE_URL}/catalog_${VARIANT}.json"
echo "  Full flash   : ${BASE_URL}/flash/${VARIANT}/"
echo ""
echo "  Paste the OTA manifest URL into the badge portal's 'Manifest URL' field."
echo "  For USB full-flash (including bootloader), use the esptool command:"
echo "  cd <downloaded flash dir> && $(python3 -c "
import json; d=json.load(open('$FLASH_JSON')); print(d['esptool_cmd'])
")"
