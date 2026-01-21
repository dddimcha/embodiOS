/* EMBODIOS Compatibility Stubs for llama.cpp
 *
 * Maps standard C library functions to kernel equivalents.
 * Provides minimal implementations where kernel equivalents don't exist.
 */

#include <embodios/types.h>
#include <embodios/mm.h>
#include <embodios/console.h>

/* Forward declarations from kernel */
extern void console_printf(const char* fmt, ...);
extern void kernel_panic(const char* msg, ...) __attribute__((noreturn));

/* Aliases for common names */
#define kprintf console_printf
#define kpanic(msg) kernel_panic(msg)

/* wchar_t for wide character functions */
typedef int wchar_t;

/* Forward declarations of functions from lib/string.c and lib/stdlib.c */
extern int atoi(const char* nptr);
extern size_t strlen(const char* s);
extern char* strcpy(char* dest, const char* src);

/* Forward declarations of functions from lib/math.c */
extern float sqrtf(float x);
extern float expf(float x);
extern float logf(float x);
extern float powf(float x, float y);
extern float sinf(float x);
extern float cosf(float x);
extern float tanhf(float x);
extern float fabsf(float x);

/* Forward declarations of functions defined later in this file */
double strtod(const char* nptr, char** endptr);
long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
char* strtok_r(char* str, const char* delim, char** saveptr);
size_t strnlen(const char* s, size_t maxlen);

/* ============================================================================
 * MEMORY ALLOCATION - Map to kernel allocators
 * ============================================================================ */

void* malloc(size_t size)
{
    return kmalloc(size);
}

void* calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void* ptr = kmalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size)
{
    return krealloc(ptr, size);
}

void free(void* ptr)
{
    kfree(ptr);
}

void* aligned_alloc(size_t alignment, size_t size)
{
    return heap_alloc_aligned(size, alignment);
}

int posix_memalign(void** memptr, size_t alignment, size_t size)
{
    if (!memptr || (alignment & (alignment - 1)) != 0 || alignment < sizeof(void*)) {
        return 22; /* EINVAL */
    }
    void* ptr = heap_alloc_aligned(size, alignment);
    if (!ptr) {
        return 12; /* ENOMEM */
    }
    *memptr = ptr;
    return 0;
}

/* ============================================================================
 * PROGRAM TERMINATION
 * ============================================================================ */

void abort(void)
{
    kpanic("abort() called");
    __builtin_unreachable();
}

void exit(int status)
{
    (void)status;
    kpanic("exit() called - no process to terminate");
    __builtin_unreachable();
}

void _Exit(int status)
{
    exit(status);
}

/* Simple atexit stub - doesn't actually register handlers */
static void (*atexit_handlers[32])(void);
static int atexit_count = 0;

int atexit(void (*func)(void))
{
    if (atexit_count >= 32) return -1;
    atexit_handlers[atexit_count++] = func;
    return 0;
}

/* ============================================================================
 * STRING CONVERSION
 * ============================================================================ */

/* atoi already implemented in lib/stdlib.c */

long atol(const char* nptr)
{
    return (long)atoi(nptr);
}

long long atoll(const char* nptr)
{
    long long result = 0;
    int sign = 1;

    while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n') nptr++;

    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') nptr++;

    while (*nptr >= '0' && *nptr <= '9') {
        result = result * 10 + (*nptr - '0');
        nptr++;
    }

    return result * sign;
}

double atof(const char* nptr)
{
    return strtod(nptr, NULL);
}

long strtol(const char* nptr, char** endptr, int base)
{
    const char* p = nptr;
    long result = 0;
    int sign = 1;

    while (*p == ' ' || *p == '\t') p++;

    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;

    if (base == 0) {
        if (*p == '0') {
            if (p[1] == 'x' || p[1] == 'X') { base = 16; p += 2; }
            else { base = 8; p++; }
        } else {
            base = 10;
        }
    } else if (base == 16 && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    while (*p) {
        int digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else break;

        if (digit >= base) break;
        result = result * base + digit;
        p++;
    }

    if (endptr) *endptr = (char*)p;
    return result * sign;
}

unsigned long strtoul(const char* nptr, char** endptr, int base)
{
    return (unsigned long)strtol(nptr, endptr, base);
}

long long strtoll(const char* nptr, char** endptr, int base)
{
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char* nptr, char** endptr, int base)
{
    return (unsigned long long)strtol(nptr, endptr, base);
}

/* Float/double string conversion */
float strtof(const char* nptr, char** endptr)
{
    return (float)strtod(nptr, endptr);
}

double strtod(const char* nptr, char** endptr)
{
    const char* p = nptr;
    double result = 0.0;
    double fraction = 0.0;
    int sign = 1;
    int exp_sign = 1;
    int exponent = 0;

    while (*p == ' ' || *p == '\t') p++;

    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;

    /* Integer part */
    while (*p >= '0' && *p <= '9') {
        result = result * 10.0 + (*p - '0');
        p++;
    }

    /* Fractional part */
    if (*p == '.') {
        double divisor = 10.0;
        p++;
        while (*p >= '0' && *p <= '9') {
            fraction += (*p - '0') / divisor;
            divisor *= 10.0;
            p++;
        }
    }

    result = (result + fraction) * sign;

    /* Exponent part */
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '-') { exp_sign = -1; p++; }
        else if (*p == '+') p++;

        while (*p >= '0' && *p <= '9') {
            exponent = exponent * 10 + (*p - '0');
            p++;
        }

        double exp_mult = 1.0;
        while (exponent-- > 0) {
            exp_mult *= (exp_sign > 0) ? 10.0 : 0.1;
        }
        result *= exp_mult;
    }

    if (endptr) *endptr = (char*)p;
    return result;
}

/* ============================================================================
 * SORTING AND SEARCHING
 * ============================================================================ */

void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*))
{
    /* Simple bubble sort for small arrays - replace with quicksort for production */
    uint8_t* arr = (uint8_t*)base;
    uint8_t* temp = (uint8_t*)kmalloc(size);
    if (!temp) return;

    for (size_t i = 0; i < nmemb - 1; i++) {
        for (size_t j = 0; j < nmemb - i - 1; j++) {
            void* a = arr + j * size;
            void* b = arr + (j + 1) * size;
            if (compar(a, b) > 0) {
                memcpy(temp, a, size);
                memcpy(a, b, size);
                memcpy(b, temp, size);
            }
        }
    }

    kfree(temp);
}

void* bsearch(const void* key, const void* base, size_t nmemb,
              size_t size, int (*compar)(const void*, const void*))
{
    const uint8_t* arr = (const uint8_t*)base;
    size_t low = 0, high = nmemb;

    while (low < high) {
        size_t mid = (low + high) / 2;
        const void* elem = arr + mid * size;
        int cmp = compar(key, elem);

        if (cmp < 0) high = mid;
        else if (cmp > 0) low = mid + 1;
        else return (void*)elem;
    }

    return NULL;
}

/* ============================================================================
 * INTEGER DIVISION
 * ============================================================================ */

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

div_t div(int numer, int denom)
{
    div_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

ldiv_t ldiv(long numer, long denom)
{
    ldiv_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

lldiv_t lldiv(long long numer, long long denom)
{
    lldiv_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

long long llabs(long long j)
{
    return (j < 0) ? -j : j;
}

/* ============================================================================
 * ENVIRONMENT - Stubs (not supported)
 * ============================================================================ */

char* getenv(const char* name)
{
    (void)name;
    return NULL;
}

int setenv(const char* name, const char* value, int overwrite)
{
    (void)name; (void)value; (void)overwrite;
    return -1;
}

int unsetenv(const char* name)
{
    (void)name;
    return -1;
}

int putenv(char* string)
{
    (void)string;
    return -1;
}

int system(const char* command)
{
    (void)command;
    return -1;
}

/* ============================================================================
 * MULTIBYTE - Stubs (not supported)
 * ============================================================================ */

int mblen(const char* s, size_t n)
{
    (void)s; (void)n;
    return -1;
}

int mbtowc(wchar_t* pwc, const char* s, size_t n)
{
    (void)pwc; (void)s; (void)n;
    return -1;
}

int wctomb(char* s, wchar_t wc)
{
    (void)s; (void)wc;
    return -1;
}

size_t mbstowcs(wchar_t* dest, const char* src, size_t n)
{
    (void)dest; (void)src; (void)n;
    return (size_t)-1;
}

size_t wcstombs(char* dest, const wchar_t* src, size_t n)
{
    (void)dest; (void)src; (void)n;
    return (size_t)-1;
}

/* ============================================================================
 * FILE OPERATIONS - Memory-backed implementation
 * ============================================================================ */

typedef struct _FILE {
    uint8_t* data;
    size_t   size;
    size_t   pos;
    int      eof;
    int      error;
    int      mode;
} FILE;

/* Standard streams - point to static buffer */
static FILE stdin_file = {NULL, 0, 0, 0, 0, 0};
static FILE stdout_file = {NULL, 0, 0, 0, 0, 0};
static FILE stderr_file = {NULL, 0, 0, 0, 0, 0};

FILE* stdin = &stdin_file;
FILE* stdout = &stdout_file;
FILE* stderr = &stderr_file;

FILE* fopen(const char* filename, const char* mode)
{
    /* No filesystem - can only open memory regions */
    (void)filename; (void)mode;
    return NULL;
}

int fclose(FILE* stream)
{
    if (stream && stream != stdin && stream != stdout && stream != stderr) {
        kfree(stream);
    }
    return 0;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream)
{
    if (!stream || !stream->data || stream->eof) return 0;

    size_t total = size * nmemb;
    size_t available = stream->size - stream->pos;

    if (total > available) {
        total = available;
        stream->eof = 1;
    }

    memcpy(ptr, stream->data + stream->pos, total);
    stream->pos += total;

    return total / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream)
{
    if (!stream || !stream->data) return 0;

    size_t total = size * nmemb;
    size_t available = stream->size - stream->pos;

    if (total > available) {
        total = available;
    }

    memcpy(stream->data + stream->pos, ptr, total);
    stream->pos += total;

    return total / size;
}

int fseek(FILE* stream, long offset, int whence)
{
    if (!stream) return -1;

    size_t new_pos;
    switch (whence) {
        case 0: /* SEEK_SET */ new_pos = offset; break;
        case 1: /* SEEK_CUR */ new_pos = stream->pos + offset; break;
        case 2: /* SEEK_END */ new_pos = stream->size + offset; break;
        default: return -1;
    }

    if (new_pos > stream->size) return -1;
    stream->pos = new_pos;
    stream->eof = 0;
    return 0;
}

long ftell(FILE* stream)
{
    return stream ? (long)stream->pos : -1;
}

void rewind(FILE* stream)
{
    if (stream) {
        stream->pos = 0;
        stream->eof = 0;
        stream->error = 0;
    }
}

int feof(FILE* stream)
{
    return stream ? stream->eof : 1;
}

int ferror(FILE* stream)
{
    return stream ? stream->error : 1;
}

void clearerr(FILE* stream)
{
    if (stream) {
        stream->eof = 0;
        stream->error = 0;
    }
}

int fflush(FILE* stream)
{
    (void)stream;
    return 0;
}

int fgetc(FILE* stream)
{
    if (!stream || !stream->data || stream->pos >= stream->size) {
        if (stream) stream->eof = 1;
        return -1; /* EOF */
    }
    return stream->data[stream->pos++];
}

int ungetc(int c, FILE* stream)
{
    if (!stream || stream->pos == 0 || c == -1) return -1;
    stream->pos--;
    stream->data[stream->pos] = (uint8_t)c;
    stream->eof = 0;
    return c;
}

char* fgets(char* s, int size, FILE* stream)
{
    if (!stream || !s || size <= 0) return NULL;

    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == -1) break;
        s[i++] = c;
        if (c == '\n') break;
    }

    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

/* File positioning */
int fgetpos(FILE* stream, long* pos)
{
    if (!stream || !pos) return -1;
    *pos = (long)stream->pos;
    return 0;
}

int fsetpos(FILE* stream, const long* pos)
{
    if (!stream || !pos) return -1;
    stream->pos = (size_t)*pos;
    return 0;
}

int remove(const char* filename)
{
    (void)filename;
    return -1;
}

int rename(const char* oldname, const char* newname)
{
    (void)oldname; (void)newname;
    return -1;
}

FILE* tmpfile(void)
{
    return NULL;
}

char* tmpnam(char* s)
{
    (void)s;
    return NULL;
}

void perror(const char* s)
{
    if (s && *s) {
        kprintf("%s: ", s);
    }
    kprintf("Error\n");
}

/* String output functions */
int puts(const char* s)
{
    if (!s) return -1;
    kprintf("%s\n", s);
    return 0;
}

int fputs(const char* s, FILE* stream)
{
    if (!s) return -1;
    if (!stream || stream == stderr || stream == stdout) {
        /* For standard streams, just print to console */
        kprintf("%s", s);
        return 0;
    }
    /* For file streams, write to buffer */
    size_t len = strlen(s);
    size_t written = fwrite(s, 1, len, stream);
    return written == len ? 0 : -1;
}

int putchar(int c)
{
    char buf[2] = {(char)c, '\0'};
    kprintf("%s", buf);
    return c;
}

int fputc(int c, FILE* stream)
{
    if (!stream || stream == stderr || stream == stdout) {
        return putchar(c);
    }
    unsigned char ch = (unsigned char)c;
    size_t written = fwrite(&ch, 1, 1, stream);
    return written == 1 ? c : -1;
}

int putc(int c, FILE* stream)
{
    return fputc(c, stream);
}

/* getline - stub */
ssize_t getline(char** lineptr, size_t* n, FILE* stream)
{
    (void)lineptr; (void)n; (void)stream;
    return -1;
}

/* ============================================================================
 * PRINTF FAMILY - Minimal implementations
 * ============================================================================ */

int printf(const char* format, ...)
{
    /* Simple passthrough to kprintf */
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    /* We can't easily pass varargs to kprintf, so just print the format string */
    kprintf("%s", format);
    __builtin_va_end(ap);
    return 0;
}

int fprintf(FILE* stream, const char* format, ...)
{
    (void)stream;
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    kprintf("%s", format);
    __builtin_va_end(ap);
    return 0;
}

/* Simple sprintf - handles %s, %d, %x, %c */
int vsnprintf(char* str, size_t size, const char* format, __builtin_va_list ap)
{
    if (!str || size == 0) return 0;

    char* out = str;
    char* end = str + size - 1;

    while (*format && out < end) {
        if (*format != '%') {
            *out++ = *format++;
            continue;
        }

        format++; /* Skip '%' */

        /* Width specifier (ignored for now) */
        while (*format >= '0' && *format <= '9') format++;

        /* Length modifiers */
        int is_long = 0;
        int is_longlong = 0;
        if (*format == 'l') {
            is_long = 1;
            format++;
            if (*format == 'l') {
                is_longlong = 1;
                format++;
            }
        } else if (*format == 'z') {
            is_long = 1;
            format++;
        }

        switch (*format) {
            case 's': {
                const char* s = __builtin_va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s && out < end) *out++ = *s++;
                break;
            }
            case 'd':
            case 'i': {
                long long val;
                if (is_longlong) val = __builtin_va_arg(ap, long long);
                else if (is_long) val = __builtin_va_arg(ap, long);
                else val = __builtin_va_arg(ap, int);

                if (val < 0) {
                    if (out < end) *out++ = '-';
                    val = -val;
                }

                char buf[21];
                int i = 0;
                do {
                    buf[i++] = '0' + (val % 10);
                    val /= 10;
                } while (val && i < 20);

                while (i > 0 && out < end) *out++ = buf[--i];
                break;
            }
            case 'u': {
                unsigned long long val;
                if (is_longlong) val = __builtin_va_arg(ap, unsigned long long);
                else if (is_long) val = __builtin_va_arg(ap, unsigned long);
                else val = __builtin_va_arg(ap, unsigned int);

                char buf[21];
                int i = 0;
                do {
                    buf[i++] = '0' + (val % 10);
                    val /= 10;
                } while (val && i < 20);

                while (i > 0 && out < end) *out++ = buf[--i];
                break;
            }
            case 'x':
            case 'X': {
                const char* digits = (*format == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
                unsigned long long val;
                if (is_longlong) val = __builtin_va_arg(ap, unsigned long long);
                else if (is_long) val = __builtin_va_arg(ap, unsigned long);
                else val = __builtin_va_arg(ap, unsigned int);

                char buf[17];
                int i = 0;
                do {
                    buf[i++] = digits[val & 0xF];
                    val >>= 4;
                } while (val && i < 16);

                while (i > 0 && out < end) *out++ = buf[--i];
                break;
            }
            case 'p': {
                void* ptr = __builtin_va_arg(ap, void*);
                unsigned long val = (unsigned long)ptr;
                if (out < end) *out++ = '0';
                if (out < end) *out++ = 'x';

                char buf[17];
                int i = 0;
                do {
                    buf[i++] = "0123456789abcdef"[val & 0xF];
                    val >>= 4;
                } while (val && i < 16);

                while (i > 0 && out < end) *out++ = buf[--i];
                break;
            }
            case 'c': {
                char c = (char)__builtin_va_arg(ap, int);
                if (out < end) *out++ = c;
                break;
            }
            case 'f':
            case 'g':
            case 'e': {
                double val = __builtin_va_arg(ap, double);
                /* Simple float formatting */
                if (val < 0) {
                    if (out < end) *out++ = '-';
                    val = -val;
                }
                long ipart = (long)val;
                double fpart = val - ipart;

                /* Integer part */
                char buf[21];
                int i = 0;
                do {
                    buf[i++] = '0' + (ipart % 10);
                    ipart /= 10;
                } while (ipart && i < 20);
                while (i > 0 && out < end) *out++ = buf[--i];

                /* Decimal point and fraction */
                if (out < end) *out++ = '.';
                for (int j = 0; j < 6 && out < end; j++) {
                    fpart *= 10;
                    int digit = (int)fpart;
                    *out++ = '0' + digit;
                    fpart -= digit;
                }
                break;
            }
            case '%':
                if (out < end) *out++ = '%';
                break;
            default:
                if (out < end) *out++ = *format;
                break;
        }
        format++;
    }

    *out = '\0';
    return out - str;
}

int snprintf(char* str, size_t size, const char* format, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    __builtin_va_end(ap);
    return ret;
}

int sprintf(char* str, const char* format, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    int ret = vsnprintf(str, 4096, format, ap); /* Assume large buffer */
    __builtin_va_end(ap);
    return ret;
}

int vprintf(const char* format, __builtin_va_list ap)
{
    char buf[1024];
    int ret = vsnprintf(buf, sizeof(buf), format, ap);
    kprintf("%s", buf);
    return ret;
}

int vfprintf(FILE* stream, const char* format, __builtin_va_list ap)
{
    (void)stream;
    return vprintf(format, ap);
}

int vsprintf(char* str, const char* format, __builtin_va_list ap)
{
    return vsnprintf(str, 4096, format, ap);
}

/* Minimal sscanf - only handles %d and %s */
int sscanf(const char* str, const char* format, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, format);

    int count = 0;
    const char* s = str;

    while (*format && *s) {
        if (*format == ' ') {
            while (*s == ' ' || *s == '\t') s++;
            format++;
            continue;
        }

        if (*format != '%') {
            if (*format != *s) break;
            format++;
            s++;
            continue;
        }

        format++; /* Skip '%' */

        /* Skip width */
        while (*format >= '0' && *format <= '9') format++;

        switch (*format) {
            case 'd': {
                int* ptr = __builtin_va_arg(ap, int*);
                int sign = 1;
                int val = 0;

                if (*s == '-') { sign = -1; s++; }
                else if (*s == '+') s++;

                while (*s >= '0' && *s <= '9') {
                    val = val * 10 + (*s - '0');
                    s++;
                }

                *ptr = val * sign;
                count++;
                break;
            }
            case 's': {
                char* ptr = __builtin_va_arg(ap, char*);
                while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
                    *ptr++ = *s++;
                }
                *ptr = '\0';
                count++;
                break;
            }
            default:
                break;
        }
        format++;
    }

    __builtin_va_end(ap);
    return count;
}

/* ============================================================================
 * ADDITIONAL STRING FUNCTIONS
 * ============================================================================ */

size_t strnlen(const char* s, size_t maxlen)
{
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

char* strncat(char* dest, const char* src, size_t n)
{
    char* d = dest;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

int strcasecmp(const char* s1, const char* s2)
{
    while (*s1 && *s2) {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncasecmp(const char* s1, const char* s2, size_t n)
{
    while (n-- && *s1 && *s2) {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char* strdup(const char* s)
{
    size_t len = strlen(s) + 1;
    char* new = (char*)kmalloc(len);
    if (new) memcpy(new, s, len);
    return new;
}

char* strndup(const char* s, size_t n)
{
    size_t len = strnlen(s, n);
    char* new = (char*)kmalloc(len + 1);
    if (new) {
        memcpy(new, s, len);
        new[len] = '\0';
    }
    return new;
}

/* Simple strtok implementation */
static char* strtok_last = NULL;

char* strtok(char* str, const char* delim)
{
    return strtok_r(str, delim, &strtok_last);
}

char* strtok_r(char* str, const char* delim, char** saveptr)
{
    if (!str) str = *saveptr;
    if (!str) return NULL;

    /* Skip leading delimiters */
    while (*str) {
        const char* d = delim;
        int is_delim = 0;
        while (*d) {
            if (*str == *d) { is_delim = 1; break; }
            d++;
        }
        if (!is_delim) break;
        str++;
    }

    if (!*str) {
        *saveptr = NULL;
        return NULL;
    }

    char* token = str;

    /* Find end of token */
    while (*str) {
        const char* d = delim;
        while (*d) {
            if (*str == *d) {
                *str = '\0';
                *saveptr = str + 1;
                return token;
            }
            d++;
        }
        str++;
    }

    *saveptr = NULL;
    return token;
}

size_t strspn(const char* s, const char* accept)
{
    size_t count = 0;
    while (*s) {
        const char* a = accept;
        int found = 0;
        while (*a) {
            if (*s == *a) { found = 1; break; }
            a++;
        }
        if (!found) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char* s, const char* reject)
{
    size_t count = 0;
    while (*s) {
        const char* r = reject;
        while (*r) {
            if (*s == *r) return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

char* strpbrk(const char* s, const char* accept)
{
    while (*s) {
        const char* a = accept;
        while (*a) {
            if (*s == *a) return (char*)s;
            a++;
        }
        s++;
    }
    return NULL;
}

void* memchr(const void* s, int c, size_t n)
{
    const uint8_t* p = (const uint8_t*)s;
    while (n--) {
        if (*p == (uint8_t)c) return (void*)p;
        p++;
    }
    return NULL;
}

char* strerror(int errnum)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "Error %d", errnum);
    return buf;
}

/* ============================================================================
 * TIME FUNCTIONS - Stubs using kernel tick counter
 * ============================================================================ */

/* Forward declaration from kernel */
extern uint64_t rdtsc(void);

typedef long time_t;
typedef long clock_t;

static time_t boot_time = 0;

time_t time(time_t* tloc)
{
    /* Return seconds since boot (not real time) */
    time_t t = boot_time + (time_t)(rdtsc() / 1000000000ULL);
    if (tloc) *tloc = t;
    return t;
}

clock_t clock(void)
{
    return (clock_t)(rdtsc() / 1000);
}

double difftime(time_t time1, time_t time0)
{
    return (double)(time1 - time0);
}

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

int clock_gettime(int clock_id, struct timespec* tp)
{
    (void)clock_id;
    if (!tp) return -1;

    uint64_t ticks = rdtsc();
    tp->tv_sec = (time_t)(ticks / 1000000000ULL);
    tp->tv_nsec = (long)(ticks % 1000000000ULL);
    return 0;
}

int clock_getres(int clock_id, struct timespec* res)
{
    (void)clock_id;
    if (!res) return -1;
    res->tv_sec = 0;
    res->tv_nsec = 1; /* 1 nanosecond resolution (idealized) */
    return 0;
}

uint64_t get_ticks(void)
{
    return rdtsc();
}

uint64_t get_ticks_per_second(void)
{
    return 1000000000ULL; /* Assume 1GHz for simplicity */
}

/* Architecture-specific CPU yield hint for busy wait */
#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __asm__ volatile("pause" ::: "memory")
#elif defined(__aarch64__)
#define cpu_relax() __asm__ volatile("yield" ::: "memory")
#else
#define cpu_relax() __asm__ volatile("" ::: "memory")
#endif

/* Sleep functions - busy wait */
int nanosleep(const struct timespec* req, struct timespec* rem)
{
    if (!req) return -1;

    uint64_t target = req->tv_sec * 1000000000ULL + req->tv_nsec;
    uint64_t start = rdtsc();

    while ((rdtsc() - start) < target) {
        /* Busy wait with CPU hint */
        cpu_relax();
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec req = { .tv_sec = seconds, .tv_nsec = 0 };
    nanosleep(&req, NULL);
    return 0;
}

int usleep(unsigned int usec)
{
    struct timespec req = { .tv_sec = usec / 1000000, .tv_nsec = (usec % 1000000) * 1000 };
    return nanosleep(&req, NULL);
}

/* Time conversion stubs */
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

static struct tm static_tm = {0, 0, 0, 1, 0, 70, 4, 0, 0}; /* Jan 1, 1970 */

struct tm* gmtime(const time_t* timep)
{
    (void)timep;
    return &static_tm;
}

struct tm* gmtime_r(const time_t* timep, struct tm* result)
{
    (void)timep;
    if (result) *result = static_tm;
    return result;
}

struct tm* localtime(const time_t* timep)
{
    return gmtime(timep);
}

struct tm* localtime_r(const time_t* timep, struct tm* result)
{
    return gmtime_r(timep, result);
}

time_t mktime(struct tm* tm)
{
    (void)tm;
    return 0;
}

char* asctime(const struct tm* tm)
{
    static char buf[26] = "Thu Jan  1 00:00:00 1970\n";
    (void)tm;
    return buf;
}

char* asctime_r(const struct tm* tm, char* buf)
{
    (void)tm;
    const char* s = "Thu Jan  1 00:00:00 1970\n";
    strcpy(buf, s);
    return buf;
}

char* ctime(const time_t* timep)
{
    return asctime(gmtime(timep));
}

char* ctime_r(const time_t* timep, char* buf)
{
    struct tm tm;
    return asctime_r(gmtime_r(timep, &tm), buf);
}

size_t strftime(char* s, size_t max, const char* format, const struct tm* tm)
{
    (void)tm; (void)format;
    if (max > 0) s[0] = '\0';
    return 0;
}

/* ============================================================================
 * MATH FUNCTIONS - Double versions (float versions in lib/math.c)
 * ============================================================================ */

double sqrt(double x)
{
    return (double)sqrtf((float)x);
}

double fabs(double x)
{
    return (x < 0.0) ? -x : x;
}

double floor(double x)
{
    long i = (long)x;
    return (x < 0.0 && x != (double)i) ? (double)(i - 1) : (double)i;
}

double ceil(double x)
{
    long i = (long)x;
    return (x > 0.0 && x != (double)i) ? (double)(i + 1) : (double)i;
}

double round(double x)
{
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

double trunc(double x)
{
    return (x >= 0.0) ? floor(x) : ceil(x);
}

double fmod(double x, double y)
{
    if (y == 0.0) return 0.0;
    return x - (long)(x / y) * y;
}

double remainder(double x, double y)
{
    return fmod(x, y);
}

double copysign(double x, double y)
{
    double ax = fabs(x);
    return (y < 0.0) ? -ax : ax;
}

double fmax(double x, double y)
{
    return (x > y) ? x : y;
}

double fmin(double x, double y)
{
    return (x < y) ? x : y;
}

double exp(double x)
{
    return (double)expf((float)x);
}

double exp2(double x)
{
    return exp(x * 0.693147180559945309);
}

double expm1(double x)
{
    return exp(x) - 1.0;
}

double log(double x)
{
    return (double)logf((float)x);
}

double log2(double x)
{
    return log(x) / 0.693147180559945309;
}

double log10(double x)
{
    return log(x) / 2.302585092994045684;
}

double log1p(double x)
{
    return log(1.0 + x);
}

double pow(double x, double y)
{
    return (double)powf((float)x, (float)y);
}

double sin(double x)
{
    return (double)sinf((float)x);
}

double cos(double x)
{
    return (double)cosf((float)x);
}

double tan(double x)
{
    return sin(x) / cos(x);
}

double tanh(double x)
{
    return (double)tanhf((float)x);
}

/* Inverse trig - simple approximations */
double asin(double x)
{
    if (x < -1.0 || x > 1.0) return 0.0;
    /* Newton iteration */
    double y = x;
    for (int i = 0; i < 10; i++) {
        y = y - (sin(y) - x) / cos(y);
    }
    return y;
}

double acos(double x)
{
    return 1.5707963267948966 - asin(x);
}

double atan(double x)
{
    /* Series expansion for small x, identity for large x */
    if (x > 1.0) return 1.5707963267948966 - atan(1.0 / x);
    if (x < -1.0) return -1.5707963267948966 - atan(1.0 / x);

    double result = 0.0;
    double term = x;
    double x2 = x * x;

    for (int i = 0; i < 20; i++) {
        result += term / (2 * i + 1);
        term *= -x2;
    }

    return result;
}

double atan2(double y, double x)
{
    if (x > 0) return atan(y / x);
    if (x < 0 && y >= 0) return atan(y / x) + 3.14159265358979323846;
    if (x < 0 && y < 0) return atan(y / x) - 3.14159265358979323846;
    if (x == 0 && y > 0) return 1.5707963267948966;
    if (x == 0 && y < 0) return -1.5707963267948966;
    return 0.0;
}

double sinh(double x)
{
    double ex = exp(x);
    return (ex - 1.0 / ex) / 2.0;
}

double cosh(double x)
{
    double ex = exp(x);
    return (ex + 1.0 / ex) / 2.0;
}

/* Float versions of inverse trig */
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x) { return (float)ceil((double)x); }
float roundf(float x) { return (float)round((double)x); }
float truncf(float x) { return (float)trunc((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
float remainderf(float x, float y) { return fmodf(x, y); }
float copysignf(float x, float y) { return (y < 0.0f) ? -fabsf(x) : fabsf(x); }
float fmaxf(float x, float y) { return (x > y) ? x : y; }
float fminf(float x, float y) { return (x < y) ? x : y; }
float exp2f(float x) { return expf(x * 0.693147180559945309f); }
float expm1f(float x) { return expf(x) - 1.0f; }
float log2f(float x) { return logf(x) / 0.693147180559945309f; }
float log10f(float x) { return logf(x) / 2.302585092994045684f; }
float log1pf(float x) { return logf(1.0f + x); }
float tanf(float x) { return sinf(x) / cosf(x); }
float asinf(float x) { return (float)asin((double)x); }
float acosf(float x) { return (float)acos((double)x); }
float atanf(float x) { return (float)atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float sinhf(float x) { return (float)sinh((double)x); }
float coshf(float x) { return (float)cosh((double)x); }

/* Hyperbolic inverse */
float asinhf(float x) { return logf(x + sqrtf(x * x + 1.0f)); }
float acoshf(float x) { return logf(x + sqrtf(x * x - 1.0f)); }
float atanhf(float x) { return 0.5f * logf((1.0f + x) / (1.0f - x)); }
double asinh(double x) { return log(x + sqrt(x * x + 1.0)); }
double acosh(double x) { return log(x + sqrt(x * x - 1.0)); }
double atanh(double x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }

/* Error functions - simple approximations */
float erff(float x) {
    /* Approximation */
    float a1 = 0.254829592f, a2 = -0.284496736f, a3 = 1.421413741f;
    float a4 = -1.453152027f, a5 = 1.061405429f, p = 0.3275911f;
    float sign = (x < 0) ? -1.0f : 1.0f;
    x = fabsf(x);
    float t = 1.0f / (1.0f + p * x);
    float y = 1.0f - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * expf(-x * x);
    return sign * y;
}

float erfcf(float x) { return 1.0f - erff(x); }
double erf(double x) { return (double)erff((float)x); }
double erfc(double x) { return 1.0 - erf(x); }

/* Gamma functions - very rough approximations */
float tgammaf(float x) {
    /* Stirling approximation for x > 0 */
    if (x <= 0) return 0.0f;
    return sqrtf(6.28318530718f / x) * powf(x / 2.71828182846f, x);
}

float lgammaf(float x) { return logf(tgammaf(x)); }
double tgamma(double x) { return (double)tgammaf((float)x); }
double lgamma(double x) { return log(tgamma(x)); }

/* FMA */
float fmaf(float x, float y, float z) { return x * y + z; }
double fma(double x, double y, double z) { return x * y + z; }

/* Decomposition functions */
float frexpf(float x, int* exp) {
    if (x == 0.0f) { *exp = 0; return 0.0f; }
    int e = 0;
    while (fabsf(x) >= 1.0f) { x *= 0.5f; e++; }
    while (fabsf(x) < 0.5f) { x *= 2.0f; e--; }
    *exp = e;
    return x;
}

float ldexpf(float x, int exp) {
    while (exp > 0) { x *= 2.0f; exp--; }
    while (exp < 0) { x *= 0.5f; exp++; }
    return x;
}

float modff(float x, float* iptr) {
    *iptr = truncf(x);
    return x - *iptr;
}

float scalbnf(float x, int n) { return ldexpf(x, n); }

double frexp(double x, int* exp) {
    if (x == 0.0) { *exp = 0; return 0.0; }
    int e = 0;
    while (fabs(x) >= 1.0) { x *= 0.5; e++; }
    while (fabs(x) < 0.5) { x *= 2.0; e--; }
    *exp = e;
    return x;
}

double ldexp(double x, int exp) {
    while (exp > 0) { x *= 2.0; exp--; }
    while (exp < 0) { x *= 0.5; exp++; }
    return x;
}

double modf(double x, double* iptr) {
    *iptr = trunc(x);
    return x - *iptr;
}

double scalbn(double x, int n) { return ldexp(x, n); }

/* Integer rounding */
long lroundf(float x) { return (long)roundf(x); }
long lround(double x) { return (long)round(x); }
long long llroundf(float x) { return (long long)roundf(x); }
long long llround(double x) { return (long long)round(x); }

/* ============================================================================
 * ERRNO - Simple implementation
 * ============================================================================ */

static int errno_value = 0;

int* __errno_location(void)
{
    return &errno_value;
}

/* ============================================================================
 * ASSERT - Map to kpanic
 * ============================================================================ */

void __assert_fail(const char* assertion, const char* file, unsigned int line, const char* function)
{
    kprintf("ASSERT FAILED: %s at %s:%u in %s\n", assertion, file, line, function);
    kpanic("Assertion failed");
}

/* ============================================================================
 * SIGNAL - Stubs (no signal handling in bare-metal)
 * ============================================================================ */

typedef void (*sighandler_t)(int);

sighandler_t signal(int signum, sighandler_t handler)
{
    (void)signum; (void)handler;
    return (sighandler_t)0; /* SIG_DFL */
}

int raise(int sig)
{
    (void)sig;
    return 0;
}

typedef unsigned long sigset_t;

struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact)
{
    (void)signum; (void)act;
    if (oldact) {
        oldact->sa_handler = (sighandler_t)0;
        oldact->sa_mask = 0;
        oldact->sa_flags = 0;
    }
    return 0;
}

int sigemptyset(sigset_t* set)
{
    if (set) *set = 0;
    return 0;
}

int sigfillset(sigset_t* set)
{
    if (set) *set = ~0UL;
    return 0;
}

int sigaddset(sigset_t* set, int signum)
{
    if (set) *set |= (1UL << signum);
    return 0;
}

int sigdelset(sigset_t* set, int signum)
{
    if (set) *set &= ~(1UL << signum);
    return 0;
}

int sigismember(const sigset_t* set, int signum)
{
    return set ? ((*set & (1UL << signum)) != 0) : 0;
}

/* ============================================================================
 * UNISTD - File and process stubs
 * ============================================================================ */

typedef int pid_t;
typedef long off_t;
typedef long ssize_t;

int access(const char* pathname, int mode)
{
    (void)pathname; (void)mode;
    return -1; /* File not found */
}

int close(int fd)
{
    (void)fd;
    return 0;
}

ssize_t read(int fd, void* buf, size_t count)
{
    (void)fd; (void)buf; (void)count;
    return -1;
}

ssize_t write(int fd, const void* buf, size_t count)
{
    (void)fd; (void)buf; (void)count;
    return -1;
}

off_t lseek(int fd, off_t offset, int whence)
{
    (void)fd; (void)offset; (void)whence;
    return -1;
}

int unlink(const char* pathname)
{
    (void)pathname;
    return -1;
}

int rmdir(const char* pathname)
{
    (void)pathname;
    return -1;
}

char* getcwd(char* buf, size_t size)
{
    if (buf && size > 1) {
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }
    return NULL;
}

int chdir(const char* path)
{
    (void)path;
    return -1;
}

pid_t getpid(void)
{
    return 1; /* Always PID 1 (init) */
}

int getuid(void)
{
    return 0; /* Root */
}

int getgid(void)
{
    return 0; /* Root */
}

long sysconf(int name)
{
    switch (name) {
        case 30: /* _SC_PAGESIZE */
            return 4096;
        default:
            return -1;
    }
}

int isatty(int fd)
{
    return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0;
}

/* ============================================================================
 * INTTYPES - Integer conversion functions
 * ============================================================================ */

long long strtoimax(const char* nptr, char** endptr, int base)
{
    return strtoll(nptr, endptr, base);
}

unsigned long long strtoumax(const char* nptr, char** endptr, int base)
{
    return strtoull(nptr, endptr, base);
}

/* ============================================================================
 * MISCELLANEOUS
 * ============================================================================ */

/* setjmp/longjmp - minimal implementation */
typedef long jmp_buf[8]; /* Placeholder */

int setjmp(jmp_buf env)
{
    (void)env;
    return 0;
}

void longjmp(jmp_buf env, int val)
{
    (void)env; (void)val;
    kpanic("longjmp called - not supported");
}

/* C++ ABI support */
void __cxa_pure_virtual(void)
{
    kpanic("Pure virtual function called");
}

void* __dso_handle = 0;

int __cxa_atexit(void (*func)(void*), void* arg, void* dso_handle)
{
    (void)func; (void)arg; (void)dso_handle;
    return 0;
}

/* Stack protector */
unsigned long __stack_chk_guard = 0xdeadbeefcafebabe;

void __stack_chk_fail(void)
{
    kpanic("Stack smashing detected");
}
