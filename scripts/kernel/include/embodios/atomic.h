/* EMBODIOS Atomic Operations
 *
 * Provides atomic operations for SMP-safe synchronization.
 * Uses compiler built-ins for portability across x86_64 and ARM64.
 *
 * Reference: Linux include/linux/atomic.h
 */

#ifndef EMBODIOS_ATOMIC_H
#define EMBODIOS_ATOMIC_H

#include <embodios/types.h>

/* ============================================================================
 * Architecture Constants
 * ============================================================================ */

#define BITS_PER_LONG       (sizeof(unsigned long) * 8)
#define BIT_WORD(nr)        ((nr) / BITS_PER_LONG)
#define BIT_MASK(nr)        (1UL << ((nr) % BITS_PER_LONG))

/* ============================================================================
 * Atomic Types
 * ============================================================================ */

typedef struct {
    volatile int32_t counter;
} atomic_t;

typedef struct {
    volatile int64_t counter;
} atomic64_t;

#define ATOMIC_INIT(i)      { (i) }
#define ATOMIC64_INIT(i)    { (i) }

/* ============================================================================
 * Memory Barriers
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)
#define smp_mb()    __asm__ volatile("mfence" ::: "memory")
#define smp_rmb()   __asm__ volatile("lfence" ::: "memory")
#define smp_wmb()   __asm__ volatile("sfence" ::: "memory")
#elif defined(__aarch64__)
#define smp_mb()    __asm__ volatile("dmb ish" ::: "memory")
#define smp_rmb()   __asm__ volatile("dmb ishld" ::: "memory")
#define smp_wmb()   __asm__ volatile("dmb ishst" ::: "memory")
#else
#define smp_mb()    __sync_synchronize()
#define smp_rmb()   __sync_synchronize()
#define smp_wmb()   __sync_synchronize()
#endif

/* Compiler barrier */
#define barrier()   __asm__ volatile("" ::: "memory")

/* ============================================================================
 * 32-bit Atomic Operations
 * ============================================================================ */

/**
 * atomic_read - Read atomic variable
 * @v: pointer to atomic_t
 *
 * Atomically reads the value of @v with acquire semantics.
 */
static inline int atomic_read(const atomic_t *v)
{
    return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE);
}

/**
 * atomic_set - Set atomic variable
 * @v: pointer to atomic_t
 * @i: value to set
 *
 * Atomically sets the value of @v to @i with release semantics.
 */
static inline void atomic_set(atomic_t *v, int i)
{
    __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE);
}

/**
 * atomic_add - Add to atomic variable
 * @i: integer value to add
 * @v: pointer to atomic_t
 *
 * Atomically adds @i to @v.
 */
static inline void atomic_add(int i, atomic_t *v)
{
    __atomic_fetch_add(&v->counter, i, __ATOMIC_RELAXED);
}

/**
 * atomic_sub - Subtract from atomic variable
 * @i: integer value to subtract
 * @v: pointer to atomic_t
 *
 * Atomically subtracts @i from @v.
 */
static inline void atomic_sub(int i, atomic_t *v)
{
    __atomic_fetch_sub(&v->counter, i, __ATOMIC_RELAXED);
}

/**
 * atomic_inc - Increment atomic variable
 * @v: pointer to atomic_t
 *
 * Atomically increments @v by 1.
 */
static inline void atomic_inc(atomic_t *v)
{
    atomic_add(1, v);
}

/**
 * atomic_dec - Decrement atomic variable
 * @v: pointer to atomic_t
 *
 * Atomically decrements @v by 1.
 */
static inline void atomic_dec(atomic_t *v)
{
    atomic_sub(1, v);
}

/**
 * atomic_add_return - Add and return
 * @i: integer value to add
 * @v: pointer to atomic_t
 *
 * Atomically adds @i to @v and returns the new value.
 */
static inline int atomic_add_return(int i, atomic_t *v)
{
    return __atomic_add_fetch(&v->counter, i, __ATOMIC_ACQ_REL);
}

/**
 * atomic_sub_return - Subtract and return
 * @i: integer value to subtract
 * @v: pointer to atomic_t
 *
 * Atomically subtracts @i from @v and returns the new value.
 */
static inline int atomic_sub_return(int i, atomic_t *v)
{
    return __atomic_sub_fetch(&v->counter, i, __ATOMIC_ACQ_REL);
}

/**
 * atomic_inc_return - Increment and return
 * @v: pointer to atomic_t
 *
 * Atomically increments @v and returns the new value.
 */
static inline int atomic_inc_return(atomic_t *v)
{
    return atomic_add_return(1, v);
}

/**
 * atomic_dec_return - Decrement and return
 * @v: pointer to atomic_t
 *
 * Atomically decrements @v and returns the new value.
 */
static inline int atomic_dec_return(atomic_t *v)
{
    return atomic_sub_return(1, v);
}

/**
 * atomic_dec_and_test - Decrement and test
 * @v: pointer to atomic_t
 *
 * Atomically decrements @v and returns true if result is 0.
 */
static inline bool atomic_dec_and_test(atomic_t *v)
{
    return atomic_dec_return(v) == 0;
}

/**
 * atomic_inc_and_test - Increment and test
 * @v: pointer to atomic_t
 *
 * Atomically increments @v and returns true if result is 0.
 */
static inline bool atomic_inc_and_test(atomic_t *v)
{
    return atomic_inc_return(v) == 0;
}

/**
 * atomic_xchg - Exchange atomic variable
 * @v: pointer to atomic_t
 * @new: new value
 *
 * Atomically exchanges @v with @new, returns old value.
 */
static inline int atomic_xchg(atomic_t *v, int new)
{
    return __atomic_exchange_n(&v->counter, new, __ATOMIC_ACQ_REL);
}

/**
 * atomic_cmpxchg - Compare and exchange
 * @v: pointer to atomic_t
 * @old: expected old value
 * @new: new value
 *
 * If current value equals @old, set to @new.
 * Returns the old value (whether exchanged or not).
 */
static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
    int expected = old;
    __atomic_compare_exchange_n(&v->counter, &expected, new,
                                 false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
}

/**
 * atomic_try_cmpxchg - Try compare and exchange
 * @v: pointer to atomic_t
 * @old: pointer to expected old value (updated on failure)
 * @new: new value
 *
 * Returns true if exchange succeeded, false otherwise.
 * On failure, *old is updated with the current value.
 */
static inline bool atomic_try_cmpxchg(atomic_t *v, int *old, int new)
{
    return __atomic_compare_exchange_n(&v->counter, old, new,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* ============================================================================
 * 64-bit Atomic Operations
 * ============================================================================ */

static inline int64_t atomic64_read(const atomic64_t *v)
{
    return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE);
}

static inline void atomic64_set(atomic64_t *v, int64_t i)
{
    __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE);
}

static inline void atomic64_add(int64_t i, atomic64_t *v)
{
    __atomic_fetch_add(&v->counter, i, __ATOMIC_RELAXED);
}

static inline void atomic64_sub(int64_t i, atomic64_t *v)
{
    __atomic_fetch_sub(&v->counter, i, __ATOMIC_RELAXED);
}

static inline void atomic64_inc(atomic64_t *v)
{
    atomic64_add(1, v);
}

static inline void atomic64_dec(atomic64_t *v)
{
    atomic64_sub(1, v);
}

static inline int64_t atomic64_add_return(int64_t i, atomic64_t *v)
{
    return __atomic_add_fetch(&v->counter, i, __ATOMIC_ACQ_REL);
}

static inline int64_t atomic64_xchg(atomic64_t *v, int64_t new)
{
    return __atomic_exchange_n(&v->counter, new, __ATOMIC_ACQ_REL);
}

static inline int64_t atomic64_cmpxchg(atomic64_t *v, int64_t old, int64_t new)
{
    int64_t expected = old;
    __atomic_compare_exchange_n(&v->counter, &expected, new,
                                 false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
}

/* ============================================================================
 * Bitwise Atomic Operations
 * ============================================================================ */

/**
 * atomic_or - Atomic OR
 * @i: value to OR
 * @v: pointer to atomic_t
 */
static inline void atomic_or(int i, atomic_t *v)
{
    __atomic_fetch_or(&v->counter, i, __ATOMIC_RELAXED);
}

/**
 * atomic_and - Atomic AND
 * @i: value to AND
 * @v: pointer to atomic_t
 */
static inline void atomic_and(int i, atomic_t *v)
{
    __atomic_fetch_and(&v->counter, i, __ATOMIC_RELAXED);
}

/**
 * atomic_xor - Atomic XOR
 * @i: value to XOR
 * @v: pointer to atomic_t
 */
static inline void atomic_xor(int i, atomic_t *v)
{
    __atomic_fetch_xor(&v->counter, i, __ATOMIC_RELAXED);
}

/* ============================================================================
 * Test-and-Set Operations (for spinlocks)
 * ============================================================================ */

/**
 * test_and_set_bit - Atomically set a bit and return old value
 * @nr: bit number
 * @addr: pointer to memory
 *
 * Returns true if bit was already set.
 */
static inline bool test_and_set_bit(int nr, volatile unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    unsigned long *p = ((unsigned long *)addr) + BIT_WORD(nr);
    unsigned long old = __atomic_fetch_or(p, mask, __ATOMIC_ACQ_REL);
    return (old & mask) != 0;
}

/**
 * test_and_clear_bit - Atomically clear a bit and return old value
 * @nr: bit number
 * @addr: pointer to memory
 *
 * Returns true if bit was set.
 */
static inline bool test_and_clear_bit(int nr, volatile unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    unsigned long *p = ((unsigned long *)addr) + BIT_WORD(nr);
    unsigned long old = __atomic_fetch_and(p, ~mask, __ATOMIC_ACQ_REL);
    return (old & mask) != 0;
}

/**
 * set_bit - Atomically set a bit
 * @nr: bit number
 * @addr: pointer to memory
 */
static inline void set_bit(int nr, volatile unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    unsigned long *p = ((unsigned long *)addr) + BIT_WORD(nr);
    __atomic_fetch_or(p, mask, __ATOMIC_RELEASE);
}

/**
 * clear_bit - Atomically clear a bit
 * @nr: bit number
 * @addr: pointer to memory
 */
static inline void clear_bit(int nr, volatile unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    unsigned long *p = ((unsigned long *)addr) + BIT_WORD(nr);
    __atomic_fetch_and(p, ~mask, __ATOMIC_RELEASE);
}

/**
 * test_bit - Test a bit (non-atomic read)
 * @nr: bit number
 * @addr: pointer to memory
 *
 * Returns true if bit is set.
 * Note: This is a non-atomic read. Use with appropriate barriers if needed.
 */
static inline bool test_bit(int nr, const volatile unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    const unsigned long *p = ((const unsigned long *)addr) + BIT_WORD(nr);
    return (__atomic_load_n(p, __ATOMIC_RELAXED) & mask) != 0;
}

#endif /* EMBODIOS_ATOMIC_H */
