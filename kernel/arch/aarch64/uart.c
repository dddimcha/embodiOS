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

/* Use explicit volatile pointers to avoid compiler optimization on device memory */
static volatile uint32_t* const UART_BASE_PTR = (volatile uint32_t*)UART0_BASE;

/* Simple MMIO functions that generate plain load/store instructions */
static void mmio_write32(uintptr_t addr, uint32_t value)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    __asm__ volatile("str %w[val], [%[ptr]]" : : [val] "r" (value), [ptr] "r" (ptr) : "memory");
}

static uint32_t mmio_read32(uintptr_t addr)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    uint32_t val;
    __asm__ volatile("ldr %w[val], [%[ptr]]" : [val] "=r" (val) : [ptr] "r" (ptr) : "memory");
    return val;
}

void uart_init(void)
{
    /* Use simple stores with explicit addresses to avoid complex addressing modes */
    volatile uint32_t *cr = (volatile uint32_t *)(UART0_BASE + 0x30);
    volatile uint32_t *ibrd = (volatile uint32_t *)(UART0_BASE + 0x24);
    volatile uint32_t *fbrd = (volatile uint32_t *)(UART0_BASE + 0x28);
    volatile uint32_t *lcrh = (volatile uint32_t *)(UART0_BASE + 0x2C);
    volatile uint32_t *icr = (volatile uint32_t *)(UART0_BASE + 0x44);
    volatile uint32_t *imsc = (volatile uint32_t *)(UART0_BASE + 0x38);

    /* Disable UART */
    *cr = 0;

    /* Set baud rate */
    *ibrd = 13;
    *fbrd = 1;

    /* Set 8 bits, no parity, 1 stop bit, enable FIFOs */
    *lcrh = UART_LCRH_WLEN_8 | UART_LCRH_FEN;

    /* Clear all interrupts */
    *icr = 0x7FF;

    /* Disable all interrupts */
    *imsc = 0;

    /* Enable UART, TX and RX */
    *cr = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
}

void uart_putchar(char c)
{
    volatile uint32_t *dr = (volatile uint32_t *)UART0_BASE;
    volatile uint32_t *fr = (volatile uint32_t *)(UART0_BASE + 0x18);

    /* Wait for TX FIFO to have space */
    while ((*fr) & UART_FR_TXFF) {
        __asm__ volatile("nop");
    }

    /* Write character */
    *dr = (uint32_t)c;

    /* Handle newline */
    if (c == '\n') {
        while ((*fr) & UART_FR_TXFF) {
            __asm__ volatile("nop");
        }
        *dr = '\r';
    }
}

char uart_getchar(void)
{
    volatile uint32_t *dr = (volatile uint32_t *)UART0_BASE;
    volatile uint32_t *fr = (volatile uint32_t *)(UART0_BASE + 0x18);

    /* Wait for RX FIFO to have data */
    while ((*fr) & UART_FR_RXFE) {
        __asm__ volatile("nop");
    }

    /* Read character */
    return (char)((*dr) & 0xFF);
}

void uart_flush(void)
{
    volatile uint32_t *fr = (volatile uint32_t *)(UART0_BASE + 0x18);

    /* Wait for TX FIFO to be empty */
    while ((*fr) & UART_FR_BUSY) {
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