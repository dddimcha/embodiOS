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
MODEL="${MODEL:-$ROOT_DIR/models/smollm-135m.gguf}"
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
    
    if ! command -v grub-mkrescue &> /dev/null && ! command -v grub2-mkrescue &> /dev/null; then
        error "grub-mkrescue not found. Install: brew install grub (macOS) or apt install grub-pc-bin (Linux)"
    fi
    
    if ! command -v xorriso &> /dev/null; then
        error "xorriso not found. Install: brew install xorriso (macOS) or apt install xorriso (Linux)"
    fi
    
    log "Dependencies OK"
}

# Build kernel with embedded model
build_kernel() {
    log "Building kernel with embedded model..."
    
    if [ ! -f "$MODEL" ]; then
        error "Model not found: $MODEL"
    fi
    
    MODEL_SIZE=$(du -h "$MODEL" | cut -f1)
    log "Using model: $MODEL ($MODEL_SIZE)"
    
    cd "$KERNEL_DIR"
    make clean
    make GGUF_MODEL="$MODEL"
    
    if [ ! -f "embodios.elf" ]; then
        error "Kernel build failed"
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
    MODEL_NAME=$(basename "$MODEL" .gguf)
    MODEL_QUANT=$(echo "$MODEL_NAME" | grep -oP 'Q[0-9]_K(_M)?' || echo "unknown")
    MODEL_PARAMS=$(du -h "$MODEL" | cut -f1)
    BUILD_DATE=$(date -Iseconds)
    GIT_COMMIT=$(cd "$ROOT_DIR" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    
    cat > "$ISO_DIR/boot/manifest.json" << EOF
{
  "version": "1.0.0",
  "name": "EMBODIOS Production ISO",
  "kernel": {
    "path": "/boot/embodios.elf",
    "version": "0.1.0-native",
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
    
    cat > "$GRUB_CFG" << 'EOF'
# EMBODIOS Boot Configuration
set timeout=5
set default=0

# Boot menu styling
set menu_color_normal=white/black
set menu_color_highlight=black/light-gray

menuentry "EMBODIOS - Bare Metal AI OS" {
    multiboot /boot/embodios.elf
    boot
}

menuentry "EMBODIOS - Debug Mode (Serial Console)" {
    multiboot /boot/embodios.elf debug serial
    boot
}

menuentry "EMBODIOS - Safe Mode (No AI)" {
    multiboot /boot/embodios.elf noai
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
    
    # Use grub-mkrescue or grub2-mkrescue
    GRUB_CMD="grub-mkrescue"
    if ! command -v grub-mkrescue &> /dev/null; then
        GRUB_CMD="grub2-mkrescue"
    fi
    
    $GRUB_CMD -o "$OUTPUT" "$ISO_DIR"
    
    if [ ! -f "$OUTPUT" ]; then
        error "ISO build failed"
    fi
    
    ISO_SIZE=$(du -h "$OUTPUT" | cut -f1)
    log "ISO created: $OUTPUT ($ISO_SIZE)"
}

# Print usage
usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -m, --model PATH    Path to GGUF model (default: models/smollm-135m.gguf)"
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
    log "To test in QEMU:"
    log "  qemu-system-x86_64 -cdrom $OUTPUT -m 1024M -serial stdio"
    log ""
    log "To write to USB:"
    log "  sudo dd if=$OUTPUT of=/dev/sdX bs=4M status=progress"
    log ""
}

main
