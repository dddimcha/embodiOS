#!/bin/bash
# EMBODIOS Raspberry Pi 4 Bootable SD Card Image Creator

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL_DIR="$PROJECT_ROOT/kernel"

IMAGE_NAME="embodios-pi4.img"
IMAGE_SIZE_MB=256
MOUNT_POINT="/tmp/embodios_pi_mount"

echo "═══════════════════════════════════════════════════════════"
echo "  EMBODIOS Raspberry Pi 4 SD Card Image Creator"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Check if kernel exists
if [ ! -f "$KERNEL_DIR/embodios.elf" ]; then
    echo "❌ Error: kernel/embodios.elf not found"
    echo "Please build the kernel first:"
    echo "  cd kernel"
    echo "  make ARCH=aarch64 CROSS_PREFIX=aarch64-linux-gnu-"
    exit 1
fi

# Check for required tools
for tool in dd mkfs.vfat parted; do
    if ! command -v $tool &> /dev/null; then
        echo "❌ Error: $tool not found"
        echo "Please install: sudo apt-get install dosfstools parted"
        exit 1
    fi
done

echo "📦 Creating $IMAGE_SIZE_MB MB disk image..."
dd if=/dev/zero of="$IMAGE_NAME" bs=1M count=$IMAGE_SIZE_MB status=progress

echo ""
echo "🔧 Creating partition table..."
parted -s "$IMAGE_NAME" mklabel msdos
parted -s "$IMAGE_NAME" mkpart primary fat32 1MiB 100%
parted -s "$IMAGE_NAME" set 1 boot on

echo ""
echo "📁 Setting up loop device..."
LOOP_DEVICE=$(sudo losetup -f --show -P "$IMAGE_NAME")
echo "Loop device: $LOOP_DEVICE"

# Wait for partition device to appear
sleep 1
PART_DEVICE="${LOOP_DEVICE}p1"

if [ ! -b "$PART_DEVICE" ]; then
    echo "⚠️  Partition device not found, trying alternative..."
    PART_DEVICE="${LOOP_DEVICE}1"
fi

echo "Partition device: $PART_DEVICE"

echo ""
echo "💾 Creating FAT32 filesystem..."
sudo mkfs.vfat -F 32 -n EMBODIOS "$PART_DEVICE"

echo ""
echo "📂 Mounting image..."
sudo mkdir -p "$MOUNT_POINT"
sudo mount "$PART_DEVICE" "$MOUNT_POINT"

echo ""
echo "📝 Creating Raspberry Pi boot configuration..."

# Create config.txt for Raspberry Pi firmware
sudo tee "$MOUNT_POINT/config.txt" > /dev/null << 'EOF'
# EMBODIOS Raspberry Pi 4 Configuration

# Enable 64-bit mode
arm_64bit=1

# Set kernel filename
kernel=kernel8.img

# Memory
gpu_mem=16

# Boot delay (for debugging)
boot_delay=1

# UART for console
enable_uart=1
uart_2ndstage=1

# Disable rainbow splash
disable_splash=1

# Core frequency
core_freq=500

# HDMI safe mode (for debugging)
# hdmi_safe=1

# Overclocking (optional, commented out for stability)
# over_voltage=2
# arm_freq=1800

# Disable Bluetooth (free up UART)
dtoverlay=disable-bt

# Additional device tree overlays
# dtparam=i2c_arm=on
# dtparam=spi=on
EOF

echo "✅ config.txt created"

# Create cmdline.txt (kernel command line arguments)
sudo tee "$MOUNT_POINT/cmdline.txt" > /dev/null << 'EOF'
console=serial0,115200 console=tty1 loglevel=7
EOF

echo "✅ cmdline.txt created"

echo ""
echo "🔥 Copying EMBODIOS kernel..."

# Copy kernel as kernel8.img (Raspberry Pi expects this name)
sudo cp "$KERNEL_DIR/embodios.elf" "$MOUNT_POINT/kernel8.img"
echo "✅ Kernel copied as kernel8.img"

# Create README on the boot partition
sudo tee "$MOUNT_POINT/README.txt" > /dev/null << 'EOF'
EMBODIOS - AI Operating System for Raspberry Pi 4
==================================================

This SD card contains EMBODIOS, an experimental operating system where
AI models run directly on hardware without a traditional OS layer.

HARDWARE REQUIREMENTS:
  - Raspberry Pi 4 (2GB+ RAM recommended)
  - Raspberry Pi 5
  - USB-C power supply
  - Micro HDMI cable (for video output)
  - USB keyboard

FIRST BOOT:
  1. Insert this SD card into Raspberry Pi 4
  2. Connect HDMI monitor and USB keyboard
  3. Connect USB-C power
  4. System will boot in ~1-2 seconds

SERIAL CONSOLE:
  - Baud rate: 115200
  - Connect to GPIO pins 14 (TX) and 15 (RX)
  - Use: screen /dev/ttyUSB0 115200

COMMANDS:
  - help       : Show available commands
  - model      : Display loaded AI model info
  - infer <prompt> : Run AI inference
  - benchmark q4k  : Run performance benchmark
  - heap       : Show memory usage

TROUBLESHOOTING:
  - No output on HDMI? Check UART console via GPIO
  - Kernel panic? Check power supply (3A+ recommended)
  - Slow performance? Check config.txt overclocking

MORE INFO:
  GitHub: https://github.com/dddimcha/embodiOS
  Documentation: docs/raspberry-pi-setup.md

WARNING: This is alpha software. Do not use in production.

Generated: $(date)
EOF

echo "✅ README.txt created"

# Show disk usage
echo ""
echo "📊 Disk usage:"
sudo df -h "$MOUNT_POINT"

# List files
echo ""
echo "📋 Files on boot partition:"
sudo ls -lh "$MOUNT_POINT"

echo ""
echo "🔓 Unmounting image..."
sudo umount "$MOUNT_POINT"
sudo rmdir "$MOUNT_POINT"

echo ""
echo "🔌 Detaching loop device..."
sudo losetup -d "$LOOP_DEVICE"

# Make image readable by user
sudo chown $(whoami):$(whoami) "$IMAGE_NAME"

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  ✅ SD Card Image Created Successfully!"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "Image file: $IMAGE_NAME"
echo "Image size: $(du -h $IMAGE_NAME | cut -f1)"
echo ""
echo "📝 To flash to SD card (Linux/macOS):"
echo "  1. Insert SD card"
echo "  2. Find device: lsblk (Linux) or diskutil list (macOS)"
echo "  3. Unmount: sudo umount /dev/sdX* (replace X with your device)"
echo "  4. Flash: sudo dd if=$IMAGE_NAME of=/dev/sdX bs=4M status=progress"
echo "  5. Sync: sudo sync"
echo ""
echo "💡 On macOS:"
echo "  sudo dd if=$IMAGE_NAME of=/dev/rdiskX bs=1m"
echo "  (use /dev/rdisk for faster writes)"
echo ""
echo "🚀 To boot on Raspberry Pi 4:"
echo "  1. Insert flashed SD card"
echo "  2. Connect HDMI monitor (micro HDMI port 0)"
echo "  3. Connect USB keyboard"
echo "  4. Power on with USB-C"
echo "  5. Should boot in 1-2 seconds!"
echo ""
echo "🔍 Debugging:"
echo "  - Connect USB-UART adapter to GPIO 14/15"
echo "  - Open serial: screen /dev/ttyUSB0 115200"
echo ""
echo "⚠️  WARNING: This is alpha software!"
echo "   - Test on spare hardware only"
echo "   - SD card will be completely erased when flashed"
echo "   - Not suitable for production use"
echo ""
