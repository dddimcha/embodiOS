/* EMBODIOS Console Implementation - Optimized for Performance (#143) */
#include <embodios/console.h>
#include <embodios/types.h>

/* Output buffer for batching writes - reduces I/O overhead */
#define CONSOLE_BUFFER_SIZE 256
static char output_buffer[CONSOLE_BUFFER_SIZE];
static size_t buffer_pos = 0;

/* Architecture-specific console drivers */
#ifdef __x86_64__
#include <arch/vga.h>
extern int keyboard_getchar_poll(void);
extern void serial_init(void);
extern int serial_getchar(void);
extern void serial_putc(char c);

/* Batch write to VGA - updates cursor only once at end */
extern void vga_write_batch(const char* buf, size_t len);

static void arch_console_init(void) {
    vga_init();
    serial_init();  /* Also initialize serial for QEMU -nographic */
}

static void arch_console_putchar(char c) {
    vga_putchar(c);  /* VGA handles serial output internally */
}

/* Batch write - much faster than char-by-char */
static void arch_console_write_batch(const char* buf, size_t len) {
    vga_write_batch(buf, len);
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
static void arch_console_write_batch(const char* buf, size_t len) {
    for (size_t i = 0; i < len; i++) uart_putchar(buf[i]);
}
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
    size_t len = 0;
    const char* s = str;
    while (*s++) len++;
    if (len > 0) {
        arch_console_write_batch(str, len);
    }
}

/* Flush output buffer */
static void flush_buffer(void)
{
    if (buffer_pos > 0) {
        arch_console_write_batch(output_buffer, buffer_pos);
        buffer_pos = 0;
    }
}

/* Buffered putchar - batches writes for efficiency */
static void buffered_putchar(char c)
{
    output_buffer[buffer_pos++] = c;
    /* Flush on newline or buffer full */
    if (c == '\n' || buffer_pos >= CONSOLE_BUFFER_SIZE - 1) {
        flush_buffer();
    }
}

/* Fast number to string conversion using lookup tables
 * Avoids division where possible by using multiply-shift for common cases */
static const char hex_digits[] = "0123456789ABCDEF";

/* Optimized hex conversion - no division, just shifts */
static int format_hex(char* buf, uint64_t num)
{
    if (num == 0) {
        buf[0] = '0';
        return 1;
    }

    char tmp[16];
    int i = 0;
    while (num > 0) {
        tmp[i++] = hex_digits[num & 0xF];
        num >>= 4;
    }

    int len = i;
    while (i > 0) {
        *buf++ = tmp[--i];
    }
    return len;
}

/* Fast decimal conversion with minimal divisions
 * Uses lookup table for 2-digit pairs to halve divisions */
static const char digit_pairs[201] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

static int format_decimal(char* buf, uint64_t num, bool is_signed)
{
    if (num == 0) {
        buf[0] = '0';
        return 1;
    }

    char tmp[24];
    int i = 0;
    bool negative = false;

    if (is_signed && (int64_t)num < 0) {
        negative = true;
        num = -(int64_t)num;
    }

    /* Process 2 digits at a time for speed */
    while (num >= 100) {
        int idx = (num % 100) * 2;
        num /= 100;
        tmp[i++] = digit_pairs[idx + 1];
        tmp[i++] = digit_pairs[idx];
    }

    /* Handle remaining 1-2 digits */
    if (num >= 10) {
        int idx = (int)num * 2;
        tmp[i++] = digit_pairs[idx + 1];
        tmp[i++] = digit_pairs[idx];
    } else {
        tmp[i++] = '0' + (int)num;
    }

    int len = 0;
    if (negative) {
        buf[len++] = '-';
    }
    while (i > 0) {
        buf[len++] = tmp[--i];
    }
    return len;
}

void console_printf(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    char num_buf[32];

    while (*fmt) {
        if (*fmt != '%') {
            buffered_putchar(*fmt++);
            continue;
        }

        fmt++; /* Skip '%' */

        /* Parse flags */
        int zero_pad = 0;
        int left_align = 0;
        while (*fmt == '0' || *fmt == '-' || *fmt == ' ' || *fmt == '+') {
            if (*fmt == '0') zero_pad = 1;
            else if (*fmt == '-') left_align = 1;
            fmt++;
        }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse precision (for strings/floats, skip for now) */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Handle length modifiers */
        int is_long_long = 0;
        int is_long = 0;
        if (*fmt == 'l') {
            fmt++;
            is_long = 1;
            if (*fmt == 'l') {
                is_long_long = 1;
                fmt++;
            }
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;  /* hh modifier */
        } else if (*fmt == 'z') {
            is_long = 1;  /* size_t is usually same as long */
            fmt++;
        }

        /* Helper to output with padding */
        #define OUTPUT_PADDED(buf, len) do { \
            int pad = (width > len) ? (width - len) : 0; \
            char pad_char = zero_pad && !left_align ? '0' : ' '; \
            if (!left_align) { for (int _p = 0; _p < pad; _p++) buffered_putchar(pad_char); } \
            for (int _j = 0; _j < len; _j++) buffered_putchar(buf[_j]); \
            if (left_align) { for (int _p = 0; _p < pad; _p++) buffered_putchar(' '); } \
        } while(0)

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t val = is_long_long ?
                __builtin_va_arg(args, long long) :
                (is_long ? __builtin_va_arg(args, long) : __builtin_va_arg(args, int));
            int len = format_decimal(num_buf, (uint64_t)val, true);
            OUTPUT_PADDED(num_buf, len);
            break;
        }
        case 'u': {
            uint64_t val = is_long_long ?
                __builtin_va_arg(args, unsigned long long) :
                (is_long ? __builtin_va_arg(args, unsigned long) : __builtin_va_arg(args, unsigned int));
            int len = format_decimal(num_buf, val, false);
            OUTPUT_PADDED(num_buf, len);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t val = is_long_long ?
                __builtin_va_arg(args, unsigned long long) :
                (is_long ? __builtin_va_arg(args, unsigned long) : __builtin_va_arg(args, unsigned int));
            int len = format_hex(num_buf, val);
            OUTPUT_PADDED(num_buf, len);
            break;
        }
        case 'p': {
            buffered_putchar('0');
            buffered_putchar('x');
            int len = format_hex(num_buf, __builtin_va_arg(args, uintptr_t));
            int pad = (width > len + 2) ? (width - len - 2) : 0;
            for (int _p = 0; _p < pad; _p++) buffered_putchar('0');
            for (int j = 0; j < len; j++) buffered_putchar(num_buf[j]);
            break;
        }
        case 's': {
            const char* s = __builtin_va_arg(args, const char*);
            if (!s) s = "(null)";
            int len = 0;
            const char* t = s;
            while (*t++) len++;
            if (precision >= 0 && len > precision) len = precision;
            int pad = (width > len) ? (width - len) : 0;
            if (!left_align) { for (int _p = 0; _p < pad; _p++) buffered_putchar(' '); }
            for (int _j = 0; _j < len; _j++) buffered_putchar(s[_j]);
            if (left_align) { for (int _p = 0; _p < pad; _p++) buffered_putchar(' '); }
            break;
        }
        case 'c':
            buffered_putchar(__builtin_va_arg(args, int));
            break;
        case '%':
            buffered_putchar('%');
            break;
        case 'f':
        case 'e':
        case 'g': {
            double val = __builtin_va_arg(args, double);
            if (val < 0) {
                buffered_putchar('-');
                val = -val;
            }
            int64_t ipart = (int64_t)val;
            int len = format_decimal(num_buf, (uint64_t)ipart, false);
            for (int j = 0; j < len; j++) buffered_putchar(num_buf[j]);
            buffered_putchar('.');
            double frac = val - (double)ipart;
            int frac2 = (int)(frac * 100.0 + 0.5);
            if (frac2 < 10) buffered_putchar('0');
            len = format_decimal(num_buf, (uint64_t)frac2, false);
            for (int j = 0; j < len; j++) buffered_putchar(num_buf[j]);
            break;
        }
        case '\0':
            /* End of format string after % */
            goto done;
        default:
            buffered_putchar('%');
            buffered_putchar(*fmt);
            break;
        }
        #undef OUTPUT_PADDED
        fmt++;
    }

done:
    /* Flush remaining buffer */
    flush_buffer();
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