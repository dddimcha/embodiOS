/* EMBODIOS HAL Interrupt Interface */
#ifndef EMBODIOS_HAL_INTERRUPT_H
#define EMBODIOS_HAL_INTERRUPT_H

#include <embodios/types.h>

/* Interrupt handler type */
typedef void (*irq_handler_t)(void);

/* Interrupt priority levels */
#define IRQ_PRIORITY_HIGH       0
#define IRQ_PRIORITY_MEDIUM     1
#define IRQ_PRIORITY_LOW        2

/* Interrupt flags */
#define IRQ_FLAG_ENABLED        (1 << 0)
#define IRQ_FLAG_EDGE_TRIGGERED (1 << 1)
#define IRQ_FLAG_LEVEL_TRIGGERED (1 << 2)

/* HAL interrupt operations structure */
struct hal_interrupt_ops {
    /* Initialization */
    void (*init)(void);

    /* Global interrupt control */
    void (*enable_global)(void);
    void (*disable_global)(void);
    bool (*are_enabled)(void);

    /* IRQ line control */
    void (*enable_irq)(int irq);
    void (*disable_irq)(int irq);
    bool (*is_irq_enabled)(int irq);

    /* Interrupt configuration */
    void (*set_priority)(int irq, int priority);
    int (*get_priority)(int irq);
    void (*set_flags)(int irq, uint32_t flags);

    /* Interrupt handling */
    void (*ack_irq)(int irq);
    void (*end_irq)(int irq);
    int (*get_active_irq)(void);

    /* Handler registration */
    void (*register_handler)(int irq, irq_handler_t handler);
    void (*unregister_handler)(int irq);
};

/* HAL interrupt interface functions */
void hal_interrupt_register(const struct hal_interrupt_ops *ops);
const struct hal_interrupt_ops* hal_interrupt_get_ops(void);

/* HAL interrupt wrapper functions */
void hal_interrupt_init(void);
void hal_interrupt_enable_global(void);
void hal_interrupt_disable_global(void);
bool hal_interrupt_are_enabled(void);
void hal_interrupt_enable_irq(int irq);
void hal_interrupt_disable_irq(int irq);
bool hal_interrupt_is_irq_enabled(int irq);
void hal_interrupt_set_priority(int irq, int priority);
int hal_interrupt_get_priority(int irq);
void hal_interrupt_set_flags(int irq, uint32_t flags);
void hal_interrupt_ack_irq(int irq);
void hal_interrupt_end_irq(int irq);
int hal_interrupt_get_active_irq(void);
void hal_interrupt_register_handler(int irq, irq_handler_t handler);
void hal_interrupt_unregister_handler(int irq);

#endif /* EMBODIOS_HAL_INTERRUPT_H */
