#ifndef VGA_IO_H
#define VGA_IO_H

#include <embodios/types.h>

/* I/O port operations for VGA */
#ifdef __x86_64__
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
#else
/* Stub implementations for non-x86 platforms */
static inline void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

static inline uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}
#endif

#endif /* VGA_IO_H */