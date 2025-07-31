/* EMBODIOS String Operations */
#include <embodios/mm.h>
#include <embodios/types.h>

void* memcpy(void* dest, const void* src, size_t n)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    
    /* Optimize for aligned copies */
    if (((uintptr_t)d & 7) == 0 && ((uintptr_t)s & 7) == 0 && (n & 7) == 0) {
        uint64_t* d64 = (uint64_t*)d;
        const uint64_t* s64 = (const uint64_t*)s;
        size_t n64 = n >> 3;
        
        while (n64--) {
            *d64++ = *s64++;
        }
    } else {
        while (n--) {
            *d++ = *s++;
        }
    }
    
    return dest;
}

void* memset(void* s, int c, size_t n)
{
    uint8_t* p = (uint8_t*)s;
    uint8_t val = (uint8_t)c;
    
    /* Optimize for aligned fills */
    if (((uintptr_t)p & 7) == 0 && (n & 7) == 0) {
        uint64_t val64 = val;
        val64 |= val64 << 8;
        val64 |= val64 << 16;
        val64 |= val64 << 32;
        
        uint64_t* p64 = (uint64_t*)p;
        size_t n64 = n >> 3;
        
        while (n64--) {
            *p64++ = val64;
        }
    } else {
        while (n--) {
            *p++ = val;
        }
    }
    
    return s;
}

void* memmove(void* dest, const void* src, size_t n)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    
    if (d < s) {
        /* Forward copy */
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        /* Backward copy */
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n)
{
    const uint8_t* p1 = (const uint8_t*)s1;
    const uint8_t* p2 = (const uint8_t*)s2;
    
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    
    return 0;
}

size_t strlen(const char* s)
{
    const char* p = s;
    while (*p) p++;
    return p - s;
}

char* strcpy(char* dest, const char* src)
{
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n)
{
    char* d = dest;
    while (n-- && (*d++ = *src++));
    while (n-- > 0) *d++ = '\0';
    return dest;
}

int strcmp(const char* s1, const char* s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n)
{
    while (n-- && *s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    if (n == (size_t)-1) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char* strcat(char* dest, const char* src)
{
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char* strchr(const char* s, int c)
{
    while (*s) {
        if (*s == c) return (char*)s;
        s++;
    }
    return (c == 0) ? (char*)s : NULL;
}

char* strrchr(const char* s, int c)
{
    const char* last = NULL;
    while (*s) {
        if (*s == c) last = s;
        s++;
    }
    if (c == 0) return (char*)s;
    return (char*)last;
}