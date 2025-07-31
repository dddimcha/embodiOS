#!/bin/bash

# QEMU test script for EMBODIOS kernel

echo "=== EMBODIOS QEMU Testing ==="

# Check if QEMU is installed
if ! command -v qemu-system-aarch64 &> /dev/null; then
    echo "Error: qemu-system-aarch64 not found"
    echo "Install with: brew install qemu"
    exit 1
fi

# Build kernel for ARM64
echo "Building ARM64 kernel..."
make clean
make ARCH=aarch64 || exit 1

# Create a simple flat binary for testing
echo "Creating test binary..."
# For now, just use the object files
ld -arch arm64 -o embodios_test.elf \
    arch/aarch64/boot.o \
    arch/aarch64/cpu.o \
    arch/aarch64/uart.o \
    arch/aarch64/early_init.o \
    core/kernel.o \
    core/console.o \
    core/panic.o \
    core/stubs.o \
    core/interrupt.o \
    core/task.o \
    mm/pmm.o \
    mm/vmm.o \
    mm/slab.o \
    mm/heap.o \
    ai/model_runtime.o \
    lib/string.o \
    lib/stdlib.o \
    -e _start \
    -static || exit 1

# Convert to raw binary
objcopy -O binary embodios_test.elf embodios_test.bin || {
    echo "Note: objcopy might not be available on macOS"
    echo "Using dd to extract binary..."
    dd if=embodios_test.elf of=embodios_test.bin bs=4096 skip=1 2>/dev/null
}

echo "Running in QEMU..."
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a72 \
    -m 256M \
    -nographic \
    -kernel embodios_test.elf \
    -serial mon:stdio \
    -append "console=ttyAMA0"

echo "QEMU test completed"