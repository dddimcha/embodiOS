#ifndef ARCH_UART_H
#define ARCH_UART_H

#include <embodios/types.h>

/* UART operations for ARM64 */
void uart_init(void);
void uart_putchar(char c);
char uart_getchar(void);
void uart_flush(void);
bool uart_has_data(void);

#endif /* ARCH_UART_H */