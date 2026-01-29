#!/bin/bash
#
# EMBODIOS Hardware AI Test - Raspberry Pi 3B
#
# This script downloads the optimal model for RPi 3B and creates a bootable SD card.
#
# Raspberry Pi 3B Specs:
#   - 1GB RAM
#   - ARM Cortex-A53 (64-bit quad-core @ 1.2GHz)
#   - No GPU acceleration for LLMs
#
# Recommended Model: SmolLM2-135M (Q4_K_M quantization)
#   - Size: ~80MB
#   - RAM usage: ~150MB during inference
#   - Leaves ~800MB for kernel + buffers
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
MODELS_DIR="$PROJECT_DIR/models"
BUILD_DIR="$PROJECT_DIR/build"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()   { echo -e "${GREEN}[RPi3B]${NC} $*"; }
warn()  { echo -e "${YELLOW}[RPi3B]${NC} $*"; }
error() { echo -e "${RED}[RPi3B]${NC} $*" >&2; }

# Model configuration for Raspberry Pi 3B (1GB RAM)
MODEL_NAME="smollm2-135m-q4_k_m"
MODEL_FILE="smollm2-135m-instruct-q4_k_m.gguf"
MODEL_URL="https://huggingface.co/unsloth/SmolLM2-135M-Instruct-GGUF/resolve/main/SmolLM2-135M-Instruct-Q4_K_M.gguf"
MODEL_SIZE_MB=80

# Alternative: Even smaller model for tight memory situations
ALT_MODEL_NAME="smollm2-135m-q4_0"
ALT_MODEL_FILE="smollm2-135m-instruct-q4_0.gguf"
ALT_MODEL_URL="https://huggingface.co/unsloth/SmolLM2-135M-Instruct-GGUF/resolve/main/SmolLM2-135M-Instruct-Q4_0.gguf"
ALT_MODEL_SIZE_MB=70

usage() {
    cat <<EOF
EMBODIOS Raspberry Pi 3B Hardware Test

Usage: $0 <command> [options]

Commands:
  download          Download the recommended model for RPi 3B
  download-alt      Download alternative smaller model (Q4_0)
  build             Build ARM64 ISO with model
  flash <device>    Flash to SD card (e.g., /dev/sdb)
  info              Show hardware requirements
  test              Run QEMU ARM64 test

Examples:
  $0 download                    # Download SmolLM2-135M Q4_K_M
  $0 build                       # Build ARM64 ISO
  $0 flash /dev/sdb              # Flash to SD card

Recommended Model for RPi 3B:
  SmolLM2-135M (Q4_K_M) - ${MODEL_SIZE_MB}MB
  - Fits comfortably in 1GB RAM
  - Good quality for simple tasks
  - ~2-5 tokens/sec on RPi 3B

EOF
}

cmd_info() {
    cat <<EOF
${BLUE}═══════════════════════════════════════════════════════════════${NC}
${BLUE}  EMBODIOS - Raspberry Pi 3B Hardware Requirements${NC}
${BLUE}═══════════════════════════════════════════════════════════════${NC}

${GREEN}Target Hardware:${NC}
  Model:          Raspberry Pi 3 Model B
  CPU:            ARM Cortex-A53 (64-bit, 4 cores @ 1.2GHz)
  RAM:            1GB LPDDR2
  Storage:        MicroSD (8GB+ recommended)

${GREEN}Recommended Model:${NC}
  Name:           SmolLM2-135M-Instruct
  Quantization:   Q4_K_M (4-bit)
  Size:           ~${MODEL_SIZE_MB}MB
  RAM Usage:      ~150MB during inference
  Speed:          ~2-5 tokens/second

${GREEN}Alternative Model (smaller):${NC}
  Name:           SmolLM2-135M-Instruct
  Quantization:   Q4_0 (4-bit, simpler)
  Size:           ~${ALT_MODEL_SIZE_MB}MB
  RAM Usage:      ~120MB during inference
  Speed:          ~3-6 tokens/second

${GREEN}Memory Budget (1GB total):${NC}
  Kernel + Drivers:    ~50MB
  Model Weights:       ~80MB
  KV Cache:            ~100MB
  Inference Buffers:   ~200MB
  Available:           ~570MB headroom

${YELLOW}Note:${NC} RPi 3B is entry-level. For better performance:
  - RPi 4 (2GB+): Use SmolLM2-360M or TinyLlama-1.1B
  - RPi 5 (4GB+): Use Phi-2 or Mistral-7B-Q4

EOF
}

cmd_download() {
    local model_url="$MODEL_URL"
    local model_file="$MODEL_FILE"
    local model_size="$MODEL_SIZE_MB"
    
    if [[ "$1" == "alt" ]]; then
        model_url="$ALT_MODEL_URL"
        model_file="$ALT_MODEL_FILE"
        model_size="$ALT_MODEL_SIZE_MB"
        log "Downloading alternative (smaller) model..."
    else
        log "Downloading recommended model for RPi 3B..."
    fi
    
    mkdir -p "$MODELS_DIR"
    
    log "Model: $model_file (~${model_size}MB)"
    log "URL: $model_url"
    
    if [[ -f "$MODELS_DIR/$model_file" ]]; then
        warn "Model already exists: $MODELS_DIR/$model_file"
        read -p "Re-download? [y/N] " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            log "Using existing model"
            return 0
        fi
    fi
    
    log "Downloading..."
    curl -L --progress-bar -o "$MODELS_DIR/$model_file" "$model_url"
    
    log "Download complete: $MODELS_DIR/$model_file"
    ls -lh "$MODELS_DIR/$model_file"
}

cmd_build() {
    local model_file="$MODELS_DIR/$MODEL_FILE"
    
    if [[ ! -f "$model_file" ]]; then
        model_file="$MODELS_DIR/$ALT_MODEL_FILE"
    fi
    
    if [[ ! -f "$model_file" ]]; then
        error "No model found. Run '$0 download' first."
        exit 1
    fi
    
    log "Building ARM64 ISO for Raspberry Pi 3B..."
    log "Model: $model_file"
    
    cd "$PROJECT_DIR"
    ./embodi iso --model "$model_file" --arch arm64
    
    log "ISO created: $BUILD_DIR/embodios.iso"
}

cmd_flash() {
    local device="$1"
    
    if [[ -z "$device" ]]; then
        error "Usage: $0 flash <device>"
        error "Example: $0 flash /dev/sdb"
        exit 1
    fi
    
    if [[ ! -b "$device" ]]; then
        error "Device not found: $device"
        exit 1
    fi
    
    local iso="$BUILD_DIR/embodios.iso"
    if [[ ! -f "$iso" ]]; then
        error "ISO not found. Run '$0 build' first."
        exit 1
    fi
    
    warn "WARNING: This will ERASE all data on $device"
    warn "Device: $device"
    lsblk "$device" 2>/dev/null || true
    
    read -p "Are you sure? Type 'yes' to continue: " -r
    if [[ "$REPLY" != "yes" ]]; then
        log "Aborted."
        exit 0
    fi
    
    log "Flashing to $device..."
    sudo dd if="$iso" of="$device" bs=4M status=progress conv=fsync
    sudo sync
    
    log "Flash complete!"
    log ""
    log "Next steps:"
    log "  1. Insert SD card into Raspberry Pi 3B"
    log "  2. Connect HDMI and USB keyboard"
    log "  3. Power on"
    log "  4. Wait for EMBODIOS boot (~10 seconds)"
    log "  5. Type 'talk' to start AI chat"
}

cmd_test() {
    log "Running ARM64 QEMU test..."
    
    if ! command -v qemu-system-aarch64 &>/dev/null; then
        error "qemu-system-aarch64 not found"
        error "Install: brew install qemu (macOS) or apt install qemu-system-arm (Linux)"
        exit 1
    fi
    
    local kernel="$PROJECT_DIR/kernel/embodios.elf"
    if [[ ! -f "$kernel" ]]; then
        error "Kernel not found. Run './embodi build' first."
        exit 1
    fi
    
    log "Starting QEMU ARM64 (simulating RPi 3B)..."
    log "Press Ctrl+A, X to exit"
    
    qemu-system-aarch64 \
        -M raspi3b \
        -kernel "$kernel" \
        -serial stdio \
        -display none
}

# Main
case "${1:-}" in
    download)
        cmd_download
        ;;
    download-alt)
        cmd_download alt
        ;;
    build)
        cmd_build
        ;;
    flash)
        cmd_flash "$2"
        ;;
    info)
        cmd_info
        ;;
    test)
        cmd_test
        ;;
    help|-h|--help|"")
        usage
        ;;
    *)
        error "Unknown command: $1"
        usage
        exit 1
        ;;
esac
