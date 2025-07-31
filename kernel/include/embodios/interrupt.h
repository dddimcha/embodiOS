/* EMBODIOS Interrupt Handling */
#ifndef _EMBODIOS_INTERRUPT_H
#define _EMBODIOS_INTERRUPT_H

#include <embodios/types.h>

/* Interrupt handler type */
typedef void (*irq_handler_t)(void);

/* Initialize interrupt system */
void interrupt_init(void);

/* Architecture-specific interrupt initialization */
void arch_interrupt_init(void);

/* Register an interrupt handler */
void register_irq_handler(int irq, irq_handler_t handler);

/* Timer functions */
void timer_interrupt_handler(void);
uint64_t get_timer_ticks(void);
void timer_delay(uint64_t ms);

#endif /* _EMBODIOS_INTERRUPT_H */