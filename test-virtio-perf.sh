#!/bin/bash
# VirtIO Block Driver Performance Test Script
# Tests read throughput to verify 100MB/s target
#
# Usage: ./test-virtio-perf.sh

set -e

echo "=== VirtIO Block Driver Performance Test ==="
echo ""

# Check if kernel binary exists
if [ ! -f "embodios.elf" ]; then
    echo "ERROR: embodios.elf not found"
    echo "Building kernel..."
    (cd kernel && make)
    cp kernel/embodios.elf .
fi

# Check if performance test disk image exists
if [ ! -f "test-perf.img" ]; then
    echo "Creating performance test disk image (100MB)..."
    python3 -c "
import os
# Create 100MB test disk for performance testing
size = 100 * 1024 * 1024
with open('test-perf.img', 'wb') as f:
    # Write test pattern to first sector
    sector = b'EMBODIOS PERFORMANCE TEST DISK' + b'\x00' * (512 - 30)
    f.write(sector)
    # Fill rest with zeros
    remaining = size - 512
    chunk_size = 1024 * 1024  # 1MB chunks
    for i in range(remaining // chunk_size):
        f.write(b'\x00' * chunk_size)
    # Write remaining bytes
    f.write(b'\x00' * (remaining % chunk_size))
print('Created test-perf.img (100MB)')
"
fi

echo ""
echo "Test disk image: test-perf.img (100MB)"
echo "Kernel binary: embodios.elf"
echo ""
echo "=== Starting QEMU ==="
echo ""
echo "Expected behavior:"
echo "  1. Kernel boots successfully"
echo "  2. VirtIO block device detected with 100MB capacity"
echo "  3. Run 'blkperf' command to measure throughput"
echo ""
echo "Performance test commands:"
echo "  - blkinfo    : Show block device information"
echo "  - blkperf    : Run performance test (50MB sequential read)"
echo "  - blktest    : Run basic I/O tests"
echo ""
echo "Acceptance criteria:"
echo "  ✓ Read throughput >= 100 MB/s"
echo ""
echo "Press Ctrl-A then X to exit QEMU"
echo ""
echo "----------------------------------------"
echo ""

# Check if QEMU is available
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo "ERROR: qemu-system-x86_64 not found"
    echo "Install QEMU: brew install qemu (macOS) or apt install qemu-system-x86 (Linux)"
    exit 1
fi

# Launch QEMU with VirtIO block device
# Use larger disk for meaningful performance testing
qemu-system-x86_64 \
    -kernel embodios.elf \
    -m 2G \
    -drive file=test-perf.img,format=raw,if=virtio,cache=none \
    -serial stdio \
    -no-reboot \
    -no-shutdown

echo ""
echo "=== Performance Test Complete ==="
