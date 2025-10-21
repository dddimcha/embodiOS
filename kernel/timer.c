/* EMBODIOS Timer Subsystem
 * 
 * Provides timing and scheduling functionality.
 * Handles timer interrupts and system tick management.
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/interrupt.h"

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

/* Initialize PIT */
static void pit_init(uint32_t frequency)
{
#ifdef __x86_64__
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
#else
    /* ARM64 uses different timer mechanism */
    (void)frequency;
#endif
}

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
    console_printf("Timer: Initializing with %d Hz frequency\n", TIMER_FREQUENCY);
    
    /* Initialize PIT */
    pit_init(TIMER_FREQUENCY);
    
    /* Timer state already initialized statically */
    timer_state.frequency = TIMER_FREQUENCY;
    
    console_printf("Timer: PIT initialized\n");
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

/* Sleep for specified milliseconds (busy wait) */
void timer_sleep(uint32_t ms)
{
    uint64_t start_ticks = timer_state.ticks;
    uint64_t ticks_to_wait = (ms * timer_state.frequency) / 1000;
    
    while ((timer_state.ticks - start_ticks) < ticks_to_wait) {
        /* Busy wait */
#ifdef __x86_64__
        __asm__ volatile("pause");
#else
        /* ARM64: yield instruction */
        __asm__ volatile("yield");
#endif
    }
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