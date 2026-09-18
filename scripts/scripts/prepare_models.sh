#!/bin/bash
# EMBODIOS Model Preparation Script
#
# Downloads and prepares AI models for use with EMBODIOS kernel.
# Creates disk images suitable for QEMU testing.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
MODELS_DIR="$PROJECT_ROOT/models"
KERNEL_DIR="$PROJECT_ROOT/kernel"
OUTPUT_DIR="$KERNEL_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=== EMBODIOS Model Preparation ===${NC}"
echo ""

# Recommended models for EMBODIOS
declare -A MODELS
MODELS[tinystories-656k]="https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin|749664|Tiny Stories 656K (fastest, demo)"
MODELS[tinystories-15m]="https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin|30000000|Tiny Stories 15M (fast)"
MODELS[smollm-135m]="local|105454208|SmolLM 135M (balanced, GGUF)"

usage() {
    echo "Usage: $0 [command] [options]"
    echo ""
    echo "Commands:"
    echo "  list          List available models"
    echo "  download      Download model(s)"
    echo "  disk          Create disk image with model"
    echo "  all           Download all models and create disk"
    echo ""
    echo "Options:"
    echo "  --model NAME  Specify model name"
    echo "  --output DIR  Output directory (default: kernel/)"
    echo ""
    echo "Examples:"
    echo "  $0 list"
    echo "  $0 download --model tinystories-656k"
    echo "  $0 disk --model smollm-135m"
    echo "  $0 all"
}

list_models() {
    echo -e "${BLUE}Available Models:${NC}"
    echo ""
    printf "%-20s %-12s %s\n" "NAME" "SIZE" "DESCRIPTION"
    printf "%-20s %-12s %s\n" "----" "----" "-----------"

    for model in "${!MODELS[@]}"; do
        IFS='|' read -r url size desc <<< "${MODELS[$model]}"
        size_mb=$((size / 1024 / 1024))
        printf "%-20s %-12s %s\n" "$model" "${size_mb}MB" "$desc"
    done

    echo ""
    echo "Local models in $MODELS_DIR:"
    ls -lh "$MODELS_DIR"/*.gguf "$MODELS_DIR"/*.bin 2>/dev/null || echo "  (none found)"
}

download_model() {
    local model_name="$1"

    if [ -z "$model_name" ]; then
        echo -e "${RED}Error: No model specified${NC}"
        return 1
    fi

    if [ -z "${MODELS[$model_name]}" ]; then
        echo -e "${RED}Error: Unknown model '$model_name'${NC}"
        echo "Use '$0 list' to see available models"
        return 1
    fi

    IFS='|' read -r url size desc <<< "${MODELS[$model_name]}"

    if [ "$url" = "local" ]; then
        local_file="$MODELS_DIR/$model_name.gguf"
        if [ -f "$local_file" ]; then
            echo -e "${GREEN}Model $model_name already exists: $local_file${NC}"
            return 0
        else
            echo -e "${YELLOW}Model $model_name needs to be manually placed at:${NC}"
            echo "  $local_file"
            return 1
        fi
    fi

    local output_file="$MODELS_DIR/${model_name}.bin"

    if [ -f "$output_file" ]; then
        echo -e "${GREEN}Model already downloaded: $output_file${NC}"
        return 0
    fi

    echo -e "${BLUE}Downloading $model_name...${NC}"
    echo "  URL: $url"
    echo "  Size: $((size / 1024 / 1024))MB"

    curl -L -o "$output_file" "$url"

    # Verify size
    actual_size=$(stat -f%z "$output_file" 2>/dev/null || stat -c%s "$output_file" 2>/dev/null)

    echo -e "${GREEN}Downloaded: $output_file ($actual_size bytes)${NC}"
}

create_disk_image() {
    local model_name="$1"
    local model_file=""

    # Find model file
    if [ -f "$MODELS_DIR/${model_name}.gguf" ]; then
        model_file="$MODELS_DIR/${model_name}.gguf"
    elif [ -f "$MODELS_DIR/${model_name}.bin" ]; then
        model_file="$MODELS_DIR/${model_name}.bin"
    else
        echo -e "${RED}Error: Model file not found for $model_name${NC}"
        echo "Run '$0 download --model $model_name' first"
        return 1
    fi

    local model_size=$(stat -f%z "$model_file" 2>/dev/null || stat -c%s "$model_file" 2>/dev/null)
    local disk_size_mb=$(( (model_size / 1024 / 1024) + 10 ))  # Add 10MB padding

    local disk_file="$OUTPUT_DIR/${model_name}.img"

    echo -e "${BLUE}Creating disk image for $model_name...${NC}"
    echo "  Model: $model_file ($((model_size / 1024 / 1024))MB)"
    echo "  Disk: $disk_file (${disk_size_mb}MB)"

    # Create empty disk image
    dd if=/dev/zero of="$disk_file" bs=1M count="$disk_size_mb" 2>/dev/null

    # Copy model to disk (raw format, starting at sector 0)
    dd if="$model_file" of="$disk_file" bs=512 conv=notrunc 2>/dev/null

    echo -e "${GREEN}Disk image created: $disk_file${NC}"
    echo ""
    echo "To use with QEMU:"
    echo "  qemu-system-x86_64 -kernel kernel/embodios.elf -m 256M \\"
    echo "      -drive file=$disk_file,format=raw,if=virtio \\"
    echo "      -serial stdio"
}

prepare_all() {
    echo -e "${BLUE}Preparing all models...${NC}"
    echo ""

    # Download tinystories-656k (smallest, good for testing)
    download_model "tinystories-656k" || true

    # Create disk image for each available model
    for model in tinystories-656k smollm-135m; do
        model_file=""
        if [ -f "$MODELS_DIR/${model}.gguf" ]; then
            model_file="$MODELS_DIR/${model}.gguf"
        elif [ -f "$MODELS_DIR/${model}.bin" ]; then
            model_file="$MODELS_DIR/${model}.bin"
        fi

        if [ -n "$model_file" ]; then
            create_disk_image "$model" || true
        fi
    done

    echo ""
    echo -e "${GREEN}=== Model Preparation Complete ===${NC}"
    echo ""
    echo "Available disk images:"
    ls -lh "$OUTPUT_DIR"/*.img 2>/dev/null || echo "  (none created)"
}

# Parse command line
COMMAND=""
MODEL=""

while [[ $# -gt 0 ]]; do
    case $1 in
        list|download|disk|all)
            COMMAND="$1"
            shift
            ;;
        --model|-m)
            MODEL="$2"
            shift 2
            ;;
        --output|-o)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            usage
            exit 1
            ;;
    esac
done

# Execute command
case $COMMAND in
    list)
        list_models
        ;;
    download)
        download_model "$MODEL"
        ;;
    disk)
        create_disk_image "$MODEL"
        ;;
    all)
        prepare_all
        ;;
    "")
        usage
        ;;
    *)
        echo -e "${RED}Unknown command: $COMMAND${NC}"
        usage
        exit 1
        ;;
esac
