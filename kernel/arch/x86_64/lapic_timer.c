/* x86_64 Local APIC Timer — calibrated periodic scheduling tick
 *
 * Replaces the 100 Hz PIT tick with the per-CPU LAPIC timer running at a
 * configurable rate (default 1 kHz, 10x finer) when the hardware allows:
 *
 *   - Probe:  CPUID.1:EDX.APIC + IA32_APIC_BASE MSR.
 *   - Enable: on UP boots smp.c never sets the LAPIC up, so this file
 *     enables it standalone (MSR bit 11, spurious vector, TPR=0). LINT0
 *     mirrors the smp.c BSP setup (ExtINT, unmasked) so the 8259 PIC
 *     passthrough for the keyboard and friends keeps working; masking
 *     LINT0 would silently kill PIC-delivered IRQs.
 *   - Calibration: LAPIC bus cycles are counted over a ~10 ms window
 *     measured against the HPET main counter (preferred) or a PIT
 *     channel 2 one-shot (fallback). If both fail, init returns <0 and
 *     the system stays on the legacy PIT tick.
 *   - Delivery: LVT timer in periodic mode on the legacy IRQ0 vector
 *     (0x20). The existing isr32 stub and tick chain are reused; idt.c
 *     routes the EOI to the LAPIC (instead of the PIC) when active, and
 *     pic.c keeps the PIT line masked.
 *   - The legacy 100 Hz tick chain (timer_tick/timer_interrupt_handler
 *     -> scheduler_tick) is decimated from the fast tick via
 *     lapic_timer_legacy_due(), keeping scheduler quantum, uptime and
 *     hal_timer semantics unchanged. rt_timer_poll() runs at the full
 *     tick rate for the real-time callback layer.
 */

#include <embodios/lapic_timer.h>
#include <embodios/rt_timer.h>
#include <embodios/hpet.h>
#include <embodios/cpu.h>
#include <embodios/console.h>
#include <embodios/types.h>
#include "../../include/arch/x86_64/paging.h"

/* ============================================================================
 * LAPIC registers and constants
 * ============================================================================ */

#define MSR_APIC_BASE        0x1B
#define APIC_BASE_ENABLE     (1U << 11)
#define APIC_BASE_BSP        (1U << 8)

#define APIC_TPR             0x080
#define APIC_EOI             0x0B0
#define APIC_SPURIOUS        0x0F0
#define APIC_LVT_TIMER       0x320
#define APIC_LVT_LINT0       0x350
#define APIC_LVT_LINT1       0x360
#define APIC_LVT_ERROR       0x370
#define APIC_TIMER_INIT_CNT  0x380
#define APIC_TIMER_CUR_CNT   0x390
#define APIC_TIMER_DIVIDE    0x3E0

#define APIC_SPURIOUS_ENABLE (1U << 8)
#define APIC_SPURIOUS_VECTOR 0xFF
#define APIC_LVT_MASKED      (1U << 16)
#define APIC_LVT_PERIODIC    (1U << 17)
#define APIC_LVT_EXTINT      0x700
#define APIC_TIMER_DIV_16    0x3

/* The LAPIC timer reuses the legacy IRQ0 vector: the existing IDT stub
 * (isr32) and dispatch path handle it without new assembly. */
#define LAPIC_TICK_VECTOR    0x20

/* ============================================================================
 * PIT channel 2 (calibration fallback)
 * ============================================================================ */

#define PIT_CH2_DATA         0x42
#define PIT_COMMAND          0x43
#define PIT_CH2_GATE_PORT    0x61
#define PIT_BASE_HZ          1193182

#define CALIB_MS             10
#define LEGACY_TICK_HZ       100     /* decimation target (PIT legacy rate) */

/* Bound for calibration busy-waits: a broken clock source must fail the
 * probe instead of hanging the boot. */
#define CALIB_MAX_SPINS      50000000ULL

/* ============================================================================
 * State
 * ============================================================================ */

static volatile uint64_t lapic_base;      /* LAPIC MMIO base (identity-mapped) */
static uint32_t          lapic_tick_hz;   /* programmed rate */
static uint32_t          legacy_div;      /* tick_hz / LEGACY_TICK_HZ */
static uint32_t          lapic_bus_hz;    /* measured bus clock (x16 divider) */
static volatile uint64_t tick_count;      /* monotonic ticks since init */
static int               timer_active;
static const char       *calib_source = "none";

/* ============================================================================
 * Low-level accessors
 * ============================================================================ */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr),
                      "a"((uint32_t)(value & 0xFFFFFFFF)),
                      "d"((uint32_t)(value >> 32)) : "memory");
}

static inline uint32_t lapic_read(uint32_t reg)
{
    return *(volatile uint32_t *)(lapic_base + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(lapic_base + reg) = value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* ============================================================================
 * Calibration
 * ============================================================================ */

/* Count LAPIC timer decrements over CALIB_MS measured on the HPET main
 * counter. The LAPIC timer runs one-shot (masked) from 0xFFFFFFFF. */
static int calibrate_hpet(uint32_t *out_cycles)
{
    uint64_t freq = hpet_get_frequency();
    if (freq == 0) {
        return -1;
    }
    uint64_t window = (freq * CALIB_MS) / 1000ULL;
    if (window == 0) {
        return -1;
    }

    lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);   /* one-shot, masked */
    lapic_write(APIC_TIMER_INIT_CNT, 0xFFFFFFFFU);

    uint64_t start = hpet_read_counter();
    uint64_t spins = 0;
    while ((hpet_read_counter() - start) < window) {
        if (++spins > CALIB_MAX_SPINS) {
            lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);
            return -1;
        }
        __asm__ volatile("pause");
    }

    uint32_t cur = lapic_read(APIC_TIMER_CUR_CNT);
    lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);   /* stop */
    *out_cycles = 0xFFFFFFFFU - cur;
    return 0;
}

/* Count LAPIC timer decrements over a PIT channel 2 one-shot of CALIB_MS.
 * Channel 2 is gated via port 0x61 bit 0; its output reads back on bit 5.
 * Channel 0 (the legacy 100 Hz tick) is left untouched. */
static int calibrate_pit(uint32_t *out_cycles)
{
    uint16_t count = (uint16_t)((PIT_BASE_HZ * CALIB_MS) / 1000);

    /* Speaker off, gate2 low while programming */
    uint8_t p61 = (uint8_t)((inb(PIT_CH2_GATE_PORT) & ~0x03U));
    outb(PIT_CH2_GATE_PORT, p61);

    /* Channel 2, lobyte/hibyte, mode 0 (interrupt on terminal count) */
    outb(PIT_COMMAND, 0xB0);
    outb(PIT_CH2_DATA, (uint8_t)(count & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(count >> 8));

    /* Pulse gate2 high to (re)start the countdown */
    outb(PIT_CH2_GATE_PORT, (uint8_t)(p61 & ~0x01U));
    outb(PIT_CH2_GATE_PORT, (uint8_t)(p61 | 0x01U));

    lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);   /* one-shot, masked */
    lapic_write(APIC_TIMER_INIT_CNT, 0xFFFFFFFFU);

    uint64_t spins = 0;
    while ((inb(PIT_CH2_GATE_PORT) & 0x20U) == 0) {
        if (++spins > CALIB_MAX_SPINS) {
            lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);
            return -1;
        }
        __asm__ volatile("pause");
    }

    uint32_t cur = lapic_read(APIC_TIMER_CUR_CNT);
    lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);   /* stop */
    *out_cycles = 0xFFFFFFFFU - cur;
    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int lapic_timer_probe(void)
{
    uint32_t eax, ebx, ecx, edx;

    eax = 1;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "0"(eax) : "memory");
    if (!(edx & (1U << 9))) {          /* CPUID.1:EDX.APIC */
        return 0;
    }

    /* IA32_APIC_BASE must be readable; a zero base means no LAPIC */
    uint64_t base = rdmsr(MSR_APIC_BASE) & 0xFFFFF000ULL;
    return base != 0;
}

int lapic_timer_init(uint32_t hz)
{
    if (hz == 0 || hz > 1000000U) {
        return -1;
    }
    if (!lapic_timer_probe()) {
        return -1;
    }

    /* Enable the LAPIC (xAPIC MMIO mode). Idempotent with smp.c's setup:
     * on SMP boots the BSP LAPIC is already enabled and mapped. */
    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);
    apic_msr |= APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, apic_msr);
    lapic_base = apic_msr & 0xFFFFF000ULL;

    if (!arch_identity_map_mmio(lapic_base, 0x1000)) {
        console_printf("lapic: failed to map MMIO at 0x%lx\n",
                       (unsigned long)lapic_base);
        return -1;
    }

    /* Software-enable via the spurious vector; accept all priorities.
     * LINT0 on the BSP stays the PIC passthrough (ExtINT, unmasked) —
     * mirroring smp.c; masking it would kill PIC-delivered IRQs. */
    lapic_write(APIC_TPR, 0);
    lapic_write(APIC_SPURIOUS, lapic_read(APIC_SPURIOUS) |
                APIC_SPURIOUS_ENABLE | APIC_SPURIOUS_VECTOR);
    lapic_write(APIC_LVT_ERROR, APIC_LVT_MASKED);
    lapic_write(APIC_LVT_LINT1, APIC_LVT_MASKED);
    if (apic_msr & APIC_BASE_BSP) {
        lapic_write(APIC_LVT_LINT0, APIC_LVT_EXTINT);
    } else {
        lapic_write(APIC_LVT_LINT0, APIC_LVT_MASKED);
    }

    /* Divide the bus clock by 16 and calibrate the decrement rate */
    lapic_write(APIC_TIMER_DIVIDE, APIC_TIMER_DIV_16);
    lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);

    uint32_t window_cycles = 0;
    const char *src = NULL;
    if (hpet_is_available() && calibrate_hpet(&window_cycles) == 0) {
        src = "HPET";
    } else if (calibrate_pit(&window_cycles) == 0) {
        src = "PIT";
    } else {
        console_printf("lapic: calibration failed (HPET and PIT ch2)\n");
        return -1;
    }

    /* Plausibility: bus clocks between ~10 MHz and ~4 GHz with a /16
     * divider give 6.25k..2.5M decrements per 10 ms window. */
    if (window_cycles < 1000U || window_cycles > 0x0F000000U) {
        console_printf("lapic: implausible calibration (%u cycles/%d ms)\n",
                       window_cycles, CALIB_MS);
        return -1;
    }

    uint64_t ticks_per_sec = (uint64_t)window_cycles * (1000U / CALIB_MS);
    uint64_t initial = ticks_per_sec / hz;
    if (initial == 0 || initial > 0xFFFFFFFFULL) {
        return -1;
    }

    lapic_tick_hz  = hz;
    lapic_bus_hz   = (uint32_t)(ticks_per_sec * 16ULL);
    calib_source   = src;
    legacy_div     = hz / LEGACY_TICK_HZ;
    if (legacy_div == 0) {
        legacy_div = 1;
    }
    tick_count     = 0;

    /* Arm the periodic timer on the IRQ0 vector. Interrupts are still
     * disabled at this point (init runs before sti), so the first tick
     * lands only after the caller finishes the enable sequence. */
    lapic_write(APIC_LVT_TIMER, LAPIC_TICK_VECTOR | APIC_LVT_PERIODIC);
    lapic_write(APIC_TIMER_INIT_CNT, (uint32_t)initial);

    timer_active = 1;
    rt_timer_set_tick_hz(hz);
    return 0;
}

uint32_t lapic_timer_hz(void)
{
    return lapic_tick_hz;
}

uint64_t lapic_timer_ticks(void)
{
    return tick_count;
}

void lapic_timer_eoi(void)
{
    lapic_write(APIC_EOI, 0);
}

int lapic_timer_active(void)
{
    return timer_active;
}

void lapic_timer_tick(void)
{
    tick_count++;
    rt_timer_poll(tick_count);
}

int lapic_timer_legacy_due(void)
{
    return timer_active && legacy_div && (tick_count % legacy_div) == 0;
}

const char *lapic_timer_calib_source(void)
{
    return calib_source;
}

/* ============================================================================
 * WS-A: per-CPU LAPIC timers on APs
 *
 * Each AP calibrates and arms its own LAPIC timer (periodic, on
 * LAPIC_AP_TICK_VECTOR) during bring-up, before the BSP's timer exists.
 * The AP tick only feeds a per-CPU liveness counter and acts as a
 * guaranteed hlt wake source; it never enters the BSP tick/scheduler
 * chain (idt.c dispatches LAPIC_AP_TICK_VECTOR to lapic_timer_ap_tick).
 * ============================================================================ */

#define LAPIC_MAX_APIC_IDS   16

static volatile uint64_t ap_tick_counts[LAPIC_MAX_APIC_IDS];

uint32_t lapic_timer_bus_hz(void)
{
    return lapic_bus_hz;
}

int lapic_timer_init_ap(uint32_t hz)
{
    if (hz == 0 || hz > 1000000U) {
        return -1;
    }
    if (!lapic_timer_probe()) {
        return -1;
    }

    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);
    if (apic_msr & APIC_BASE_BSP) {
        return -1;      /* BSP uses lapic_timer_init() */
    }
    apic_msr |= APIC_BASE_ENABLE;   /* already set by smp.c; keep exact */
    wrmsr(MSR_APIC_BASE, apic_msr);

    /* The LAPIC MMIO window is shared (same address on every CPU) and was
     * identity-mapped by smp.c before the AP boot; lapic_base may still
     * be zero here because the BSP calibrates its timer later. */
    if (!lapic_base) {
        lapic_base = apic_msr & 0xFFFFF000ULL;
    }

    /* LAPIC software enable + TPR=0 were done by apic_init_current_cpu()
     * in smp.c; LINT0/LINT1/Error are masked there on APs. */

    lapic_write(APIC_TIMER_DIVIDE, APIC_TIMER_DIV_16);
    lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);

    /* Reuse the calibration base when another CPU already measured the
     * bus clock; otherwise calibrate this CPU's timer against the HPET
     * (preferred) or a PIT channel 2 one-shot. APs boot sequentially, so
     * the calibration helpers never run concurrently. */
    uint32_t window_cycles = 0;
    if (lapic_bus_hz) {
        window_cycles = lapic_bus_hz / 16U / (1000U / CALIB_MS);
    } else if (hpet_is_available() && calibrate_hpet(&window_cycles) == 0) {
        /* calibrated vs HPET */
    } else if (calibrate_pit(&window_cycles) == 0) {
        /* calibrated vs PIT ch2 */
    } else {
        return -1;
    }

    /* Same plausibility bound as the BSP path */
    if (window_cycles < 1000U || window_cycles > 0x0F000000U) {
        return -1;
    }

    uint64_t ticks_per_sec = (uint64_t)window_cycles * (1000U / CALIB_MS);
    uint64_t initial = ticks_per_sec / hz;
    if (initial == 0 || initial > 0xFFFFFFFFULL) {
        return -1;
    }
    if (!lapic_bus_hz) {
        lapic_bus_hz = (uint32_t)(ticks_per_sec * 16ULL);
    }

    /* Arm the periodic local tick on the AP tick vector. Interrupts are
     * still disabled on this AP (smp_ap_main sti's later), so the first
     * tick lands only after the park loop enables them. */
    lapic_write(APIC_LVT_TIMER, LAPIC_AP_TICK_VECTOR | APIC_LVT_PERIODIC);
    lapic_write(APIC_TIMER_INIT_CNT, (uint32_t)initial);
    return 0;
}

void lapic_timer_ap_tick(void)
{
    lapic_write(APIC_EOI, 0);
    uint32_t apic_id = cpu_get_id();
    if (apic_id < LAPIC_MAX_APIC_IDS) {
        ap_tick_counts[apic_id]++;
    }
}

uint64_t lapic_timer_ap_ticks(uint32_t apic_id)
{
    if (apic_id >= LAPIC_MAX_APIC_IDS) {
        return 0;
    }
    return ap_tick_counts[apic_id];
}
