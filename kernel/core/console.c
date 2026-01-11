/* EMBODIOS Console Implementation */
#include <embodios/console.h>
#include <embodios/types.h>

/* Architecture-specific console drivers */
#ifdef __x86_64__
#include <arch/vga.h>
extern int keyboard_getchar_poll(void);
extern void serial_init(void);
extern int serial_getchar(void);
extern void serial_putc(char c);

static void arch_console_init(void) {
    vga_init();
    serial_init();  /* Also initialize serial for QEMU -nographic */
}

static void arch_console_putchar(char c) {
    vga_putchar(c);  /* VGA handles serial output internally */
}

static int arch_console_getchar(void) {
    /* Try serial first (for QEMU -nographic), then keyboard */
    int c = serial_getchar();
    if (c != -1) return c;
    return keyboard_getchar_poll();
}

static void arch_console_flush(void) { /* No-op for VGA */ }
#elif defined(__aarch64__)
#include <arch/uart.h>
extern char uart_getchar(void);
extern void uart_flush(void);
static void arch_console_init(void) { uart_init(); }
static void arch_console_putchar(char c) { uart_putchar(c); }
static int arch_console_getchar(void) { return (int)uart_getchar(); }
static void arch_console_flush(void) { uart_flush(); }
#endif

/* Console state */
static struct {
    uint8_t fg_color;
    uint8_t bg_color;
    bool initialized;
} console_state = {
    .fg_color = COLOR_WHITE,
    .bg_color = COLOR_BLACK,
    .initialized = false
};

void console_init(void)
{
    arch_console_init();
    console_state.initialized = true;
}

void console_putchar(char c)
{
    if (!console_state.initialized) return;
    arch_console_putchar(c);
}

void console_puts(const char* str)
{
    if (!str) return;
    while (*str) {
        console_putchar(*str++);
    }
}

/* Simple printf implementation */
static void print_number(uint64_t num, int base, bool sign)
{
    char buffer[32];
    const char* digits = "0123456789ABCDEF";
    int i = 0;
    
    if (num == 0) {
        console_putchar('0');
        return;
    }
    
    if (sign && (int64_t)num < 0) {
        console_putchar('-');
        num = -(int64_t)num;
    }
    
    while (num > 0) {
        buffer[i++] = digits[num % base];
        num /= base;
    }
    
    while (i > 0) {
        console_putchar(buffer[--i]);
    }
}

void console_printf(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            console_putchar(*fmt++);
            continue;
        }

        fmt++; /* Skip '%' */

        /* Handle length modifiers */
        int is_long_long = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                is_long_long = 1;
                fmt++;
            }
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            if (is_long_long) {
                print_number(__builtin_va_arg(args, long long), 10, true);
            } else {
                print_number(__builtin_va_arg(args, int), 10, true);
            }
            break;
        case 'u':
            if (is_long_long) {
                print_number(__builtin_va_arg(args, unsigned long long), 10, false);
            } else {
                print_number(__builtin_va_arg(args, unsigned int), 10, false);
            }
            break;
        case 'x':
        case 'X':
            if (is_long_long) {
                print_number(__builtin_va_arg(args, unsigned long long), 16, false);
            } else {
                print_number(__builtin_va_arg(args, unsigned int), 16, false);
            }
            break;
        case 'p':
            console_puts("0x");
            print_number(__builtin_va_arg(args, uintptr_t), 16, false);
            break;
        case 's':
            console_puts(__builtin_va_arg(args, const char*));
            break;
        case 'c':
            console_putchar(__builtin_va_arg(args, int));
            break;
        case 'z':
            if (*(fmt + 1) == 'u') {
                fmt++;
                print_number(__builtin_va_arg(args, size_t), 10, false);
            }
            break;
        case '%':
            console_putchar('%');
            break;
        default:
            console_putchar('%');
            if (is_long_long) {
                console_puts("ll");
            }
            console_putchar(*fmt);
            break;
        }
        fmt++;
    }

    __builtin_va_end(args);
}

void console_flush(void)
{
    if (!console_state.initialized) return;
    arch_console_flush();
}

size_t console_readline(char* buffer, size_t max_len)
{
    size_t pos = 0;

    if (!buffer || max_len == 0) return 0;

    /* Flush any pending output before waiting for input */
    console_flush();

    while (pos < max_len - 1) {
        int c = console_getchar();

        if (c == -1) {
            /* No input available, wait or return */
            continue;
        }

        if (c == '\n' || c == '\r') {
            console_putchar('\n');
            break;
        } else if (c == '\b' || c == 127) { /* Backspace */
            if (pos > 0) {
                pos--;
                console_putchar('\b');
                console_putchar(' ');
                console_putchar('\b');
            }
        } else if (c >= 32 && c < 127) { /* Printable */
            buffer[pos++] = (char)c;
            console_putchar((char)c);
        }
    }

    buffer[pos] = '\0';
    return pos;
}

int console_getchar(void)
{
    if (!console_state.initialized) return -1;
    return arch_console_getchar();
}

void console_clear(void)
{
    /* TODO: Implement based on architecture */
}

void console_set_color(uint8_t fg, uint8_t bg)
{
    console_state.fg_color = fg;
    console_state.bg_color = bg;
    /* TODO: Apply colors to output */
}