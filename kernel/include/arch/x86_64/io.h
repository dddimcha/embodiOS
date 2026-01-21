/* EMBODIOS x86_64 I/O Port Operations
 *
 * Low-level I/O port access for x86_64 architecture.
 */

#ifndef ARCH_X86_64_IO_H
#define ARCH_X86_64_IO_H

#include <embodios/types.h>

/* Output a byte to an I/O port */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Input a byte from an I/O port */
static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Output a word (16-bit) to an I/O port */
static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* Input a word (16-bit) from an I/O port */
static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Output a dword (32-bit) to an I/O port */
static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* Input a dword (32-bit) from an I/O port */
static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* I/O wait (small delay) */
static inline void io_wait(void)
{
    /* Writing to port 0x80 causes a small delay */
    outb(0x80, 0);
}

#endif /* ARCH_X86_64_IO_H */
