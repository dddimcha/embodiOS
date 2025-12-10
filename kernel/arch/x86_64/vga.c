/* x86_64 VGA Text Mode Driver */
#include <arch/vga.h>
#include <embodios/mm.h>
#include "vga_io.h"

/* Serial port output for QEMU -nographic mode */
#define SERIAL_COM1 0x3F8

static void serial_init(void)
{
    outb(SERIAL_COM1 + 1, 0x00);    /* Disable interrupts */
    outb(SERIAL_COM1 + 3, 0x80);    /* Enable DLAB */
    outb(SERIAL_COM1 + 0, 0x03);    /* 38400 baud */
    outb(SERIAL_COM1 + 1, 0x00);
    outb(SERIAL_COM1 + 3, 0x03);    /* 8N1 */
    outb(SERIAL_COM1 + 2, 0xC7);    /* FIFO */
    outb(SERIAL_COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static void serial_putchar(char c)
{
    while ((inb(SERIAL_COM1 + 5) & 0x20) == 0);
    outb(SERIAL_COM1, c);
}

/* VGA state */
static struct {
    uint16_t* buffer;
    uint16_t cursor_x;
    uint16_t cursor_y;
    uint8_t color;
} vga_state = {
    .buffer = (uint16_t*)VGA_BUFFER,
    .cursor_x = 0,
    .cursor_y = 0,
    .color = VGA_COLOR(7, 0)  /* Light gray on black */
};

/* Update hardware cursor */
static void update_cursor(void)
{
    uint16_t pos = vga_state.cursor_y * VGA_WIDTH + vga_state.cursor_x;
    
    /* Send cursor position to VGA controller */
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* Scroll screen up one line */
static void scroll(void)
{
    /* Move all lines up */
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_state.buffer[y * VGA_WIDTH + x] = 
                vga_state.buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    
    /* Clear last line */
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_state.buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = 
            ' ' | (vga_state.color << 8);
    }
    
    vga_state.cursor_y = VGA_HEIGHT - 1;
}

/* Initialize VGA driver */
void vga_init(void)
{
    /* Initialize serial for QEMU -nographic mode */
    serial_init();

    /* Clear screen */
    vga_clear();

    /* Enable cursor */
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
}

/* Put character to screen */
void vga_putchar(char c)
{
    /* Also output to serial for QEMU -nographic mode */
    serial_putchar(c);

    switch (c) {
    case '\n':
        vga_state.cursor_x = 0;
        vga_state.cursor_y++;
        break;
        
    case '\r':
        vga_state.cursor_x = 0;
        break;
        
    case '\b':
        if (vga_state.cursor_x > 0) {
            vga_state.cursor_x--;
            vga_state.buffer[vga_state.cursor_y * VGA_WIDTH + vga_state.cursor_x] = 
                ' ' | (vga_state.color << 8);
        }
        break;
        
    case '\t':
        vga_state.cursor_x = (vga_state.cursor_x + 8) & ~7;
        break;
        
    default:
        if (c >= 32 && c < 127) {
            vga_state.buffer[vga_state.cursor_y * VGA_WIDTH + vga_state.cursor_x] = 
                c | (vga_state.color << 8);
            vga_state.cursor_x++;
        }
        break;
    }
    
    /* Handle line wrap */
    if (vga_state.cursor_x >= VGA_WIDTH) {
        vga_state.cursor_x = 0;
        vga_state.cursor_y++;
    }
    
    /* Handle scrolling */
    if (vga_state.cursor_y >= VGA_HEIGHT) {
        scroll();
    }
    
    update_cursor();
}

/* Clear screen */
void vga_clear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_state.buffer[i] = ' ' | (vga_state.color << 8);
    }
    
    vga_state.cursor_x = 0;
    vga_state.cursor_y = 0;
    update_cursor();
}

/* Set text color */
void vga_set_color(uint8_t color)
{
    vga_state.color = color;
}

/* Set cursor position */
void vga_set_cursor(uint16_t pos)
{
    vga_state.cursor_x = pos % VGA_WIDTH;
    vga_state.cursor_y = pos / VGA_WIDTH;
    update_cursor();
}