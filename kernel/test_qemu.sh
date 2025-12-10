#!/bin/bash
# Test EMBODIOS in QEMU with timeout

echo "Starting QEMU test..."
echo "Press Ctrl+C or wait 15 seconds to exit"
echo "=========================================="

# Run QEMU in background
qemu-system-x86_64 -cdrom embodios_uefi.iso -m 512M -serial stdio 2>&1 &
QEMU_PID=$!

# Wait 15 seconds
sleep 15

# Kill QEMU
kill -9 $QEMU_PID 2>/dev/null

echo ""
echo "=========================================="
echo "QEMU test completed"
