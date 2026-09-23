/* x86_64 SMP (Symmetric Multi-Processing) Boot Sequence
 *
 * Real AP bring-up via INIT-SIPI-SIPI:
 *   - BSP enables its Local APIC (MMIO identity-mapped on demand)
 *   - 16/32/64-bit trampoline (smp_trampoline.S) is copied to physical 0x8000
 *   - Each AP gets a 64KB stack, boots through the trampoline into
 *     smp_ap_main(), registers itself, calibrates its own LAPIC timer
 *     (ipi_init_ap -> lapic_timer_init_ap) and parks in `sti; hlt` with
 *     IF=1, woken by a wakeup IPI when work is posted (WS-A; the legacy
 *     IF=0 pause-polling loop remains only as a fallback when the AP
 *     cannot arm its LAPIC timer)
 *   - Work is delegated to APs through per-CPU mailboxes
 *     (smp_work_dispatch / smp_work_wait), used by parallel_inference.
 */
#include <embodios/kernel.h>
#include <embodios/types.h>
#include <embodios/mm.h>
#include <embodios/console.h>
#include <embodios/cpu.h>
#include <embodios/percpu.h>
#include <embodios/tsc.h>
#include <embodios/atomic.h>
#include <embodios/ipi.h>
#include <embodios/lapic_timer.h>
#include "../../include/arch/x86_64/paging.h"

/* ============================================================================
 * APIC Constants and Registers
 * ============================================================================ */

#define MSR_APIC_BASE       0x1B
#define APIC_BASE_ENABLE    (1 << 11)
#define APIC_BASE_BSP       (1 << 8)

/* Local APIC Register Offsets (Memory-Mapped) */
#define APIC_ID             0x020
#define APIC_TPR            0x080
#define APIC_EOI            0x0B0
#define APIC_SPURIOUS       0x0F0
#define APIC_ICR_LOW        0x300
#define APIC_ICR_HIGH       0x310
#define APIC_LVT_TIMER      0x320
#define APIC_LVT_LINT0      0x350
#define APIC_LVT_LINT1      0x360
#define APIC_LVT_ERROR      0x370

/* ICR (Interrupt Command Register) Bits */
#define ICR_DEST_SHIFT      24
#define ICR_INIT            (5 << 8)
#define ICR_STARTUP         (6 << 8)
#define ICR_LEVEL_ASSERT    (1 << 14)
#define ICR_DEST_PHYSICAL   (0 << 11)
#define ICR_DELIVERY_STATUS (1 << 12)

#define APIC_SPURIOUS_ENABLE (1 << 8)
#define APIC_SPURIOUS_VECTOR 0xFF
#define APIC_LVT_MASKED      (1 << 16)

#define MAX_CPUS            16
#define CPU_STACK_SIZE      (64 * 1024)

/* Trampoline load address (SIPI vector 0x08 -> physical 0x8000) */
#define TRAMP_PHYS          0x8000ULL
#define TRAMP_VECTOR        (TRAMP_PHYS >> 12)

/* Boot timeouts (TSC-frequency based) */
#define AP_ONLINE_TIMEOUT_US   2000000ULL   /* 2s per AP */
#define AP_STACK_ALIGN         16

/* ============================================================================
 * Trampoline interface (smp_trampoline.S)
 * ============================================================================ */

extern char smp_trampoline_start[];
extern char smp_trampoline_end[];
extern char tramp_gdt[];
extern char tramp_gdt_desc[];
extern char tramp_cr3[];
extern char tramp_stack_slot[];
extern char tramp_entry_slot[];
extern void smp_ap_boot_entry(void);

/* ============================================================================
 * SMP State Management
 * ============================================================================ */

struct cpu_data {
    uint32_t apic_id;           /* APIC ID */
    uint32_t cpu_id;            /* Sequential CPU number */
    void *stack_base;           /* Stack base address */
    void *stack_top;            /* Stack top address */
    bool online;                /* CPU is online */
    bool bsp;                   /* Bootstrap processor flag */
    volatile bool parked_if1;   /* AP parks with IF=1 (hlt + IPI wakeup);
                                 * false => legacy IF=0 polling fallback */
} __attribute__((aligned(64)));

/* Per-CPU work mailbox: BSP posts fn/arg and bumps seq; the AP executes the
 * function in its park loop and sets done = seq. One outstanding item per
 * CPU (dispatch spins if the previous item is still in flight). Stats are
 * written by the owning CPU only, read cross-CPU for the 'cpus' command.
 *
 * Wake protocol (WS-A): when the AP parks with IF=1 (parked_if1), seq++ is
 * followed by a wakeup IPI; the AP's cli/check/sti;hlt sequence closes the
 * post-vs-sleep race (a pending IPI wakes hlt immediately). In the IF=0
 * fallback the AP pause-polls seq and no IPI is sent. */
typedef struct ap_mailbox {
    volatile uint64_t seq;
    volatile uint64_t done;
    smp_work_fn_t fn;
    void *arg;
    volatile uint64_t work_count;   /* completed work items */
    volatile uint64_t work_cycles;  /* TSC cycles spent in work items */
    volatile uint64_t polls;        /* poll iterations (IF=0 fallback only) */
    volatile uint64_t sleeps;       /* hlt park entries (IF=1 mode) */
} ap_mailbox_t;

static struct {
    bool initialized;
    uint32_t num_cpus;
    volatile uint32_t num_online;
    uint64_t apic_base;
    struct cpu_data cpus[MAX_CPUS];
    ap_mailbox_t mailboxes[MAX_CPUS];
} smp_state = {
    .initialized = false,
    .num_cpus = 0,
    .num_online = 0,
    .apic_base = 0,
};

/* ============================================================================
 * MSR and APIC Access Functions
 * ============================================================================ */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}

static inline uint32_t apic_read(uint32_t reg)
{
    volatile uint32_t *apic = (volatile uint32_t *)(smp_state.apic_base + reg);
    return *apic;
}

static inline void apic_write(uint32_t reg, uint32_t value)
{
    volatile uint32_t *apic = (volatile uint32_t *)(smp_state.apic_base + reg);
    *apic = value;
}

/* ============================================================================
 * Timing (calibrated TSC; hal_timer_init/tsc_init ran before smp_init)
 * ============================================================================ */

static void smp_udelay(uint64_t usec)
{
    uint64_t freq = tsc_get_frequency();           /* Hz, 0 if uncalibrated */
    uint64_t cycles = freq ? (freq / 1000000ULL) * usec : usec * 1000ULL;
    uint64_t start = cpu_get_timestamp();
    while ((cpu_get_timestamp() - start) < cycles) {
        __asm__ volatile("pause");
    }
}

/* ============================================================================
 * Local APIC Initialization (BSP and each AP)
 * ============================================================================ */

/**
 * Initialize the Local APIC of the calling CPU and register it in smp_state.
 * Returns the sequential CPU id assigned to this CPU.
 *
 * APs are booted strictly one at a time (BSP waits for each to come online
 * before sending the next SIPI), so the num_online++ assignment is race-free.
 */
static uint32_t apic_init_current_cpu(void)
{
    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);
    bool is_bsp = (apic_msr & APIC_BASE_BSP) != 0;

    /* Enable the LAPIC (xAPIC MMIO mode) */
    apic_msr |= APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, apic_msr);

    /* Enable via spurious vector; task priority 0 (accept all) */
    apic_write(APIC_TPR, 0);
    apic_write(APIC_SPURIOUS, apic_read(APIC_SPURIOUS) |
               APIC_SPURIOUS_ENABLE | APIC_SPURIOUS_VECTOR);

    /* LVT setup:
     * - Timer masked for now; ipi_init_ap() arms a periodic per-CPU tick
     *   on APs (LAPIC_AP_TICK_VECTOR), the BSP arms its own 1 kHz tick
     *   later in lapic_timer_init().
     * - Error masked everywhere.
     * - LINT0 on the BSP stays the 8259 PIC passthrough (ExtINT mode,
     *   unmasked): QEMU routes the PIC INTR line through the LAPIC once it
     *   is enabled, so masking LINT0 would silently kill PIT IRQ0.
     * - LINT1 (NMI) masked everywhere. */
    apic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);
    apic_write(APIC_LVT_ERROR, APIC_LVT_MASKED);
    apic_write(APIC_LVT_LINT1, APIC_LVT_MASKED);
    if (is_bsp) {
        apic_write(APIC_LVT_LINT0, 0x700);   /* ExtINT, unmasked (PIC INTR) */
    } else {
        apic_write(APIC_LVT_LINT0, APIC_LVT_MASKED);
    }

    uint32_t apic_id = apic_read(APIC_ID) >> 24;

    uint32_t cpu_id = smp_state.num_online;
    if (cpu_id < MAX_CPUS) {
        smp_state.cpus[cpu_id].apic_id = apic_id;
        smp_state.cpus[cpu_id].cpu_id = cpu_id;
        smp_state.cpus[cpu_id].bsp = is_bsp;
        smp_wmb();
        smp_state.cpus[cpu_id].online = true;
        smp_wmb();
        smp_state.num_online++;
        smp_wmb();
    }
    return cpu_id;
}

/* ============================================================================
 * IPI Helpers
 * ============================================================================ */

static void send_init_ipi(uint32_t apic_id)
{
    apic_write(APIC_ICR_HIGH, apic_id << ICR_DEST_SHIFT);
    apic_write(APIC_ICR_LOW, ICR_INIT | ICR_LEVEL_ASSERT | ICR_DEST_PHYSICAL);
    while (apic_read(APIC_ICR_LOW) & ICR_DELIVERY_STATUS) {
        __asm__ volatile("pause");
    }
}

static void send_startup_ipi(uint32_t apic_id, uint32_t vector)
{
    apic_write(APIC_ICR_HIGH, apic_id << ICR_DEST_SHIFT);
    apic_write(APIC_ICR_LOW, ICR_STARTUP | ICR_DEST_PHYSICAL | vector);
    while (apic_read(APIC_ICR_LOW) & ICR_DELIVERY_STATUS) {
        __asm__ volatile("pause");
    }
}

/* ============================================================================
 * Trampoline Installation
 * ============================================================================ */

static bool smp_install_trampoline(void)
{
    uint64_t size = (uint64_t)(smp_trampoline_end - smp_trampoline_start);
    uint8_t *tramp = (uint8_t *)(uintptr_t)TRAMP_PHYS;  /* identity mapped */

    if (size > 0x1000) {
        console_printf("SMP: ERROR trampoline too big (%llu bytes)\n",
                       (unsigned long long)size);
        return false;
    }

    memcpy(tramp, smp_trampoline_start, size);

    /* Patch GDT descriptor base -> physical address of trampoline GDT */
    uint64_t off_gdt  = (uint64_t)(tramp_gdt - smp_trampoline_start);
    uint64_t off_desc = (uint64_t)(tramp_gdt_desc - smp_trampoline_start);
    *(volatile uint64_t *)(tramp + off_desc + 2) = TRAMP_PHYS + off_gdt;

    /* Kernel CR3: page tables are identity-mapped, so the running CR3 value
     * is the physical address the AP must load too. */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    *(volatile uint64_t *)(tramp + (tramp_cr3 - smp_trampoline_start)) = cr3;

    /* 64-bit C-entry trampoline (absolute kernel address) */
    *(volatile uint64_t *)(tramp + (tramp_entry_slot - smp_trampoline_start)) =
        (uint64_t)(uintptr_t)smp_ap_boot_entry;

    smp_wmb();
    return true;
}

static void smp_set_ap_stack(void *stack_top)
{
    uint8_t *tramp = (uint8_t *)(uintptr_t)TRAMP_PHYS;
    *(volatile uint64_t *)(tramp + (tramp_stack_slot - smp_trampoline_start)) =
        (uint64_t)(uintptr_t)stack_top;
    smp_wmb();
}

/* ============================================================================
 * AP Entry (C side, reached via smp_ap_boot_entry)
 * ============================================================================ */

/**
 * Execute one mailbox work item (seq s). Runs outside IRQ context on the
 * AP's own stack, so SSE/FPU use in work functions is safe (enabled by
 * smp_ap_boot_entry). Called with interrupts disabled; re-enables them
 * around the work so the AP local tick and wakeup IPI stay live.
 */
static void ap_mailbox_run(ap_mailbox_t *mb, uint64_t s)
{
    smp_rmb();
    smp_work_fn_t fn = mb->fn;
    void *arg = mb->arg;
    smp_rmb();

    __asm__ volatile("sti");
    uint64_t t0 = cpu_get_timestamp();
    fn(arg);
    uint64_t t1 = cpu_get_timestamp();
    __asm__ volatile("cli");

    mb->work_cycles += (t1 - t0);
    mb->work_count++;
    smp_wmb();
    mb->done = s;
    smp_wmb();
}

/**
 * smp_ap_main - AP main after trampoline. Runs with kernel GDT, per-AP
 * stack, IF=0. Registers the CPU, arms its per-CPU LAPIC timer, then
 * parks with IF=1 (`sti; hlt`) waiting for wakeup IPIs. Never returns.
 */
void smp_ap_main(void)
{
    /* Shared IDT so exceptions and LAPIC vectors on this AP hit the real
     * handlers (idt_init ran on the BSP; each CPU needs its own lidt). */
    extern void idt_ap_reload(void);
    idt_ap_reload();

    uint32_t cpu_id = apic_init_current_cpu();
    struct cpu_data *me = &smp_state.cpus[cpu_id];

    if (cpu_id < MAX_CPUS) {
        percpu_init_cpu(cpu_id);
    }

    /* Per-CPU LAPIC timer (periodic local tick on LAPIC_AP_TICK_VECTOR).
     * Success means this AP can park with IF=1 and be woken by IPI; on
     * failure keep the legacy IF=0 pause-polling loop as fallback. */
    bool if1_park = (cpu_id < MAX_CPUS) && (ipi_init_ap() == 0);
    me->parked_if1 = if1_park;
    smp_wmb();

    console_printf("SMP: AP cpu %u (APIC ID %u) online, stack %p, parking %s\n",
                   cpu_id, me->apic_id, me->stack_base,
                   if1_park ? "sti;hlt IF=1 (IPI wakeup)"
                            : "IF=0 polling (fallback, no LAPIC timer)");

    ap_mailbox_t *mb = &smp_state.mailboxes[cpu_id];

    if (!if1_park) {
        /* Legacy fallback: pause-polling with IF=0 (no LAPIC timer on
         * this AP, so hlt would never wake). */
        for (;;) {
            uint64_t s = mb->seq;
            if (s != mb->done) {
                ap_mailbox_run(mb, s);
            }
            mb->polls++;
            __asm__ volatile("pause");
        }
    }

    /* IF=1 park loop: cli -> re-check -> sti;hlt is the atomic sleep
     * sequence. A wakeup IPI posted after the seq check but before hlt
     * stays pending through the sti shadow and wakes the hlt immediately,
     * so no work post is ever missed. The AP local tick (100 Hz) is an
     * additional unconditional wake source. */
    for (;;) {
        __asm__ volatile("cli");
        uint64_t s = mb->seq;
        if (s != mb->done) {
            ap_mailbox_run(mb, s);      /* sti/cli around fn internally */
            continue;
        }
        mb->sleeps++;
        smp_wmb();
        __asm__ volatile("sti; hlt");
    }
}

/* ============================================================================
 * Secondary CPU Startup
 * ============================================================================ */

static bool start_secondary_cpu(uint32_t cpu_slot, uint32_t apic_id)
{
    /* Allocate and publish the AP stack before the SIPI */
    void *stack = kmalloc(CPU_STACK_SIZE);
    if (!stack) {
        console_printf("SMP: Failed to allocate stack for CPU %u\n", cpu_slot);
        return false;
    }
    uintptr_t top = ((uintptr_t)stack + CPU_STACK_SIZE) & ~(uintptr_t)(AP_STACK_ALIGN - 1);
    smp_state.cpus[cpu_slot].stack_base = stack;
    smp_state.cpus[cpu_slot].stack_top = (void *)top;
    smp_set_ap_stack((void *)top);

    /* Intel MP spec: INIT, wait 10ms, SIPI, wait 200us, SIPI */
    send_init_ipi(apic_id);
    smp_udelay(10000);

    send_startup_ipi(apic_id, (uint32_t)TRAMP_VECTOR);
    smp_udelay(200);
    send_startup_ipi(apic_id, (uint32_t)TRAMP_VECTOR);
    smp_udelay(200);

    /* Wait for the AP to come online (it sets cpus[slot].online in
     * apic_init_current_cpu; sequential boot => slot == next online id). */
    uint64_t freq = tsc_get_frequency();
    uint64_t timeout = freq ? (freq / 1000000ULL) * AP_ONLINE_TIMEOUT_US
                            : AP_ONLINE_TIMEOUT_US * 1000ULL;
    uint64_t start = cpu_get_timestamp();
    while (!smp_state.cpus[cpu_slot].online) {
        if ((cpu_get_timestamp() - start) > timeout) {
            console_printf("SMP: TIMEOUT waiting for CPU %u (APIC ID %u)\n",
                           cpu_slot, apic_id);
            return false;
        }
        __asm__ volatile("pause");
    }
    return true;
}

/* ============================================================================
 * SMP Initialization
 * ============================================================================ */

void smp_init(void)
{
    if (smp_state.initialized) {
        return;
    }

    console_printf("SMP: Initializing multi-processor support\n");

    smp_state.num_cpus = smp_num_cpus();
    console_printf("SMP: Detected %u CPU(s)\n", smp_state.num_cpus);

    if (smp_state.num_cpus > MAX_CPUS) {
        console_printf("SMP: Warning - limiting to %u CPUs\n", MAX_CPUS);
        smp_state.num_cpus = MAX_CPUS;
    }

    if (smp_state.num_cpus <= 1) {
        console_printf("SMP: Single processor system (skipping APIC init)\n");
        smp_state.num_online = 1;
        smp_state.cpus[0].cpu_id = 0;
        smp_state.cpus[0].apic_id = 0;
        smp_state.cpus[0].online = true;
        smp_state.cpus[0].bsp = true;
        smp_state.initialized = true;
        return;
    }

    /* LAPIC MMIO (typically 0xFEE00000) is above the boot-time 1GB identity
     * map - map it before touching any APIC register. */
    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);
    smp_state.apic_base = apic_msr & 0xFFFFF000ULL;
    if (!arch_identity_map_mmio(smp_state.apic_base, 0x1000)) {
        console_printf("SMP: Failed to map LAPIC MMIO at 0x%lx - staying UP\n",
                       (unsigned long)smp_state.apic_base);
        smp_state.num_cpus = 1;
        smp_state.num_online = 1;
        smp_state.cpus[0].cpu_id = 0;
        smp_state.cpus[0].online = true;
        smp_state.cpus[0].bsp = true;
        smp_state.initialized = true;
        return;
    }

    /* BSP registers itself first (cpu 0) */
    uint32_t bsp_id = apic_init_current_cpu();
    console_printf("SMP: BSP is CPU %u (APIC ID %u), LAPIC at 0x%lx\n",
                   bsp_id, smp_state.cpus[bsp_id].apic_id,
                   (unsigned long)smp_state.apic_base);

    /* Install the AP startup trampoline at physical 0x8000 */
    if (!smp_install_trampoline()) {
        console_printf("SMP: Trampoline install failed - staying UP\n");
        smp_state.num_cpus = 1;
        smp_state.initialized = true;
        return;
    }

    /* Boot APs strictly sequentially (mailbox/trampoline slots are shared) */
    uint32_t bsp_apic = smp_state.cpus[bsp_id].apic_id;
    uint32_t started = 1;
    for (uint32_t apic = 0; apic < smp_state.num_cpus && started < smp_state.num_cpus; apic++) {
        if (apic == bsp_apic) {
            continue;
        }
        if (start_secondary_cpu(started, apic)) {
            console_printf("SMP: CPU %u (APIC ID %u) started\n", started, apic);
            started++;
        } else {
            console_printf("SMP: CPU with APIC ID %u failed to start\n", apic);
        }
    }

    console_printf("SMP: Initialization complete (%u CPUs detected, %u online)\n",
                   smp_state.num_cpus, smp_state.num_online);
    smp_state.initialized = true;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

uint32_t smp_get_num_online(void)
{
    return smp_state.num_online;
}

bool smp_is_initialized(void)
{
    return smp_state.initialized;
}

struct cpu_data* smp_get_current_cpu(void)
{
    uint32_t apic_id = cpu_get_id();
    for (uint32_t i = 0; i < smp_state.num_online && i < MAX_CPUS; i++) {
        if (smp_state.cpus[i].apic_id == apic_id) {
            return &smp_state.cpus[i];
        }
    }
    return NULL;
}

int smp_get_cpu_info(uint32_t cpu, struct smp_cpu_info *out)
{
    if (!out || cpu >= MAX_CPUS || cpu >= smp_state.num_online) {
        return -1;
    }
    struct cpu_data *c = &smp_state.cpus[cpu];
    ap_mailbox_t *mb = &smp_state.mailboxes[cpu];
    out->cpu_id = c->cpu_id;
    out->apic_id = c->apic_id;
    out->online = c->online ? 1 : 0;
    out->bsp = c->bsp ? 1 : 0;
    out->parked_if1 = c->parked_if1 ? 1 : 0;
    out->work_count = mb->work_count;
    out->work_cycles = mb->work_cycles;
    out->polls = mb->polls;
    out->ipi_wakeups = ipi_wakeup_count(cpu);
    out->ap_ticks = lapic_timer_ap_ticks(c->apic_id);
    return 0;
}

int smp_cpu_to_apic_id(uint32_t cpu)
{
    if (cpu >= MAX_CPUS || cpu >= smp_state.num_online ||
        !smp_state.cpus[cpu].online) {
        return -1;
    }
    return (int)smp_state.cpus[cpu].apic_id;
}

/* ============================================================================
 * Cross-CPU Work Queue (mailboxes)
 * ============================================================================ */

int smp_work_dispatch(uint32_t cpu, smp_work_fn_t fn, void *arg)
{
    if (!fn || cpu == 0 || cpu >= smp_state.num_online || cpu >= MAX_CPUS) {
        return -1;
    }
    if (!smp_state.cpus[cpu].online) {
        return -1;
    }

    ap_mailbox_t *mb = &smp_state.mailboxes[cpu];

    /* One outstanding item per mailbox: wait for the previous one */
    while (mb->seq != mb->done) {
        __asm__ volatile("pause");
    }

    mb->arg = arg;
    mb->fn = fn;
    smp_wmb();
    mb->seq++;      /* release: AP sees fn/arg before the new seq */
    smp_wmb();

    /* Wake the parked AP (WS-A). The IPI is only needed when the AP
     * sleeps in hlt with IF=1; the IF=0 polling fallback picks the work
     * up by itself. A lost/spurious wakeup is harmless: the park loop
     * re-checks seq after every wake. */
    if (smp_state.cpus[cpu].parked_if1) {
        ipi_send(cpu, IPI_WAKEUP_VECTOR);
    }
    return 0;
}

int smp_work_wait(uint32_t cpu)
{
    if (cpu == 0 || cpu >= MAX_CPUS) {
        return -1;
    }
    ap_mailbox_t *mb = &smp_state.mailboxes[cpu];
    while (mb->done != mb->seq) {
        __asm__ volatile("pause");
    }
    smp_rmb();
    return 0;
}
