/* EMBODIOS ARM64 GICv2 Driver (GIC-400, QEMU virt machine)
 *
 * Distributor  (GICD) @ 0x08000000
 * CPU interface (GICC) @ 0x08010000
 *
 * INTIDs of interest:
 *   30  - EL1 physical timer (PPI, CNTP)     -> timer tick chain
 *   33  - PL011 UART0 (SPI 1)                -> optional RX (unused, polled)
 *  1023 - spurious
 *
 * The IRQ dispatcher drives the same tick chain as x86_64 IRQ0:
 *   timer_tick()              arch/aarch64/hal_timer.c (ticks, rt_timer_poll)
 *   timer_interrupt_handler() kernel/timer.c (seconds, scheduler_tick)
 */

#include "embodios/types.h"

/* ------------------------------------------------------------------------
 * GIC register map (GICv2 / GIC-400)
 * ------------------------------------------------------------------------ */
#define GICD_BASE        0x08000000UL
#define GICC_BASE        0x08010000UL

#define GICD_CTLR        (GICD_BASE + 0x000)
#define GICD_ISENABLER0  (GICD_BASE + 0x100)   /* 1 bit per INTID, 32/reg  */
#define GICD_ICENABLER0  (GICD_BASE + 0x180)
#define GICD_ICPENDR0    (GICD_BASE + 0x280)
#define GICD_IPRIORITYR0 (GICD_BASE + 0x400)   /* 1 byte per INTID, 4/reg  */
#define GICD_ITARGETSR0  (GICD_BASE + 0x800)   /* 1 byte per INTID (SPIs)  */
#define GICD_ICFGR0      (GICD_BASE + 0xC00)

#define GICC_CTLR        (GICC_BASE + 0x00)
#define GICC_PMR         (GICC_BASE + 0x04)
#define GICC_IAR         (GICC_BASE + 0x0C)
#define GICC_EOIR        (GICC_BASE + 0x10)

#define INTID_TIMER_PHYS_EL1  30     /* CNTP PPI */
#define INTID_UART0           33     /* PL011 SPI 1 */
#define INTID_SPURIOUS        1023

#define GICC_PMR_UNMASK_ALL   0xF0   /* allow priorities 0x00-0xE0 */

static inline void gic_write32(uintptr_t addr, uint32_t value)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    *ptr = value;
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline uint32_t gic_read32(uintptr_t addr)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    return *ptr;
}

/* Enable one INTID in the distributor (PPI/SGI target registers are
 * read-only on GICv2; level-triggered PPI defaults are fine). */
static void gic_enable_intid(uint32_t intid, uint8_t priority)
{
    uint32_t reg = intid / 32;
    uint32_t bit = intid % 32;

    /* Priority: lower value = higher priority; must be < PMR to fire */
    uintptr_t preg = GICD_IPRIORITYR0 + (intid & ~3UL);
    uint32_t shift = (intid & 3) * 8;
    uint32_t v = gic_read32(preg);
    v &= ~(0xFFu << shift);
    v |= ((uint32_t)priority << shift);
    gic_write32(preg, v);

    gic_write32(GICD_ICPENDR0 + reg * 4, (1u << bit));  /* clear pending */
    gic_write32(GICD_ISENABLER0 + reg * 4, (1u << bit));
}

extern void uart_putchar(char c);
extern void uart_puts(const char *s);

/* ------------------------------------------------------------------------
 * Initialization: install VBAR_EL1, enable distributor + CPU interface
 * ------------------------------------------------------------------------ */

extern char exception_vectors[];

void gic_init(void)
{
    /* Install the exception vector table */
    __asm__ volatile("msr vbar_el1, %0" :: "r"(exception_vectors));
    __asm__ volatile("isb");

    /* Distributor off while configuring */
    gic_write32(GICD_CTLR, 0);

    /* EL1 physical timer PPI (INTID 30) */
    gic_enable_intid(INTID_TIMER_PHYS_EL1, 0x80);

    /* PL011 UART0 (INTID 33): routed but unused for now (polled input) */
    gic_enable_intid(INTID_UART0, 0xA0);

    /* CPU interface: priority mask, enable signaling.
     * CTLR bit 1 (EOImodeNS) left 0: a single EOIR write both drops
     * priority and deactivates (simplest correct mode for our handlers). */
    gic_write32(GICC_PMR, GICC_PMR_UNMASK_ALL);
    gic_write32(GICC_CTLR, 1);

    /* Distributor on */
    gic_write32(GICD_CTLR, 1);

    uart_puts("GICv2: distributor + CPU interface enabled (timer PPI 30, UART SPI 33)\n");
}

/* ------------------------------------------------------------------------
 * IRQ dispatch (called from vectors.S with the saved register frame)
 * ------------------------------------------------------------------------ */

extern void timer_tick(void);               /* arch/aarch64/hal_timer.c */
extern void timer_interrupt_handler(void);  /* kernel/timer.c */

void irq_dispatch(uint64_t *frame)
{
    (void)frame;

    uint32_t iar = gic_read32(GICC_IAR);
    uint32_t intid = iar & 0x3FF;

    if (intid == INTID_SPURIOUS) {
        return;  /* spurious: no EOI needed */
    }

    /* EOI (priority drop + deactivate, EOImode=0) BEFORE running handlers.
     * Mandatory here: scheduler_tick() -> schedule() -> context_switch()
     * can move the frozen remainder of this dispatch (and its interrupt
     * frame) onto another task's stack to be unwound much later. If the
     * running priority stayed at 0x80 until then, equal-priority timer
     * ticks could never be signaled again and the system would deadlock
     * (observed: tasktest hangs after the first preemption). Early EOI
     * is safe: handler bodies run with DAIF.I masked, so no reentrant
     * dispatch of the same IRQ happens on the live stack. */
    gic_write32(GICC_EOIR, intid);

    switch (intid) {
    case INTID_TIMER_PHYS_EL1:
        timer_tick();               /* HAL tick: ticks++, reprogram CNTP */
        timer_interrupt_handler();  /* core tick: seconds, scheduler_tick */
        break;

    case INTID_UART0:
        /* UART RX: v0.8.0 uses polled input; just clear the IRQ at the
         * device would go here. Mask it to avoid an IRQ storm. */
        gic_write32(GICD_ICENABLER0 + (INTID_UART0 / 32) * 4,
                    (1u << (INTID_UART0 % 32)));
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------
 * Exception dump helpers (synchronous / FIQ / SError): print and halt.
 * Console may be mid-lock; use raw UART so the dump always gets out.
 * ------------------------------------------------------------------------ */

static void dump_hex64(uint64_t v)
{
    static const char hexd[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uart_putchar(hexd[(v >> i) & 0xF]);
    }
}

/* Frame layout must match SAVE_REGS in vectors.S */
#define FRAME_X(n)   (frame[(n)])
#define FRAME_X30    (frame[30])
#define FRAME_ELR    (frame[31])
#define FRAME_SPSR   (frame[32])
#define FRAME_ESR    (frame[33])

static void dump_frame(uint64_t *frame, const char *kind, uint64_t which)
{
    uart_puts("\n*** EXCEPTION: ");
    uart_puts(kind);
    uart_puts(" (group ");
    uart_putchar((char)('0' + which));
    uart_puts(") ***\n");

    uart_puts("ESR  = "); dump_hex64(FRAME_ESR);  uart_puts("\n");
    uart_puts("ELR  = "); dump_hex64(FRAME_ELR);  uart_puts("\n");
    uart_puts("SPSR = "); dump_hex64(FRAME_SPSR); uart_puts("\n");

    for (int i = 0; i < 30; i += 2) {
        uart_puts("x"); uart_putchar((char)('0' + i / 10));
        uart_putchar((char)('0' + i % 10));
        uart_puts(" = "); dump_hex64(FRAME_X(i));
        uart_puts("  x"); uart_putchar((char)('0' + (i + 1) / 10));
        uart_putchar((char)('0' + (i + 1) % 10));
        uart_puts(" = "); dump_hex64(FRAME_X(i + 1));
        uart_puts("\n");
    }
    uart_puts("x30 = "); dump_hex64(FRAME_X30); uart_puts("\n");

    uart_puts("System halted.\n");
}

static void halt_forever(void)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void sync_exception_handler(uint64_t *frame, uint64_t which)
{
    dump_frame(frame, "synchronous", which);
    halt_forever();
}

void fiq_handler(uint64_t *frame)
{
    dump_frame(frame, "FIQ", 0);
    halt_forever();
}

void serr_handler(uint64_t *frame)
{
    dump_frame(frame, "SError", 0);
    halt_forever();
}
