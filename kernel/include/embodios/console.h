#ifndef EMBODIOS_CONSOLE_H
#define EMBODIOS_CONSOLE_H

#include <embodios/types.h>

/* Console initialization */
void console_init(void);

/* Console output */
void console_putchar(char c);
void console_puts(const char* str);
void console_printf(const char* fmt, ...);

/* Console input */
int console_getchar(void);
size_t console_readline(char* buffer, size_t max_len);

/* Console control */
void console_clear(void);
void console_set_color(uint8_t fg, uint8_t bg);

/* Color codes */
#define COLOR_BLACK     0
#define COLOR_BLUE      1
#define COLOR_GREEN     2
#define COLOR_CYAN      3
#define COLOR_RED       4
#define COLOR_MAGENTA   5
#define COLOR_BROWN     6
#define COLOR_LIGHT_GRAY 7
#define COLOR_DARK_GRAY 8
#define COLOR_LIGHT_BLUE 9
#define COLOR_LIGHT_GREEN 10
#define COLOR_LIGHT_CYAN 11
#define COLOR_LIGHT_RED 12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW    14
#define COLOR_WHITE     15

#endif /* EMBODIOS_CONSOLE_H */