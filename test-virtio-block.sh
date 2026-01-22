#!/bin/bash
# VirtIO Block Driver Integration Test Script
# Tests block_read_bytes() implementation with QEMU virtual disk
#
# Usage: ./test-virtio-block.sh

set -e

echo "=== VirtIO Block Driver Integration Test ==="
echo ""

# Check if kernel binary exists
if [ ! -f "embodios.elf" ]; then
    echo "ERROR: embodios.elf not found"
    echo "Building kernel..."
    (cd kernel && make)
    # Copy binary to working directory
    if [ -f "kernel/embodios.elf" ]; then
        cp kernel/embodios.elf .
    fi
fi

# Check if test disk image exists
if [ ! -f "test.img" ]; then
    echo "Creating test disk image (10MB)..."
    python3 -c "
import os
# Create 10MB test disk image
with open('test.img', 'wb') as f:
    # Write test data to first sector
    sector = b'EMBODIOS TEST DISK - VirtIO Block Driver Test Image' + b'\x00' * (512 - 52)
    f.write(sector)
    # Fill rest with zeros
    f.write(b'\x00' * (10 * 1024 * 1024 - 512))
print('Created test.img (10MB)')
"
fi

echo ""
echo "Test disk image: test.img (10MB)"
echo "Kernel binary: embodios.elf"
echo ""
echo "=== Starting QEMU ==="
echo ""
echo "Expected behavior:"
echo "  1. Kernel boots successfully"
echo "  2. VirtIO block device detected at I/O port"
echo "  3. Device shows capacity and sector size"
echo ""
echo "Manual test commands (run in EMBODIOS shell):"
echo "  - blkinfo    : Show block device information"
echo "  - blktest    : Run basic I/O tests (tests block_read)"
echo "  - blkdevs    : List all block devices"
echo "  - loadmodel  : Test GGUF loading via block_read_bytes()"
echo ""
echo "Press Ctrl-A then X to exit QEMU"
echo ""
echo "----------------------------------------"
echo ""

# Check if QEMU is available
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo "ERROR: qemu-system-x86_64 not found"
    echo "Install QEMU: brew install qemu"
    exit 1
fi

# Launch QEMU with VirtIO block device
qemu-system-x86_64 \
    -kernel embodios.elf \
    -m 2G \
    -drive file=test.img,format=raw,if=virtio \
    -serial stdio \
    -no-reboot \
    -no-shutdown

echo ""
echo "=== QEMU Test Complete ==="
