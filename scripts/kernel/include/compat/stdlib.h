/* Kernel stub for stdlib.h
 * Maps standard allocation to kernel functions
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_STDLIB_H
#define _COMPAT_STDLIB_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory allocation - mapped to kmalloc/kfree */
void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);

/* Aligned allocation */
void* aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void** memptr, size_t alignment, size_t size);

/* Program termination */
void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
int atexit(void (*func)(void));

/* String conversion */
int atoi(const char* nptr);
long atol(const char* nptr);
long long atoll(const char* nptr);
double atof(const char* nptr);
long strtol(const char* nptr, char** endptr, int base);
unsigned long strtoul(const char* nptr, char** endptr, int base);
long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
float strtof(const char* nptr, char** endptr);
double strtod(const char* nptr, char** endptr);

/* Random numbers */
#define RAND_MAX 0x7FFFFFFF
int rand(void);
void srand(unsigned int seed);

/* Environment - not supported */
char* getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int putenv(char* string);

/* Sorting and searching */
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb,
              size_t size, int (*compar)(const void*, const void*));

/* Integer arithmetic */
int abs(int j);
long labs(long j);
long long llabs(long long j);

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

/* Multibyte/wide char - stubs */
#define MB_CUR_MAX 4
int mblen(const char* s, size_t n);
int mbtowc(wchar_t* pwc, const char* s, size_t n);
int wctomb(char* s, wchar_t wc);
size_t mbstowcs(wchar_t* dest, const char* src, size_t n);
size_t wcstombs(char* dest, const wchar_t* src, size_t n);

/* System - not supported */
int system(const char* command);

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_STDLIB_H */
