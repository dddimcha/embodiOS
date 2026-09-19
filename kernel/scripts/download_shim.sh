#!/bin/bash
# EMBODIOS Shim Bootloader Downloader
# Downloads pre-signed shim bootloader for Microsoft UEFI CA compatibility

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(dirname "$SCRIPT_DIR")"
SHIM_DIR="$KERNEL_DIR/secure-boot/shim"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== EMBODIOS Shim Bootloader Downloader ===${NC}"
echo ""

# Create shim directory
mkdir -p "$SHIM_DIR"

# Parse arguments
FORCE=false
VERBOSE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --force|-f)
            FORCE=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --force       Force re-download even if files exist"
            echo "  --verbose     Verbose output"
            echo "  --help        Show this help"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Check if already downloaded
if [ -f "$SHIM_DIR/shimx64.efi" ] && [ -f "$SHIM_DIR/grubx64.efi" ] && [ "$FORCE" = false ]; then
    echo -e "${GREEN}Shim bootloader already downloaded${NC}"
    echo "  Location: $SHIM_DIR"
    echo "  Files: shimx64.efi, grubx64.efi"
    echo ""
    echo "Use --force to re-download"
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

echo "Checking required tools..."
MISSING_TOOLS=0
check_tool "curl" || MISSING_TOOLS=1
check_tool "ar" || MISSING_TOOLS=1
check_tool "tar" || MISSING_TOOLS=1

if [ $MISSING_TOOLS -eq 1 ]; then
    echo ""
    echo "Install missing tools:"
    echo "  macOS: brew install curl binutils gnu-tar"
    echo "  Ubuntu: sudo apt install curl binutils tar"
    exit 1
fi

# Ubuntu package URLs (using signed packages from Ubuntu repositories)
# These are signed by Microsoft's UEFI CA and will work with Secure Boot
SHIM_PACKAGE_URL="http://archive.ubuntu.com/ubuntu/pool/main/s/shim-signed/shim-signed_1.51.3+15.7-0ubuntu1_amd64.deb"
GRUB_PACKAGE_URL="http://archive.ubuntu.com/ubuntu/pool/main/g/grub2-signed/grub-efi-amd64-signed_1.187.6+2.06-2ubuntu14_amd64.deb"

echo "Downloading shim bootloader..."
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

cd "$TEMP_DIR"

# Download shim-signed package
if [ "$VERBOSE" = true ]; then
    echo "  Downloading shim-signed package..."
fi
curl -L -o shim-signed.deb "$SHIM_PACKAGE_URL" 2>/dev/null || {
    echo -e "${RED}Failed to download shim package${NC}"
    exit 1
}

# Download grub-efi-amd64-signed package
if [ "$VERBOSE" = true ]; then
    echo "  Downloading grub-efi-amd64-signed package..."
fi
curl -L -o grub-signed.deb "$GRUB_PACKAGE_URL" 2>/dev/null || {
    echo -e "${RED}Failed to download grub package${NC}"
    exit 1
}

echo "Extracting packages..."

# Extract shim package
if [ "$VERBOSE" = true ]; then
    echo "  Extracting shim-signed..."
fi
ar x shim-signed.deb
tar xf data.tar.* 2>/dev/null

# Find and copy shimx64.efi
if [ -f usr/lib/shim/shimx64.efi.signed ]; then
    cp usr/lib/shim/shimx64.efi.signed "$SHIM_DIR/shimx64.efi"
elif [ -f usr/lib/shim/shimx64.efi ]; then
    cp usr/lib/shim/shimx64.efi "$SHIM_DIR/shimx64.efi"
else
    echo -e "${RED}Error: shimx64.efi not found in package${NC}"
    exit 1
fi

# Clean up for grub extraction
rm -f data.tar.* control.tar.*

# Extract grub package
if [ "$VERBOSE" = true ]; then
    echo "  Extracting grub-efi-amd64-signed..."
fi
ar x grub-signed.deb
tar xf data.tar.* 2>/dev/null

# Find and copy grubx64.efi
if [ -f usr/lib/grub/x86_64-efi-signed/grubx64.efi.signed ]; then
    cp usr/lib/grub/x86_64-efi-signed/grubx64.efi.signed "$SHIM_DIR/grubx64.efi"
elif [ -f usr/lib/grub/x86_64-efi-signed/grubx64.efi ]; then
    cp usr/lib/grub/x86_64-efi-signed/grubx64.efi "$SHIM_DIR/grubx64.efi"
else
    echo -e "${RED}Error: grubx64.efi not found in package${NC}"
    exit 1
fi

# Verify files
echo ""
echo "Verifying downloaded files..."
if [ ! -f "$SHIM_DIR/shimx64.efi" ] || [ ! -f "$SHIM_DIR/grubx64.efi" ]; then
    echo -e "${RED}Error: Failed to extract bootloader files${NC}"
    exit 1
fi

# Get file sizes
SHIM_SIZE=$(du -h "$SHIM_DIR/shimx64.efi" | cut -f1)
GRUB_SIZE=$(du -h "$SHIM_DIR/grubx64.efi" | cut -f1)

echo -e "${GREEN}=== Shim Bootloader Downloaded Successfully ===${NC}"
echo "  Location: $SHIM_DIR"
echo "  shimx64.efi: $SHIM_SIZE"
echo "  grubx64.efi: $GRUB_SIZE"
echo ""
echo "These files are pre-signed by Microsoft's UEFI CA and can be used"
echo "with Secure Boot enabled systems."
echo ""
echo "To create a Secure Boot ISO with shim support:"
echo "  ./scripts/make_iso.sh --secure-boot --shim"
