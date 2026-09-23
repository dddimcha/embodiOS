#!/bin/bash
# EMBODIOS Production ISO Builder
# Creates a bootable ISO with embedded AI model

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
KERNEL_DIR="$ROOT_DIR/kernel"
ISO_DIR="$ROOT_DIR/build/iso"
GRUB_CFG="$ISO_DIR/boot/grub/grub.cfg"

# Default model (can be overridden)
# Default model matches what `embodi pull smollm` downloads
MODEL="${MODEL:-$ROOT_DIR/models/smollm-135m-instruct-q4_k_m.gguf}"
OUTPUT="${OUTPUT:-$ROOT_DIR/build/embodios.iso}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() { echo -e "${GREEN}[ISO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Check dependencies
check_deps() {
    log "Checking dependencies..."
    
    # Find grub-mkrescue (different names on different systems)
    GRUB_MKRESCUE=""
    if command -v grub-mkrescue &> /dev/null; then
        GRUB_MKRESCUE="grub-mkrescue"
    elif command -v grub2-mkrescue &> /dev/null; then
        GRUB_MKRESCUE="grub2-mkrescue"
    elif command -v x86_64-elf-grub-mkrescue &> /dev/null; then
        GRUB_MKRESCUE="x86_64-elf-grub-mkrescue"
    elif command -v i686-elf-grub-mkrescue &> /dev/null; then
        GRUB_MKRESCUE="i686-elf-grub-mkrescue"
    fi
    
    if [ -z "$GRUB_MKRESCUE" ]; then
        error "grub-mkrescue not found. Install: brew install x86_64-elf-grub (macOS) or apt install grub-pc-bin (Linux)"
    fi
    
    log "Using: $GRUB_MKRESCUE"

    if ! command -v xorriso &> /dev/null; then
        error "xorriso not found. Install: brew install xorriso (macOS) or apt install xorriso (Linux)"
    fi

    # Detect GRUB platform modules: i386-pc (BIOS El Torito) + x86_64-efi (UEFI ESP).
    # grub-mkrescue emits a hybrid BIOS+UEFI ISO automatically when both are present.
    GRUB_LIBDIR=""
    for d in /tmp/lib/grub /usr/lib/grub "$HOME/sysroot/usr/lib/grub" /usr/local/lib/grub; do
        if [ -d "$d/i386-pc" ]; then GRUB_LIBDIR="$d"; break; fi
    done
    if [ -n "$GRUB_LIBDIR" ]; then
        if [ -d "$GRUB_LIBDIR/x86_64-efi" ]; then
            log "GRUB modules: i386-pc + x86_64-efi found in $GRUB_LIBDIR -> hybrid BIOS+UEFI ISO"
        else
            warn "x86_64-efi GRUB modules not found in $GRUB_LIBDIR - ISO will be BIOS-only"
            warn "Install grub-efi-amd64-bin (or run scripts/bootstrap_toolchain.sh) for UEFI support"
        fi
    else
        warn "Could not locate GRUB module directory - relying on grub-mkrescue defaults"
    fi

    log "Dependencies OK"
}

# Build kernel with embedded model
build_kernel() {
    log "Building kernel with embedded model..."
    
    # Convert to absolute path
    if [[ "$MODEL" != /* ]]; then
        MODEL="$ROOT_DIR/$MODEL"
    fi
    
    cd "$KERNEL_DIR"
    make clean

    if [ ! -f "$MODEL" ]; then
        warn "Model not found: $MODEL"
        warn "Building kernel WITHOUT an embedded model"
        MODEL=""
        make GGUF_MODEL=
    else
        MODEL_SIZE=$(du -h "$MODEL" | cut -f1)
        log "Using model: $MODEL ($MODEL_SIZE)"
        # Use absolute path for make
        make GGUF_MODEL="$MODEL"
    fi
    
    if [ ! -f "embodios.elf" ]; then
        error "Kernel build failed"
    fi
    
    # Verify model was embedded by checking kernel size
    KERNEL_SIZE=$(stat -f%z "embodios.elf" 2>/dev/null || stat -c%s "embodios.elf" 2>/dev/null)
    if [ "$KERNEL_SIZE" -lt 10000000 ]; then
        warn "Kernel is only $(du -h embodios.elf | cut -f1) - model may not be embedded!"
    else
        log "Kernel size: $(du -h embodios.elf | cut -f1) (model embedded)"
    fi
    
    log "Kernel built successfully"
}

# Create ISO directory structure
create_iso_structure() {
    log "Creating ISO structure..."
    
    rm -rf "$ISO_DIR"
    mkdir -p "$ISO_DIR/boot/grub"
    
    # Copy kernel
    cp "$KERNEL_DIR/embodios.elf" "$ISO_DIR/boot/"
    
    # Create manifest
    if [ -n "$MODEL" ] && [ -f "$MODEL" ]; then
        MODEL_NAME=$(basename "$MODEL" .gguf)
        MODEL_QUANT=$(echo "$MODEL_NAME" | grep -oP 'Q[0-9]_K(_M)?' || echo "unknown")
        MODEL_PARAMS=$(du -h "$MODEL" | cut -f1)
    else
        MODEL_NAME="none"
        MODEL_QUANT="none"
        MODEL_PARAMS="0"
    fi
    BUILD_DATE=$(date -Iseconds)
    GIT_COMMIT=$(cd "$ROOT_DIR" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    # Keep manifest in sync with the kernel version (single source of truth)
    KERNEL_VERSION=$(grep -oP 'kernel_version = "\Kv[0-9.]+' "$KERNEL_DIR/core/kernel.c" || echo "unknown")
    
    cat > "$ISO_DIR/boot/manifest.json" << EOF
{
  "version": "1.0.0",
  "name": "EMBODIOS Production ISO",
  "kernel": {
    "path": "/boot/embodios.elf",
    "version": "$KERNEL_VERSION",
    "arch": "x86_64"
  },
  "model": {
    "name": "$MODEL_NAME",
    "quantization": "$MODEL_QUANT",
    "size": "$MODEL_PARAMS"
  },
  "build": {
    "date": "$BUILD_DATE",
    "commit": "$GIT_COMMIT"
  }
}
EOF
    
    log "ISO structure created with manifest"
}

# Create GRUB config
create_grub_config() {
    log "Creating GRUB configuration..."
    
    # KERNEL_VERSION comes from kernel/core/kernel.c (set in create_iso_structure)
    local v="${KERNEL_VERSION:-unknown}"
    cat > "$GRUB_CFG" << EOF
# EMBODIOS Boot Configuration (BIOS + UEFI via GRUB multiboot2)
set timeout=3
set default=0

# Boot menu styling
set menu_color_normal=white/black
set menu_color_highlight=black/light-gray

menuentry "EMBODIOS $v - AI OS" {
    multiboot2 /boot/embodios.elf
    boot
}

menuentry "EMBODIOS $v - Debug Mode (Serial Console)" {
    multiboot2 /boot/embodios.elf debug serial
    boot
}

menuentry "EMBODIOS $v - Safe Mode (No AI)" {
    multiboot2 /boot/embodios.elf noai
    boot
}

menuentry "Reboot" {
    reboot
}

menuentry "Shutdown" {
    halt
}
EOF
    
    log "GRUB config created"
}

# Build ISO
build_iso() {
    log "Building ISO..."
    
    mkdir -p "$(dirname "$OUTPUT")"
    
    # GRUB_MKRESCUE is set in check_deps()
    log "Running: $GRUB_MKRESCUE -o $OUTPUT $ISO_DIR"
    $GRUB_MKRESCUE -o "$OUTPUT" "$ISO_DIR"

    if [ ! -f "$OUTPUT" ]; then
        error "ISO build failed"
    fi

    ISO_SIZE=$(du -h "$OUTPUT" | cut -f1)
    log "ISO created: $OUTPUT ($ISO_SIZE)"

    # Report El Torito boot entries (BIOS + UEFI) for verification
    if command -v xorriso &> /dev/null; then
        log "El Torito catalog:"
        xorriso -indev "$OUTPUT" -report_el_torito plain 2>/dev/null | grep -E '^(El Torito|.*boot img|.*platform|.*bootability)' || true
    fi
}

# Print usage
usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -m, --model PATH    Path to GGUF model (default: models/smollm-135m-instruct-q4_k_m.gguf)"
    echo "  -o, --output PATH   Output ISO path (default: build/embodios.iso)"
    echo "  -h, --help          Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                                    # Use default model"
    echo "  $0 -m models/tinyllama.gguf           # Use TinyLlama"
    echo "  $0 -m models/phi-2.Q4_K_M.gguf        # Use Phi-2"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -m|--model)
            MODEL="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            error "Unknown option: $1"
            ;;
    esac
done

# Main
main() {
    log "=========================================="
    log "EMBODIOS Production ISO Builder"
    log "=========================================="
    
    check_deps
    build_kernel
    create_iso_structure
    create_grub_config
    build_iso
    
    log "=========================================="
    log "ISO build complete!"
    log "=========================================="
    log ""
    log "To test in QEMU (BIOS):"
    log "  qemu-system-x86_64 -cdrom $OUTPUT -m 1024M -serial stdio -display none"
    log ""
    log "To test in QEMU (UEFI):"
    log "  qemu-system-x86_64 -bios /usr/share/OVMF/OVMF_CODE.fd -cdrom $OUTPUT -m 1024M -serial stdio -display none"
    log ""
    log "To write to USB (hybrid image, BIOS+UEFI):"
    log "  sudo dd if=$OUTPUT of=/dev/sdX bs=4M status=progress"
    log ""
}

main
