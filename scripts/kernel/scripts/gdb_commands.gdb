# EMBODIOS GDB Commands File
#
# Automatically load symbols and connect to QEMU GDB server.
# Use with: gdb embodios.elf -x scripts/gdb_commands.gdb
#
# This file provides:
# - Automatic connection to QEMU GDB server
# - Symbol loading
# - Common breakpoint setup
# - Custom commands for kernel state inspection
# - Pretty-printing for kernel data structures

# ==============================================================================
# Connection Setup
# ==============================================================================

# Connect to QEMU GDB server (default port 1234)
# QEMU must be started with -s -S flags
target remote :1234

# Set architecture to x86_64
set architecture i386:x86-64

# Enable pretty printing
set print pretty on
set print array on
set print array-indexes on

# Show source code context when stopping
set listsize 20

# ==============================================================================
# Symbol Loading
# ==============================================================================

# Symbols are loaded automatically from the ELF file passed to gdb
# Verify with: info files

# ==============================================================================
# Initial Breakpoints (commented out by default)
# ==============================================================================

# Uncomment to break at kernel entry point
# break kernel_main

# Uncomment to break when GDB stub initializes
# break gdb_stub_init

# Uncomment to break on any GDB exception
# break gdb_handle_exception

# Uncomment to break on panic/assertion failures
# break kernel_panic

# ==============================================================================
# Custom GDB Commands for Kernel Debugging
# ==============================================================================

# Command: print-gdb-state
# Prints the current state of the GDB stub
define print-gdb-state
    printf "=== GDB Stub State ===\n"

    # Check if gdb_stub structure is available
    if $_probe_symbol("gdb_stub") == 1
        printf "Initialized:     %d\n", gdb_stub.initialized
        printf "Connected:       %d\n", gdb_stub.connected
        printf "Single Stepping: %d\n", gdb_stub.single_stepping
        printf "Num Breakpoints: %d\n", gdb_stub.num_breakpoints
        printf "Packets RX:      %llu\n", gdb_stub.packets_rx
        printf "Packets TX:      %llu\n", gdb_stub.packets_tx
        printf "\n"
    else
        printf "GDB stub symbol not found. Is kernel built with debug symbols?\n"
    end
end

document print-gdb-state
Print the current state of the EMBODIOS GDB stub.
Shows initialization status, connection state, breakpoint count, and packet statistics.
Usage: print-gdb-state
end

# Command: print-breakpoints
# Lists all active GDB stub breakpoints
define print-breakpoints
    printf "=== Active Breakpoints ===\n"

    if $_probe_symbol("gdb_stub") == 1
        set $i = 0
        set $count = gdb_stub.num_breakpoints

        if $count == 0
            printf "No breakpoints set.\n"
        else
            while $i < $count
                if gdb_stub.breakpoints[$i].active
                    printf "[%d] Address: 0x%016lx  Saved: 0x%02x\n", \
                           $i, \
                           gdb_stub.breakpoints[$i].addr, \
                           gdb_stub.breakpoints[$i].saved_byte
                end
                set $i = $i + 1
            end
        end
    else
        printf "GDB stub symbol not found.\n"
    end
    printf "\n"
end

document print-breakpoints
List all active software breakpoints in the EMBODIOS GDB stub.
Shows breakpoint address and original byte value.
Usage: print-breakpoints
end

# Command: print-registers
# Pretty-print CPU register context
define print-registers
    printf "=== CPU Register Context ===\n"

    if $_probe_symbol("gdb_stub") == 1
        printf "RAX: 0x%016lx  RBX: 0x%016lx\n", gdb_stub.regs.rax, gdb_stub.regs.rbx
        printf "RCX: 0x%016lx  RDX: 0x%016lx\n", gdb_stub.regs.rcx, gdb_stub.regs.rdx
        printf "RSI: 0x%016lx  RDI: 0x%016lx\n", gdb_stub.regs.rsi, gdb_stub.regs.rdi
        printf "RBP: 0x%016lx  RSP: 0x%016lx\n", gdb_stub.regs.rbp, gdb_stub.regs.rsp
        printf "R8:  0x%016lx  R9:  0x%016lx\n", gdb_stub.regs.r8, gdb_stub.regs.r9
        printf "R10: 0x%016lx  R11: 0x%016lx\n", gdb_stub.regs.r10, gdb_stub.regs.r11
        printf "R12: 0x%016lx  R13: 0x%016lx\n", gdb_stub.regs.r12, gdb_stub.regs.r13
        printf "R14: 0x%016lx  R15: 0x%016lx\n", gdb_stub.regs.r14, gdb_stub.regs.r15
        printf "RIP: 0x%016lx  RFLAGS: 0x%016lx\n", gdb_stub.regs.rip, gdb_stub.regs.rflags
        printf "CS:  0x%016lx  SS:  0x%016lx\n", gdb_stub.regs.cs, gdb_stub.regs.ss
    else
        printf "Using standard GDB info registers:\n"
        info registers
    end
    printf "\n"
end

document print-registers
Print CPU register context from the EMBODIOS GDB stub.
Falls back to standard GDB 'info registers' if stub unavailable.
Usage: print-registers
end

# Command: print-kernel-info
# Display kernel version and build information
define print-kernel-info
    printf "=== Kernel Information ===\n"

    # Try to print kernel version string if available
    if $_probe_symbol("kernel_version")
        printf "Version: %s\n", kernel_version
    else
        printf "Version: <symbol not found>\n"
    end

    # Try to print kernel name
    if $_probe_symbol("kernel_name")
        printf "Name:    %s\n", kernel_name
    end

    printf "\n"
    printf "To view more kernel state, use:\n"
    printf "  print-gdb-state     - GDB stub status\n"
    printf "  print-breakpoints   - Active breakpoints\n"
    printf "  print-registers     - CPU registers\n"
    printf "  print-memory-map    - Memory layout\n"
    printf "\n"
end

document print-kernel-info
Display EMBODIOS kernel information and available debug commands.
Usage: print-kernel-info
end

# Command: print-memory-map
# Display kernel memory layout
define print-memory-map
    printf "=== Kernel Memory Map ===\n"
    printf "Use 'info files' to see loaded sections.\n"
    printf "Use 'info proc mappings' for detailed memory mappings (if available).\n"
    printf "\n"

    # Show loaded sections
    info files
    printf "\n"
end

document print-memory-map
Display kernel memory layout and loaded sections.
Shows ELF section addresses and memory mappings.
Usage: print-memory-map
end

# Command: kernel-bt
# Print backtrace with kernel-specific formatting
define kernel-bt
    printf "=== Kernel Backtrace ===\n"
    backtrace
    printf "\n"
end

document kernel-bt
Print kernel backtrace with formatting.
Usage: kernel-bt
end

# Command: reset-kernel
# Reset the kernel by disconnecting and reconnecting
define reset-kernel
    printf "Resetting kernel (disconnect and reconnect)...\n"
    disconnect
    target remote :1234
    printf "Reconnected to QEMU GDB server.\n"
end

document reset-kernel
Disconnect and reconnect to QEMU GDB server.
Useful for resetting after kernel modifications.
Usage: reset-kernel
end

# ==============================================================================
# Startup Messages
# ==============================================================================

printf "\n"
printf "================================================\n"
printf "  EMBODIOS Kernel Debugging Session\n"
printf "================================================\n"
printf "\n"
printf "Connected to QEMU GDB server on port 1234\n"
printf "Kernel symbols loaded from embodios.elf\n"
printf "\n"
printf "Custom Commands:\n"
printf "  print-gdb-state      - Show GDB stub status\n"
printf "  print-breakpoints    - List active breakpoints\n"
printf "  print-registers      - Display CPU registers\n"
printf "  print-kernel-info    - Show kernel information\n"
printf "  print-memory-map     - Display memory layout\n"
printf "  kernel-bt            - Show kernel backtrace\n"
printf "  reset-kernel         - Reconnect to QEMU\n"
printf "\n"
printf "Quick Start:\n"
printf "  break kernel_main    - Break at kernel entry\n"
printf "  continue             - Start kernel execution\n"
printf "  next / step          - Step through code\n"
printf "  print <var>          - Print variable\n"
printf "  x/32x <addr>         - Examine memory (hex)\n"
printf "\n"
printf "Type 'help' for GDB command reference.\n"
printf "Type 'help <command>' for command-specific help.\n"
printf "\n"
printf "Kernel is paused. Type 'continue' to start execution.\n"
printf "================================================\n"
printf "\n"
