/* EMBODIOS ARM64 UART Driver (PL011) */

#include "embodios/types.h"

/* PL011 UART registers for QEMU virt machine */
#define UART0_BASE      0x09000000
#define UART_DR         (UART0_BASE + 0x00)  /* Data Register */
#define UART_FR         (UART0_BASE + 0x18)  /* Flag Register */
#define UART_IBRD       (UART0_BASE + 0x24)  /* Integer Baud Rate Divisor */
#define UART_FBRD       (UART0_BASE + 0x28)  /* Fractional Baud Rate Divisor */
#define UART_LCRH       (UART0_BASE + 0x2C)  /* Line Control Register */
#define UART_CR         (UART0_BASE + 0x30)  /* Control Register */
#define UART_IMSC       (UART0_BASE + 0x38)  /* Interrupt Mask Set/Clear */
#define UART_ICR        (UART0_BASE + 0x44)  /* Interrupt Clear Register */

/* Flag register bits */
#define UART_FR_TXFF    (1 << 5)  /* Transmit FIFO full */
#define UART_FR_RXFE    (1 << 4)  /* Receive FIFO empty */
#define UART_FR_BUSY    (1 << 3)  /* UART busy transmitting */

/* Control register bits */
#define UART_CR_UARTEN  (1 << 0)  /* UART enable */
#define UART_CR_TXE     (1 << 8)  /* Transmit enable */
#define UART_CR_RXE     (1 << 9)  /* Receive enable */

/* Line control register bits */
#define UART_LCRH_FEN   (1 << 4)  /* Enable FIFOs */
#define UART_LCRH_WLEN_8 (3 << 5) /* 8 bits word length */

static inline void mmio_write32(uintptr_t addr, uint32_t value)
{
    *(volatile uint32_t*)addr = value;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t*)addr;
}

void uart_init(void)
{
    /* Disable UART */
    mmio_write32(UART_CR, 0);
    
    /* Set baud rate to 115200 (assuming 24MHz clock) */
    /* Baud rate divisor = UART clock / (16 * baud rate) */
    /* For 24MHz: divisor = 24000000 / (16 * 115200) = 13.02 */
    mmio_write32(UART_IBRD, 13);
    mmio_write32(UART_FBRD, 1);
    
    /* Set 8 bits, no parity, 1 stop bit, enable FIFOs */
    mmio_write32(UART_LCRH, UART_LCRH_WLEN_8 | UART_LCRH_FEN);
    
    /* Clear all interrupts */
    mmio_write32(UART_ICR, 0x7FF);
    
    /* Disable all interrupts */
    mmio_write32(UART_IMSC, 0);
    
    /* Enable UART, TX and RX */
    mmio_write32(UART_CR, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}

void uart_putchar(char c)
{
    /* Wait for TX FIFO to have space */
    while (mmio_read32(UART_FR) & UART_FR_TXFF) {
        __asm__ volatile("nop");
    }
    
    /* Write character */
    mmio_write32(UART_DR, (uint32_t)c);
    
    /* Handle newline */
    if (c == '\n') {
        uart_putchar('\r');
    }
}

char uart_getchar(void)
{
    /* Wait for RX FIFO to have data */
    while (mmio_read32(UART_FR) & UART_FR_RXFE) {
        __asm__ volatile("nop");
    }

    /* Read character */
    return (char)(mmio_read32(UART_DR) & 0xFF);
}

void uart_flush(void)
{
    /* Wait for TX FIFO to be empty */
    while (mmio_read32(UART_FR) & UART_FR_BUSY) {
        __asm__ volatile("nop");
    }
}

/* Simple string output for early boot */
void uart_puts(const char* str)
{
    while (*str) {
        uart_putchar(*str++);
    }
}

/* Early boot test */
void uart_early_test(void)
{
    uart_init();
    uart_puts("EMBODIOS ARM64 booting...\n");
}