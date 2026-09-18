/* EMBODIOS Timer Subsystem
 *
 * Provides timing and scheduling functionality.
 * Handles timer interrupts and system tick management.
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/interrupt.h"
#include "embodios/hal_timer.h"

/* Timer frequency (100 Hz = 10ms tick) */
#define TIMER_FREQUENCY 100

/* Timer state */
static struct {
    uint64_t ticks;           /* System ticks since boot */
    uint64_t seconds;         /* Seconds since boot */
    uint32_t frequency;       /* Timer frequency in Hz */
    void (*tick_handler)(void); /* Optional tick callback */
} timer_state = {
    .ticks = 0,
    .seconds = 0,
    .frequency = TIMER_FREQUENCY,
    .tick_handler = NULL
};


/* Timer interrupt handler (called from IRQ0) */
void timer_interrupt_handler(void)
{
    timer_state.ticks++;
    
    /* Update seconds counter */
    if (timer_state.ticks % timer_state.frequency == 0) {
        timer_state.seconds++;
    }
    
    /* Call registered tick handler if any */
    if (timer_state.tick_handler) {
        timer_state.tick_handler();
    }
}

/* Initialize timer subsystem */
void timer_init(void)
{
    console_printf("Timer: Initializing timer subsystem\n");

    /* Initialize HAL timer */
    hal_timer_init();

    /* Get actual frequency from HAL */
    timer_state.frequency = (uint32_t)hal_timer_get_frequency();

    console_printf("Timer: HAL timer initialized (frequency: %u Hz)\n",
                   timer_state.frequency);
}

/* Get system ticks */
uint64_t timer_get_ticks(void)
{
    return timer_state.ticks;
}

/* Compatibility alias for get_timer_ticks */
uint64_t get_timer_ticks(void)
{
    return timer_get_ticks();
}

/* Get system uptime in seconds */
uint64_t timer_get_seconds(void)
{
    return timer_state.seconds;
}

/* Get high-resolution microseconds since boot */
uint64_t timer_get_microseconds(void)
{
    return hal_timer_get_microseconds();
}

/* Get high-resolution milliseconds since boot */
uint64_t timer_get_milliseconds(void)
{
    return hal_timer_get_milliseconds();
}

/* Sleep for specified milliseconds (busy wait) */
void timer_sleep(uint32_t ms)
{
    /* Use HAL high-resolution timer for accurate delays */
    hal_timer_delay_ms(ms);
}

/* Compatibility alias for timer_delay */
void timer_delay(uint64_t ms)
{
    timer_sleep(ms);
}

/* Register tick handler */
void timer_register_tick_handler(void (*handler)(void))
{
    timer_state.tick_handler = handler;
}

/* Get timer frequency */
uint32_t timer_get_frequency(void)
{
    return timer_state.frequency;
}

/* Timer statistics */
void timer_stats(void)
{
    console_printf("Timer Statistics:\n");
    console_printf("  Frequency: %d Hz\n", timer_state.frequency);
    console_printf("  Ticks: %lu\n", timer_state.ticks);
    console_printf("  Uptime: %lu seconds\n", timer_state.seconds);
    console_printf("  Tick handler: %s\n", 
                   timer_state.tick_handler ? "Registered" : "None");
}