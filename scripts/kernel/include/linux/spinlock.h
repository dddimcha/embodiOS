/* SPDX-License-Identifier: GPL-2.0 */
/**
 * Linux Compatibility Layer - Spinlocks
 *
 * Provides Linux kernel spinlock APIs for EMBODIOS.
 * Reference: linux/include/linux/spinlock.h
 *
 * Part of EMBODIOS Linux Driver Compatibility Shim (~50 APIs)
 *
 * Implementation Notes:
 * ---------------------
 * EMBODIOS is currently single-core, so spinlocks are implemented as
 * interrupt disable/enable operations. This provides mutual exclusion
 * without actual spinning, which is correct for UP (uniprocessor) systems.
 *
 * For SMP support in the future, these would need to be replaced with
 * actual atomic test-and-set operations.
 */

#ifndef _LINUX_SPINLOCK_H
#define _LINUX_SPINLOCK_H

#include <linux/types.h>

/* ============================================================================
 * Architecture-specific interrupt control
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)

/* x86: Use CLI/STI for interrupt control */
static inline unsigned long arch_local_irq_save(void)
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

static inline void arch_local_irq_restore(unsigned long flags)
{
    __asm__ volatile(
        "pushq %0\n\t"
        "popfq"
        :
        : "r" (flags)
        : "memory", "cc"
    );
}

static inline void arch_local_irq_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

static inline void arch_local_irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

static inline bool arch_irqs_disabled(void)
{
    unsigned long flags;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0"
        : "=r" (flags)
    );
    return !(flags & 0x200);  /* IF flag is bit 9 */
}

#elif defined(__aarch64__)

/* ARM64: Use DAIF for interrupt control */
static inline unsigned long arch_local_irq_save(void)
{
    unsigned long flags;
    __asm__ volatile(
        "mrs %0, daif\n\t"
        "msr daifset, #2"  /* Disable IRQ */
        : "=r" (flags)
        :
        : "memory"
    );
    return flags;
}

static inline void arch_local_irq_restore(unsigned long flags)
{
    __asm__ volatile(
        "msr daif, %0"
        :
        : "r" (flags)
        : "memory"
    );
}

static inline void arch_local_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static inline void arch_local_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline bool arch_irqs_disabled(void)
{
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r" (flags));
    return !!(flags & 0x80);  /* IRQ mask bit */
}

#else

/* Generic fallback - no interrupt control */
static inline unsigned long arch_local_irq_save(void) { return 0; }
static inline void arch_local_irq_restore(unsigned long flags) { (void)flags; }
static inline void arch_local_irq_disable(void) { }
static inline void arch_local_irq_enable(void) { }
static inline bool arch_irqs_disabled(void) { return false; }

#endif

/* ============================================================================
 * Spinlock type definition
 * ============================================================================ */

/*
 * spinlock_t - Spinlock structure
 *
 * For single-core EMBODIOS, the lock field is just for debugging.
 * Actual locking is done via interrupt disable.
 */
typedef struct spinlock {
    volatile unsigned int lock;
#ifdef CONFIG_DEBUG_SPINLOCK
    const char *name;
    const char *file;
    int line;
#endif
} spinlock_t;

/* Raw spinlock (same as spinlock_t in EMBODIOS) */
typedef spinlock_t raw_spinlock_t;

/* ============================================================================
 * Spinlock initialization
 * ============================================================================ */

#define __SPIN_LOCK_UNLOCKED    { .lock = 0 }
#define DEFINE_SPINLOCK(x)      spinlock_t x = __SPIN_LOCK_UNLOCKED
#define DEFINE_RAW_SPINLOCK(x)  raw_spinlock_t x = __SPIN_LOCK_UNLOCKED

/* Runtime initialization */
static inline void spin_lock_init(spinlock_t *lock)
{
    lock->lock = 0;
}

#define raw_spin_lock_init(lock)    spin_lock_init(lock)

/* ============================================================================
 * Basic spinlock operations
 * ============================================================================
 *
 * For UP systems, spin_lock() disables preemption (or interrupts).
 * Since EMBODIOS doesn't have preemption, we use interrupt disable.
 */

/*
 * spin_lock - Acquire spinlock
 * @lock: the spinlock to acquire
 *
 * Disables interrupts and marks lock as held.
 */
static inline void spin_lock(spinlock_t *lock)
{
    arch_local_irq_disable();
    lock->lock = 1;
}

/*
 * spin_unlock - Release spinlock
 * @lock: the spinlock to release
 *
 * Marks lock as released and enables interrupts.
 */
static inline void spin_unlock(spinlock_t *lock)
{
    lock->lock = 0;
    arch_local_irq_enable();
}

/*
 * spin_trylock - Try to acquire spinlock
 * @lock: the spinlock to acquire
 *
 * Returns: 1 if lock acquired, 0 if already held
 */
static inline int spin_trylock(spinlock_t *lock)
{
    /* Disable IRQ first to avoid race condition */
    arch_local_irq_disable();

    if (lock->lock) {
        /* Lock already held - re-enable IRQ and fail */
        arch_local_irq_enable();
        return 0;
    }

    lock->lock = 1;
    return 1;
}

/*
 * spin_is_locked - Check if spinlock is held
 * @lock: the spinlock to check
 */
static inline int spin_is_locked(spinlock_t *lock)
{
    return lock->lock != 0;
}

/* ============================================================================
 * IRQ-safe spinlock operations
 * ============================================================================
 *
 * These save/restore interrupt state, allowing nested locking.
 */

/*
 * spin_lock_irqsave - Acquire spinlock and save IRQ state
 * @lock: the spinlock to acquire
 * @flags: variable to store IRQ state
 */
static inline void __spin_lock_irqsave(spinlock_t *lock, unsigned long *flags)
{
    *flags = arch_local_irq_save();
    lock->lock = 1;
}
#define spin_lock_irqsave(lock, flags) \
    __spin_lock_irqsave(lock, &(flags))

/*
 * spin_unlock_irqrestore - Release spinlock and restore IRQ state
 * @lock: the spinlock to release
 * @flags: saved IRQ state
 */
static inline void __spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    lock->lock = 0;
    arch_local_irq_restore(flags);
}
#define spin_unlock_irqrestore(lock, flags) \
    __spin_unlock_irqrestore(lock, flags)

/*
 * spin_lock_irq - Acquire spinlock and disable IRQs
 * @lock: the spinlock to acquire
 */
static inline void spin_lock_irq(spinlock_t *lock)
{
    arch_local_irq_disable();
    lock->lock = 1;
}

/*
 * spin_unlock_irq - Release spinlock and enable IRQs
 * @lock: the spinlock to release
 */
static inline void spin_unlock_irq(spinlock_t *lock)
{
    lock->lock = 0;
    arch_local_irq_enable();
}

/*
 * spin_trylock_irqsave - Try to acquire spinlock with IRQ save
 * @lock: the spinlock to acquire
 * @flags: variable to store IRQ state
 *
 * Returns: 1 if lock acquired, 0 if already held
 */
static inline int __spin_trylock_irqsave(spinlock_t *lock, unsigned long *flags)
{
    *flags = arch_local_irq_save();
    if (lock->lock) {
        arch_local_irq_restore(*flags);
        return 0;
    }
    lock->lock = 1;
    return 1;
}
#define spin_trylock_irqsave(lock, flags) \
    __spin_trylock_irqsave(lock, &(flags))

/* ============================================================================
 * BH (Bottom Half) spinlock operations
 * ============================================================================
 *
 * EMBODIOS doesn't have softirqs/BH, so these are same as regular spinlocks.
 */

static inline void spin_lock_bh(spinlock_t *lock)
{
    spin_lock(lock);
}

static inline void spin_unlock_bh(spinlock_t *lock)
{
    spin_unlock(lock);
}

static inline int spin_trylock_bh(spinlock_t *lock)
{
    return spin_trylock(lock);
}

/* ============================================================================
 * Raw spinlock operations (same as regular in EMBODIOS)
 * ============================================================================ */

#define raw_spin_lock(lock)                 spin_lock(lock)
#define raw_spin_unlock(lock)               spin_unlock(lock)
#define raw_spin_trylock(lock)              spin_trylock(lock)
#define raw_spin_lock_irqsave(lock, flags)  spin_lock_irqsave(lock, flags)
#define raw_spin_unlock_irqrestore(lock, flags) spin_unlock_irqrestore(lock, flags)
#define raw_spin_lock_irq(lock)             spin_lock_irq(lock)
#define raw_spin_unlock_irq(lock)           spin_unlock_irq(lock)

/* ============================================================================
 * Local IRQ control (not lock-based)
 * ============================================================================ */

#define local_irq_save(flags)       do { flags = arch_local_irq_save(); } while (0)
#define local_irq_restore(flags)    arch_local_irq_restore(flags)
#define local_irq_disable()         arch_local_irq_disable()
#define local_irq_enable()          arch_local_irq_enable()
#define irqs_disabled()             arch_irqs_disabled()

/* ============================================================================
 * Read-Write Spinlocks (simplified - same as regular spinlock)
 * ============================================================================
 *
 * For UP systems, read-write locks degrade to regular spinlocks.
 */

typedef spinlock_t rwlock_t;

#define __RW_LOCK_UNLOCKED      __SPIN_LOCK_UNLOCKED
#define DEFINE_RWLOCK(x)        rwlock_t x = __RW_LOCK_UNLOCKED

static inline void rwlock_init(rwlock_t *lock)
{
    spin_lock_init(lock);
}

/* Read lock operations */
#define read_lock(lock)                     spin_lock(lock)
#define read_unlock(lock)                   spin_unlock(lock)
#define read_lock_irqsave(lock, flags)      spin_lock_irqsave(lock, flags)
#define read_unlock_irqrestore(lock, flags) spin_unlock_irqrestore(lock, flags)
#define read_lock_irq(lock)                 spin_lock_irq(lock)
#define read_unlock_irq(lock)               spin_unlock_irq(lock)
#define read_lock_bh(lock)                  spin_lock_bh(lock)
#define read_unlock_bh(lock)                spin_unlock_bh(lock)

/* Write lock operations */
#define write_lock(lock)                    spin_lock(lock)
#define write_unlock(lock)                  spin_unlock(lock)
#define write_lock_irqsave(lock, flags)     spin_lock_irqsave(lock, flags)
#define write_unlock_irqrestore(lock, flags) spin_unlock_irqrestore(lock, flags)
#define write_lock_irq(lock)                spin_lock_irq(lock)
#define write_unlock_irq(lock)              spin_unlock_irq(lock)
#define write_lock_bh(lock)                 spin_lock_bh(lock)
#define write_unlock_bh(lock)               spin_unlock_bh(lock)

/* ============================================================================
 * Memory barriers
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)
#define mb()    __asm__ volatile("mfence" ::: "memory")
#define rmb()   __asm__ volatile("lfence" ::: "memory")
#define wmb()   __asm__ volatile("sfence" ::: "memory")
#elif defined(__aarch64__)
#define mb()    __asm__ volatile("dmb sy" ::: "memory")
#define rmb()   __asm__ volatile("dmb ld" ::: "memory")
#define wmb()   __asm__ volatile("dmb st" ::: "memory")
#else
#define mb()    __asm__ volatile("" ::: "memory")
#define rmb()   __asm__ volatile("" ::: "memory")
#define wmb()   __asm__ volatile("" ::: "memory")
#endif

#define smp_mb()    mb()
#define smp_rmb()   rmb()
#define smp_wmb()   wmb()

/* Compiler barrier */
#define barrier()   __asm__ volatile("" ::: "memory")

/* CPU relax for busy-wait loops - reduces power and allows other threads */
#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __asm__ volatile("pause" ::: "memory")
#elif defined(__aarch64__)
#define cpu_relax() __asm__ volatile("yield" ::: "memory")
#else
#define cpu_relax() barrier()
#endif

/* ============================================================================
 * Seqlock (simplified)
 * ============================================================================
 *
 * Sequence locks for read-mostly data. Simplified to regular spinlock.
 */

typedef struct {
    spinlock_t lock;
    unsigned int sequence;
} seqlock_t;

#define __SEQLOCK_UNLOCKED      { .lock = __SPIN_LOCK_UNLOCKED, .sequence = 0 }
#define DEFINE_SEQLOCK(x)       seqlock_t x = __SEQLOCK_UNLOCKED

static inline void seqlock_init(seqlock_t *sl)
{
    spin_lock_init(&sl->lock);
    sl->sequence = 0;
}

static inline void write_seqlock(seqlock_t *sl)
{
    spin_lock(&sl->lock);
    sl->sequence++;
}

static inline void write_sequnlock(seqlock_t *sl)
{
    sl->sequence++;
    spin_unlock(&sl->lock);
}

static inline unsigned int read_seqbegin(const seqlock_t *sl)
{
    unsigned int seq;
    do {
        seq = sl->sequence;
        smp_rmb();  /* Ensure we read a consistent sequence */
    } while (seq & 1);  /* Wait if write in progress (odd = writing) */
    return seq;
}

static inline unsigned int read_seqretry(const seqlock_t *sl, unsigned int start)
{
    smp_rmb();  /* Ensure all reads complete before checking */
    /* Retry if sequence changed OR if we started during a write (odd) */
    return (sl->sequence != start) || (start & 1);
}

/* ============================================================================
 * Assert macros
 * ============================================================================ */

#define assert_spin_locked(lock)    (void)(lock)
#define lockdep_assert_held(lock)   (void)(lock)

#endif /* _LINUX_SPINLOCK_H */
