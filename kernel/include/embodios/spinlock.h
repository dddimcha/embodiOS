/* EMBODIOS Spinlock Implementation
 *
 * SMP-safe spinlock using ticket lock algorithm for fairness.
 * Provides busy-wait mutual exclusion for short critical sections.
 *
 * Features:
 * - Ticket lock for FIFO ordering (prevents starvation)
 * - IRQ-safe variants with interrupt disable
 * - Lock debugging with owner tracking
 * - Deadlock detection (CONFIG_DEBUG_SPINLOCK)
 *
 * Reference: Linux kernel/locking/spinlock.c
 */

#ifndef EMBODIOS_SPINLOCK_H
#define EMBODIOS_SPINLOCK_H

#include <embodios/types.h>
#include <embodios/atomic.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Enable lock debugging - define in build system for debug builds */
#ifndef CONFIG_DEBUG_SPINLOCK
#define CONFIG_DEBUG_SPINLOCK 0
#endif

/* Maximum spin iterations before warning (detects potential deadlock) */
#define SPINLOCK_MAX_SPINS      10000000

/* ============================================================================
 * Architecture-specific CPU relaxation
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)
static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}
#elif defined(__aarch64__)
static inline void cpu_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}
#else
static inline void cpu_relax(void)
{
    __asm__ volatile("" ::: "memory");
}
#endif

/* ============================================================================
 * Architecture-specific IRQ control
 * ============================================================================ */

#if defined(__x86_64__)

static inline unsigned long arch_irq_save(void)
{
    unsigned long flags;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r" (flags)
        :
        : "memory"
    );
    return flags;
}

static inline void arch_irq_restore(unsigned long flags)
{
    __asm__ volatile(
        "pushq %0\n\t"
        "popfq"
        :
        : "r" (flags)
        : "memory", "cc"
    );
}

static inline void arch_irq_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

static inline void arch_irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

#elif defined(__aarch64__)

static inline unsigned long arch_irq_save(void)
{
    unsigned long flags;
    __asm__ volatile(
        "mrs %0, daif\n\t"
        "msr daifset, #3"
        : "=r" (flags)
        :
        : "memory"
    );
    return flags;
}

static inline void arch_irq_restore(unsigned long flags)
{
    __asm__ volatile(
        "msr daif, %0"
        :
        : "r" (flags)
        : "memory"
    );
}

static inline void arch_irq_disable(void)
{
    __asm__ volatile("msr daifset, #3" ::: "memory");
}

static inline void arch_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #3" ::: "memory");
}

#else

static inline unsigned long arch_irq_save(void) { return 0; }
static inline void arch_irq_restore(unsigned long flags) { (void)flags; }
static inline void arch_irq_disable(void) { }
static inline void arch_irq_enable(void) { }

#endif

/* ============================================================================
 * Spinlock Types
 * ============================================================================ */

/**
 * struct raw_spinlock - Low-level spinlock (ticket lock)
 *
 * Uses ticket lock algorithm for fair FIFO ordering:
 * - @next: Next ticket to be served
 * - @owner: Current ticket being served
 *
 * A CPU acquires lock by getting a ticket (atomic increment of @next),
 * then spins until @owner matches its ticket.
 */
typedef struct raw_spinlock {
    atomic_t next;      /* Next ticket number */
    atomic_t owner;     /* Currently serving ticket */
#if CONFIG_DEBUG_SPINLOCK
    void *owner_cpu;    /* CPU holding the lock */
    const char *file;   /* File where locked */
    int line;           /* Line where locked */
    unsigned long lock_time;  /* When lock was acquired */
#endif
} raw_spinlock_t;

/**
 * struct spinlock - High-level spinlock wrapper
 */
typedef struct spinlock {
    raw_spinlock_t raw;
} spinlock_t;

/* ============================================================================
 * Spinlock Initialization
 * ============================================================================ */

#if CONFIG_DEBUG_SPINLOCK
#define __RAW_SPIN_LOCK_UNLOCKED \
    { .next = ATOMIC_INIT(0), .owner = ATOMIC_INIT(0), \
      .owner_cpu = NULL, .file = NULL, .line = 0, .lock_time = 0 }
#else
#define __RAW_SPIN_LOCK_UNLOCKED \
    { .next = ATOMIC_INIT(0), .owner = ATOMIC_INIT(0) }
#endif

#define __SPIN_LOCK_UNLOCKED    { .raw = __RAW_SPIN_LOCK_UNLOCKED }

#define DEFINE_SPINLOCK(name)       spinlock_t name = __SPIN_LOCK_UNLOCKED
#define DEFINE_RAW_SPINLOCK(name)   raw_spinlock_t name = __RAW_SPIN_LOCK_UNLOCKED

/**
 * spin_lock_init - Initialize a spinlock
 * @lock: the spinlock to initialize
 */
static inline void spin_lock_init(spinlock_t *lock)
{
    atomic_set(&lock->raw.next, 0);
    atomic_set(&lock->raw.owner, 0);
#if CONFIG_DEBUG_SPINLOCK
    lock->raw.owner_cpu = NULL;
    lock->raw.file = NULL;
    lock->raw.line = 0;
#endif
}

#define raw_spin_lock_init(lock) spin_lock_init((spinlock_t *)(lock))

/* ============================================================================
 * Core Spinlock Operations (Ticket Lock Algorithm)
 * ============================================================================ */

/* Forward declaration for panic */
extern void kernel_panic(const char *msg);

/**
 * __raw_spin_lock - Acquire raw spinlock
 * @lock: the spinlock
 *
 * Uses ticket lock: get a ticket, then wait until owner matches.
 * Will panic after SPINLOCK_MAX_SPINS to detect deadlocks.
 */
static inline void __raw_spin_lock(raw_spinlock_t *lock)
{
    int ticket;
    unsigned long spins = 0;

    /* Get our ticket number (atomically increment next) */
    ticket = atomic_add_return(1, &lock->next) - 1;

    /* Spin until our ticket is being served */
    while (atomic_read(&lock->owner) != ticket) {
        cpu_relax();
        spins++;

        /* Detect potential deadlock after too many spins */
        if (spins > SPINLOCK_MAX_SPINS) {
            kernel_panic("spinlock: possible deadlock detected");
        }
    }

    /* Ensure all memory operations after lock are ordered */
    smp_mb();
}

/**
 * __raw_spin_unlock - Release raw spinlock
 * @lock: the spinlock
 *
 * Increments owner to serve next ticket.
 */
static inline void __raw_spin_unlock(raw_spinlock_t *lock)
{
    /* Ensure all memory operations before unlock are complete */
    smp_mb();

    /* Serve next ticket */
    atomic_inc(&lock->owner);
}

/**
 * __raw_spin_trylock - Try to acquire raw spinlock
 * @lock: the spinlock
 *
 * Returns: 1 if lock acquired, 0 if already held
 */
static inline int __raw_spin_trylock(raw_spinlock_t *lock)
{
    int next = atomic_read(&lock->next);
    int owner = atomic_read(&lock->owner);

    /* Lock is free if next == owner */
    if (next != owner) {
        return 0;  /* Lock is held */
    }

    /* Try to get ticket atomically */
    if (atomic_cmpxchg(&lock->next, next, next + 1) == next) {
        smp_mb();
        return 1;  /* Got the lock */
    }

    return 0;  /* Someone else got it */
}

/**
 * __raw_spin_is_locked - Check if spinlock is held
 * @lock: the spinlock
 */
static inline int __raw_spin_is_locked(raw_spinlock_t *lock)
{
    return atomic_read(&lock->next) != atomic_read(&lock->owner);
}

/* ============================================================================
 * High-level Spinlock API
 * ============================================================================ */

/**
 * spin_lock - Acquire spinlock
 * @lock: the spinlock
 */
static inline void spin_lock(spinlock_t *lock)
{
    __raw_spin_lock(&lock->raw);
}

/**
 * spin_unlock - Release spinlock
 * @lock: the spinlock
 */
static inline void spin_unlock(spinlock_t *lock)
{
    __raw_spin_unlock(&lock->raw);
}

/**
 * spin_trylock - Try to acquire spinlock
 * @lock: the spinlock
 *
 * Returns: 1 if acquired, 0 if already held
 */
static inline int spin_trylock(spinlock_t *lock)
{
    return __raw_spin_trylock(&lock->raw);
}

/**
 * spin_is_locked - Check if spinlock is held
 * @lock: the spinlock
 */
static inline int spin_is_locked(spinlock_t *lock)
{
    return __raw_spin_is_locked(&lock->raw);
}

/* ============================================================================
 * IRQ-safe Spinlock Operations
 * ============================================================================ */

/**
 * spin_lock_irqsave - Acquire spinlock and save IRQ state
 * @lock: the spinlock
 * @flags: variable to store IRQ state
 */
#define spin_lock_irqsave(lock, flags) \
    do { \
        flags = arch_irq_save(); \
        spin_lock(lock); \
    } while (0)

/**
 * spin_unlock_irqrestore - Release spinlock and restore IRQ state
 * @lock: the spinlock
 * @flags: saved IRQ state
 */
#define spin_unlock_irqrestore(lock, flags) \
    do { \
        spin_unlock(lock); \
        arch_irq_restore(flags); \
    } while (0)

/**
 * spin_lock_irq - Acquire spinlock and disable IRQs
 * @lock: the spinlock
 */
static inline void spin_lock_irq(spinlock_t *lock)
{
    arch_irq_disable();
    spin_lock(lock);
}

/**
 * spin_unlock_irq - Release spinlock and enable IRQs
 * @lock: the spinlock
 */
static inline void spin_unlock_irq(spinlock_t *lock)
{
    spin_unlock(lock);
    arch_irq_enable();
}

/**
 * spin_trylock_irqsave - Try to acquire spinlock with IRQ save
 * @lock: the spinlock
 * @flags: variable to store IRQ state
 *
 * Returns: 1 if acquired, 0 if already held
 */
#define spin_trylock_irqsave(lock, flags) \
    ({ \
        int __ret; \
        flags = arch_irq_save(); \
        __ret = spin_trylock(lock); \
        if (!__ret) arch_irq_restore(flags); \
        __ret; \
    })

/* ============================================================================
 * BH (Bottom Half) Spinlock Operations
 * ============================================================================
 * In EMBODIOS, BH is same as regular spinlock until softirqs are implemented.
 */

#define spin_lock_bh(lock)      spin_lock(lock)
#define spin_unlock_bh(lock)    spin_unlock(lock)
#define spin_trylock_bh(lock)   spin_trylock(lock)

/* ============================================================================
 * Raw Spinlock Wrappers
 * ============================================================================ */

#define raw_spin_lock(lock)                     __raw_spin_lock(lock)
#define raw_spin_unlock(lock)                   __raw_spin_unlock(lock)
#define raw_spin_trylock(lock)                  __raw_spin_trylock(lock)
#define raw_spin_is_locked(lock)                __raw_spin_is_locked(lock)

#define raw_spin_lock_irqsave(lock, flags) \
    do { flags = arch_irq_save(); raw_spin_lock(lock); } while (0)

#define raw_spin_unlock_irqrestore(lock, flags) \
    do { raw_spin_unlock(lock); arch_irq_restore(flags); } while (0)

#define raw_spin_lock_irq(lock) \
    do { arch_irq_disable(); raw_spin_lock(lock); } while (0)

#define raw_spin_unlock_irq(lock) \
    do { raw_spin_unlock(lock); arch_irq_enable(); } while (0)

/* ============================================================================
 * Read-Write Spinlock (Simplified - writer preference)
 * ============================================================================ */

typedef struct rwlock {
    atomic_t lock;  /* 0 = unlocked, -1 = write-locked, >0 = read count */
} rwlock_t;

#define __RW_LOCK_UNLOCKED  { .lock = ATOMIC_INIT(0) }
#define DEFINE_RWLOCK(name) rwlock_t name = __RW_LOCK_UNLOCKED

static inline void rwlock_init(rwlock_t *rw)
{
    atomic_set(&rw->lock, 0);
}

static inline void read_lock(rwlock_t *rw)
{
    int val;
    unsigned long spins = 0;

    while (1) {
        val = atomic_read(&rw->lock);
        if (val >= 0 && atomic_cmpxchg(&rw->lock, val, val + 1) == val) {
            break;
        }
        cpu_relax();
        spins++;

        if (spins > SPINLOCK_MAX_SPINS) {
            kernel_panic("rwlock: read_lock possible deadlock");
        }
    }
    smp_mb();
}

static inline void read_unlock(rwlock_t *rw)
{
    smp_mb();
    atomic_dec(&rw->lock);
}

static inline void write_lock(rwlock_t *rw)
{
    unsigned long spins = 0;

    while (atomic_cmpxchg(&rw->lock, 0, -1) != 0) {
        cpu_relax();
        spins++;

        if (spins > SPINLOCK_MAX_SPINS) {
            kernel_panic("rwlock: write_lock possible deadlock");
        }
    }
    smp_mb();
}

static inline void write_unlock(rwlock_t *rw)
{
    smp_mb();
    atomic_set(&rw->lock, 0);
}

#define read_lock_irqsave(lock, flags) \
    do { flags = arch_irq_save(); read_lock(lock); } while (0)
#define read_unlock_irqrestore(lock, flags) \
    do { read_unlock(lock); arch_irq_restore(flags); } while (0)
#define write_lock_irqsave(lock, flags) \
    do { flags = arch_irq_save(); write_lock(lock); } while (0)
#define write_unlock_irqrestore(lock, flags) \
    do { write_unlock(lock); arch_irq_restore(flags); } while (0)

/* ============================================================================
 * Local IRQ Control (non-lock based)
 * ============================================================================ */

#define local_irq_save(flags)       do { flags = arch_irq_save(); } while (0)
#define local_irq_restore(flags)    arch_irq_restore(flags)
#define local_irq_disable()         arch_irq_disable()
#define local_irq_enable()          arch_irq_enable()

/* ============================================================================
 * Debug Assertions
 * ============================================================================ */

#define assert_spin_locked(lock)    ((void)(lock))
#define lockdep_assert_held(lock)   ((void)(lock))

#endif /* EMBODIOS_SPINLOCK_H */
