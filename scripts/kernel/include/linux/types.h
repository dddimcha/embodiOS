/* SPDX-License-Identifier: GPL-2.0 */
/**
 * Linux Compatibility Layer - Basic Types
 *
 * Provides Linux kernel type definitions mapped to EMBODIOS equivalents.
 * Reference: linux/include/linux/types.h
 *
 * Part of EMBODIOS Linux Driver Compatibility Shim (~50 APIs)
 */

#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H

#include <embodios/types.h>

/* ============================================================================
 * Fixed-width integer types (Linux style)
 * ============================================================================ */

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

/* Explicit endian types (treated as native for now) */
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;

/* ============================================================================
 * Common kernel types
 * ============================================================================ */

typedef unsigned int    uint;
typedef unsigned long   ulong;

/* Atomic types - simple implementation for single-core */
typedef struct {
    volatile int counter;
} atomic_t;

typedef struct {
    volatile s64 counter;
} atomic64_t;

/* Resource types */
typedef u64 phys_addr_t;
typedef u64 dma_addr_t;
typedef u64 resource_size_t;

/* File/device types */
typedef u32 dev_t;
typedef u32 mode_t;
typedef s32 pid_t;
typedef u32 uid_t;
typedef u32 gid_t;
typedef s64 loff_t;
typedef s64 off_t;
typedef s64 time64_t;

/* Sector/block types */
typedef u64 sector_t;
typedef u64 blkcnt_t;

/* Generic callback function pointer */
typedef void (*callback_t)(void *);

/* ============================================================================
 * Type attributes and qualifiers
 * ============================================================================ */

#ifndef __user
#define __user
#endif

#ifndef __kernel
#define __kernel
#endif

#ifndef __iomem
#define __iomem
#endif

#ifndef __percpu
#define __percpu
#endif

#ifndef __rcu
#define __rcu
#endif

#ifndef __force
#define __force
#endif

#ifndef __bitwise
#define __bitwise
#endif

/* ============================================================================
 * GFP flags (memory allocation flags)
 * ============================================================================ */

typedef unsigned int gfp_t;

#define GFP_KERNEL      0x00000001
#define GFP_ATOMIC      0x00000002
#define GFP_USER        0x00000004
#define GFP_DMA         0x00000008
#define GFP_ZERO        0x00000010
#define __GFP_ZERO      GFP_ZERO
#define __GFP_NOWARN    0x00000020
#define __GFP_NOFAIL    0x00000040

/* ============================================================================
 * Boolean constants
 * ============================================================================ */

#ifndef __cplusplus
/* bool, true, false already defined in embodios/types.h */
#endif

/* ============================================================================
 * Limits and sizes
 * ============================================================================ */

#define U8_MAX      ((u8)~0U)
#define S8_MAX      ((s8)(U8_MAX >> 1))
#define S8_MIN      ((s8)(-S8_MAX - 1))

#define U16_MAX     ((u16)~0U)
#define S16_MAX     ((s16)(U16_MAX >> 1))
#define S16_MIN     ((s16)(-S16_MAX - 1))

#define U32_MAX     ((u32)~0U)
#define S32_MAX     ((s32)(U32_MAX >> 1))
#define S32_MIN     ((s32)(-S32_MAX - 1))

#define U64_MAX     ((u64)~0ULL)
#define S64_MAX     ((s64)(U64_MAX >> 1))
#define S64_MIN     ((s64)(-S64_MAX - 1))

#define UINT_MAX    ((unsigned int)~0U)
#define INT_MAX     ((int)(UINT_MAX >> 1))
#define INT_MIN     (-INT_MAX - 1)

#define ULONG_MAX   ((unsigned long)~0UL)
#define LONG_MAX    ((long)(ULONG_MAX >> 1))
#define LONG_MIN    (-LONG_MAX - 1)

/* ============================================================================
 * Container and offset macros
 * ============================================================================ */

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t)&((TYPE *)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({                  \
    const typeof(((type *)0)->member) *__mptr = (ptr);      \
    (type *)((char *)__mptr - offsetof(type, member)); })
#endif

/* ============================================================================
 * Alignment macros
 * ============================================================================ */

#define ALIGN(x, a)             __ALIGN_MASK(x, (typeof(x))(a) - 1)
#define __ALIGN_MASK(x, mask)   (((x) + (mask)) & ~(mask))
#define ALIGN_DOWN(x, a)        ((x) & ~((typeof(x))(a) - 1))
#define IS_ALIGNED(x, a)        (((x) & ((typeof(x))(a) - 1)) == 0)

/* Page alignment */
#ifndef PAGE_SIZE
#define PAGE_SIZE       4096
#endif
#ifndef PAGE_SHIFT
#define PAGE_SHIFT      12
#endif
#ifndef PAGE_MASK
#define PAGE_MASK       (~(PAGE_SIZE - 1))
#endif
#ifndef PAGE_ALIGN
#define PAGE_ALIGN(addr)    ALIGN(addr, PAGE_SIZE)
#endif

/* ============================================================================
 * Bit manipulation
 * ============================================================================ */

#define BIT(nr)                 (1UL << (nr))
#define BIT_ULL(nr)             (1ULL << (nr))
#define BIT_MASK(nr)            (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)            ((nr) / BITS_PER_LONG)
#define BITS_PER_BYTE           8
#define BITS_PER_LONG           (sizeof(long) * BITS_PER_BYTE)
#define BITS_PER_LONG_LONG      64

#define GENMASK(h, l) \
    (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

#define GENMASK_ULL(h, l) \
    (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))

/* ============================================================================
 * Array utilities
 * ============================================================================ */

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

/* ============================================================================
 * Min/Max macros
 * ============================================================================ */

#define min(x, y) ({                \
    typeof(x) _min1 = (x);          \
    typeof(y) _min2 = (y);          \
    (void) (&_min1 == &_min2);      \
    _min1 < _min2 ? _min1 : _min2; })

#define max(x, y) ({                \
    typeof(x) _max1 = (x);          \
    typeof(y) _max2 = (y);          \
    (void) (&_max1 == &_max2);      \
    _max1 > _max2 ? _max1 : _max2; })

#define min3(x, y, z) min((typeof(x))min(x, y), z)
#define max3(x, y, z) max((typeof(x))max(x, y), z)

#define clamp(val, lo, hi) min((typeof(val))max(val, lo), hi)

/* Type-safe min/max */
#define min_t(type, x, y) ({        \
    type __min1 = (x);              \
    type __min2 = (y);              \
    __min1 < __min2 ? __min1 : __min2; })

#define max_t(type, x, y) ({        \
    type __max1 = (x);              \
    type __max2 = (y);              \
    __max1 > __max2 ? __max1 : __max2; })

/* ============================================================================
 * Swap macro
 * ============================================================================ */

#define swap(a, b) \
    do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

/* ============================================================================
 * Compiler hints
 * ============================================================================ */

#ifndef likely
#define likely(x)       __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x)     __builtin_expect(!!(x), 0)
#endif

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

#ifndef noinline
#define noinline        __attribute__((noinline))
#endif

#ifndef __must_check
#define __must_check    __attribute__((warn_unused_result))
#endif

#ifndef __deprecated
#define __deprecated    __attribute__((deprecated))
#endif

/* ============================================================================
 * Error pointer helpers
 * ============================================================================ */

#define MAX_ERRNO       4095

#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error)
{
    return (void *)error;
}

static inline long PTR_ERR(const void *ptr)
{
    return (long)ptr;
}

static inline bool IS_ERR(const void *ptr)
{
    return IS_ERR_VALUE((unsigned long)ptr);
}

static inline bool IS_ERR_OR_NULL(const void *ptr)
{
    return unlikely(!ptr) || IS_ERR_VALUE((unsigned long)ptr);
}

#endif /* _LINUX_TYPES_H */
