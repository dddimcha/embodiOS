/* Minimal interrupt handling for EMBODIOS */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"

/* Interrupt handler table */
typedef void (*irq_handler_t)(void);
static irq_handler_t irq_handlers[256];

/* Timer tick counter */
volatile uint64_t timer_ticks = 0;

/* Initialize interrupt system */
void interrupt_init(void)
{
    /* Clear all handlers */
    for (int i = 0; i < 256; i++) {
        irq_handlers[i] = NULL;
    }
    
    console_printf("Interrupts: Basic handler table initialized\n");
}

/* Register an interrupt handler */
void register_irq_handler(int irq, irq_handler_t handler)
{
    if (irq >= 0 && irq < 256) {
        irq_handlers[irq] = handler;
    }
}

/* Timer interrupt handler */
void timer_interrupt_handler(void)
{
    timer_ticks++;
    
    /* Call scheduler every 10ms (assuming 100Hz timer) */
    if (timer_ticks % 10 == 0) {
        /* Scheduler will be called here */
        schedule();
    }
}

/* Get current timer ticks */
uint64_t get_timer_ticks(void)
{
    return timer_ticks;
}

/* Simple delay using timer */
void timer_delay(uint64_t ms)
{
    uint64_t start = timer_ticks;
    uint64_t ticks_to_wait = ms; /* Assuming 1ms per tick */
    
    while ((timer_ticks - start) < ticks_to_wait) {
        /* Busy wait */
        __asm__ volatile("nop");
    }
}

