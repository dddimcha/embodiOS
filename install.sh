#!/bin/bash
# NOVA Installation Script

set -e

NOVA_VERSION="${NOVA_VERSION:-latest}"
INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"
NOVA_HOME="${HOME}/.nova"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}Installing NOVA...${NC}"

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

# Create NOVA home directory
mkdir -p "$NOVA_HOME"

# Download NOVA
DOWNLOAD_URL="https://github.com/nova-os/nova/releases/download/${NOVA_VERSION}/nova-${PLATFORM}-${ARCH}"

echo -e "${BLUE}Downloading NOVA for ${PLATFORM}/${ARCH}...${NC}"
curl -fsSL "$DOWNLOAD_URL" -o /tmp/nova || {
    echo -e "${RED}Failed to download NOVA${NC}"
    echo "Trying alternative installation method..."
    
    # Fall back to Python installation
    if command -v python3 &> /dev/null; then
        echo -e "${BLUE}Installing via pip...${NC}"
        python3 -m pip install nova-os
        echo -e "${GREEN}✓ NOVA installed via pip${NC}"
        exit 0
    else
        echo -e "${RED}Python 3 not found. Please install Python 3.8+${NC}"
        exit 1
    fi
}

# Make executable
chmod +x /tmp/nova

# Install
sudo mv /tmp/nova "$INSTALL_DIR/nova" || {
    echo -e "${RED}Failed to install to $INSTALL_DIR${NC}"
    echo "Trying user installation..."
    mkdir -p "$HOME/.local/bin"
    mv /tmp/nova "$HOME/.local/bin/nova"
    INSTALL_DIR="$HOME/.local/bin"
    echo -e "${BLUE}Installed to $INSTALL_DIR${NC}"
    echo -e "${BLUE}Add $INSTALL_DIR to your PATH${NC}"
}

# Verify installation
if nova --version &> /dev/null; then
    echo -e "${GREEN}✓ NOVA installed successfully!${NC}"
    nova --version
else
    echo -e "${RED}Installation failed${NC}"
    exit 1
fi

echo -e "\n${GREEN}Get started with:${NC}"
echo "  nova init        # Initialize a new project"
echo "  nova build       # Build an AI-OS image"
echo "  nova run         # Run an AI-OS image"
echo ""
echo -e "${BLUE}Documentation: https://docs.nova.ai${NC}"