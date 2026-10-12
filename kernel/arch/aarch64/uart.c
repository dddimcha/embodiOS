/* EMBODIOS ARM64 UART Driver (PL011)
 * WS-B (pi-periph), v0.9.0 "Galvani": Pi board bring-up path
 * Author: Dmitry Dimcha <dddimcha@gmail.com>
 *
 * Boards:
 *  - QEMU virt: UART0 @ 0x09000000, PL011 clock 24 MHz (v0.8.0 behaviour).
 *  - pi3/pi4:   UART0 @ PERIPH+0x201000, GPIO14/15 -> ALT0 (GPFSEL1),
 *               pulls off via the BCM2835 GPPUD/GPPUDCLK procedure,
 *               baud divisor from the mailbox UART clock (id 2), falling
 *               back to the 48 MHz firmware default if the mailbox does
 *               not answer (boot must continue regardless).
 *  - pi5:       RP1 southbridge; GPIO14/15 -> ALT4 for UART0_TX/RX via
 *               kernel/drivers/gpio/bcm_gpio.c (verified: rp1.dtsi
 *               rp1_uart0_14_15 + Pi5 `pinctrl` dump "14: a4 pn TXD0 /
 *               15: a4 pu RXD0").
 *
 * Sources:
 *  - BCM2835 ARM Peripherals ch. 6 (GPFSEL1 @ +0x04, GPPUD @ +0x94,
 *    GPPUDCLK0 @ +0x98, pull sequence with ~150-cycle waits) and ch. 13
 *    (PL011 registers, IBRD/FBRD = clk/(16*baud)).
 *  - https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 *    (clock id 2 = UART; default UART clock on Pi3 firmware is 48 MHz).
 */

#include "embodios/types.h"

/* TEMPORARY: shim in lieu of WS-A's kernel/arch/aarch64/board.h.
 * Integrator: replace with the real board.h include and delete
 * kernel/include/arch-aarch64-board-shim/. */
#include "board.h"

#if defined(BOARD_HAS_MAILBOX) && BOARD_HAS_MAILBOX
#include <embodios/bcm_mailbox.h>
#include <embodios/mailbox_fb.h>
#endif

/* PL011 UART registers (base from board config) */
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

#define UART_BAUD       115200u

#if defined(BOARD_HAS_MAILBOX) && BOARD_HAS_MAILBOX
/* Firmware default PL011 clock on Pi3/Pi4 (mailbox get clock rate is the
 * authoritative source; this is only the fallback). */
#define UART_DEFAULT_CLK_HZ  48000000u
#else
/* QEMU virt PL011 clock (24 MHz): 24000000/(16*115200) = 13.02 -> 13/1 */
#define UART_DEFAULT_CLK_HZ  24000000u
#endif

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

#if defined(BOARD_HAS_MAILBOX) && BOARD_HAS_MAILBOX && !defined(BOARD_PI5)
/* GPIO14/15 -> ALT0 (TXD0/RXD0), pulls off.  BCM2835 ARM Peripherals ch. 6:
 * GPFSEL1 covers GPIO10-19 (pin N at bits [(N-10)*3 +2 : (N-10)*3]); the
 * pull sequence is: GPPUD=mode, wait >=150 cycles, GPPUDCLKn=pin mask,
 * wait >=150 cycles, GPPUDCLKn=0. */
#define GPIO_BASE       (PERIPH_BASE + 0x200000UL)
#define GPFSEL1         (GPIO_BASE + 0x04)
#define GPPUD           (GPIO_BASE + 0x94)
#define GPPUDCLK0       (GPIO_BASE + 0x98)
#define GPIO_FSEL_ALT0  4u

static void gpio_wait_150(void)
{
    for (volatile int i = 0; i < 150; i++) {
        __asm__ volatile("nop");
    }
}

static void uart_gpio_init(void)
{
    uint32_t r = mmio_read32(GPFSEL1);
    r &= ~((7u << 12) | (7u << 15));            /* GPIO14, GPIO15 fields */
    r |= (GPIO_FSEL_ALT0 << 12) | (GPIO_FSEL_ALT0 << 15);
    mmio_write32(GPFSEL1, r);

    mmio_write32(GPPUD, 0);                     /* pulls off */
    gpio_wait_150();
    mmio_write32(GPPUDCLK0, (1u << 14) | (1u << 15));
    gpio_wait_150();
    mmio_write32(GPPUDCLK0, 0);
}
#elif defined(BOARD_PI5)
extern void bcm_gpio_uart0_pins_pi5(void);      /* drivers/gpio/bcm_gpio.c */
static void uart_gpio_init(void)
{
    bcm_gpio_uart0_pins_pi5();
}
#endif

/* IBRD/FBRD for clk_hz/(16*baud), FBRD rounded to nearest 1/64. */
static void uart_set_baud(uint32_t clk_hz)
{
    uint32_t div16 = 16u * UART_BAUD;
    uint32_t ibrd = clk_hz / div16;
    uint32_t fbrd = ((clk_hz % div16) * 64u + div16 / 2u) / div16;
    if (ibrd == 0) {
        ibrd = 1;
        fbrd = 0;
    }
    mmio_write32(UART_IBRD, ibrd);
    mmio_write32(UART_FBRD, fbrd);
}

void uart_puts(const char* str);

#if defined(BOARD_HAS_MAILBOX) && BOARD_HAS_MAILBOX
/* One-shot board identification dump straight to the serial line (the
 * console may not exist yet when uart_init runs from uart_early_test). */
static int uart_board_info_printed = 0;

static void uart_print_hex(uint32_t v)
{
    char buf[11];
    const char *hex = "0123456789abcdef";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - i * 4)) & 0xF];
    }
    buf[10] = '\0';
    uart_puts(buf);
}

static void uart_print_dec(uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    if (v == 0) {
        uart_puts("0");
        return;
    }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    uart_puts(&buf[i]);
}

static void uart_board_info_dump(uint32_t uart_clk_hz, int clk_from_mbox)
{
    uint32_t fw_rev = 0, model = 0;
    uart_puts("MBOX: board=");
    uart_puts(BOARD_NAME);
    uart_puts(" fw rev=");
    if (bcm_mbox_get_fw_revision(&fw_rev) == 0) {
        uart_print_hex(fw_rev);
    } else {
        uart_puts("n/a");
    }
    uart_puts(" board model=");
    if (bcm_mbox_get_board_model(&model) == 0) {
        uart_print_hex(model);
    } else {
        uart_puts("n/a");
    }
    uart_puts(" uart clk=");
    uart_print_dec(uart_clk_hz);
    uart_puts(clk_from_mbox ? " Hz (mailbox)\n" : " Hz (default)\n");
}
#endif

void uart_init(void)
{
    /* Disable UART */
    mmio_write32(UART_CR, 0);

#if defined(BOARD_HAS_MAILBOX) && BOARD_HAS_MAILBOX
    /* Route GPIO14/15 to the PL011 before enabling TX/RX */
    uart_gpio_init();

    /* Divisor from the real UART clock; fall back to the 48 MHz firmware
     * default so the kernel keeps booting if the mailbox is silent. */
    uint32_t clk_hz = 0;
    int clk_from_mbox = (bcm_mbox_get_uart_clock(&clk_hz) == 0 && clk_hz != 0);
    if (!clk_from_mbox) {
        clk_hz = UART_DEFAULT_CLK_HZ;
    }
    uart_set_baud(clk_hz);
#else
    /* QEMU virt: 24 MHz PL011 clock -> IBRD 13, FBRD 1 (v0.8.0 values) */
    uart_set_baud(UART_DEFAULT_CLK_HZ);
#endif

    /* Set 8 bits, no parity, 1 stop bit, enable FIFOs */
    mmio_write32(UART_LCRH, UART_LCRH_WLEN_8 | UART_LCRH_FEN);

    /* Clear all interrupts */
    mmio_write32(UART_ICR, 0x7FF);

    /* Disable all interrupts */
    mmio_write32(UART_IMSC, 0);

    /* Enable UART, TX and RX */
    mmio_write32(UART_CR, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);

#if defined(BOARD_HAS_MAILBOX) && BOARD_HAS_MAILBOX
    if (!uart_board_info_printed) {
        uart_board_info_printed = 1;
        uart_board_info_dump(clk_hz, clk_from_mbox);
    }
    /* Bring up the HDMI framebuffer console now (uart_init runs from
     * console_init before any output), so the "FB: ..." boot line is not
     * injected mid-line by a lazy init on the first mirrored character.
     * Best-effort: failure just leaves the mirror dark. */
    mailbox_fb_init();
#endif
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
        while (mmio_read32(UART_FR) & UART_FR_TXFF) {
            __asm__ volatile("nop");
        }
        mmio_write32(UART_DR, '\r');
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
