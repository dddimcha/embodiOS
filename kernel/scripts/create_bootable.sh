#!/bin/bash
# EMBODIOS Bootable Image Creator
# Creates bootable ISO/USB images with embedded AI kernel

set -e

KERNEL_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$KERNEL_DIR/build/iso"
OUTPUT_ISO="embodios-ai.iso"

echo "[EMBODIOS] Creating bootable image..."

# Check if kernel exists
if [ ! -f "$KERNEL_DIR/embodios.elf" ]; then
    echo "[ERROR] Kernel not found. Build it first with: make ARCH=x86_64"
    exit 1
fi

# Create ISO directory structure
mkdir -p "$BUILD_DIR/boot/grub"

# Copy kernel
cp "$KERNEL_DIR/embodios.elf" "$BUILD_DIR/boot/"

# Create GRUB configuration
cat > "$BUILD_DIR/boot/grub/grub.cfg" <<EOF
set timeout=5
set default=0

menuentry "EMBODIOS AI-OS" {
    multiboot2 /boot/embodios.elf
    boot
}

menuentry "EMBODIOS AI-OS (Debug)" {
    multiboot2 /boot/embodios.elf debug=1
    boot
}
EOF

# Check for grub-mkrescue
if command -v grub-mkrescue >/dev/null 2>&1; then
    echo "[EMBODIOS] Creating ISO with grub-mkrescue..."
    grub-mkrescue -o "$KERNEL_DIR/$OUTPUT_ISO" "$BUILD_DIR" 2>/dev/null
    echo "[SUCCESS] Created bootable ISO: $KERNEL_DIR/$OUTPUT_ISO"
    echo ""
    echo "To test in QEMU:"
    echo "  qemu-system-x86_64 -cdrom $OUTPUT_ISO -m 512M"
    echo ""
    echo "To write to USB:"
    echo "  sudo dd if=$OUTPUT_ISO of=/dev/sdX bs=4M status=progress"
else
    echo "[WARNING] grub-mkrescue not found"
    echo "Install with:"
    echo "  Ubuntu/Debian: sudo apt install grub-pc-bin xorriso"
    echo "  macOS: brew install xorriso"
    
    # Alternative: Create simple disk image
    echo ""
    echo "[EMBODIOS] Creating simple disk image instead..."
    
    # Create 64MB disk image
    dd if=/dev/zero of="$KERNEL_DIR/embodios.img" bs=1M count=64 2>/dev/null
    
    echo "[INFO] Created disk image: $KERNEL_DIR/embodios.img"
    echo "Note: This requires manual bootloader installation"
fi

# Cleanup
rm -rf "$BUILD_DIR"

echo "[EMBODIOS] Done!"