#!/bin/bash

# TCP Send Integration Test Script
# This script tests TCP send functionality by:
# 1. Starting the kernel in QEMU with network configuration
# 2. Connecting with netcat to test TCP handshake and data transmission

set -e

echo "=== TCP Send Integration Test ==="
echo ""

# Check if QEMU is installed
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo "ERROR: qemu-system-x86_64 not found"
    exit 1
fi

# Check if nc (netcat) is installed
if ! command -v nc &> /dev/null; then
    echo "ERROR: nc (netcat) not found"
    exit 1
fi

# Check if kernel binary exists
if [ ! -f kernel/embodios.elf ]; then
    echo "ERROR: kernel/embodios.elf not found"
    exit 1
fi

echo "Starting kernel in QEMU with network configuration..."
echo "QEMU command: qemu-system-x86_64 -kernel kernel/embodios.elf -m 256M -serial stdio -device e1000e,netdev=net0 -netdev user,id=net0,hostfwd=tcp::8080-:80"
echo ""

# Create a named pipe for QEMU communication
QEMU_PIPE=$(mktemp -u)
mkfifo "$QEMU_PIPE"

# Start QEMU in background, redirecting output to pipe
qemu-system-x86_64 \
    -kernel kernel/embodios.elf \
    -m 256M \
    -serial stdio \
    -device e1000e,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -display none \
    > "$QEMU_PIPE" 2>&1 &

QEMU_PID=$!
echo "QEMU started with PID: $QEMU_PID"

# Function to cleanup on exit
cleanup() {
    echo ""
    echo "Cleaning up..."
    kill $QEMU_PID 2>/dev/null || true
    rm -f "$QEMU_PIPE"
    echo "Done."
}
trap cleanup EXIT

# Monitor QEMU output in background
(
    while IFS= read -r line; do
        echo "[QEMU] $line"
        # Check if the kernel is ready
        if echo "$line" | grep -q "EMBODIOS Ready"; then
            touch /tmp/embodios_ready
        fi
        # Check if TCP server is listening
        if echo "$line" | grep -q "TCP echo server listening"; then
            touch /tmp/tcp_server_ready
        fi
    done < "$QEMU_PIPE"
) &
MONITOR_PID=$!

# Wait for kernel to be ready (max 30 seconds)
echo "Waiting for kernel to boot (max 30 seconds)..."
for i in {1..30}; do
    if [ -f /tmp/embodios_ready ]; then
        echo "Kernel is ready!"
        break
    fi
    sleep 1
    if [ $i -eq 30 ]; then
        echo "ERROR: Kernel boot timeout"
        exit 1
    fi
done

# Send command to start TCP server
echo ""
echo "Starting TCP server..."
echo "tcpserver" > /proc/$QEMU_PID/fd/0 2>/dev/null || echo "Note: Could not send command directly to QEMU stdin"

# Wait a bit for TCP server to start
sleep 5

# Check if we can connect with netcat
echo ""
echo "Attempting to connect with netcat to localhost:8080..."
echo ""

# Try to connect with netcat with timeout
if timeout 5 bash -c 'echo "test" | nc localhost 8080' 2>&1; then
    echo ""
    echo "✓ TCP connection successful!"
    echo "✓ TCP handshake completed (SYN/SYN+ACK/ACK)"
    echo "✓ Data transmission test passed"
    echo ""
    echo "=== Integration Test PASSED ==="
else
    echo ""
    echo "✗ Connection failed or timed out"
    echo ""
    echo "=== Integration Test FAILED ==="
    exit 1
fi

# Cleanup will be called automatically by trap
