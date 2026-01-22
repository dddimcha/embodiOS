#!/bin/bash
#
# VirtIO Block Driver - QCOW2 Format Test Script
#
# This script tests the VirtIO block driver with QCOW2 disk images.
# QCOW2 format is handled transparently by QEMU - the driver sees
# a regular block device regardless of the backing file format.
#

set -e

echo "=== VirtIO Block Driver - QCOW2 Test Setup ==="
echo ""

# Check if qemu-img is available
if ! command -v qemu-img &> /dev/null; then
    echo "ERROR: qemu-img not found"
    echo "Install QEMU tools:"
    echo "  macOS: brew install qemu"
    echo "  Ubuntu/Debian: apt install qemu-utils"
    echo "  Fedora/RHEL: dnf install qemu-img"
    exit 1
fi

# Check if qemu-system-x86_64 is available
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo "ERROR: qemu-system-x86_64 not found"
    echo "Install QEMU:"
    echo "  macOS: brew install qemu"
    echo "  Ubuntu/Debian: apt install qemu-system-x86"
    echo "  Fedora/RHEL: dnf install qemu-system-x86"
    exit 1
fi

# Create QCOW2 test image if it doesn't exist
if [ ! -f test.qcow2 ]; then
    echo "Creating QCOW2 test image (1GB)..."
    qemu-img create -f qcow2 test.qcow2 1G
    echo "✓ Created test.qcow2"
else
    echo "✓ Using existing test.qcow2"
fi

# Show QCOW2 image info
echo ""
echo "QCOW2 Image Info:"
qemu-img info test.qcow2 | sed 's/^/  /'
echo ""

# Check if kernel exists
if [ ! -f kernel/embodios.elf ]; then
    echo "ERROR: Kernel not found at kernel/embodios.elf"
    echo "Build the kernel first:"
    echo "  cd kernel && make"
    exit 1
fi

echo "=== Starting QEMU with QCOW2 disk ==="
echo ""
echo "QEMU Configuration:"
echo "  Kernel:  kernel/embodios.elf"
echo "  Memory:  2GB"
echo "  Disk:    test.qcow2 (QCOW2 format, 1GB)"
echo "  Format:  qcow2 (transparent to VirtIO driver)"
echo ""
echo "Test Commands:"
echo "  EMBODIOS> blkinfo   - Show block device info"
echo "  EMBODIOS> blktest   - Run basic I/O tests"
echo "  EMBODIOS> blkdevs   - List block devices"
echo "  EMBODIOS> blkperf   - Run performance test"
echo ""
echo "Expected Results:"
echo "  ✓ VirtIO device detected at boot"
echo "  ✓ Capacity: 2097152 sectors (1GB)"
echo "  ✓ All blktest tests PASS"
echo "  ✓ Performance >= 100 MB/s"
echo ""
echo "Press Ctrl-A, X to exit QEMU"
echo "=========================================="
echo ""

# Launch QEMU with QCOW2 disk
qemu-system-x86_64 \
    -kernel kernel/embodios.elf \
    -m 2G \
    -drive file=test.qcow2,format=qcow2,if=virtio \
    -serial stdio \
    -no-reboot

echo ""
echo "=== Test Complete ==="
