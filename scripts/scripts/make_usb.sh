#!/bin/bash
# EMBODIOS USB Boot Media Creator
#
# Creates a bootable USB drive from the EMBODIOS ISO image.
# Supports macOS and Linux.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
KERNEL_DIR="$PROJECT_ROOT/kernel"
ISO_FILE="$KERNEL_DIR/embodios.iso"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=== EMBODIOS USB Boot Media Creator ===${NC}"
echo ""

usage() {
    echo "Usage: $0 [options] <device>"
    echo ""
    echo "Options:"
    echo "  -i, --iso FILE    Specify ISO file (default: kernel/embodios.iso)"
    echo "  -f, --force       Skip confirmation prompts"
    echo "  -v, --verify      Verify write after completion"
    echo "  -h, --help        Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 /dev/sdb                    # Linux"
    echo "  $0 /dev/disk2                  # macOS"
    echo "  $0 --iso custom.iso /dev/sdb   # Custom ISO"
    echo ""
    echo "WARNING: This will DESTROY all data on the target device!"
}

# Parse arguments
FORCE=false
VERIFY=false
DEVICE=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--iso)
            ISO_FILE="$2"
            shift 2
            ;;
        -f|--force)
            FORCE=true
            shift
            ;;
        -v|--verify)
            VERIFY=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo -e "${RED}Error: Unknown option $1${NC}"
            usage
            exit 1
            ;;
        *)
            DEVICE="$1"
            shift
            ;;
    esac
done

# Check for device
if [ -z "$DEVICE" ]; then
    echo -e "${RED}Error: No device specified${NC}"
    echo ""
    usage
    exit 1
fi

# Check if ISO exists
if [ ! -f "$ISO_FILE" ]; then
    echo -e "${RED}Error: ISO file not found: $ISO_FILE${NC}"
    echo ""
    echo "Build the ISO first:"
    echo "  cd kernel && make iso"
    exit 1
fi

# Check if device exists
if [ ! -e "$DEVICE" ]; then
    echo -e "${RED}Error: Device not found: $DEVICE${NC}"
    exit 1
fi

# Detect OS
OS=$(uname -s)

# Safety check - don't write to main disk
case "$DEVICE" in
    /dev/sda|/dev/nvme0n1|/dev/disk0|/dev/mmcblk0)
        echo -e "${RED}DANGER: Refusing to write to what looks like a system disk!${NC}"
        echo "Device: $DEVICE"
        exit 1
        ;;
esac

# Get device info
echo -e "${BLUE}Target Device:${NC} $DEVICE"
if [ "$OS" = "Darwin" ]; then
    # macOS
    diskutil info "$DEVICE" 2>/dev/null | grep -E "(Device|Total Size|Media Name)" || true
else
    # Linux
    lsblk "$DEVICE" 2>/dev/null || fdisk -l "$DEVICE" 2>/dev/null | head -3 || true
fi

echo ""
echo -e "${BLUE}ISO File:${NC} $ISO_FILE"
ls -lh "$ISO_FILE"
echo ""

# Confirmation
if [ "$FORCE" != "true" ]; then
    echo -e "${YELLOW}WARNING: ALL DATA ON $DEVICE WILL BE DESTROYED!${NC}"
    read -p "Are you sure you want to continue? (yes/no): " confirm
    if [ "$confirm" != "yes" ]; then
        echo "Aborted."
        exit 1
    fi
fi

# Unmount device
echo -e "${BLUE}Unmounting device...${NC}"
if [ "$OS" = "Darwin" ]; then
    # macOS
    diskutil unmountDisk "$DEVICE" 2>/dev/null || true
    # Use raw device for faster writes
    RAW_DEVICE="${DEVICE/disk/rdisk}"
else
    # Linux
    umount "${DEVICE}"* 2>/dev/null || true
    RAW_DEVICE="$DEVICE"
fi

# Write ISO
echo -e "${BLUE}Writing ISO to $DEVICE...${NC}"
echo "This may take a few minutes..."

if [ "$OS" = "Darwin" ]; then
    sudo dd if="$ISO_FILE" of="$RAW_DEVICE" bs=4m status=progress 2>&1
else
    sudo dd if="$ISO_FILE" of="$RAW_DEVICE" bs=4M status=progress conv=fdatasync 2>&1
fi

# Sync
echo -e "${BLUE}Syncing...${NC}"
sync

# Verify if requested
if [ "$VERIFY" = "true" ]; then
    echo -e "${BLUE}Verifying write...${NC}"
    ISO_SIZE=$(stat -f%z "$ISO_FILE" 2>/dev/null || stat -c%s "$ISO_FILE" 2>/dev/null)
    ISO_MD5=$(dd if="$ISO_FILE" bs=1M 2>/dev/null | md5sum | cut -d' ' -f1)
    DEV_MD5=$(sudo dd if="$RAW_DEVICE" bs=1M count=$((ISO_SIZE / 1048576 + 1)) 2>/dev/null | head -c "$ISO_SIZE" | md5sum | cut -d' ' -f1)

    if [ "$ISO_MD5" = "$DEV_MD5" ]; then
        echo -e "${GREEN}Verification PASSED${NC}"
    else
        echo -e "${RED}Verification FAILED!${NC}"
        echo "ISO MD5: $ISO_MD5"
        echo "USB MD5: $DEV_MD5"
        exit 1
    fi
fi

# Eject
echo -e "${BLUE}Ejecting device...${NC}"
if [ "$OS" = "Darwin" ]; then
    diskutil eject "$DEVICE"
else
    sudo eject "$DEVICE" 2>/dev/null || true
fi

echo ""
echo -e "${GREEN}=== USB Boot Media Created Successfully ===${NC}"
echo ""
echo "Next steps:"
echo "1. Insert USB into target machine (Intel NUC, etc.)"
echo "2. Enter BIOS/UEFI setup (usually F2 at boot)"
echo "3. Disable Secure Boot"
echo "4. Set USB as first boot device"
echo "5. Save and reboot"
echo ""
echo "See docs/Intel-NUC-Boot-Guide.md for detailed instructions."
