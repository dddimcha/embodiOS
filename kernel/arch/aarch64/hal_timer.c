/* EMBODIOS aarch64 HAL Timer Implementation
 *
 * Provides aarch64-specific timer implementation using the ARM Generic Timer.
 */

#include "embodios/types.h"
#include "embodios/hal_timer.h"
#include <embodios/console.h>
#include "board.h"
#include "embodios/rt_timer.h"

/* Kernel tick frequency: 1 kHz on Pi boards (EMG RT loop), 100 Hz on virt (TCG-friendly) */
#ifndef BOARD_TICK_HZ
#define BOARD_TICK_HZ 100
#endif
#define TIMER_FREQUENCY BOARD_TICK_HZ

/* Timer state */
static struct {
    uint64_t ticks;           /* Timer ticks since boot */
    uint64_t frequency;       /* Timer frequency in Hz */
    uint64_t counter_freq;    /* Hardware counter frequency */
    bool enabled;             /* Timer enabled flag */
} timer_state = {
    .ticks = 0,
    .frequency = TIMER_FREQUENCY,
    .counter_freq = 0,
    .enabled = false
};

/* Read ARM Generic Timer counter frequency */
static uint64_t arm_timer_get_counter_freq(void)
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

/* Read ARM Generic Timer counter value (physical counter, matches CNTP) */
static uint64_t arm_timer_get_counter(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/* Configure ARM Generic Timer (EL1 physical timer, CNTP -> GIC PPI INTID 30) */
static void arm_timer_configure_hw(uint64_t interval_ticks)
{
    /* Program relative down-counter, then enable unmasked */
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(interval_ticks));
    /* CNTP_CTL_EL0: bit 0 = enable, bit 1 = IMASK (0 = not masked) */
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)1));
    __asm__ volatile("isb");
}

/* Disable ARM Generic Timer */
static void arm_timer_disable_hw(void)
{
    /* Disable timer: bit 0 = 0 (disabled) */
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)0));
}

/* HAL timer initialization */
static void aarch64_timer_init(void)
{
    /* Read hardware counter frequency */
    timer_state.counter_freq = arm_timer_get_counter_freq();

    /* Validate and log counter frequency */
    if (timer_state.counter_freq == 0) {
        console_printf("aarch64_timer: WARNING - counter frequency is 0\n");
    } else if (timer_state.counter_freq < 1000000ULL) {
        console_printf("aarch64_timer: WARNING - counter frequency %llu Hz is unusually low\n",
                       timer_state.counter_freq);
    } else if (timer_state.counter_freq > 1000000000ULL) {
        console_printf("aarch64_timer: WARNING - counter frequency %llu Hz is unusually high\n",
                       timer_state.counter_freq);
    } else {
        console_printf("aarch64_timer: counter frequency %llu Hz\n",
                       timer_state.counter_freq);
    }

    /* Publish tick rate to the RT timer subsystem (EMG 1 kHz loop needs it) */
    rt_timer_set_tick_hz(TIMER_FREQUENCY);

    /* Set default tick frequency */
    timer_state.frequency = TIMER_FREQUENCY;
    timer_state.ticks = 0;
    timer_state.enabled = false;

    /* Disable timer initially */
    arm_timer_disable_hw();
}

/* HAL timer enable */
static void aarch64_timer_enable(void)
{
    timer_state.enabled = true;

    /* Configure timer with default interval */
    if (timer_state.counter_freq > 0) {
        uint64_t interval = timer_state.counter_freq / timer_state.frequency;
        arm_timer_configure_hw(interval);
    }
}

/* HAL timer disable */
static void aarch64_timer_disable(void)
{
    timer_state.enabled = false;
    arm_timer_disable_hw();
}

/* HAL timer configure */
static void aarch64_timer_configure(const struct timer_config *config)
{
    if (!config) {
        return;
    }

    /* Update frequency if specified */
    if (config->frequency > 0 && config->frequency != timer_state.frequency) {
        timer_state.frequency = config->frequency;

        /* Reconfigure hardware timer if enabled */
        if (timer_state.enabled && timer_state.counter_freq > 0) {
            uint64_t interval = timer_state.counter_freq / timer_state.frequency;
            arm_timer_configure_hw(interval);
        }
    }

    /* Update enabled state based on flags */
    if (config->flags & TIMER_FLAG_ENABLED) {
        timer_state.enabled = true;
    }
}

/* HAL timer get ticks */
static uint64_t aarch64_timer_get_ticks(void)
{
    return timer_state.ticks;
}

/* HAL timer get frequency */
static uint64_t aarch64_timer_get_frequency(void)
{
    return timer_state.frequency;
}

/* HAL timer get microseconds */
static uint64_t aarch64_timer_get_microseconds(void)
{
    if (timer_state.counter_freq == 0) {
        return 0;
    }

    /* Read hardware counter directly for high-resolution timing */
    uint64_t counter = arm_timer_get_counter();
    return (counter * 1000000ULL) / timer_state.counter_freq;
}

/* HAL timer get milliseconds */
static uint64_t aarch64_timer_get_milliseconds(void)
{
    return (timer_state.ticks * 1000ULL) / timer_state.frequency;
}

/* HAL timer delay microseconds */
static void aarch64_timer_delay_us(uint64_t microseconds)
{
    if (timer_state.counter_freq == 0) {
        return;
    }

    /* Calculate counter ticks to wait */
    uint64_t counter_ticks = (microseconds * timer_state.counter_freq) / 1000000ULL;
    uint64_t start = arm_timer_get_counter();

    while ((arm_timer_get_counter() - start) < counter_ticks) {
        /* Busy wait with wait-for-event hint */
        __asm__ volatile("wfe");
    }
}

/* HAL timer delay milliseconds */
static void aarch64_timer_delay_ms(uint64_t milliseconds)
{
    if (timer_state.counter_freq == 0) {
        return;
    }

    /* Calculate counter ticks to wait */
    uint64_t counter_ticks = (milliseconds * timer_state.counter_freq) / 1000ULL;
    uint64_t start = arm_timer_get_counter();

    while ((arm_timer_get_counter() - start) < counter_ticks) {
        /* Busy wait with wait-for-event hint */
        __asm__ volatile("wfe");
    }
}

/* HAL timer convert ticks to microseconds */
static uint64_t aarch64_timer_ticks_to_us(uint64_t ticks)
{
    return (ticks * 1000000ULL) / timer_state.frequency;
}

/* HAL timer convert microseconds to ticks */
static uint64_t aarch64_timer_us_to_ticks(uint64_t microseconds)
{
    return (microseconds * timer_state.frequency) / 1000000ULL;
}

/* aarch64 timer operations structure */
static const struct hal_timer_ops aarch64_timer_ops = {
    .init = aarch64_timer_init,
    .enable = aarch64_timer_enable,
    .disable = aarch64_timer_disable,
    .configure = aarch64_timer_configure,
    .get_ticks = aarch64_timer_get_ticks,
    .get_frequency = aarch64_timer_get_frequency,
    .get_microseconds = aarch64_timer_get_microseconds,
    .get_milliseconds = aarch64_timer_get_milliseconds,
    .delay_us = aarch64_timer_delay_us,
    .delay_ms = aarch64_timer_delay_ms,
    .ticks_to_us = aarch64_timer_ticks_to_us,
    .us_to_ticks = aarch64_timer_us_to_ticks
};

/* Register aarch64 timer HAL */
void arch_timer_init(void)
{
    hal_timer_register(&aarch64_timer_ops);
}

/* Timer interrupt handler (called from GIC IRQ dispatch, INTID 30) */
extern void rt_timer_poll(uint64_t now_ticks);

void timer_tick(void)
{
    if (timer_state.enabled) {
        timer_state.ticks++;

        /* rt_timer periodic-callback layer polls at the tick rate
         * (mirrors the x86_64 PIT-fallback path). */
        rt_timer_poll(timer_state.ticks);

        /* Reconfigure timer for next tick */
        if (timer_state.counter_freq > 0) {
            uint64_t interval = timer_state.counter_freq / timer_state.frequency;
            arm_timer_configure_hw(interval);
        }
    }
}

/* ========================================================================
 * TSC-compatible high-resolution counter API (embodios/tsc.h)
 *
 * arch/x86_64/tsc.c provides these for x86_64; on aarch64 the generic
 * timer counter (cntpct_el0 @ cntfrq_el0 Hz) plays the same role for
 * rt_timer jitter accounting, benchmarks and the motor demo.
 * ======================================================================== */

int tsc_init(void)
{
    return 0;
}

uint64_t tsc_get_frequency(void)
{
    if (timer_state.counter_freq == 0) {
        timer_state.counter_freq = arm_timer_get_counter_freq();
    }
    return timer_state.counter_freq;
}

uint64_t tsc_calibrate(void)
{
    return tsc_get_frequency();
}

uint64_t tsc_to_microseconds(uint64_t cycles)
{
    uint64_t freq = tsc_get_frequency();
    if (freq == 0) {
        return 0;
    }
    return (cycles * 1000000ULL) / freq;
}

uint64_t tsc_to_nanoseconds(uint64_t cycles)
{
    uint64_t freq = tsc_get_frequency();
    if (freq == 0) {
        return 0;
    }
    return (cycles * 1000000000ULL) / freq;
}

uint64_t microseconds_to_tsc(uint64_t us)
{
    uint64_t freq = tsc_get_frequency();
    if (freq == 0) {
        return 0;
    }
    return (us * freq) / 1000000ULL;
}

bool tsc_is_stable(void)
{
    /* The ARM generic timer runs at a fixed frequency */
    return true;
}
