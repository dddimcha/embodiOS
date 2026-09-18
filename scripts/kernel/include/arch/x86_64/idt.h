#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <stdint.h>

/* Initialize the Interrupt Descriptor Table */
void idt_init(void);

/* Install a custom interrupt handler */
void idt_install_handler(uint8_t num, uint64_t handler);

#endif /* ARCH_X86_64_IDT_H */
