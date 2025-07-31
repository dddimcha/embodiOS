/* EMBODIOS ARM64 Early Initialization */

#include "embodios/types.h"

void arch_early_init(void)
{
    /* ARM64-specific early initialization */
    /* TODO: Set up initial MMU, configure system registers */
}

void arch_console_init(void)
{
    /* ARM64 console initialization stub */
    /* TODO: Initialize UART or other console device */
}

void arch_console_putchar(char c)
{
    /* ARM64 console output stub */
    /* TODO: Write to UART or other console device */
    (void)c;  /* Suppress unused parameter warning */
}