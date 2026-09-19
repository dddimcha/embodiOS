/* Serial Port Driver for x86_64 - Works in both BIOS and UEFI */
#include <embodios/types.h>

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Initialize serial port */
void serial_init(void) {
    static int already_initialized = 0;
    if (already_initialized) return;
    already_initialized = 1;
    /* NOTE: boot.S has already configured the UART before the boot log.
     * Re-running the DLAB/divisor/FCR sequence here races with incoming
     * bytes (drops/corrupts the one being received), so only do a full
     * re-init when the port looks unconfigured (LCR == 0). */
    if ((inb(COM1_PORT + 3) & 0x7F) == 0x03) {
        return;  /* already set to 8N1 by boot.S — leave it alone */
    }
    outb(COM1_PORT + 1, 0x00);    // Disable interrupts
    outb(COM1_PORT + 3, 0x80);    // Enable DLAB
    outb(COM1_PORT + 0, 0x03);    // Divisor low (38400 baud)
    outb(COM1_PORT + 1, 0x00);    // Divisor high
    outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop
    /* 0xC1: FIFO on, 14-byte trigger; do NOT use 0xC7 here — its
       reset bits would drop RX bytes typed during boot (this init
       runs after boot.S and would race with early user input). */
    outb(COM1_PORT + 2, 0xC1);    // Enable FIFO (no RX/TX reset)
    outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

/* Check if transmit is empty */
static int serial_transmit_empty(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

/* Write character to serial */
void serial_putc(char c) {
    while (!serial_transmit_empty());
    outb(COM1_PORT, c);
}

/* Write string to serial */
void serial_puts(const char* str) {
    while (*str) {
        if (*str == '\n') {
            serial_putc('\r');
        }
        serial_putc(*str++);
    }
}

/* Check if data is available */
static int serial_received(void) {
    return inb(COM1_PORT + 5) & 0x01;
}

/* Read character from serial (non-blocking) */
int serial_getchar(void) {
    if (!serial_received()) {
        return -1;  /* No data available */
    }
    return (int)inb(COM1_PORT);
}
