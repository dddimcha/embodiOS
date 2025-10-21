/* x86_64 Interrupt Descriptor Table */
#include <stdint.h>
#include <stddef.h>
#include "../../include/arch/x86_64/idt.h"
#include "../../include/arch/x86_64/interrupt.h"
#include "../../include/embodios/mm.h"

/* IDT entry structure */
struct idt_entry {
    uint16_t offset_low;    /* Offset bits 0-15 */
    uint16_t selector;      /* Code segment selector */
    uint8_t  ist;          /* Interrupt Stack Table */
    uint8_t  type_attr;    /* Type and attributes */
    uint16_t offset_mid;    /* Offset bits 16-31 */
    uint32_t offset_high;   /* Offset bits 32-63 */
    uint32_t zero;         /* Reserved */
} __attribute__((packed));

/* IDT pointer structure */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* IDT with 256 entries */
static struct idt_entry idt[256];
static struct idt_ptr idtp;

/* External interrupt stub table */
extern void* interrupt_stub_table[];

/* Set an IDT gate */
static void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags)
{
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].selector = selector;
    idt[num].ist = 0;
    idt[num].type_attr = flags;
    idt[num].zero = 0;
}

/* Initialize the IDT */
void idt_init(void)
{
    /* Set up IDT pointer */
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)&idt;

    /* Clear IDT */
    memset(&idt, 0, sizeof(idt));

    /* Set up exception handlers (0-31) */
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, (uint64_t)interrupt_stub_table[i], 0x08, 0x8E);
    }

    /* Set up IRQ handlers (32-47) */
    for (int i = 32; i < 48; i++) {
        idt_set_gate(i, (uint64_t)interrupt_stub_table[i], 0x08, 0x8E);
    }

    /* Load IDT */
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

/* Install a custom interrupt handler */
void idt_install_handler(uint8_t num, uint64_t handler)
{
    idt_set_gate(num, handler, 0x08, 0x8E);
}

/* C interrupt handler - called from assembly stub */
void interrupt_handler(struct interrupt_frame* frame)
{
    /* For now, just a stub that does nothing */
    /* In a full implementation, this would:
     * 1. Dispatch to registered handlers
     * 2. Handle exceptions (print error, maybe panic)
     * 3. Send EOI to PIC/APIC
     */
    (void)frame;
}
