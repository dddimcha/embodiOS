#!/bin/bash
# EMBODIOS One-Click ISO Builder
# Builds a bootable ISO using Docker for reproducible builds

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
DOCKER_IMAGE="embodios-builder:latest"
OUTPUT_ISO="$PROJECT_ROOT/embodios.iso"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== EMBODIOS One-Click ISO Builder ===${NC}"
echo ""

# Default options
INCLUDE_MODELS=false
MODEL_PATH=""
VERBOSE=false
REBUILD_IMAGE=false
SKIP_BUILD_CHECK=false

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "One-click script to build EMBODIOS bootable ISO using Docker"
    echo ""
    echo "Options:"
    echo "  --model PATH      Include specific model in ISO (e.g., models/model.gguf)"
    echo "  --models          Include all models from ./models directory"
    echo "  --output FILE     Output ISO filename (default: embodios.iso)"
    echo "  --rebuild         Force rebuild of Docker image"
    echo "  --verbose, -v     Verbose output"
    echo "  --help, -h        Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                                    # Build basic ISO"
    echo "  $0 --models                           # Build ISO with all models"
    echo "  $0 --model models/qwen2.5-0.5b.gguf  # Build with specific model"
    echo "  $0 --output custom.iso                # Custom output filename"
    echo ""
    echo "Requirements:"
    echo "  - Docker must be installed and running"
    echo ""
    echo "Output:"
    echo "  - Bootable ISO image: $OUTPUT_ISO"
    echo "  - Test with: qemu-system-x86_64 -cdrom embodios.iso -m 256M -serial stdio"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --model)
            MODEL_PATH="$2"
            INCLUDE_MODELS=true
            shift 2
            ;;
        --models)
            INCLUDE_MODELS=true
            shift
            ;;
        --output|-o)
            OUTPUT_ISO="$PROJECT_ROOT/$2"
            shift 2
            ;;
        --rebuild)
            REBUILD_IMAGE=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option '$1'${NC}"
            echo ""
            usage
            exit 1
            ;;
    esac
done

# Check for Docker
check_docker() {
    if ! command -v docker &> /dev/null; then
        echo -e "${RED}Error: Docker is not installed${NC}"
        echo ""
        echo "Install Docker:"
        echo "  macOS:  https://docs.docker.com/desktop/install/mac-install/"
        echo "  Linux:  https://docs.docker.com/engine/install/"
        echo "  Windows: https://docs.docker.com/desktop/install/windows-install/"
        return 1
    fi

    # Check if Docker daemon is running
    if ! docker info &> /dev/null; then
        echo -e "${RED}Error: Docker daemon is not running${NC}"
        echo "Please start Docker and try again"
        return 1
    fi

    return 0
}

echo "Checking Docker..."
if ! check_docker; then
    exit 1
fi
echo -e "${GREEN}✓ Docker is available${NC}"
echo ""

# Check if Docker image exists
check_image() {
    if docker image inspect "$DOCKER_IMAGE" &> /dev/null; then
        return 0
    else
        return 1
    fi
}

# Build Docker image if needed
if [ "$REBUILD_IMAGE" = true ] || ! check_image; then
    if [ "$REBUILD_IMAGE" = true ]; then
        echo -e "${YELLOW}Rebuilding Docker image...${NC}"
    else
        echo -e "${YELLOW}Docker image not found, building...${NC}"
    fi
    echo "This may take several minutes on first build (cross-compiler compilation)"
    echo ""

    if [ "$VERBOSE" = true ]; then
        docker build -t "$DOCKER_IMAGE" "$PROJECT_ROOT"
    else
        docker build -t "$DOCKER_IMAGE" "$PROJECT_ROOT" > /tmp/embodios-build.log 2>&1 || {
            echo -e "${RED}Docker build failed. Check log:${NC}"
            cat /tmp/embodios-build.log
            exit 1
        }
    fi

    echo -e "${GREEN}✓ Docker image built successfully${NC}"
    echo ""
else
    echo -e "${GREEN}✓ Using existing Docker image${NC}"
    echo ""
fi

# Prepare build command
BUILD_CMD="cd /workspace && make -C kernel clean && make -C kernel"

# Build ISO command
ISO_CMD="make -C kernel iso-prod"
if [ "$INCLUDE_MODELS" = true ]; then
    if [ -n "$MODEL_PATH" ]; then
        # Specific model
        ISO_CMD="bash kernel/scripts/make_iso.sh --models"
        echo -e "${BLUE}Building ISO with model: $MODEL_PATH${NC}"
    else
        # All models
        ISO_CMD="bash kernel/scripts/make_iso.sh --models"
        echo -e "${BLUE}Building ISO with all models${NC}"
    fi
else
    echo -e "${BLUE}Building basic ISO (no models)${NC}"
fi

# Prepare Docker run command
DOCKER_RUN_CMD="docker run --rm"

# Add volume mounts
DOCKER_RUN_CMD="$DOCKER_RUN_CMD -v \"$PROJECT_ROOT/kernel:/workspace/kernel\""

# Mount models directory if including models
if [ "$INCLUDE_MODELS" = true ]; then
    DOCKER_RUN_CMD="$DOCKER_RUN_CMD -v \"$PROJECT_ROOT/models:/workspace/models\""
fi

# Set working directory
DOCKER_RUN_CMD="$DOCKER_RUN_CMD -w /workspace"

# Add image name
DOCKER_RUN_CMD="$DOCKER_RUN_CMD $DOCKER_IMAGE"

# Add build commands
DOCKER_RUN_CMD="$DOCKER_RUN_CMD bash -c '$BUILD_CMD && $ISO_CMD'"

echo ""
echo -e "${GREEN}Starting build process...${NC}"
echo ""

# Show command if verbose
if [ "$VERBOSE" = true ]; then
    echo -e "${BLUE}Docker command:${NC}"
    echo "$DOCKER_RUN_CMD"
    echo ""
fi

# Run Docker build
echo "Building kernel and creating ISO..."
echo ""

if [ "$VERBOSE" = true ]; then
    eval "$DOCKER_RUN_CMD"
else
    # Run with progress indicators
    eval "$DOCKER_RUN_CMD" 2>&1 | while IFS= read -r line; do
        if [[ "$line" =~ ^(CC|LD|GRUB|ISO) ]] || [[ "$line" =~ (Building|Creating|Copying) ]]; then
            echo "$line"
        fi
    done
fi

# Check if ISO was created
ISO_LOCATION="$PROJECT_ROOT/kernel/embodios.iso"
if [ -f "$ISO_LOCATION" ]; then
    # Move ISO to desired output location
    if [ "$OUTPUT_ISO" != "$ISO_LOCATION" ]; then
        mv "$ISO_LOCATION" "$OUTPUT_ISO"
    fi

    ISO_SIZE=$(du -h "$OUTPUT_ISO" | cut -f1)

    echo ""
    echo -e "${GREEN}=== Build Complete! ===${NC}"
    echo ""
    echo -e "${GREEN}✓ ISO created successfully${NC}"
    echo "  Location: $OUTPUT_ISO"
    echo "  Size:     $ISO_SIZE"
    echo ""
    echo -e "${BLUE}Next steps:${NC}"
    echo ""
    echo "1. Test in QEMU:"
    echo "   qemu-system-x86_64 -cdrom $OUTPUT_ISO -m 256M -serial stdio"
    echo ""
    echo "2. Write to USB drive:"
    echo "   sudo dd if=$OUTPUT_ISO of=/dev/sdX bs=4M status=progress"
    echo ""
    echo "3. Boot on physical hardware"
    echo ""
else
    echo -e "${RED}Error: ISO file not created${NC}"
    echo "Build may have failed. Run with --verbose for details"
    exit 1
fi
