/* x86_64 Programmable Interrupt Controller (8259 PIC) */
#ifndef ARCH_X86_64_PIC_H
#define ARCH_X86_64_PIC_H

#include <stdint.h>

/* Remapped IRQ vectors (after pic_init) */
#define PIC_IRQ_BASE        0x20    /* IRQ0-7  -> INT 0x20-0x27 */
#define PIC_IRQ_SLAVE_BASE  0x28    /* IRQ8-15 -> INT 0x28-0x2F */
#define IRQ_TIMER           0
#define IRQ_KEYBOARD        1

/* Remap both PICs and mask all IRQ lines */
void pic_init(void);

/* Send End-Of-Interrupt for an IRQ line (handles spurious IRQ7/15) */
void pic_send_eoi(uint8_t irq);

/* Mask/unmask individual IRQ lines */
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);

/* Mask everything (used by panic paths) */
void pic_mask_all(void);

#endif /* ARCH_X86_64_PIC_H */
