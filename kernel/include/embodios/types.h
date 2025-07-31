#ifndef EMBODIOS_TYPES_H
#define EMBODIOS_TYPES_H

/* Basic types */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

typedef unsigned long       size_t;
typedef long                ssize_t;
typedef unsigned long       uintptr_t;
typedef long                intptr_t;

/* Boolean type */
typedef _Bool               bool;
#define true                1
#define false               0

/* NULL pointer */
#define NULL                ((void*)0)

/* Attributes */
#define __packed            __attribute__((packed))
#define __aligned(x)        __attribute__((aligned(x)))
#define __noreturn          __attribute__((noreturn))
#define __unused            __attribute__((unused))

/* Static assertions */
#define STATIC_ASSERT(cond) _Static_assert(cond, #cond)

#endif /* EMBODIOS_TYPES_H */