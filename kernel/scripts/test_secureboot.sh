#!/bin/bash
# EMBODIOS Secure Boot Test Script
# Tests kernel with OVMF UEFI firmware and Secure Boot support

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(dirname "$SCRIPT_DIR")"
ISO_PATH="$KERNEL_DIR/embodios.iso"
MEMORY="512M"
OVMF_CODE=""
OVMF_VARS=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Parse arguments
CHECK_ONLY=false
USE_SIGNED=false
VERBOSE=false
USE_ISO=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --check)
            CHECK_ONLY=true
            shift
            ;;
        --signed)
            USE_SIGNED=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --iso)
            USE_ISO=true
            ISO_PATH="$2"
            shift 2
            ;;
        --memory|-m)
            MEMORY="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --check       Check for OVMF firmware only (don't run)"
            echo "  --signed      Use signed kernel for Secure Boot"
            echo "  --iso FILE    Boot from ISO instead of kernel ELF"
            echo "  --memory SIZE VM memory size (default: 512M)"
            echo "  --verbose     Verbose output"
            echo "  --help        Show this help"
            echo ""
            echo "Examples:"
            echo "  $0 --check              # Check if OVMF is available"
            echo "  $0 --signed             # Test signed kernel with Secure Boot"
            echo "  $0 --iso embodios.iso   # Test bootable ISO"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Function to find OVMF firmware files
find_ovmf() {
    local search_paths=(
        # Linux standard paths
        "/usr/share/OVMF"
        "/usr/share/edk2-ovmf/x64"
        "/usr/share/edk2/ovmf"
        "/usr/share/qemu/ovmf-x86_64"
        # macOS Homebrew paths
        "/usr/local/share/ovmf"
        "/opt/homebrew/share/ovmf"
        "/usr/local/Cellar/ovmf"
        "/opt/homebrew/Cellar/ovmf"
    )

    for base_path in "${search_paths[@]}"; do
        # Look for OVMF_CODE.fd (firmware code)
        if [ -f "$base_path/OVMF_CODE.fd" ]; then
            OVMF_CODE="$base_path/OVMF_CODE.fd"
            OVMF_VARS="$base_path/OVMF_VARS.fd"
            return 0
        fi

        # Look for OVMF.fd (combined firmware)
        if [ -f "$base_path/OVMF.fd" ]; then
            OVMF_CODE="$base_path/OVMF.fd"
            return 0
        fi

        # Check for secure boot variants
        if [ -f "$base_path/OVMF_CODE.secboot.fd" ]; then
            OVMF_CODE="$base_path/OVMF_CODE.secboot.fd"
            OVMF_VARS="$base_path/OVMF_VARS.secboot.fd"
            return 0
        fi
    done

    # Check Homebrew cellar with version wildcards
    for cellar in /usr/local/Cellar/ovmf/* /opt/homebrew/Cellar/ovmf/*; do
        if [ -d "$cellar/share/ovmf" ]; then
            if [ -f "$cellar/share/ovmf/OVMF_CODE.fd" ]; then
                OVMF_CODE="$cellar/share/ovmf/OVMF_CODE.fd"
                OVMF_VARS="$cellar/share/ovmf/OVMF_VARS.fd"
                return 0
            fi
        fi
    done

    return 1
}

# Find OVMF firmware
if ! find_ovmf; then
    echo -e "${RED}Error: OVMF firmware not found${NC}"
    echo ""
    echo "Install OVMF:"
    echo "  macOS:  brew install ovmf"
    echo "  Ubuntu: sudo apt install ovmf"
    echo "  Fedora: sudo dnf install edk2-ovmf"
    exit 1
fi

if [ "$VERBOSE" = true ]; then
    echo "OVMF firmware found:"
    echo "  CODE: $OVMF_CODE"
    if [ -n "$OVMF_VARS" ]; then
        echo "  VARS: $OVMF_VARS"
    fi
fi

# If --check flag is set, just verify OVMF exists and exit
if [ "$CHECK_ONLY" = true ]; then
    echo "OVMF found"
    exit 0
fi

# Check for required tools
check_tool() {
    if ! command -v "$1" &> /dev/null; then
        echo -e "${RED}Error: $1 is required but not installed${NC}"
        return 1
    fi
    return 0
}

echo -e "${GREEN}=== EMBODIOS Secure Boot Test ===${NC}"
echo ""

echo "Checking required tools..."
if ! check_tool "qemu-system-x86_64"; then
    echo ""
    echo "Install QEMU:"
    echo "  macOS:  brew install qemu"
    echo "  Ubuntu: sudo apt install qemu-system-x86"
    exit 1
fi

# Determine what to boot
BOOT_SOURCE=""
BOOT_TYPE=""

if [ "$USE_ISO" = true ]; then
    if [ ! -f "$ISO_PATH" ]; then
        echo -e "${RED}Error: ISO not found: $ISO_PATH${NC}"
        echo "Build ISO first: cd kernel && ./scripts/make_iso.sh"
        exit 1
    fi
    BOOT_SOURCE="$ISO_PATH"
    BOOT_TYPE="ISO"
elif [ "$USE_SIGNED" = true ]; then
    if [ ! -f "$KERNEL_DIR/embodios.elf.signed" ]; then
        echo -e "${RED}Error: Signed kernel not found${NC}"
        echo "Create signed kernel: cd kernel && make sign"
        exit 1
    fi
    BOOT_SOURCE="$KERNEL_DIR/embodios.elf.signed"
    BOOT_TYPE="Signed Kernel"
else
    if [ ! -f "$KERNEL_DIR/embodios.elf" ]; then
        echo -e "${RED}Error: Kernel not found${NC}"
        echo "Build kernel: cd kernel && make"
        exit 1
    fi
    BOOT_SOURCE="$KERNEL_DIR/embodios.elf"
    BOOT_TYPE="Kernel"
fi

echo "Boot configuration:"
echo "  Type:     $BOOT_TYPE"
echo "  Source:   $BOOT_SOURCE"
echo "  Memory:   $MEMORY"
echo "  Firmware: OVMF UEFI"
if [ "$USE_SIGNED" = true ]; then
    echo "  Secure Boot: Enabled"
fi
echo ""

# Create temporary VARS file (writable copy)
TEMP_VARS=$(mktemp)
if [ -n "$OVMF_VARS" ] && [ -f "$OVMF_VARS" ]; then
    cp "$OVMF_VARS" "$TEMP_VARS"
else
    # Create empty vars file if not available
    touch "$TEMP_VARS"
fi

# Cleanup on exit
cleanup() {
    rm -f "$TEMP_VARS"
}
trap cleanup EXIT

# Build QEMU command
QEMU_CMD="qemu-system-x86_64"
QEMU_ARGS=(
    "-m" "$MEMORY"
    "-serial" "stdio"
    "-drive" "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
)

# Add VARS if available
if [ -n "$OVMF_VARS" ] && [ -f "$OVMF_VARS" ]; then
    QEMU_ARGS+=("-drive" "if=pflash,format=raw,file=$TEMP_VARS")
fi

# Add boot source
if [ "$USE_ISO" = true ]; then
    QEMU_ARGS+=("-cdrom" "$BOOT_SOURCE")
else
    QEMU_ARGS+=("-kernel" "$BOOT_SOURCE")
fi

# Add VGA for better output
QEMU_ARGS+=("-vga" "std")

if [ "$VERBOSE" = true ]; then
    echo "QEMU command:"
    echo "$QEMU_CMD ${QEMU_ARGS[*]}"
    echo ""
fi

echo -e "${GREEN}Starting QEMU with OVMF firmware...${NC}"
echo "Press Ctrl+C to exit"
echo ""

# Run QEMU
"$QEMU_CMD" "${QEMU_ARGS[@]}"
