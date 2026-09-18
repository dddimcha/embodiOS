#!/bin/bash
# EMBODIOS Kernel Signing Script
# Signs the kernel binary for UEFI Secure Boot using sbsign

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(dirname "$SCRIPT_DIR")"
SECURE_BOOT_DIR="$KERNEL_DIR/secure-boot"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Parse arguments
VERBOSE=false
KERNEL_FILE=""
OUTPUT_FILE=""

show_help() {
    echo "Usage: $0 <kernel_file> [options]"
    echo ""
    echo "Signs a kernel binary for UEFI Secure Boot"
    echo ""
    echo "Arguments:"
    echo "  kernel_file   Path to kernel binary to sign"
    echo ""
    echo "Options:"
    echo "  --output FILE Output filename (default: <kernel>.signed)"
    echo "  --verbose     Verbose output"
    echo "  --help        Show this help"
    echo ""
    echo "Example:"
    echo "  $0 kernel/embodios.elf"
    echo "  $0 kernel/embodios.elf --output kernel/embodios-signed.elf"
}

# Check for help first
for arg in "$@"; do
    if [ "$arg" = "--help" ] || [ "$arg" = "-h" ]; then
        show_help
        exit 0
    fi
done

# Parse command line arguments
if [ $# -eq 0 ]; then
    echo -e "${RED}Error: No kernel file specified${NC}"
    echo ""
    show_help
    exit 1
fi

KERNEL_FILE="$1"
shift

while [[ $# -gt 0 ]]; do
    case $1 in
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --output|-o)
            OUTPUT_FILE="$2"
            shift 2
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            echo ""
            show_help
            exit 1
            ;;
    esac
done

echo -e "${GREEN}=== EMBODIOS Kernel Signer ===${NC}"
echo ""

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
check_tool "sbsign" || MISSING_TOOLS=1

if [ $MISSING_TOOLS -eq 1 ]; then
    echo ""
    echo "Install missing tools:"
    echo "  macOS: brew install sbsigntool"
    echo "  Ubuntu: sudo apt install sbsigntool"
    exit 1
fi

# Check kernel file exists
if [ ! -f "$KERNEL_FILE" ]; then
    echo -e "${RED}Error: Kernel file not found: $KERNEL_FILE${NC}"
    exit 1
fi

# Check secure boot keys exist
if [ ! -f "$SECURE_BOOT_DIR/DB.key" ] || [ ! -f "$SECURE_BOOT_DIR/DB.crt" ]; then
    echo -e "${RED}Error: Secure boot keys not found in $SECURE_BOOT_DIR${NC}"
    echo "Expected files:"
    echo "  - DB.key (private key)"
    echo "  - DB.crt (certificate)"
    exit 1
fi

# Set output filename
if [ -z "$OUTPUT_FILE" ]; then
    OUTPUT_FILE="${KERNEL_FILE}.signed"
fi

echo "Signing kernel..."
if [ "$VERBOSE" = true ]; then
    echo "  Input:  $KERNEL_FILE"
    echo "  Output: $OUTPUT_FILE"
    echo "  Key:    $SECURE_BOOT_DIR/DB.key"
    echo "  Cert:   $SECURE_BOOT_DIR/DB.crt"
fi

# Sign the kernel
sbsign --key "$SECURE_BOOT_DIR/DB.key" \
       --cert "$SECURE_BOOT_DIR/DB.crt" \
       --output "$OUTPUT_FILE" \
       "$KERNEL_FILE"

# Verify the signed kernel was created
if [ -f "$OUTPUT_FILE" ]; then
    OUTPUT_SIZE=$(du -h "$OUTPUT_FILE" | cut -f1)
    echo ""
    echo -e "${GREEN}=== Kernel Signed Successfully ===${NC}"
    echo "  Signed:  $OUTPUT_FILE"
    echo "  Size:    $OUTPUT_SIZE"
    echo ""
    echo "To verify signature:"
    echo "  sbverify --cert $SECURE_BOOT_DIR/DB.crt $OUTPUT_FILE"
else
    echo -e "${RED}Failed to sign kernel${NC}"
    exit 1
fi
