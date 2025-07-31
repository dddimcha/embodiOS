# EMBODIOS Native Kernel

The EMBODIOS kernel is a bare-metal AI operating system kernel that runs AI models directly on hardware without traditional OS overhead.

## Architecture

The kernel is structured into several key components:

### Core Components
- **Boot System**: Multiboot2-compliant bootloader for x86_64, UEFI support for ARM64
- **Memory Management**: 
  - Physical Memory Manager (PMM) using buddy allocator
  - Virtual Memory Manager (VMM) with 4-level paging
  - Slab allocator for kernel heap management
- **CPU Management**: Feature detection, SIMD support for AI workloads
- **Console I/O**: VGA text mode (x86_64), UART (ARM64)

### Memory Layout

#### x86_64
- `0x0000000000000000 - 0x00007FFFFFFFFFFF`: User space
- `0xFFFFFFFF80000000 - 0xFFFFFFFF80100000`: Kernel image (1MB)
- `0xFFFFFFFF80100000 - 0xFFFFFFFF90000000`: Kernel heap (256MB)
- `0xFFFFFFFF90000000 - 0xFFFFFFFFA0000000`: VMM heap (256MB)

#### ARM64
- `0x0000000000000000 - 0x0000FFFFFFFFFFFF`: User space (48-bit)
- `0xFFFF000000000000 - 0xFFFF000000200000`: Kernel image (2MB)
- `0xFFFF000000200000 - 0xFFFF000010000000`: Kernel heap

## Building

### Prerequisites
- GCC cross-compiler for target architecture
- GNU Make
- GRUB2 (for x86_64 ISO creation)

### Build Commands
```bash
# Build for x86_64 (default)
make

# Build for ARM64
make ARCH=arm64

# Create bootable ISO (x86_64 only)
make iso

# Clean build artifacts
make clean
```

## Features

### Implemented
- ✅ Multiboot2 boot protocol (x86_64)
- ✅ 64-bit long mode initialization
- ✅ Physical memory management (buddy allocator)
- ✅ Virtual memory management (4-level paging)
- ✅ Kernel heap allocator (slab allocator)
- ✅ Basic console output
- ✅ CPU feature detection
- ✅ Kernel panic handler

### In Progress
- 🔄 Interrupt handling (IDT/GDT for x86_64, GIC for ARM64)
- 🔄 Model runtime integration
- 🔄 Command processor
- 🔄 Task scheduler

### Planned
- ⏳ Network stack
- ⏳ Persistent storage
- ⏳ Multi-core support
- ⏳ Power management

## Model Integration

The kernel can embed AI model weights directly in the binary:
- Model weights are included via assembly in the `.model_weights` section
- Weights are loaded from `_model_weights_start` to `_model_weights_end`
- The model runtime provides inference capabilities in pure C

## Development

### Adding New Features
1. Create header in `include/embodios/`
2. Implement in appropriate subdirectory
3. Add to Makefile sources
4. Update architecture-specific code if needed

### Testing
Currently, the kernel can be tested using:
- QEMU for x86_64: `qemu-system-x86_64 -kernel embodios.elf`
- QEMU for ARM64: `qemu-system-aarch64 -M virt -cpu cortex-a72 -kernel embodios.bin`

## License

See LICENSE file in the repository root.