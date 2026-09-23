/* x86_64 Inter-Processor Interrupts (IPI)
 *
 * Fixed-mode, physical-destination IPI delivery over the Local APIC ICR,
 * plus the wakeup-vector path that replaces the AP IF=0 polling mailbox:
 * APs park with `sti; hlt` (IF=1) and smp_work_dispatch() pokes the
 * target with IPI_WAKEUP_VECTOR after posting work.
 *
 * The wakeup handler runs in IRQ context and deliberately does the
 * minimum (EOI + accounting): mailbox work executes in the AP park loop
 * after hlt returns, outside IRQ context, so SSE/FPU use in work
 * functions stays safe without fxsave fencing.
 *
 * Vector map (x86_64):
 *   0x20  BSP LAPIC timer tick (legacy IRQ0 vector, lapic_timer.c)
 *   0xF0  IPI wakeup (this file)
 *   0xF1  AP local LAPIC timer tick (lapic_timer.c, APs only)
 *   0xFF  LAPIC spurious vector (no EOI allowed)
 */

#include <embodios/ipi.h>
#include <embodios/cpu.h>
#include <embodios/lapic_timer.h>
#include <embodios/console.h>

/* ============================================================================
 * LAPIC registers (ICR) — the MMIO window is identity-mapped by smp.c /
 * lapic_timer.c before any IPI can be requested.
 * ============================================================================ */

#define MSR_APIC_BASE        0x1B
#define APIC_BASE_ENABLE     (1U << 11)

#define APIC_EOI             0x0B0
#define APIC_ICR_LOW         0x300
#define APIC_ICR_HIGH        0x310

#define ICR_DEST_SHIFT       24
#define ICR_DELIVERY_STATUS  (1U << 12)

/* Bound for the ICR delivery-status wait: a wedged LAPIC must fail the
 * send instead of hanging the dispatcher. */
#define IPI_SEND_MAX_SPINS   10000000U

/* Per-APIC-ID accounting (QEMU APIC IDs are small; bounded by MAX) */
#define IPI_MAX_APIC_IDS     16

/* ============================================================================
 * State
 * ============================================================================ */

static volatile uint64_t ipi_lapic_base;   /* shared LAPIC MMIO window */
static volatile uint64_t wake_counts[IPI_MAX_APIC_IDS];
static volatile uint64_t spurious_irqs;

/* ============================================================================
 * Low-level accessors
 * ============================================================================ */

static inline uint64_t ipi_rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline uint32_t ipi_apic_read(uint32_t reg)
{
    return *(volatile uint32_t *)(ipi_lapic_base + reg);
}

static inline void ipi_apic_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(ipi_lapic_base + reg) = value;
}

/* Resolve and cache the LAPIC MMIO base; fails when the LAPIC is not
 * enabled (UP boots skip APIC setup entirely -> all sends fail and the
 * caller keeps its fallback path). */
static int ipi_ensure_lapic(void)
{
    if (ipi_lapic_base) {
        return 0;
    }
    uint64_t msr = ipi_rdmsr(MSR_APIC_BASE);
    if (!(msr & APIC_BASE_ENABLE)) {
        return -1;
    }
    uint64_t base = msr & 0xFFFFF000ULL;
    if (base == 0) {
        return -1;
    }
    ipi_lapic_base = base;
    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int ipi_send(uint32_t cpu, uint8_t vector)
{
    if (vector < 0x10) {            /* 0x00-0x0F are not valid IPI vectors */
        return -1;
    }
    int apic_id = smp_cpu_to_apic_id(cpu);
    if (apic_id < 0) {
        return -1;
    }
    if (ipi_ensure_lapic() != 0) {
        return -1;
    }

    /* Fixed delivery, physical destination, edge-triggered. ICR_HIGH is
     * programmed before ICR_LOW; writing ICR_LOW triggers the send. */
    ipi_apic_write(APIC_ICR_HIGH, (uint32_t)apic_id << ICR_DEST_SHIFT);
    ipi_apic_write(APIC_ICR_LOW, (uint32_t)vector);

    for (uint32_t spin = 0; spin < IPI_SEND_MAX_SPINS; spin++) {
        if (!(ipi_apic_read(APIC_ICR_LOW) & ICR_DELIVERY_STATUS)) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

int ipi_init_ap(void)
{
    if (ipi_ensure_lapic() != 0) {
        return -1;
    }
    /* Arm this AP's own LAPIC timer (periodic local tick) so the AP can
     * sleep with IF=1 and always has a wake source even if an IPI is
     * lost. Reuses the lapic_timer.c calibration helpers (HPET-preferred,
     * PIT channel 2 fallback, shared bus-clock base when available). */
    if (lapic_timer_init_ap(IPI_AP_TICK_HZ) != 0) {
        return -1;
    }
    return 0;
}

void ipi_wakeup_handler(void)
{
    /* EOI first: the park loop re-reads the mailbox after hlt wakes, so
     * no further work is needed in IRQ context. */
    if (ipi_lapic_base) {
        ipi_apic_write(APIC_EOI, 0);
    }
    uint32_t apic_id = cpu_get_id();
    if (apic_id < IPI_MAX_APIC_IDS) {
        wake_counts[apic_id]++;
    }
}

uint64_t ipi_wakeup_count(uint32_t cpu)
{
    int apic_id = smp_cpu_to_apic_id(cpu);
    if (apic_id < 0 || apic_id >= IPI_MAX_APIC_IDS) {
        return 0;
    }
    return wake_counts[apic_id];
}

void ipi_spurious_handler(void)
{
    /* LAPIC spurious interrupts require no EOI — count and dismiss. */
    spurious_irqs++;
}

uint64_t ipi_spurious_count(void)
{
    return spurious_irqs;
}
