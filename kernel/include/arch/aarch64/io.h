/* ARM64 I/O Operations
 *
 * ARM64 uses memory-mapped I/O instead of port I/O.
 * These functions provide compatibility stubs for code
 * written for x86 port I/O.
 */

#ifndef _ARCH_AARCH64_IO_H
#define _ARCH_AARCH64_IO_H

#include <embodios/types.h>

/* ARM64 uses memory-mapped I/O - these are stubs for x86 compatibility */

/* Port I/O stubs - ARM doesn't have port I/O, use MMIO instead */
static inline void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
    /* ARM64: No port I/O - use MMIO functions instead */
}

static inline uint8_t inb(uint16_t port)
{
    (void)port;
    /* ARM64: No port I/O - use MMIO functions instead */
    return 0;
}

static inline void outw(uint16_t port, uint16_t value)
{
    (void)port;
    (void)value;
}

static inline uint16_t inw(uint16_t port)
{
    (void)port;
    return 0;
}

static inline void outl(uint16_t port, uint32_t value)
{
    (void)port;
    (void)value;
}

static inline uint32_t inl(uint16_t port)
{
    (void)port;
    return 0;
}

/* Memory-mapped I/O for ARM64 */
static inline void mmio_write8(volatile void *addr, uint8_t value)
{
    *(volatile uint8_t *)addr = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint8_t mmio_read8(volatile void *addr)
{
    uint8_t value = *(volatile uint8_t *)addr;
    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}

static inline void mmio_write32(volatile void *addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t mmio_read32(volatile void *addr)
{
    uint32_t value = *(volatile uint32_t *)addr;
    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}

/* ARM64 PL011 UART addresses (Raspberry Pi, QEMU virt) */
#define ARM64_UART_BASE     0x09000000  /* QEMU virt machine UART */
#define PL011_DR            0x00        /* Data register */
#define PL011_FR            0x18        /* Flag register */
#define PL011_IBRD          0x24        /* Integer baud rate divisor */
#define PL011_FBRD          0x28        /* Fractional baud rate divisor */
#define PL011_LCR_H         0x2C        /* Line control register */
#define PL011_CR            0x30        /* Control register */

#define PL011_FR_TXFF       (1 << 5)    /* TX FIFO full */
#define PL011_FR_RXFE       (1 << 4)    /* RX FIFO empty */

/* UART helper functions for ARM64 GDB stub */
static inline void arm64_uart_putc(char c)
{
    volatile uint32_t *uart = (volatile uint32_t *)ARM64_UART_BASE;
    while (uart[PL011_FR / 4] & PL011_FR_TXFF);
    uart[PL011_DR / 4] = c;
}

static inline int arm64_uart_getc(void)
{
    volatile uint32_t *uart = (volatile uint32_t *)ARM64_UART_BASE;
    if (uart[PL011_FR / 4] & PL011_FR_RXFE) {
        return -1;
    }
    return uart[PL011_DR / 4] & 0xFF;
}

static inline bool arm64_uart_rx_ready(void)
{
    volatile uint32_t *uart = (volatile uint32_t *)ARM64_UART_BASE;
    return !(uart[PL011_FR / 4] & PL011_FR_RXFE);
}

static inline bool arm64_uart_tx_ready(void)
{
    volatile uint32_t *uart = (volatile uint32_t *)ARM64_UART_BASE;
    return !(uart[PL011_FR / 4] & PL011_FR_TXFF);
}

#endif /* _ARCH_AARCH64_IO_H */
