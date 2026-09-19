/* x86_64 8259 PIC driver
 *
 * Remaps the legacy PIC so IRQ0-15 land on INT 0x20-0x2F (above the CPU
 * exception vectors) and provides EOI + per-line mask control. This is the
 * minimal interrupt delivery path for the PIT timer; LAPIC/IOAPIC can be
 * layered on later by the SMP work without changing the IRQ contract.
 */
#include <stdint.h>
#include "../../include/arch/x86_64/pic.h"

#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

#define PIC_EOI         0x20

#define ICW1_INIT       0x10
#define ICW1_ICW4       0x01
#define ICW4_8086       0x01

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void)
{
    /* Port 0x80 (POST code port) is safe to write and wastes a few cycles */
    outb(0x80, 0);
}

void pic_init(void)
{
    /* Save current masks (not strictly needed right after boot, but cheap) */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: start initialization, edge-triggered, cascade, ICW4 needed */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, PIC_IRQ_BASE);        io_wait();   /* IRQ0-7 -> 0x20-0x27 */
    outb(PIC2_DATA, PIC_IRQ_SLAVE_BASE);  io_wait();   /* IRQ8-15 -> 0x28-0x2F */

    /* ICW3: master has slave on IRQ2; slave identity = 2 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    /* ICW4: 8086 mode, normal EOI */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask every line: handlers unmask what they need via pic_unmask_irq() */
    (void)mask1; (void)mask2;
    pic_mask_all();
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        /* Spurious IRQ15: the slave never actually raised it; only ack the
         * master (cascade). Check the slave ISR bit to distinguish. */
        if (irq == 15) {
            outb(PIC2_COMMAND, 0x0B);           /* read ISR */
            uint8_t isr = inb(PIC2_COMMAND);
            if ((isr & 0x80) == 0) {
                outb(PIC1_COMMAND, PIC_EOI);    /* spurious: ack master only */
                return;
            }
        }
        outb(PIC2_COMMAND, PIC_EOI);
    } else if (irq == 7) {
        /* Spurious IRQ7: if ISR bit 7 is clear the PIC did not really raise
         * it; sending EOI is still harmless for edge-triggered IRQ7. */
        outb(PIC1_COMMAND, PIC_EOI);
        return;
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_mask_irq(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq & 7;
    outb(port, inb(port) | (uint8_t)(1 << bit));
}

void pic_unmask_irq(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq & 7;
    outb(port, inb(port) & (uint8_t)~(1 << bit));
}

void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
