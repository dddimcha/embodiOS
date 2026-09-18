/* EMBODIOS Standard Library Functions */
#include <embodios/types.h>

/* Simple number to string conversion */
int itoa(int value, char* str, int base)
{
    static const char digits[] = "0123456789ABCDEF";
    char buffer[32];
    int i = 0;
    int negative = 0;
    
    if (value < 0 && base == 10) {
        negative = 1;
        value = -value;
    }
    
    if (value == 0) {
        str[0] = '0';
        str[1] = '\0';
        return 1;
    }
    
    while (value > 0) {
        buffer[i++] = digits[value % base];
        value /= base;
    }
    
    int j = 0;
    if (negative) {
        str[j++] = '-';
    }
    
    while (i > 0) {
        str[j++] = buffer[--i];
    }
    
    str[j] = '\0';
    return j;
}

/* String to integer conversion */
int atoi(const char* str)
{
    int result = 0;
    int sign = 1;
    
    /* Skip whitespace */
    while (*str == ' ' || *str == '\t' || *str == '\n') {
        str++;
    }
    
    /* Handle sign */
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    /* Convert digits */
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

/* Unsigned long to string conversion */
int ultoa(unsigned long value, char* str, int base)
{
    static const char digits[] = "0123456789ABCDEF";
    char buffer[64];
    int i = 0;
    
    if (value == 0) {
        str[0] = '0';
        str[1] = '\0';
        return 1;
    }
    
    while (value > 0) {
        buffer[i++] = digits[value % base];
        value /= base;
    }
    
    int j = 0;
    while (i > 0) {
        str[j++] = buffer[--i];
    }
    
    str[j] = '\0';
    return j;
}

/* Simple random number generator (LCG) */
static unsigned long rand_seed = 1;

void srand(unsigned int seed)
{
    rand_seed = seed;
}

int rand(void)
{
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed / 65536) % 32768;
}

/* Absolute value */
int abs(int n)
{
    return (n < 0) ? -n : n;
}

long labs(long n)
{
    return (n < 0) ? -n : n;
}

/* Min/Max functions */
int min(int a, int b)
{
    return (a < b) ? a : b;
}

int max(int a, int b)
{
    return (a > b) ? a : b;
}

unsigned int umin(unsigned int a, unsigned int b)
{
    return (a < b) ? a : b;
}

unsigned int umax(unsigned int a, unsigned int b)
{
    return (a > b) ? a : b;
}