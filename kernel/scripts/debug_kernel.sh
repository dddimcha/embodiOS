#!/bin/bash
# EMBODIOS Kernel Debugging Script
#
# Launches QEMU with GDB server enabled for remote debugging.
# Use in conjunction with kernel/scripts/gdb_commands.gdb for full debugging experience.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(dirname "$SCRIPT_DIR")"
KERNEL_ELF="$KERNEL_DIR/embodios.elf"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Launches QEMU with GDB server for kernel debugging"
    echo ""
    echo "Options:"
    echo "  -m, --memory SIZE   Memory size (default: 256M)"
    echo "  -p, --port PORT     GDB server port (default: 1234)"
    echo "  -k, --kernel PATH   Kernel ELF path (default: ../embodios.elf)"
    echo "  -d, --disk PATH     Optional disk image path"
    echo "  -h, --help          Show this help message"
    echo ""
    echo "GDB Connection:"
    echo "  After QEMU starts with -S flag, it will pause at startup."
    echo "  Connect GDB with: gdb embodios.elf -x scripts/gdb_commands.gdb"
    echo "  Or manually: gdb embodios.elf"
    echo "               (gdb) target remote :1234"
    echo ""
    echo "Examples:"
    echo "  $0                          # Launch with defaults"
    echo "  $0 -m 512M                  # Launch with 512MB RAM"
    echo "  $0 -d tinystories-656k.img  # Launch with model disk"
}

# Default options
MEMORY="256M"
GDB_PORT="1234"
DISK_IMAGE=""

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -m|--memory)
            MEMORY="$2"
            shift 2
            ;;
        -p|--port)
            GDB_PORT="$2"
            shift 2
            ;;
        -k|--kernel)
            KERNEL_ELF="$2"
            shift 2
            ;;
        -d|--disk)
            DISK_IMAGE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option '$1'${NC}"
            usage
            exit 1
            ;;
    esac
done

# Verify kernel exists
if [ ! -f "$KERNEL_ELF" ]; then
    echo -e "${RED}Error: Kernel not found at $KERNEL_ELF${NC}"
    echo "Build the kernel first with: cd kernel && make"
    exit 1
fi

# Verify kernel has debug symbols
if ! file "$KERNEL_ELF" | grep -q "not stripped"; then
    echo -e "${YELLOW}Warning: Kernel appears to be stripped (no debug symbols)${NC}"
    echo "For best debugging experience, rebuild with: cd kernel && make clean && make"
fi

# Build QEMU command
QEMU_CMD="qemu-system-x86_64"
QEMU_ARGS=(
    "-kernel" "$KERNEL_ELF"
    "-m" "$MEMORY"
    "-serial" "stdio"
    "-s"  # Start GDB server on port 1234 (or :$GDB_PORT)
    "-S"  # Pause at startup, wait for GDB connection
)

# Add optional disk image
if [ -n "$DISK_IMAGE" ]; then
    if [ ! -f "$DISK_IMAGE" ]; then
        echo -e "${RED}Error: Disk image not found at $DISK_IMAGE${NC}"
        exit 1
    fi
    QEMU_ARGS+=("-drive" "file=$DISK_IMAGE,format=raw,if=virtio")
fi

# Custom GDB port (non-default)
if [ "$GDB_PORT" != "1234" ]; then
    # Replace -s with -gdb tcp::$GDB_PORT
    QEMU_ARGS=("${QEMU_ARGS[@]/-s/}")  # Remove -s
    QEMU_ARGS+=("-gdb" "tcp::$GDB_PORT")
fi

# Display launch information
echo -e "${GREEN}=== EMBODIOS Kernel Debug Launch ===${NC}"
echo ""
echo -e "${BLUE}Kernel:${NC}      $KERNEL_ELF"
echo -e "${BLUE}Memory:${NC}      $MEMORY"
echo -e "${BLUE}GDB Port:${NC}    $GDB_PORT"
if [ -n "$DISK_IMAGE" ]; then
    echo -e "${BLUE}Disk Image:${NC}  $DISK_IMAGE"
fi
echo ""
echo -e "${YELLOW}QEMU will start and PAUSE waiting for GDB connection.${NC}"
echo ""
echo -e "Connect GDB with:"
echo -e "  ${GREEN}gdb $KERNEL_ELF -x scripts/gdb_commands.gdb${NC}"
echo ""
echo -e "Or manually:"
echo -e "  ${GREEN}gdb $KERNEL_ELF${NC}"
echo -e "  ${GREEN}(gdb) target remote :$GDB_PORT${NC}"
echo -e "  ${GREEN}(gdb) continue${NC}"
echo ""
echo -e "${BLUE}Starting QEMU...${NC}"
echo ""

# Launch QEMU
exec "$QEMU_CMD" "${QEMU_ARGS[@]}"
