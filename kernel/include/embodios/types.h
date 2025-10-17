#ifndef EMBODIOS_TYPES_H
#define EMBODIOS_TYPES_H

/* Basic types */
#ifndef __INT8_TYPE__
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
#else
/* Use compiler-provided types if available (for NEON compatibility) */
#include <stdint.h>
#endif

typedef unsigned long       size_t;
typedef long                ssize_t;
typedef unsigned long       uintptr_t;
typedef long                intptr_t;

/* Boolean type (C only - C++ has built-in bool) */
#ifndef __cplusplus
#if __STDC_VERSION__ < 202311L
/* C versions before C23 need bool typedef */
typedef _Bool               bool;
#define true                1
#define false               0
#else
/* C23 and later have bool as a keyword */
#include <stdbool.h>
#endif
#endif

/* NULL pointer (C only - C++ has nullptr) */
#ifndef __cplusplus
#define NULL                ((void*)0)
#endif

/* Attributes */
#define __packed            __attribute__((packed))
#define __aligned(x)        __attribute__((aligned(x)))
#define __noreturn          __attribute__((noreturn))
#define __unused            __attribute__((unused))

/* Fixed-point arithmetic type */
typedef int32_t fixed_t;

/* Static assertions */
#define STATIC_ASSERT(cond) _Static_assert(cond, #cond)

#endif /* EMBODIOS_TYPES_H */