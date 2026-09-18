/* Minimal interrupt handling for EMBODIOS */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"

/* Interrupt handler table */
typedef void (*irq_handler_t)(void);
static irq_handler_t irq_handlers[256];

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

/* Timer functions are now provided by timer.c */

