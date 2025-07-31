#ifndef EMBODIOS_KERNEL_H
#define EMBODIOS_KERNEL_H

#include <embodios/types.h>

/* Kernel constants */
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define KERNEL_STACK_SIZE (64 * 1024)

/* Alignment macros */
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))

/* Kernel entry point */
void kernel_main(void);
void kernel_loop(void);
void kernel_panic(const char* msg, ...) __attribute__((noreturn));

/* Architecture interface */
void arch_early_init(void);
void arch_cpu_init(void);
void arch_interrupt_init(void);
void arch_enable_interrupts(void);
void arch_disable_interrupts(void);
void arch_halt(void);

/* Forward declaration */
struct embodios_model;

/* Command processing */
void command_processor_init(struct embodios_model* model);
void process_command(const char* cmd);

/* Task scheduling */
void schedule(void);

#endif /* EMBODIOS_KERNEL_H */