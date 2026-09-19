/* EMBODIOS Real-Time Periodic Callback Layer
 *
 * Up to 8 periodic callbacks dispatched from the system tick IRQ. Runs
 * entirely on the BSP: the tick IRQ is the only writer of the dispatch
 * path, register/unregister (task context) are serialized against it
 * with a local irq-save critical section.
 *
 * Callback rules (IRQ context):
 *   - no sleeping, no allocation, no console output;
 *   - keep it short: callbacks run at up to the tick rate (1 kHz);
 *   - FPU/SSE state of the interrupted context is saved/restored around
 *     every invocation, so float math (e.g. a PID loop) is safe.
 *
 * Jitter accounting: while armed, each invocation records its lateness
 * (actual inter-callback period minus the requested period, rdtsc-based)
 * in microseconds into a ring buffer, cyclictest-style. Samples are
 * u32 us values, clamped to [0, 1s].
 */

#include <embodios/rt_timer.h>
#include <embodios/tsc.h>
#include <embodios/spinlock.h>
#include <embodios/types.h>

#define RT_TIMER_MAX    8
#define JITTER_RING     8192            /* power of two */
#define JITTER_CLAMP_US 1000000U

typedef struct {
    rt_timer_fn fn;
    void       *arg;
    uint32_t    period_us;
    uint64_t    period_ticks;      /* period in tick-hz units (>= 1) */
    uint64_t    next_due;          /* next dispatch tick */
    uint64_t    last_tsc;          /* rdtsc at previous invocation */
    uint8_t     active;
} rt_entry_t;

static rt_entry_t entries[RT_TIMER_MAX];
static uint64_t   rt_now;          /* last polled tick (timebase) */
static uint32_t   rt_tick_hz = 100;
static uint64_t   rt_seq;          /* callback invocation sequence */

static volatile int      jitter_armed;
static volatile uint32_t jitter_head;    /* next write slot */
static volatile uint32_t jitter_count;   /* valid samples (<= JITTER_RING) */
static uint32_t          jitter_ring[JITTER_RING];

/* ============================================================================
 * FPU/SSE context preservation (x86_64)
 *
 * The IRQ stub saves GP registers only. Callbacks compiled with
 * -mfpmath=sse may clobber XMM0-15/MXCSR of the interrupted context
 * (e.g. an in-flight inference), so bracket each invocation with
 * fxsave/fxrstor.
 * ============================================================================ */

#if defined(__x86_64__)
static struct { uint8_t data[512]; } fpu_buf __attribute__((aligned(16)));

static inline void fpu_save(void)
{
    __asm__ volatile("fxsave %0" : "=m"(fpu_buf));
}

static inline void fpu_restore(void)
{
    __asm__ volatile("fxrstor %0" :: "m"(fpu_buf));
}
#else
static inline void fpu_save(void) {}
static inline void fpu_restore(void) {}
#endif

/* ============================================================================
 * Tick rate
 * ============================================================================ */

void rt_timer_set_tick_hz(uint32_t hz)
{
    if (hz > 0) {
        rt_tick_hz = hz;
    }
}

uint32_t rt_timer_get_tick_hz(void)
{
    return rt_tick_hz;
}

/* ============================================================================
 * Registration (task context)
 * ============================================================================ */

int rt_timer_register(rt_timer_fn fn, uint32_t period_us, void *arg)
{
    if (!fn || period_us == 0) {
        return -1;
    }

    unsigned long flags = arch_irq_save();

    int slot = -1;
    for (int i = 0; i < RT_TIMER_MAX; i++) {
        if (entries[i].active && entries[i].fn == fn) {
            arch_irq_restore(flags);
            return -1;              /* duplicate */
        }
        if (!entries[i].active && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        arch_irq_restore(flags);
        return -1;                  /* full */
    }

    uint64_t period_ticks = ((uint64_t)period_us * rt_tick_hz) / 1000000ULL;
    if (period_ticks == 0) {
        period_ticks = 1;           /* clamp to the tick rate */
    }

    entries[slot].fn           = fn;
    entries[slot].arg          = arg;
    entries[slot].period_us    = period_us;
    entries[slot].period_ticks = period_ticks;
    entries[slot].next_due     = rt_now + period_ticks;
    entries[slot].last_tsc     = rdtsc();
    entries[slot].active       = 1;

    arch_irq_restore(flags);
    return 0;
}

void rt_timer_unregister(rt_timer_fn fn)
{
    if (!fn) {
        return;
    }
    unsigned long flags = arch_irq_save();
    for (int i = 0; i < RT_TIMER_MAX; i++) {
        if (entries[i].active && entries[i].fn == fn) {
            entries[i].active = 0;
            entries[i].fn = NULL;
        }
    }
    arch_irq_restore(flags);
}

/* ============================================================================
 * Dispatch (tick IRQ context)
 * ============================================================================ */

void rt_timer_poll(uint64_t now_ticks)
{
    rt_now = now_ticks;

    for (int i = 0; i < RT_TIMER_MAX; i++) {
        if (!entries[i].active) {
            continue;
        }
        if (now_ticks < entries[i].next_due) {
            continue;
        }

        /* Schedule the next slot; resync (skip missed periods) after a
         * long stall instead of burst-firing catch-up callbacks. */
        entries[i].next_due += entries[i].period_ticks;
        if (entries[i].next_due <= now_ticks) {
            entries[i].next_due = now_ticks + entries[i].period_ticks;
        }

        if (jitter_armed) {
            uint64_t now = rdtsc();
            uint64_t actual_us = tsc_to_microseconds(now - entries[i].last_tsc);
            entries[i].last_tsc = now;

            uint64_t late = (actual_us > entries[i].period_us)
                          ? (actual_us - entries[i].period_us) : 0;
            if (late > JITTER_CLAMP_US) {
                late = JITTER_CLAMP_US;
            }
            jitter_ring[jitter_head] = (uint32_t)late;
            jitter_head = (jitter_head + 1) & (JITTER_RING - 1);
            if (jitter_count < JITTER_RING) {
                jitter_count++;
            }
        }

        rt_seq++;
        fpu_save();
        entries[i].fn(rt_seq, entries[i].arg);
        fpu_restore();
    }
}

/* ============================================================================
 * Jitter instrumentation
 * ============================================================================ */

void rt_timer_jitter_arm(void)
{
    unsigned long flags = arch_irq_save();
    jitter_head = 0;
    jitter_count = 0;
    jitter_armed = 1;
    arch_irq_restore(flags);
}

void rt_timer_jitter_disarm(void)
{
    unsigned long flags = arch_irq_save();
    jitter_armed = 0;
    arch_irq_restore(flags);
}

uint32_t rt_timer_jitter_samples(void)
{
    return jitter_count;
}

uint32_t rt_timer_jitter_copy(uint32_t *dst, uint32_t max)
{
    if (!dst || max == 0) {
        return 0;
    }

    unsigned long flags = arch_irq_save();
    uint32_t n = jitter_count;
    if (n > max) {
        n = max;
    }
    /* Oldest sample sits at (head - count) modulo the ring size */
    uint32_t start = (jitter_head - jitter_count) & (JITTER_RING - 1);
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = jitter_ring[(start + i) & (JITTER_RING - 1)];
    }
    arch_irq_restore(flags);
    return n;
}
