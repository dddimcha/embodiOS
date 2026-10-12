#!/bin/bash
# EMBODIOS v0.9.0 "Galvani" - Raspberry Pi SD card staging
#
# Usage: make_sdcard.sh <pi3|pi4|pi5> <path-to-kernel8.img> [outdir]
#
# Stages a FAT32 boot-partition layout in dist/sdcard/pi<N>/:
#   kernel8.img   raw aarch64 kernel image (loaded at 0x80000 by firmware)
#   config.txt    firmware configuration
#
# Copy the contents of dist/sdcard/pi<N>/ onto the FAT32 boot partition of
# an SD card (or drop into a Pi OS card replacing kernel8.img).
set -euo pipefail

BOARD="${1:?usage: make_sdcard.sh <pi3|pi4|pi5> <kernel8.img> [outdir]}"
KERNEL="${2:?usage: make_sdcard.sh <pi3|pi4|pi5> <kernel8.img> [outdir]}"

case "$BOARD" in
    pi3|pi4|pi5) ;;
    *) echo "make_sdcard.sh: unknown board '$BOARD' (expected pi3|pi4|pi5)" >&2; exit 1 ;;
esac

if [ ! -f "$KERNEL" ]; then
    echo "make_sdcard.sh: kernel image '$KERNEL' not found" >&2
    exit 1
fi

# Repo root = parent of this script's directory; default output goes there.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${3:-$SCRIPT_DIR/../dist/sdcard/$BOARD}"
mkdir -p "$OUT"

cp "$KERNEL" "$OUT/kernel8.img"

# config.txt template (SPEC v0.9.0). enable_gic=1 is required on Pi 4 for
# bare-metal GIC-400 access (BCM2711 ARM Peripherals sec.6.5.1); harmless
# elsewhere, emitted only for pi4.
cat > "$OUT/config.txt" <<EOF
# EMBODIOS $BOARD boot configuration
arm_64bit=1
kernel=kernel8.img
enable_uart=1
arm_boost=1
disable_overscan=1
EOF
if [ "$BOARD" = "pi4" ]; then
    echo "enable_gic=1" >> "$OUT/config.txt"
fi

echo "make_sdcard.sh: staged $BOARD SD boot files in $OUT:"
ls -l "$OUT"
