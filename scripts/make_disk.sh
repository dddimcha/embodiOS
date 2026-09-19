#!/bin/bash
# make_disk.sh - Create a raw disk image for EMBODIOS persistent storage (embfs)
#
# Usage: ./scripts/make_disk.sh [output.img] [size_mb]
#        Default: disk.img, 64 MB
#
# The image is raw zeros; embfs auto-formats it on first use (fsave/fstest)
# or explicitly with the 'fformat' shell command.
#
# QEMU launch (persistent storage enabled):
#   $QEMU_BIN -kernel embodios.elf -m 2G \
#       -drive file=disk.img,format=raw,if=virtio \
#       -serial stdio -display none
#
# embfs layout on the disk: 1 MB reserved area (superblock + double-buffered
# file table, CRC32-protected) followed by 4K-aligned data blocks.
# The disk is safe to combine with a separate GGUF model disk: embfs never
# touches devices whose first bytes look like a GGUF model.
set -e

OUT="${1:-disk.img}"
SIZE_MB="${2:-64}"

echo "Creating raw disk image: $OUT (${SIZE_MB} MB)"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
sync "$OUT" 2>/dev/null || true

ls -lh "$OUT"
echo ""
echo "Boot EMBODIOS with this disk:"
echo "  \$QEMU_BIN -kernel embodios.elf -m 2G \\"
echo "      -drive file=$OUT,format=raw,if=virtio \\"
echo "      -serial stdio -display none"
echo ""
echo "Try in the EMBODIOS shell: fstest, fsave note hello, fls, fload note, df"
