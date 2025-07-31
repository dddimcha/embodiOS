#!/bin/bash
# Setup script for EMBODIOS development environment

set -e

echo "=== EMBODIOS Development Setup ==="
echo

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Function to install git hooks
install_git_hooks() {
    echo -e "${YELLOW}Installing Git hooks...${NC}"
    
    # Create hooks directory if it doesn't exist
    mkdir -p .git/hooks
    
    # Copy pre-commit hook
    if [ -f "scripts/pre-commit" ]; then
        cp scripts/pre-commit .git/hooks/pre-commit
        chmod +x .git/hooks/pre-commit
        echo -e "${GREEN}✓ Git pre-commit hook installed${NC}"
    else
        echo "Warning: scripts/pre-commit not found"
    fi
}

# Function to install pre-commit framework (optional)
install_precommit_framework() {
    echo -e "${YELLOW}Checking for pre-commit framework...${NC}"
    
    if command -v pre-commit &> /dev/null; then
        echo "pre-commit is already installed"
        pre-commit install
        echo -e "${GREEN}✓ pre-commit framework configured${NC}"
    else
        echo "pre-commit not found. To install it, run:"
        echo "  pip install pre-commit"
        echo "  pre-commit install"
    fi
}

# Function to setup Python environment
setup_python_env() {
    echo -e "${YELLOW}Setting up Python environment...${NC}"
    
    if command -v python3 &> /dev/null; then
        # Install development dependencies
        if [ -f "requirements-dev.txt" ]; then
            echo "Installing Python development dependencies..."
            pip3 install -r requirements-dev.txt || echo "Some dependencies failed to install"
        fi
        echo -e "${GREEN}✓ Python environment ready${NC}"
    else
        echo "Python 3 not found. Please install Python 3.8 or later."
    fi
}

# Function to check C development tools
check_c_tools() {
    echo -e "${YELLOW}Checking C development tools...${NC}"
    
    if command -v gcc &> /dev/null; then
        echo -e "${GREEN}✓ GCC found: $(gcc --version | head -1)${NC}"
    else
        echo "⚠️  GCC not found. Install Xcode Command Line Tools:"
        echo "  xcode-select --install"
    fi
    
    # Optional tools
    echo
    echo "Optional tools status:"
    command -v clang-format &> /dev/null && echo "  ✓ clang-format installed" || echo "  ○ clang-format not installed"
    command -v cppcheck &> /dev/null && echo "  ✓ cppcheck installed" || echo "  ○ cppcheck not installed"
    command -v valgrind &> /dev/null && echo "  ✓ valgrind installed" || echo "  ○ valgrind not installed (not available on macOS)"
}

# Main setup
echo "This script will set up your EMBODIOS development environment."
echo

# Install git hooks
install_git_hooks

# Setup Python environment
setup_python_env

# Check C tools
check_c_tools

echo
echo -e "${GREEN}=== Setup Complete ===${NC}"
echo
echo "Git hooks are now active. They will run automatically before each commit to:"
echo "  - Check Python syntax and style"
echo "  - Verify C code compilation"
echo "  - Detect large files and potential secrets"
echo "  - Run basic tests"
echo
echo "To skip hooks temporarily, use: git commit --no-verify"
echo
echo "Happy coding!"