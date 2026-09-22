/* x86_64 Interrupt Descriptor Table */
#include <stdint.h>
#include <stddef.h>
#include "../../include/arch/x86_64/idt.h"
#include "../../include/arch/x86_64/interrupt.h"
#include "../../include/arch/x86_64/pic.h"
#include "../../include/embodios/mm.h"
#include "../../include/embodios/kernel.h"
#include "../../include/embodios/console.h"
#include "../../include/embodios/interrupt.h"
#include "../../include/embodios/lapic_timer.h"

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

    /* Clear IDT - use simple loop instead of memset to avoid potential issues */
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].type_attr = 0;
        idt[i].offset_mid = 0;
        idt[i].offset_high = 0;
        idt[i].zero = 0;
    }

    /* Set up exception handlers (0-31) - check if stub table is valid */
    if (interrupt_stub_table != NULL) {
        for (int i = 0; i < 32; i++) {
            if (interrupt_stub_table[i] != NULL) {
                idt_set_gate(i, (uint64_t)interrupt_stub_table[i], 0x08, 0x8E);
            }
        }

        /* Set up IRQ handlers (32-47) */
        for (int i = 32; i < 48; i++) {
            if (interrupt_stub_table[i] != NULL) {
                idt_set_gate(i, (uint64_t)interrupt_stub_table[i], 0x08, 0x8E);
            }
        }
    }

    /* Load IDT */
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

/* Install a custom interrupt handler */
void idt_install_handler(uint8_t num, uint64_t handler)
{
    idt_set_gate(num, handler, 0x08, 0x8E);
}

/* Load the shared IDT on a secondary CPU (AP bring-up, smp.c).
 * The IDT itself is global; each CPU needs its own lidt. */
void idt_ap_reload(void)
{
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

/* Exception names for vectors 0-31 */
static const char* const exception_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating point", "Alignment check", "Machine check", "SIMD floating point",
    "Virtualization", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Security exception", "Reserved"
};

/* arch/x86_64/hal_timer.c: PIT tick accounting (gated by hal_timer_enable) */
extern void timer_tick(void);

/* IRQ dispatch table: drivers can hook vectors 32-47 */
static void (*irq_dispatch[16])(void);

void irq_register_dispatch(uint8_t irq, void (*handler)(void))
{
    if (irq < 16) {
        irq_dispatch[irq] = handler;
    }
}

/* Dump the saved frame for an exception before panicking */
static void dump_exception_frame(struct interrupt_frame* frame)
{
    console_printf("\n*** CPU EXCEPTION %llu: %s (err=0x%llx) ***\n",
                   (unsigned long long)frame->int_no,
                   exception_names[frame->int_no & 31],
                   (unsigned long long)frame->err_code);
    console_printf("  RIP=%p  CS=0x%llx  RFLAGS=0x%llx\n",
                   (void*)frame->rip,
                   (unsigned long long)frame->cs,
                   (unsigned long long)frame->rflags);
    console_printf("  RSP=%p  SS=0x%llx\n",
                   (void*)frame->rsp,
                   (unsigned long long)frame->ss);
    console_printf("  RAX=%p RBX=%p RCX=%p RDX=%p\n",
                   (void*)frame->rax, (void*)frame->rbx,
                   (void*)frame->rcx, (void*)frame->rdx);
    console_printf("  RSI=%p RDI=%p RBP=%p\n",
                   (void*)frame->rsi, (void*)frame->rdi, (void*)frame->rbp);
    console_printf("  R8 =%p R9 =%p R10=%p R11=%p\n",
                   (void*)frame->r8, (void*)frame->r9,
                   (void*)frame->r10, (void*)frame->r11);
    console_printf("  R12=%p R13=%p R14=%p R15=%p\n",
                   (void*)frame->r12, (void*)frame->r13,
                   (void*)frame->r14, (void*)frame->r15);
}

/* C interrupt handler - called from assembly stub (interrupt.S).
 *
 * The stub has already saved every general-purpose register; the frame
 * pointer identifies the vector. CPU exceptions (0-31) are fatal and go to
 * kernel_panic with a full register dump. Hardware IRQs (32-47) get an EOI
 * to the PIC; IRQ0 additionally drives the PIT tick and the scheduler. */
void interrupt_handler(struct interrupt_frame* frame)
{
    uint64_t vector = frame->int_no;

    if (vector < 32) {
        /* CPU exception - dump state and panic (never returns) */
        dump_exception_frame(frame);
        kernel_panic("Unhandled CPU exception %llu (%s) at RIP=%p",
                     (unsigned long long)vector,
                     exception_names[vector & 31],
                     (void*)frame->rip);
    }

    if (vector >= 32 && vector < 48) {
        uint8_t irq = (uint8_t)(vector - 32);

        if (irq == IRQ_TIMER && lapic_timer_active()) {
            /* LAPIC timer tick (the PIT line is masked at the PIC, so
             * vector 0x20 is delivered by the LAPIC): EOI goes to the
             * LAPIC, not the 8259. The fast tick drives the rt_timer
             * callback layer; the legacy 100 Hz chain (tick accounting +
             * scheduler_tick) is decimated so preemption semantics,
             * uptime and hal_timer behavior are unchanged. */
            lapic_timer_eoi();
            lapic_timer_tick();
            if (lapic_timer_legacy_due()) {
                timer_tick();
                timer_interrupt_handler();
            }
            return;
        }

        /* Acknowledge the PIC first: the timer path below may context-switch
         * away from this stack, and the PIT must be allowed to raise the next
         * IRQ0 while the other task runs. */
        pic_send_eoi(irq);

        if (irq == IRQ_TIMER) {
            /* PIT tick: advance HAL tick counter, then the generic timer
             * subsystem (ticks/uptime + scheduler_tick via tick_handler). */
            timer_tick();
            timer_interrupt_handler();
        } else if (irq_dispatch[irq]) {
            irq_dispatch[irq]();
        }
        return;
    }

    /* Vectors 48+ have no IDT stubs installed; if we ever get here, panic */
    kernel_panic("Unexpected interrupt vector %llu", (unsigned long long)vector);
}
