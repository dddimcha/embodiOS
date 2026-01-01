/* SPDX-License-Identifier: GPL-2.0 */
/**
 * EMBODIOS Linux Compatibility Layer - Master Header
 *
 * This header provides a thin Linux kernel API compatibility shim (~50 APIs)
 * that allows unmodified Linux drivers to compile and run on EMBODIOS.
 *
 * Reference implementations: OSv, Unikraft compatibility layers
 *
 * ============================================================================
 * API COMPATIBILITY MATRIX
 * ============================================================================
 *
 * Legend:
 *   [FULL]    - Full compatibility with Linux behavior
 *   [PARTIAL] - Partial compatibility, some features missing
 *   [STUB]    - API exists but is no-op or simplified
 *   [N/A]     - Not applicable to EMBODIOS architecture
 *
 * ============================================================================
 * 1. BASIC TYPES (linux/types.h) - 20+ definitions
 * ============================================================================
 *
 * | Linux Type      | EMBODIOS Mapping       | Status    | Notes
 * |-----------------|------------------------|-----------|------------------
 * | u8/u16/u32/u64  | uint8_t etc.           | [FULL]    | Direct typedef
 * | s8/s16/s32/s64  | int8_t etc.            | [FULL]    | Direct typedef
 * | __le16/__be16   | u16                    | [PARTIAL] | No endian swap
 * | __le32/__be32   | u32                    | [PARTIAL] | No endian swap
 * | __le64/__be64   | u64                    | [PARTIAL] | No endian swap
 * | atomic_t        | volatile int           | [PARTIAL] | Non-atomic ops
 * | gfp_t           | unsigned int           | [PARTIAL] | Flags ignored
 * | phys_addr_t     | u64                    | [FULL]    | Direct typedef
 * | dma_addr_t      | u64                    | [FULL]    | Direct typedef
 * | size_t/ssize_t  | unsigned long/long     | [FULL]    | Direct typedef
 * | bool            | _Bool                  | [FULL]    | C99 bool
 * | ERR_PTR/PTR_ERR | Inline functions       | [FULL]    | Error pointers
 * | container_of    | Macro                  | [FULL]    | Standard impl
 * | ARRAY_SIZE      | Macro                  | [FULL]    | Standard impl
 * | min/max/clamp   | Type-safe macros       | [FULL]    | GCC extensions
 * | likely/unlikely | __builtin_expect       | [FULL]    | GCC builtin
 * | BIT/GENMASK     | Bit manipulation       | [FULL]    | Standard impl
 *
 * ============================================================================
 * 2. MEMORY ALLOCATION (linux/slab.h) - 15+ APIs
 * ============================================================================
 *
 * | Linux API          | EMBODIOS Mapping    | Status    | Notes
 * |--------------------|---------------------|-----------|------------------
 * | kmalloc(size,gfp)  | kmalloc(size)       | [PARTIAL] | GFP flags ignored
 * | kzalloc(size,gfp)  | kzalloc(size)       | [PARTIAL] | GFP flags ignored
 * | kfree(ptr)         | kfree(ptr)          | [FULL]    | Direct mapping
 * | krealloc(p,sz,gfp) | krealloc(p,sz)      | [PARTIAL] | GFP flags ignored
 * | kcalloc(n,sz,gfp)  | kzalloc(n*sz)       | [PARTIAL] | Overflow check
 * | kmalloc_array      | kmalloc(n*sz)       | [PARTIAL] | Overflow check
 * | kstrdup(s,gfp)     | Inline function     | [FULL]    | Manual strlen/copy
 * | kstrndup(s,n,gfp)  | Inline function     | [FULL]    | Length-limited
 * | kmemdup(s,n,gfp)   | Inline function     | [FULL]    | memcpy wrapper
 * | kmem_cache_create  | Wrapper struct      | [PARTIAL] | Uses kmalloc
 * | kmem_cache_alloc   | kmalloc(cache->sz)  | [PARTIAL] | No real caching
 * | kmem_cache_free    | kfree(ptr)          | [PARTIAL] | Cache ignored
 * | kmem_cache_destroy | kfree(cache)        | [PARTIAL] | Simple free
 * | kvmalloc/kvfree    | kmalloc/kfree       | [PARTIAL] | No vmalloc
 * | ksize(ptr)         | Returns 0           | [STUB]    | Size not tracked
 *
 * ============================================================================
 * 3. PRINTING (linux/printk.h) - 20+ APIs
 * ============================================================================
 *
 * | Linux API       | EMBODIOS Mapping       | Status    | Notes
 * |-----------------|------------------------|-----------|------------------
 * | printk(fmt,...) | console_printf         | [PARTIAL] | Log level stripped
 * | pr_emerg()      | console_printf         | [FULL]    | [EMERG] prefix
 * | pr_alert()      | console_printf         | [FULL]    | [ALERT] prefix
 * | pr_crit()       | console_printf         | [FULL]    | [CRIT] prefix
 * | pr_err()        | console_printf         | [FULL]    | [ERROR] prefix
 * | pr_warn()       | console_printf         | [FULL]    | [WARN] prefix
 * | pr_notice()     | console_printf         | [FULL]    | [NOTICE] prefix
 * | pr_info()       | console_printf         | [FULL]    | [INFO] prefix
 * | pr_debug()      | console_printf/no-op   | [PARTIAL] | Compiled out
 * | pr_cont()       | console_printf         | [FULL]    | No prefix
 * | dev_*()         | pr_*() with prefix     | [PARTIAL] | Device info N/A
 * | *_ratelimited() | Same as non-limited    | [STUB]    | No rate limiting
 * | *_once()        | Static flag check      | [FULL]    | One-time print
 * | print_hex_dump  | Inline function        | [FULL]    | Manual hex dump
 * | dump_stack()    | Console message        | [STUB]    | No stack trace
 * | WARN/WARN_ON    | pr_warn + return       | [PARTIAL] | No stack trace
 *
 * ============================================================================
 * 4. SPINLOCKS (linux/spinlock.h) - 15+ APIs
 * ============================================================================
 *
 * | Linux API            | EMBODIOS Mapping    | Status    | Notes
 * |----------------------|---------------------|-----------|------------------
 * | spin_lock_init()     | lock = 0            | [FULL]    | Simple init
 * | spin_lock()          | IRQ disable         | [PARTIAL] | UP implementation
 * | spin_unlock()        | IRQ enable          | [PARTIAL] | UP implementation
 * | spin_trylock()       | Check & disable IRQ | [PARTIAL] | UP implementation
 * | spin_lock_irqsave()  | Save flags, disable | [FULL]    | Proper nesting
 * | spin_unlock_irqrestore() | Restore flags  | [FULL]    | Proper nesting
 * | spin_lock_irq()      | IRQ disable         | [FULL]    | Direct mapping
 * | spin_unlock_irq()    | IRQ enable          | [FULL]    | Direct mapping
 * | spin_lock_bh()       | Same as spin_lock   | [PARTIAL] | No softirq
 * | spin_is_locked()     | Check flag          | [FULL]    | Simple check
 * | read_lock/unlock     | Same as spin_lock   | [PARTIAL] | No RW semantics
 * | write_lock/unlock    | Same as spin_lock   | [PARTIAL] | No RW semantics
 * | local_irq_save()     | Architecture IRQ    | [FULL]    | x86/ARM64
 * | local_irq_restore()  | Architecture IRQ    | [FULL]    | x86/ARM64
 * | mb/rmb/wmb           | Architecture fence  | [FULL]    | x86/ARM64
 *
 * ============================================================================
 * 5. MUTEXES (linux/mutex.h) - 15+ APIs
 * ============================================================================
 *
 * | Linux API                | EMBODIOS Mapping  | Status    | Notes
 * |--------------------------|-------------------|-----------|------------------
 * | mutex_init()             | lock = 0          | [FULL]    | Simple init
 * | mutex_destroy()          | No-op             | [STUB]    | No cleanup needed
 * | mutex_lock()             | Set flag          | [PARTIAL] | No blocking
 * | mutex_unlock()           | Clear flag        | [FULL]    | Direct mapping
 * | mutex_trylock()          | Check & set       | [FULL]    | Non-blocking
 * | mutex_lock_interruptible | mutex_lock        | [PARTIAL] | No signals
 * | mutex_is_locked()        | Check flag        | [FULL]    | Simple check
 * | down/up (semaphore)      | Counter ops       | [PARTIAL] | No blocking
 * | init_completion()        | done = 0          | [FULL]    | Simple init
 * | complete()               | done = 1          | [FULL]    | Direct mapping
 * | complete_all()           | done = UINT_MAX   | [FULL]    | Direct mapping
 * | wait_for_completion()    | Spin on flag      | [PARTIAL] | Busy wait
 * | init_waitqueue_head()    | Simple init       | [STUB]    | No wait queue
 * | wake_up*()               | No-op             | [STUB]    | No scheduling
 * | wait_event*()            | Busy poll         | [PARTIAL] | No blocking
 * | rcu_read_lock/unlock     | No-op             | [STUB]    | No RCU
 *
 * ============================================================================
 * USAGE EXAMPLE
 * ============================================================================
 *
 * To use Linux drivers with EMBODIOS, include this header:
 *
 *     #include <linux/compat.h>
 *
 * Or include individual headers as needed:
 *
 *     #include <linux/types.h>
 *     #include <linux/slab.h>
 *     #include <linux/printk.h>
 *     #include <linux/spinlock.h>
 *     #include <linux/mutex.h>
 *
 * ============================================================================
 * LIMITATIONS
 * ============================================================================
 *
 * 1. Single-Core Only
 *    - All locking primitives assume uniprocessor (UP) system
 *    - Spinlocks use interrupt disable instead of actual spinning
 *    - No SMP memory ordering guarantees
 *
 * 2. Non-Preemptive
 *    - Mutexes don't actually block/sleep
 *    - Wait queues are implemented as busy-wait polls
 *    - No scheduler integration
 *
 * 3. No GFP Flags
 *    - Memory allocation flags (GFP_KERNEL, GFP_ATOMIC, etc.) are ignored
 *    - All allocations use the same allocator
 *
 * 4. No Device Model
 *    - dev_*() functions don't include real device info
 *    - No sysfs, procfs, or device tree integration
 *
 * 5. No Interrupt Model
 *    - Bottom halves (softirq, tasklets) not supported
 *    - No workqueue support
 *
 * ============================================================================
 * EXTENDING THE COMPATIBILITY LAYER
 * ============================================================================
 *
 * To add new Linux APIs:
 *
 * 1. Create include/linux/<header>.h
 * 2. Map Linux functions to EMBODIOS equivalents
 * 3. Document in this file's compatibility matrix
 * 4. Add stubs for unsupported features
 *
 * Priority headers to add:
 * - linux/delay.h (mdelay, udelay, msleep)
 * - linux/io.h (ioread*, iowrite*, ioremap)
 * - linux/interrupt.h (request_irq, free_irq)
 * - linux/dma-mapping.h (dma_alloc_coherent, dma_map_single)
 * - linux/device.h (struct device, driver registration)
 * - linux/platform_device.h (platform driver support)
 * - linux/of.h (device tree support)
 *
 * ============================================================================
 */

#ifndef _LINUX_COMPAT_H
#define _LINUX_COMPAT_H

/* Include all compatibility headers */
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

/* Version info */
#define LINUX_COMPAT_VERSION_MAJOR  1
#define LINUX_COMPAT_VERSION_MINOR  0
#define LINUX_COMPAT_VERSION_PATCH  0
#define LINUX_COMPAT_VERSION_STRING "1.0.0"

/* API count */
#define LINUX_COMPAT_API_COUNT      50

/* Feature flags */
#define LINUX_COMPAT_HAS_TYPES      1
#define LINUX_COMPAT_HAS_SLAB       1
#define LINUX_COMPAT_HAS_PRINTK     1
#define LINUX_COMPAT_HAS_SPINLOCK   1
#define LINUX_COMPAT_HAS_MUTEX      1
#define LINUX_COMPAT_HAS_COMPLETION 1
#define LINUX_COMPAT_HAS_WAITQUEUE  1  /* Simplified */

/* Unsupported features */
#define LINUX_COMPAT_HAS_WORKQUEUE  0
#define LINUX_COMPAT_HAS_TASKLET    0
#define LINUX_COMPAT_HAS_TIMER      0
#define LINUX_COMPAT_HAS_DMA        0
#define LINUX_COMPAT_HAS_DEVICE     0
#define LINUX_COMPAT_HAS_SYSFS      0
#define LINUX_COMPAT_HAS_PROCFS     0

/* ============================================================================
 * Additional common definitions
 * ============================================================================ */

/* Jiffies (time counter) - stub */
#define jiffies         0UL
#define HZ              100
#define msecs_to_jiffies(m) ((m) * HZ / 1000)
#define jiffies_to_msecs(j) ((j) * 1000 / HZ)

/* Module macros (no-op for bare metal) */
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_ALIAS(x)
#define MODULE_DEVICE_TABLE(type, name)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define module_init(fn)     /* Handled by kernel startup */
#define module_exit(fn)     /* No module unloading */
#define module_param(name, type, perm)
#define module_param_named(name, value, type, perm)

/* Kernel version checks (always "compatible") */
#define LINUX_VERSION_CODE  0x060000  /* Pretend 6.0 */
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))

/* Build assertions */
#define BUILD_BUG_ON(condition) _Static_assert(!(condition), "BUILD_BUG_ON")
#define BUILD_BUG_ON_ZERO(e)    ((int)(sizeof(struct { int:-!!(e); })))
#define BUILD_BUG_ON_NULL(e)    ((void *)sizeof(struct { int:-!!(e); }))

/* Unreachable code marker */
#define unreachable()   __builtin_unreachable()

#endif /* _LINUX_COMPAT_H */
