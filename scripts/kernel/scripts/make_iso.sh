#!/bin/bash
# EMBODIOS Production ISO Builder
# Creates a bootable ISO image from the manifest

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$KERNEL_DIR/build"
ISO_DIR="$KERNEL_DIR/iso_build"
MANIFEST="$KERNEL_DIR/iso.manifest"
OUTPUT="$KERNEL_DIR/embodios.iso"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== EMBODIOS ISO Builder ===${NC}"
echo ""

# Parse arguments
INCLUDE_MODELS=false
VERBOSE=false
SECURE_BOOT=false
SHIM=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --models)
            INCLUDE_MODELS=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --output|-o)
            OUTPUT="$2"
            shift 2
            ;;
        --secure-boot)
            SECURE_BOOT=true
            shift
            ;;
        --shim)
            SHIM=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --models      Include AI models in ISO"
            echo "  --verbose     Verbose output"
            echo "  --output FILE Output ISO filename (default: embodios.iso)"
            echo "  --secure-boot Enable UEFI Secure Boot support"
            echo "  --shim        Include shim bootloader for Microsoft UEFI CA"
            echo "  --help        Show this help"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

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
check_tool "grub-mkrescue" || MISSING_TOOLS=1
check_tool "xorriso" || MISSING_TOOLS=1

if [ $MISSING_TOOLS -eq 1 ]; then
    echo ""
    echo "Install missing tools:"
    echo "  macOS: brew install grub xorriso"
    echo "  Ubuntu: sudo apt install grub-pc-bin xorriso"
    exit 1
fi

# Check kernel exists
if [ ! -f "$KERNEL_DIR/embodios.elf" ]; then
    echo -e "${YELLOW}Kernel not found, building...${NC}"
    cd "$KERNEL_DIR"
    make
fi

# Check for secure boot requirements
if [ "$SECURE_BOOT" = true ]; then
    echo "Secure Boot enabled - checking requirements..."

    # Check for signed kernel
    if [ ! -f "$KERNEL_DIR/embodios.elf.signed" ]; then
        echo -e "${YELLOW}Signed kernel not found, creating...${NC}"
        cd "$KERNEL_DIR"
        make sign
        if [ ! -f "$KERNEL_DIR/embodios.elf.signed" ]; then
            echo -e "${RED}Error: Failed to create signed kernel${NC}"
            echo "Run 'make sign' manually to troubleshoot"
            exit 1
        fi
    fi

    # Check for certificates
    if [ ! -f "$KERNEL_DIR/secure-boot/DB.crt" ]; then
        echo -e "${RED}Error: Secure boot certificates not found${NC}"
        echo "Run './scripts/generate_keys.sh' to create certificates"
        exit 1
    fi

    echo -e "${GREEN}Secure Boot requirements satisfied${NC}"
fi

# Check for shim bootloader if requested
if [ "$SHIM" = true ]; then
    echo "Shim bootloader enabled - checking requirements..."

    # Check for shim files
    if [ ! -f "$KERNEL_DIR/secure-boot/shim/shimx64.efi" ] || [ ! -f "$KERNEL_DIR/secure-boot/shim/grubx64.efi" ]; then
        echo -e "${YELLOW}Shim bootloader not found, downloading...${NC}"
        cd "$SCRIPT_DIR"
        ./download_shim.sh
        if [ ! -f "$KERNEL_DIR/secure-boot/shim/shimx64.efi" ] || [ ! -f "$KERNEL_DIR/secure-boot/shim/grubx64.efi" ]; then
            echo -e "${RED}Error: Failed to download shim bootloader${NC}"
            echo "Run './scripts/download_shim.sh' manually to troubleshoot"
            exit 1
        fi
    fi

    echo -e "${GREEN}Shim bootloader requirements satisfied${NC}"
fi

# Clean and create ISO directory structure
echo "Preparing ISO directory..."
rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR/boot/grub"
mkdir -p "$ISO_DIR/models"
mkdir -p "$ISO_DIR/config"
mkdir -p "$ISO_DIR/docs"

# Create EFI/BOOT directory if shim is enabled
if [ "$SHIM" = true ]; then
    mkdir -p "$ISO_DIR/EFI/BOOT"
fi

# Copy kernel
echo "Copying kernel..."
cp "$KERNEL_DIR/embodios.elf" "$ISO_DIR/boot/"

# Copy signed kernel and certificates if secure boot is enabled
if [ "$SECURE_BOOT" = true ]; then
    echo "Copying signed kernel for Secure Boot..."
    cp "$KERNEL_DIR/embodios.elf.signed" "$ISO_DIR/boot/"

    echo "Copying Secure Boot certificates..."
    mkdir -p "$ISO_DIR/boot/grub/secureboot"
    cp "$KERNEL_DIR/secure-boot/DB.crt" "$ISO_DIR/boot/grub/secureboot/"
    cp "$KERNEL_DIR/secure-boot/CERTIFICATES.txt" "$ISO_DIR/boot/grub/secureboot/" 2>/dev/null || true

    if [ "$VERBOSE" = true ]; then
        echo "  Signed kernel: embodios.elf.signed"
        echo "  Certificate: DB.crt"
    fi
fi

# Copy shim bootloader files if enabled
if [ "$SHIM" = true ]; then
    echo "Copying shim bootloader for Microsoft UEFI CA..."
    cp "$KERNEL_DIR/secure-boot/shim/shimx64.efi" "$ISO_DIR/EFI/BOOT/BOOTX64.EFI"
    cp "$KERNEL_DIR/secure-boot/shim/grubx64.efi" "$ISO_DIR/EFI/BOOT/grubx64.efi"

    if [ "$VERBOSE" = true ]; then
        echo "  BOOTX64.EFI (shimx64.efi): Copied"
        echo "  grubx64.efi: Copied"
    fi
fi

# Copy GRUB config
echo "Copying GRUB configuration..."
cp "$KERNEL_DIR/grub_iso/boot/grub/grub.cfg" "$ISO_DIR/boot/grub/"

# Process manifest
if [ -f "$MANIFEST" ]; then
    echo "Processing manifest..."
    while IFS=: read -r type src dst; do
        # Skip comments and empty lines
        [[ "$type" =~ ^#.*$ ]] && continue
        [[ -z "$type" ]] && continue

        # Resolve paths
        src_path="$KERNEL_DIR/$src"
        dst_path="$ISO_DIR/$dst"

        # Create destination directory
        mkdir -p "$(dirname "$dst_path")"

        # Copy if exists
        if [ -f "$src_path" ]; then
            if [ "$VERBOSE" = true ]; then
                echo "  $type: $src -> $dst"
            fi
            cp "$src_path" "$dst_path"
        elif [ "$VERBOSE" = true ]; then
            echo -e "  ${YELLOW}Skip: $src (not found)${NC}"
        fi
    done < "$MANIFEST"
fi

# Include models if requested
if [ "$INCLUDE_MODELS" = true ]; then
    echo "Including AI models..."
    MODEL_DIR="$KERNEL_DIR/../models"
    if [ -d "$MODEL_DIR" ]; then
        for model in "$MODEL_DIR"/*.bin "$MODEL_DIR"/*.gguf; do
            if [ -f "$model" ]; then
                echo "  Adding: $(basename "$model")"
                cp "$model" "$ISO_DIR/models/"
            fi
        done
    else
        echo -e "${YELLOW}  Models directory not found${NC}"
    fi
fi

# Copy example config
echo "Copying configuration..."
cp "$KERNEL_DIR/config/embodios.conf.example" "$ISO_DIR/config/" 2>/dev/null || true

# Create version file
echo "Creating version info..."
if [ "$SECURE_BOOT" = true ]; then
    VERSION_CONTENT="EMBODIOS Production ISO (Secure Boot Enabled)
Build Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Kernel: $(md5sum "$ISO_DIR/boot/embodios.elf" | cut -d' ' -f1)
Signed Kernel: $(md5sum "$ISO_DIR/boot/embodios.elf.signed" | cut -d' ' -f1)
Secure Boot: Enabled
Certificate: $(openssl x509 -in "$KERNEL_DIR/secure-boot/DB.crt" -noout -subject 2>/dev/null || echo "N/A")"

    if [ "$SHIM" = true ]; then
        VERSION_CONTENT="$VERSION_CONTENT
Shim Bootloader: Enabled (Microsoft UEFI CA signed)"
    fi

    echo "$VERSION_CONTENT" > "$ISO_DIR/VERSION"
else
    cat > "$ISO_DIR/VERSION" << EOF
EMBODIOS Production ISO
Build Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Kernel: $(md5sum "$ISO_DIR/boot/embodios.elf" | cut -d' ' -f1)
EOF
fi

# Create the ISO
echo ""
echo "Creating ISO image..."
grub-mkrescue -o "$OUTPUT" "$ISO_DIR" 2>/dev/null

if [ -f "$OUTPUT" ]; then
    ISO_SIZE=$(du -h "$OUTPUT" | cut -f1)
    echo ""
    echo -e "${GREEN}=== ISO Created Successfully ===${NC}"
    echo "  Output: $OUTPUT"
    echo "  Size:   $ISO_SIZE"
    if [ "$SECURE_BOOT" = true ]; then
        echo "  Secure Boot: Enabled"
        echo "  Signed Kernel: Included"
    fi
    if [ "$SHIM" = true ]; then
        echo "  Shim Bootloader: Included (Microsoft UEFI CA)"
    fi
    echo ""
    if [ "$SECURE_BOOT" = true ]; then
        echo "To test with QEMU (UEFI Secure Boot):"
        echo "  ./scripts/test_secureboot.sh --signed"
        echo ""
        echo "To test with QEMU (standard):"
        echo "  qemu-system-x86_64 -cdrom $OUTPUT -m 256M -serial stdio"
    else
        echo "To test with QEMU:"
        echo "  qemu-system-x86_64 -cdrom $OUTPUT -m 256M -serial stdio"
    fi
    echo ""
    echo "To write to USB:"
    echo "  sudo dd if=$OUTPUT of=/dev/sdX bs=4M status=progress"
else
    echo -e "${RED}Failed to create ISO${NC}"
    exit 1
fi

# Cleanup
rm -rf "$ISO_DIR"
