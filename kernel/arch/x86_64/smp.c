/* x86_64 SMP (Symmetric Multi-Processing) Boot Sequence */
#include <embodios/kernel.h>
#include <embodios/types.h>
#include <embodios/mm.h>
#include <embodios/console.h>
#include <embodios/cpu.h>

/* ============================================================================
 * APIC/x2APIC Constants and Registers
 * ============================================================================ */

/* APIC Base Address (from MSR) */
#define MSR_APIC_BASE       0x1B
#define APIC_BASE_ENABLE    (1 << 11)
#define APIC_BASE_X2APIC    (1 << 10)
#define APIC_BASE_BSP       (1 << 8)

/* Local APIC Register Offsets (Memory-Mapped) */
#define APIC_ID             0x020
#define APIC_VERSION        0x030
#define APIC_TPR            0x080
#define APIC_EOI            0x0B0
#define APIC_SPURIOUS       0x0F0
#define APIC_ICR_LOW        0x300
#define APIC_ICR_HIGH       0x310
#define APIC_LVT_TIMER      0x320
#define APIC_LVT_ERROR      0x370

/* ICR (Interrupt Command Register) Bits */
#define ICR_DEST_SHIFT      24
#define ICR_INIT            (5 << 8)
#define ICR_STARTUP         (6 << 8)
#define ICR_LEVEL_ASSERT    (1 << 14)
#define ICR_LEVEL_DEASSERT  (0 << 14)
#define ICR_DEST_PHYSICAL   (0 << 11)
#define ICR_DEST_ALL_EX_SELF (3 << 18)

/* Spurious Interrupt Vector Register */
#define APIC_SPURIOUS_ENABLE (1 << 8)
#define APIC_SPURIOUS_VECTOR 0xFF

/* Maximum number of CPUs supported */
#define MAX_CPUS            16

/* Per-CPU stack size (64KB) */
#define CPU_STACK_SIZE      (64 * 1024)

/* ============================================================================
 * SMP State Management
 * ============================================================================ */

/* Per-CPU data structure */
struct cpu_data {
    uint32_t apic_id;           /* APIC ID */
    uint32_t cpu_id;            /* Sequential CPU number */
    void *stack_base;           /* Stack base address */
    void *stack_top;            /* Stack top address */
    bool online;                /* CPU is online */
    bool bsp;                   /* Bootstrap processor flag */
} __attribute__((aligned(64)));

/* SMP global state */
static struct {
    bool initialized;
    uint32_t num_cpus;
    uint32_t num_online;
    uint64_t apic_base;
    bool x2apic_mode;
    struct cpu_data cpus[MAX_CPUS];
} smp_state = {
    .initialized = false,
    .num_cpus = 0,
    .num_online = 0,
    .apic_base = 0,
    .x2apic_mode = false
};

/* ============================================================================
 * MSR and APIC Access Functions
 * ============================================================================ */

/**
 * Read Model-Specific Register
 */
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/**
 * Write Model-Specific Register
 */
static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}

/**
 * Read APIC register (memory-mapped mode)
 */
static inline uint32_t apic_read(uint32_t reg)
{
    volatile uint32_t *apic = (volatile uint32_t *)(smp_state.apic_base + reg);
    return *apic;
}

/**
 * Write APIC register (memory-mapped mode)
 */
static inline void apic_write(uint32_t reg, uint32_t value)
{
    volatile uint32_t *apic = (volatile uint32_t *)(smp_state.apic_base + reg);
    *apic = value;
}

/* ============================================================================
 * APIC Initialization
 * ============================================================================ */

/**
 * Initialize Local APIC for the current CPU
 */
static void apic_init_current_cpu(void)
{
    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);

    /* Get APIC base address (bits 12-35, page-aligned) */
    smp_state.apic_base = apic_msr & 0xFFFFF000;

    /* Check if BSP (Bootstrap Processor) */
    bool is_bsp = (apic_msr & APIC_BASE_BSP) != 0;

    /* Enable APIC (bit 11) */
    apic_msr |= APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, apic_msr);

    /* Enable Local APIC via spurious interrupt vector register */
    uint32_t spurious = apic_read(APIC_SPURIOUS);
    spurious |= APIC_SPURIOUS_ENABLE | APIC_SPURIOUS_VECTOR;
    apic_write(APIC_SPURIOUS, spurious);

    /* Get APIC ID */
    uint32_t apic_id = apic_read(APIC_ID) >> 24;

    /* Record this CPU in our state */
    uint32_t cpu_id = smp_state.num_online;
    if (cpu_id < MAX_CPUS) {
        smp_state.cpus[cpu_id].apic_id = apic_id;
        smp_state.cpus[cpu_id].cpu_id = cpu_id;
        smp_state.cpus[cpu_id].bsp = is_bsp;
        smp_state.cpus[cpu_id].online = true;
        smp_state.num_online++;

        if (is_bsp) {
            console_printf("SMP: BSP APIC ID %u (CPU %u)\n", apic_id, cpu_id);
        }
    }
}

/* ============================================================================
 * Secondary CPU Startup
 * ============================================================================ */

/**
 * Send INIT IPI to a specific APIC ID
 */
static void send_init_ipi(uint32_t apic_id)
{
    /* Set destination APIC ID in ICR high register */
    apic_write(APIC_ICR_HIGH, apic_id << ICR_DEST_SHIFT);

    /* Send INIT IPI: INIT | LEVEL_ASSERT | PHYSICAL */
    apic_write(APIC_ICR_LOW, ICR_INIT | ICR_LEVEL_ASSERT | ICR_DEST_PHYSICAL);

    /* Wait for delivery */
    while (apic_read(APIC_ICR_LOW) & (1 << 12)) {
        __asm__ volatile("pause");
    }
}

/**
 * Send STARTUP IPI (SIPI) to a specific APIC ID
 */
static void send_startup_ipi(uint32_t apic_id, uint32_t vector)
{
    /* Set destination APIC ID */
    apic_write(APIC_ICR_HIGH, apic_id << ICR_DEST_SHIFT);

    /* Send STARTUP IPI with vector page (vector = startup address / 4096) */
    apic_write(APIC_ICR_LOW, ICR_STARTUP | ICR_DEST_PHYSICAL | vector);

    /* Wait for delivery */
    while (apic_read(APIC_ICR_LOW) & (1 << 12)) {
        __asm__ volatile("pause");
    }
}

/**
 * Simple delay loop
 */
static void udelay(uint32_t usec)
{
    /* Approximate delay using CPU cycles */
    /* Assumes ~1GHz CPU, adjust multiplier as needed */
    uint64_t cycles = (uint64_t)usec * 1000;
    uint64_t start = cpu_get_timestamp();
    while ((cpu_get_timestamp() - start) < cycles) {
        __asm__ volatile("pause");
    }
}

/**
 * Secondary CPU entry point (called by assembly trampoline)
 *
 * This is where secondary CPUs start executing after SIPI.
 * For now, this is a stub - actual implementation would require
 * assembly trampoline code at a low memory address.
 */
void smp_secondary_entry(void)
{
    /* Initialize this CPU's APIC */
    apic_init_current_cpu();

    /* Get our CPU ID */
    uint32_t cpu_id = cpu_get_id();

    console_printf("SMP: CPU %u online\n", cpu_id);

    /* TODO: Load GDT, IDT, page tables for this CPU */
    /* TODO: Initialize per-CPU data structures */
    /* TODO: Enable interrupts */

    /* Idle loop for now */
    while (1) {
        __asm__ volatile("hlt");
    }
}

/**
 * Start a secondary CPU using INIT-SIPI-SIPI sequence
 */
static bool start_secondary_cpu(uint32_t apic_id)
{
    /*
     * Intel MultiProcessor Specification: INIT-SIPI-SIPI sequence
     *
     * 1. Send INIT IPI to reset the target CPU
     * 2. Wait 10ms
     * 3. Send first SIPI with startup vector
     * 4. Wait 200us
     * 5. Send second SIPI with startup vector
     * 6. Wait for CPU to come online
     *
     * Note: For a complete implementation, we would need:
     * - Trampoline code at physical address 0x8000 (32KB)
     * - 16-bit real mode startup code
     * - Transition to protected mode, then long mode
     * - Jump to smp_secondary_entry()
     */

    /* For now, we'll just simulate the IPI sequence */
    /* A real implementation would require the trampoline code */

    console_printf("SMP: Starting CPU with APIC ID %u\n", apic_id);

    /* Send INIT IPI */
    send_init_ipi(apic_id);
    udelay(10000);  /* 10ms delay */

    /* Send first SIPI (vector = 0x08 for address 0x8000) */
    send_startup_ipi(apic_id, 0x08);
    udelay(200);    /* 200us delay */

    /* Send second SIPI */
    send_startup_ipi(apic_id, 0x08);
    udelay(200);    /* 200us delay */

    /*
     * In a real implementation, the secondary CPU would:
     * 1. Start executing at 0x8000
     * 2. Initialize itself
     * 3. Call smp_secondary_entry()
     * 4. Increment a counter or set a flag
     *
     * For now, we just return true to indicate we sent the IPIs.
     */

    return true;
}

/* ============================================================================
 * SMP Initialization
 * ============================================================================ */

/**
 * Allocate per-CPU stacks
 */
static bool allocate_cpu_stacks(void)
{
    for (uint32_t i = 0; i < smp_state.num_cpus; i++) {
        /* Skip BSP - it already has a stack */
        if (smp_state.cpus[i].bsp) {
            continue;
        }

        /* Allocate stack for this CPU */
        void *stack = kmalloc(CPU_STACK_SIZE);
        if (!stack) {
            console_printf("SMP: Failed to allocate stack for CPU %u\n", i);
            return false;
        }

        /* Stack grows downward, so top = base + size */
        smp_state.cpus[i].stack_base = stack;
        smp_state.cpus[i].stack_top = (uint8_t *)stack + CPU_STACK_SIZE;

        console_printf("SMP: Allocated stack for CPU %u: %p - %p\n",
                       i, stack, smp_state.cpus[i].stack_top);
    }

    return true;
}

/**
 * Initialize SMP (Symmetric Multi-Processing)
 *
 * This function:
 * 1. Initializes the BSP's Local APIC
 * 2. Detects the number of CPUs
 * 3. Allocates per-CPU stacks
 * 4. Starts secondary CPUs using INIT-SIPI-SIPI
 */
void smp_init(void)
{
    if (smp_state.initialized) {
        return;
    }

    console_printf("SMP: Initializing multi-processor support\n");

    /* Get number of CPUs from CPUID */
    smp_state.num_cpus = smp_num_cpus();
    console_printf("SMP: Detected %u CPU(s)\n", smp_state.num_cpus);

    if (smp_state.num_cpus > MAX_CPUS) {
        console_printf("SMP: Warning - limiting to %u CPUs\n", MAX_CPUS);
        smp_state.num_cpus = MAX_CPUS;
    }

    /* Initialize BSP's Local APIC */
    apic_init_current_cpu();

    /* If only 1 CPU, we're done */
    if (smp_state.num_cpus <= 1) {
        console_printf("SMP: Single processor system\n");
        smp_state.initialized = true;
        return;
    }

    /* Allocate stacks for secondary CPUs */
    if (!allocate_cpu_stacks()) {
        console_printf("SMP: Failed to allocate CPU stacks\n");
        return;
    }

    /*
     * Start secondary CPUs
     *
     * Note: This is currently a stub implementation.
     * A complete implementation would require:
     *
     * 1. Trampoline code in low memory (< 1MB)
     *    - 16-bit real mode entry point
     *    - Transition to protected mode
     *    - Enable paging and long mode
     *    - Jump to 64-bit entry point
     *
     * 2. Per-CPU GDT, IDT, page tables
     *
     * 3. Synchronization primitives
     *    - Wait for secondary CPUs to come online
     *    - Atomic counters for CPU count
     *
     * 4. Proper stack setup
     *    - Set RSP for each CPU
     *    - Initialize per-CPU data
     *
     * For now, we just send the INIT-SIPI-SIPI sequence
     * to demonstrate the boot protocol.
     */

    console_printf("SMP: Starting secondary CPUs\n");

    /*
     * In a real system, we would enumerate APIC IDs from ACPI MADT
     * For now, assume sequential APIC IDs starting from 0
     */
    for (uint32_t i = 1; i < smp_state.num_cpus; i++) {
        uint32_t apic_id = i;  /* Simplified - would come from ACPI */

        if (start_secondary_cpu(apic_id)) {
            console_printf("SMP: Sent startup IPIs to APIC ID %u\n", apic_id);
        } else {
            console_printf("SMP: Failed to start CPU with APIC ID %u\n", apic_id);
        }
    }

    console_printf("SMP: Initialization complete (%u CPUs detected, %u online)\n",
                   smp_state.num_cpus, smp_state.num_online);

    smp_state.initialized = true;
}

/**
 * Get number of online CPUs
 */
uint32_t smp_get_num_online(void)
{
    return smp_state.num_online;
}

/**
 * Check if SMP is initialized
 */
bool smp_is_initialized(void)
{
    return smp_state.initialized;
}

/**
 * Get current CPU's data structure
 */
struct cpu_data* smp_get_current_cpu(void)
{
    uint32_t apic_id = cpu_get_id();

    /* Find CPU by APIC ID */
    for (uint32_t i = 0; i < smp_state.num_online; i++) {
        if (smp_state.cpus[i].apic_id == apic_id) {
            return &smp_state.cpus[i];
        }
    }

    return NULL;
}
