#!/bin/bash
# Download required models for EMBODIOS testing with version tracking

set -e

echo "EMBODIOS Model Downloader"
echo "========================"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to calculate SHA256
calculate_sha256() {
    if command -v sha256sum > /dev/null; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum > /dev/null; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        echo "WARNING: Cannot calculate SHA256 - no suitable command found" >&2
        echo "unknown"
    fi
}

# Function to download and verify a model
download_model() {
    local model_key="$1"
    local model_info=$(jq -r ".models.${model_key}" models/manifest.json)
    
    if [ "$model_info" = "null" ]; then
        echo -e "${RED}✗ Model '$model_key' not found in manifest${NC}"
        return 1
    fi
    
    local name=$(echo "$model_info" | jq -r '.name')
    local version=$(echo "$model_info" | jq -r '.version')
    local revision=$(echo "$model_info" | jq -r '.revision')
    local filename=$(echo "$model_info" | jq -r '.files.gguf.filename')
    local expected_size=$(echo "$model_info" | jq -r '.files.gguf.size')
    local expected_sha256=$(echo "$model_info" | jq -r '.files.gguf.sha256')
    local url=$(echo "$model_info" | jq -r '.files.gguf.url')
    
    local model_dir="models/$model_key"
    local model_path="$model_dir/$filename"
    local version_file="$model_dir/.version"
    
    mkdir -p "$model_dir"
    
    echo "Model: $name"
    echo "Version: $version (revision: $revision)"
    echo ""
    
    # Check if model exists and verify version
    if [ -f "$model_path" ] && [ -f "$version_file" ]; then
        local current_version=$(cat "$version_file")
        if [ "$current_version" = "$version" ]; then
            echo -e "${GREEN}✓ Model already exists with correct version${NC}"
            
            # Verify checksum
            echo -n "Verifying checksum... "
            local actual_sha256=$(calculate_sha256 "$model_path")
            if [ "$actual_sha256" = "$expected_sha256" ] || [ "$actual_sha256" = "unknown" ]; then
                echo -e "${GREEN}OK${NC}"
                return 0
            else
                echo -e "${RED}FAILED${NC}"
                echo "Expected: $expected_sha256"
                echo "Actual:   $actual_sha256"
                echo "Model file may be corrupted, re-downloading..."
                rm -f "$model_path" "$version_file"
            fi
        else
            echo -e "${YELLOW}⚠ Different version found${NC}"
            echo "Current: $current_version"
            echo "Required: $version"
            echo "Re-downloading..."
            rm -f "$model_path" "$version_file"
        fi
    fi
    
    # Download model
    echo "Downloading $name..."
    echo "Size: $(( expected_size / 1024 / 1024 )) MB"
    echo "URL: $url"
    echo ""
    
    # Download with curl (shows progress)
    if ! curl -L -o "$model_path" "$url"; then
        echo -e "${RED}✗ Failed to download model${NC}"
        return 1
    fi
    
    # Verify size
    actual_size=$(stat -f%z "$model_path" 2>/dev/null || stat -c%s "$model_path" 2>/dev/null || echo 0)
    if [ "$actual_size" -ne "$expected_size" ]; then
        echo -e "${YELLOW}⚠ Warning: File size mismatch${NC}"
        echo "Expected: $expected_size bytes"
        echo "Actual:   $actual_size bytes"
    fi
    
    # Verify checksum
    echo -n "Verifying checksum... "
    actual_sha256=$(calculate_sha256 "$model_path")
    if [ "$actual_sha256" = "$expected_sha256" ]; then
        echo -e "${GREEN}OK${NC}"
    elif [ "$actual_sha256" = "unknown" ]; then
        echo -e "${YELLOW}SKIPPED${NC}"
    else
        echo -e "${RED}FAILED${NC}"
        echo "Expected: $expected_sha256"
        echo "Actual:   $actual_sha256"
        echo -e "${RED}WARNING: Checksum verification failed!${NC}"
    fi
    
    # Save version info
    echo "$version" > "$version_file"
    echo "$revision" > "$model_dir/.revision"
    
    echo -e "${GREEN}✓ Successfully downloaded $name${NC}"
    echo "  Location: $model_path"
    echo "  Size: $(du -h $model_path | cut -f1)"
    echo "  Version: $version"
}

# Check for jq
if ! command -v jq > /dev/null; then
    echo -e "${RED}Error: jq is required but not installed${NC}"
    echo "Install with: brew install jq (macOS) or apt-get install jq (Linux)"
    exit 1
fi

# Check manifest exists
if [ ! -f "models/manifest.json" ]; then
    echo -e "${RED}Error: models/manifest.json not found${NC}"
    echo "Run this script from the EMBODIOS repository root"
    exit 1
fi

# Default to downloading TinyLlama if no argument provided
if [ $# -eq 0 ]; then
    models=("tinyllama")
else
    models=("$@")
fi

# Download requested models
for model in "${models[@]}"; do
    echo "----------------------------------------"
    download_model "$model"
    echo ""
done

# Show all available models
echo "----------------------------------------"
echo "Available models:"
echo ""
for model_dir in models/*/; do
    if [ -d "$model_dir" ] && [ "$model_dir" != "models/*/" ]; then
        model_name=$(basename "$model_dir")
        if [ -f "$model_dir/.version" ]; then
            version=$(cat "$model_dir/.version")
            echo "  • $model_name (version: $version)"
            ls -lh "$model_dir"/*.gguf 2>/dev/null | awk '{print "    - " $9 ": " $5}'
        fi
    fi
done

echo ""
echo "To download additional models:"
echo "  ./scripts/download-models.sh phi-2"
echo ""
echo "To use models in tests:"
echo "  pytest tests/integration/test_real_model_inference.py"
echo ""
echo "To run interactive demo:"
echo "  ./scripts/testing/test_interactive.sh"