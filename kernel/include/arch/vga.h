#ifndef ARCH_VGA_H
#define ARCH_VGA_H

#include <embodios/types.h>

/* VGA text mode constants */
#define VGA_WIDTH       80
#define VGA_HEIGHT      25
#define VGA_BUFFER      0xB8000

/* VGA colors */
#define VGA_COLOR(fg, bg) ((bg << 4) | (fg & 0x0F))

/* VGA operations */
void vga_init(void);
void vga_putchar(char c);
void vga_clear(void);
void vga_set_color(uint8_t color);
void vga_set_cursor(uint16_t pos);

#endif /* ARCH_VGA_H */