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
- **Task Scheduler**: RTOS-style priority scheduler with deadline support and priority inheritance
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

### Task Scheduler

The kernel implements an RTOS-style priority scheduler designed for mixed-criticality workloads, enabling safety-critical AI inference to preempt lower-priority tasks.

#### Priority Scheduling
- **Priority Levels**: 0-31 (0 = highest priority, 31 = lowest)
- **Preemptive**: High-priority tasks immediately preempt lower-priority tasks
- **Round-Robin**: Tasks with equal priority use round-robin scheduling (10-tick time quantum)
- **Timer-Based**: Preemption occurs on timer interrupts (10ms tick rate)

Interrupts are enabled at the end of `kernel_main()` once the heap, console,
model runtime and scheduler are up: the 8259 PIC is remapped to INT 0x20-0x2F,
PIT IRQ0 drives the system tick (100 Hz) and `scheduler_tick()`, and `sti`
sets IF. The boot context is adopted as task `main`, so the shell itself is
preemptible. Boot with `poll` on the kernel cmdline to keep the legacy
polling mode (no interrupts). Kernel test mode (`-append test`) runs before
the interrupt-enable point, so tests always execute in polling mode. Try
`tasktest` in the shell for a live preemption demo and `uptime`/`tasks` for
tick and scheduler statistics.

### Power Management

`kernel_loop` sleeps with `hlt` whenever the shell is idle and interrupts
are enabled (wakes on the next PIT tick, 100 Hz quantum) — host CPU usage
drops from ~100% (busy polling) to ~2% in QEMU. Shell commands (see
`core/cmd_power.c`):

- `shutdown` / `poweroff` / `halt` — ACPI S5 poweroff via PM1a_CNT
  (`SLP_TYP|SLP_EN`); the southbridge (PIIX4 on i440fx, ICH9 on Q35) is
  probed through PCI config space (PMBASE @ 0x40), QEMU default 0x600 is
  the fallback. QEMU exits with status 0.
- `reboot` / `reset` — PCI reset control (port 0xCF9), then 8042 keyboard
  controller pulse, then a deliberate triple fault as last resort.
- `power` — power status: uptime, idle `hlt` loop count (rough idle % of
  ticks), interrupt state (IF flag).

The kernel test framework keeps using `isa-debug-exit` (port 0x501) for its
own shutdown; it runs before interrupts are enabled and is unaffected.

#### Deadline Support
- Tasks can specify absolute deadlines (in timer ticks)
- Deadline-aware scheduling automatically boosts priority for tasks approaching deadlines (<10 ticks)
- Missed deadlines are logged for debugging and analysis
- Deadline of 0 means no deadline constraint

#### Priority Inheritance Protocol
Prevents unbounded priority inversion in mixed-criticality systems:
- When a high-priority task blocks on a resource held by a low-priority task, the low-priority task inherits the high priority
- Transitive inheritance: priority propagates through blocking chains (A waits for B waits for C)
- Priority is restored to original level when the resource is released
- Priority inversions are detected and logged with task details

#### API Functions
```c
// Task creation with priority
task_t* task_create(const char *name, task_func_t entry, void *arg, uint8_t priority);

// Priority management
void task_set_priority(task_t *task, uint8_t priority);
uint8_t task_get_priority(task_t *task);

// Deadline management
void task_set_deadline(task_t *task, uint64_t deadline_ticks);
uint64_t task_get_deadline(task_t *task);

// Preemption control (for critical sections)
void scheduler_disable_preemption(void);
void scheduler_enable_preemption(void);

// Priority inheritance (used internally by synchronization primitives)
void task_inherit_priority(task_t *from, task_t *to);
void task_restore_priority(task_t *task);
```

#### Use Cases
- **Safety-Critical Inference**: High-priority tasks for real-time control decisions (e.g., collision detection in robotics)
- **Background Processing**: Lower-priority tasks for model training or data logging
- **Time-Bounded Operations**: Deadline support for guaranteeing inference completion times
- **Mixed Workloads**: Run multiple AI models with different criticality levels on a single system

#### Scheduler Statistics
The scheduler tracks key metrics:
- Context switches: Total number of task switches
- Preemptions: Number of times high-priority tasks preempted running tasks
- Priority inversions: Number of detected priority inversion events
- Access via `scheduler_stats()` for debugging and performance analysis

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
- ✅ RTOS-style priority scheduler (0-31 priority levels)
- ✅ Deadline-aware scheduling with automatic priority boosting
- ✅ Priority inheritance protocol (prevents priority inversion)
- ✅ Preemptive multitasking with timer-based preemption

### In Progress
- 🔄 Interrupt handling (IDT/GDT for x86_64, GIC for ARM64)
- 🔄 Model runtime integration
- 🔄 Command processor

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