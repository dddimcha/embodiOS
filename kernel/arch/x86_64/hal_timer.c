/* EMBODIOS x86_64 HAL Timer Implementation
 *
 * Provides x86_64-specific timer implementation using the PIT
 * (Programmable Interval Timer) for low-frequency ticks, TSC
 * (Time Stamp Counter) for high-resolution timing, and HPET
 * (High Precision Event Timer) as an alternative high-resolution source.
 */

#include "embodios/types.h"
#include "embodios/hal_timer.h"
#include "embodios/tsc.h"
#include "embodios/hpet.h"

/* Timer frequency (100 Hz = 10ms tick) */
#define TIMER_FREQUENCY 100
#define PIT_FREQUENCY 1193182

/* PIT (Programmable Interval Timer) ports */
#define PIT_CHANNEL0_DATA 0x40
#define PIT_CHANNEL1_DATA 0x41
#define PIT_CHANNEL2_DATA 0x42
#define PIT_COMMAND 0x43

/* PIT command bits */
#define PIT_CMD_BINARY 0x00
#define PIT_CMD_BCD 0x01
#define PIT_CMD_MODE0 0x00
#define PIT_CMD_MODE2 0x04
#define PIT_CMD_MODE3 0x06
#define PIT_CMD_READBACK 0xC0
#define PIT_CMD_COUNTER0 0x00
#define PIT_CMD_COUNTER1 0x40
#define PIT_CMD_COUNTER2 0x80
#define PIT_CMD_LATCHCOUNT 0x00
#define PIT_CMD_LOBYTE 0x10
#define PIT_CMD_HIBYTE 0x20
#define PIT_CMD_LOHIBYTE 0x30

/* Timer state */
static struct {
    uint64_t ticks;           /* Timer ticks since boot (PIT-based) */
    uint64_t frequency;       /* Timer frequency in Hz (PIT) */
    bool enabled;             /* Timer enabled flag */
    uint64_t tsc_boot;        /* TSC value at boot for high-res timing */
    uint64_t hpet_boot;       /* HPET value at boot for high-res timing */
} timer_state = {
    .ticks = 0,
    .frequency = TIMER_FREQUENCY,
    .enabled = false,
    .tsc_boot = 0,
    .hpet_boot = 0
};

/* Initialize PIT hardware */
static void pit_init(uint32_t frequency)
{
    /* Calculate divisor */
    uint32_t divisor = PIT_FREQUENCY / frequency;

    /* Send command byte */
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)(PIT_CMD_COUNTER0 |
                                                    PIT_CMD_LOHIBYTE |
                                                    PIT_CMD_MODE3 |
                                                    PIT_CMD_BINARY)),
                                      "Nd"((uint16_t)PIT_COMMAND));

    /* Send divisor */
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)(divisor & 0xFF)),
                                      "Nd"((uint16_t)PIT_CHANNEL0_DATA));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)((divisor >> 8) & 0xFF)),
                                      "Nd"((uint16_t)PIT_CHANNEL0_DATA));
}

/* HAL timer initialization */
static void x86_64_timer_init(void)
{
    /* Initialize TSC for high-resolution timing */
    tsc_init();

    /* Record boot TSC value */
    timer_state.tsc_boot = rdtsc();

    /* Initialize HPET as alternative high-resolution timer source */
    hpet_init();

    /* Record boot HPET value if available */
    if (hpet_is_available()) {
        timer_state.hpet_boot = hpet_read_counter();
    }

    /* Initialize PIT with default frequency for tick-based timing */
    pit_init(TIMER_FREQUENCY);
    timer_state.frequency = TIMER_FREQUENCY;
    timer_state.ticks = 0;
    timer_state.enabled = false;
}

/* HAL timer enable */
static void x86_64_timer_enable(void)
{
    timer_state.enabled = true;
}

/* HAL timer disable */
static void x86_64_timer_disable(void)
{
    timer_state.enabled = false;
}

/* HAL timer configure */
static void x86_64_timer_configure(const struct timer_config *config)
{
    if (!config) {
        return;
    }

    /* Update frequency if specified */
    if (config->frequency > 0 && config->frequency != timer_state.frequency) {
        pit_init((uint32_t)config->frequency);
        timer_state.frequency = config->frequency;
    }

    /* Update enabled state based on flags */
    if (config->flags & TIMER_FLAG_ENABLED) {
        timer_state.enabled = true;
    }
}

/* HAL timer get ticks */
static uint64_t x86_64_timer_get_ticks(void)
{
    return timer_state.ticks;
}

/* HAL timer get frequency */
static uint64_t x86_64_timer_get_frequency(void)
{
    return timer_state.frequency;
}

/* HAL timer get microseconds */
static uint64_t x86_64_timer_get_microseconds(void)
{
    /* Use TSC for high-resolution timing if available */
    uint64_t tsc_freq = tsc_get_frequency();
    if (tsc_freq > 0) {
        uint64_t tsc_now = rdtsc();
        uint64_t tsc_elapsed = tsc_now - timer_state.tsc_boot;
        return tsc_to_microseconds(tsc_elapsed);
    }

    /* Try HPET if TSC is unavailable or unreliable */
    if (hpet_is_available()) {
        uint64_t hpet_now = hpet_read_counter();
        uint64_t hpet_elapsed = hpet_now - timer_state.hpet_boot;
        return hpet_ticks_to_microseconds(hpet_elapsed);
    }

    /* Fall back to PIT-based timing */
    return (timer_state.ticks * 1000000ULL) / timer_state.frequency;
}

/* HAL timer get milliseconds */
static uint64_t x86_64_timer_get_milliseconds(void)
{
    /* Use TSC for high-resolution timing if available */
    uint64_t tsc_freq = tsc_get_frequency();
    if (tsc_freq > 0) {
        uint64_t tsc_now = rdtsc();
        uint64_t tsc_elapsed = tsc_now - timer_state.tsc_boot;
        return tsc_to_microseconds(tsc_elapsed) / 1000ULL;
    }

    /* Try HPET if TSC is unavailable or unreliable */
    if (hpet_is_available()) {
        uint64_t hpet_now = hpet_read_counter();
        uint64_t hpet_elapsed = hpet_now - timer_state.hpet_boot;
        return hpet_ticks_to_microseconds(hpet_elapsed) / 1000ULL;
    }

    /* Fall back to PIT-based timing */
    return (timer_state.ticks * 1000ULL) / timer_state.frequency;
}

/* HAL timer delay microseconds */
static void x86_64_timer_delay_us(uint64_t microseconds)
{
    /* Use TSC for high-resolution timing if available */
    uint64_t tsc_freq = tsc_get_frequency();
    if (tsc_freq > 0) {
        uint64_t tsc_start = rdtsc();
        uint64_t tsc_target = tsc_start + (microseconds * tsc_freq) / 1000000ULL;

        while (rdtsc() < tsc_target) {
            /* Busy wait with pause instruction */
            __asm__ volatile("pause");
        }
        return;
    }

    /* Try HPET if TSC is unavailable or unreliable */
    if (hpet_is_available()) {
        hpet_delay_us(microseconds);
        return;
    }

    /* Fall back to PIT-based timing */
    uint64_t start_ticks = timer_state.ticks;
    uint64_t ticks_to_wait = (microseconds * timer_state.frequency) / 1000000ULL;

    while ((timer_state.ticks - start_ticks) < ticks_to_wait) {
        /* Busy wait with pause instruction */
        __asm__ volatile("pause");
    }
}

/* HAL timer delay milliseconds */
static void x86_64_timer_delay_ms(uint64_t milliseconds)
{
    uint64_t start_ticks = timer_state.ticks;
    uint64_t ticks_to_wait = (milliseconds * timer_state.frequency) / 1000ULL;

    while ((timer_state.ticks - start_ticks) < ticks_to_wait) {
        /* Busy wait with pause instruction */
        __asm__ volatile("pause");
    }
}

/* HAL timer convert ticks to microseconds */
static uint64_t x86_64_timer_ticks_to_us(uint64_t ticks)
{
    return (ticks * 1000000ULL) / timer_state.frequency;
}

/* HAL timer convert microseconds to ticks */
static uint64_t x86_64_timer_us_to_ticks(uint64_t microseconds)
{
    return (microseconds * timer_state.frequency) / 1000000ULL;
}

/* x86_64 timer operations structure */
static const struct hal_timer_ops x86_64_timer_ops = {
    .init = x86_64_timer_init,
    .enable = x86_64_timer_enable,
    .disable = x86_64_timer_disable,
    .configure = x86_64_timer_configure,
    .get_ticks = x86_64_timer_get_ticks,
    .get_frequency = x86_64_timer_get_frequency,
    .get_microseconds = x86_64_timer_get_microseconds,
    .get_milliseconds = x86_64_timer_get_milliseconds,
    .delay_us = x86_64_timer_delay_us,
    .delay_ms = x86_64_timer_delay_ms,
    .ticks_to_us = x86_64_timer_ticks_to_us,
    .us_to_ticks = x86_64_timer_us_to_ticks
};

/* Register x86_64 timer HAL */
void arch_timer_init(void)
{
    hal_timer_register(&x86_64_timer_ops);
}

/* Timer interrupt handler (called from IRQ0) */
void timer_tick(void)
{
    if (timer_state.enabled) {
        timer_state.ticks++;
    }
}
