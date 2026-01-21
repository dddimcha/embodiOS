/* EMBODIOS ARM64 Early Initialization */

#include "embodios/types.h"

/* External UART functions from uart.c */
extern void uart_init(void);
extern void uart_putchar(char c);
extern void uart_puts(const char* str);
extern void uart_flush(void);

/* ARM64 System Register Access */
static inline uint64_t read_mpidr_el1(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(val));
    return val;
}

static inline uint64_t read_currentel(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(val));
    return (val >> 2) & 0x3;
}

static inline void enable_fpu(void) {
    /* Enable FPU/SIMD access (CPACR_EL1) */
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3UL << 20);  /* FPEN = 0b11 - enable EL0 and EL1 access */
    __asm__ volatile("msr cpacr_el1, %0" :: "r"(cpacr));
    __asm__ volatile("isb");
}

void arch_early_init(void)
{
    /* Enable FPU/SIMD for NEON operations */
    enable_fpu();

    /* Read CPU information */
    uint64_t mpidr = read_mpidr_el1();
    uint64_t el = read_currentel();

    /* Initialize UART first so we can print */
    uart_init();

    uart_puts("\n");
    uart_puts("=== EMBODIOS ARM64 ===\n");
    uart_puts("Bare-metal AI Operating System\n");
    uart_puts("\n");

    /* Print current exception level */
    uart_puts("Exception Level: EL");
    uart_putchar('0' + (char)el);
    uart_puts("\n");

    /* Print CPU ID */
    uart_puts("CPU ID (MPIDR): 0x");
    /* Simple hex print */
    for (int i = 60; i >= 0; i -= 4) {
        int nibble = (mpidr >> i) & 0xF;
        uart_putchar(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
    uart_puts("\n");

    uart_puts("FPU/NEON: Enabled\n");
    uart_puts("\n");
}

void arch_console_init(void)
{
    /* UART already initialized in arch_early_init */
    /* This is called later by the kernel for additional setup */
}

void arch_console_putchar(char c)
{
    uart_putchar(c);
}

void arch_console_puts(const char* str)
{
    uart_puts(str);
}

void arch_interrupt_init(void)
{
    /*
     * ARM64 GIC (Generic Interrupt Controller) initialization
     * For QEMU virt machine, GIC-400 is at 0x08000000
     *
     * TODO: Full GIC implementation includes:
     * - GICD (Distributor) configuration
     * - GICC (CPU Interface) configuration
     * - Configure timer interrupts
     * - Set up exception vectors
     */
    uart_puts("GIC: Not yet implemented (single-threaded mode)\n");
}