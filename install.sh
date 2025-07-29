#!/bin/bash
# EMBODIOS Installation Script

set -e

EMBODIOS_VERSION="${EMBODIOS_VERSION:-latest}"
INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"
EMBODIOS_HOME="${HOME}/.embodi"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}Installing EMBODIOS...${NC}"

# Detect OS
OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Linux*)     PLATFORM="linux";;
    Darwin*)    PLATFORM="macos";;
    *)          echo -e "${RED}Unsupported OS: $OS${NC}"; exit 1;;
esac

case "$ARCH" in
    x86_64)     ARCH="amd64";;
    aarch64)    ARCH="arm64";;
    arm64)      ARCH="arm64";;
    *)          echo -e "${RED}Unsupported architecture: $ARCH${NC}"; exit 1;;
esac

# Create EMBODIOS home directory
mkdir -p "$EMBODIOS_HOME"

# Download EMBODIOS
DOWNLOAD_URL="https://github.com/embodi-os/embodi/releases/download/${EMBODIOS_VERSION}/embodi-${PLATFORM}-${ARCH}"

echo -e "${BLUE}Downloading EMBODIOS for ${PLATFORM}/${ARCH}...${NC}"
curl -fsSL "$DOWNLOAD_URL" -o /tmp/embodi || {
    echo -e "${RED}Failed to download EMBODIOS${NC}"
    echo "Trying alternative installation method..."
    
    # Fall back to Python installation
    if command -v python3 &> /dev/null; then
        echo -e "${BLUE}Installing via pip...${NC}"
        python3 -m pip install embodi-os
        echo -e "${GREEN}✓ EMBODIOS installed via pip${NC}"
        exit 0
    else
        echo -e "${RED}Python 3 not found. Please install Python 3.8+${NC}"
        exit 1
    fi
}

# Make executable
chmod +x /tmp/embodi

# Install
sudo mv /tmp/embodi "$INSTALL_DIR/embodi" || {
    echo -e "${RED}Failed to install to $INSTALL_DIR${NC}"
    echo "Trying user installation..."
    mkdir -p "$HOME/.local/bin"
    mv /tmp/embodi "$HOME/.local/bin/embodi"
    INSTALL_DIR="$HOME/.local/bin"
    echo -e "${BLUE}Installed to $INSTALL_DIR${NC}"
    echo -e "${BLUE}Add $INSTALL_DIR to your PATH${NC}"
}

# Verify installation
if embodi --version &> /dev/null; then
    echo -e "${GREEN}✓ EMBODIOS installed successfully!${NC}"
    embodi --version
else
    echo -e "${RED}Installation failed${NC}"
    exit 1
fi

echo -e "\n${GREEN}Get started with:${NC}"
echo "  embodi init        # Initialize a new project"
echo "  embodi build       # Build an AI-OS image"
echo "  embodi run         # Run an AI-OS image"
echo ""
echo -e "${BLUE}Documentation: https://docs.embodi.ai${NC}"