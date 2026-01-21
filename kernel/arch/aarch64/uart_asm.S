/* EMBODIOS ARM64 UART Driver (PL011) - Assembly Version
 * Written in assembly to avoid compiler generating HVF-incompatible instructions
 */

/* PL011 UART registers for QEMU virt machine */
.equ UART0_BASE,    0x09000000
.equ UART_DR,       0x00    /* Data Register */
.equ UART_FR,       0x18    /* Flag Register */
.equ UART_IBRD,     0x24    /* Integer Baud Rate Divisor */
.equ UART_FBRD,     0x28    /* Fractional Baud Rate Divisor */
.equ UART_LCRH,     0x2C    /* Line Control Register */
.equ UART_CR,       0x30    /* Control Register */
.equ UART_IMSC,     0x38    /* Interrupt Mask Set/Clear */
.equ UART_ICR,      0x44    /* Interrupt Clear Register */

/* Flag register bits */
.equ UART_FR_TXFF,  0x20    /* Transmit FIFO full (bit 5) */
.equ UART_FR_RXFE,  0x10    /* Receive FIFO empty (bit 4) */
.equ UART_FR_BUSY,  0x08    /* UART busy (bit 3) */

.section .text

/*
 * uart_init - Initialize UART
 * Uses only simple str/ldr with immediate offsets
 */
.global uart_init
uart_init:
    /* Load base address */
    mov x0, #UART0_BASE

    /* Disable UART (CR = 0) */
    str wzr, [x0, #UART_CR]

    /* Set baud rate: IBRD = 13, FBRD = 1 */
    mov w1, #13
    str w1, [x0, #UART_IBRD]
    mov w1, #1
    str w1, [x0, #UART_FBRD]

    /* Set 8N1, enable FIFOs: LCRH = 0x70 */
    mov w1, #0x70
    str w1, [x0, #UART_LCRH]

    /* Clear interrupts: ICR = 0x7FF */
    mov w1, #0x7FF
    str w1, [x0, #UART_ICR]

    /* Disable interrupts: IMSC = 0 */
    str wzr, [x0, #UART_IMSC]

    /* Enable UART, TX, RX: CR = 0x301 */
    mov w1, #0x301
    str w1, [x0, #UART_CR]

    ret

/*
 * uart_putchar - Write a character to UART
 * Input: w0 = character
 */
.global uart_putchar
uart_putchar:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    and w3, w0, #0xFF       /* Save char in w3 */

    /* Load base and flag register addresses */
    mov x1, #UART0_BASE
    add x2, x1, #UART_FR

    /* Wait for TX FIFO not full */
1:  ldr w0, [x2]
    tst w0, #UART_FR_TXFF
    b.ne 1b

    /* Write character */
    str w3, [x1, #UART_DR]

    /* If newline, also write carriage return */
    cmp w3, #'\n'
    b.ne 2f

    /* Wait for TX FIFO not full */
3:  ldr w0, [x2]
    tst w0, #UART_FR_TXFF
    b.ne 3b

    /* Write CR */
    mov w0, #'\r'
    str w0, [x1, #UART_DR]

2:  ldp x29, x30, [sp], #16
    ret

/*
 * uart_getchar - Read a character from UART
 * Output: w0 = character
 */
.global uart_getchar
uart_getchar:
    mov x1, #UART0_BASE
    add x2, x1, #UART_FR

    /* Wait for RX FIFO not empty */
1:  ldr w0, [x2]
    tst w0, #UART_FR_RXFE
    b.ne 1b

    /* Read character */
    ldr w0, [x1, #UART_DR]
    and w0, w0, #0xFF
    ret

/*
 * uart_flush - Wait for TX to complete
 */
.global uart_flush
uart_flush:
    mov x1, #UART0_BASE
    add x1, x1, #UART_FR

1:  ldr w0, [x1]
    tst w0, #UART_FR_BUSY
    b.ne 1b
    ret

/*
 * uart_puts - Write a string to UART
 * Input: x0 = pointer to null-terminated string
 */
.global uart_puts
uart_puts:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]

    mov x19, x0             /* Save string pointer */

1:  ldrb w0, [x19], #1      /* Load next char */
    cbz w0, 2f              /* Exit if null */
    bl uart_putchar
    b 1b

2:  ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

/*
 * uart_early_test - Early boot UART test
 */
.global uart_early_test
uart_early_test:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    bl uart_init

    adr x0, early_msg
    bl uart_puts

    ldp x29, x30, [sp], #16
    ret

.section .rodata
early_msg:
    .asciz "EMBODIOS ARM64 booting...\n"
