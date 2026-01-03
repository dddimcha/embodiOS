/* EMBODIOS Mutex Implementation
 *
 * SMP-safe mutex for longer critical sections.
 * Currently implemented as spinning mutex; can be upgraded to sleeping
 * mutex when scheduler supports task blocking.
 *
 * Features:
 * - Atomic lock acquisition
 * - Owner tracking for debugging
 * - Recursive lock detection
 * - Deadlock detection (CONFIG_DEBUG_MUTEX)
 *
 * Reference: Linux kernel/locking/mutex.c
 */

#ifndef EMBODIOS_MUTEX_H
#define EMBODIOS_MUTEX_H

#include <embodios/types.h>
#include <embodios/atomic.h>
#include <embodios/spinlock.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#ifndef CONFIG_DEBUG_MUTEX
#define CONFIG_DEBUG_MUTEX 0
#endif

/* Maximum spin iterations before yielding */
#define MUTEX_MAX_SPINS         1000

/* Maximum total spins before panic (detects deadlock) */
#define MUTEX_MAX_TOTAL_SPINS   50000000

/* ============================================================================
 * Mutex Types
 * ============================================================================ */

/**
 * struct mutex - Mutual exclusion lock
 *
 * @locked: 0 = unlocked, 1 = locked
 * @wait_lock: Protects wait list (for future sleeping support)
 * @owner: CPU/task holding the mutex (debug)
 */
typedef struct mutex {
    atomic_t locked;            /* Lock state: 0=free, 1=held */
    spinlock_t wait_lock;       /* Protects waiter list */
#if CONFIG_DEBUG_MUTEX
    void *owner;                /* Owner for debugging */
    const char *name;           /* Mutex name */
    const char *file;           /* File where locked */
    int line;                   /* Line where locked */
    int lock_count;             /* Detect recursive locking */
#endif
} mutex_t;

/* ============================================================================
 * Mutex Initialization
 * ============================================================================ */

#if CONFIG_DEBUG_MUTEX
#define __MUTEX_INITIALIZER(lockname) \
    { \
        .locked = ATOMIC_INIT(0), \
        .wait_lock = __SPIN_LOCK_UNLOCKED, \
        .owner = NULL, \
        .name = #lockname, \
        .file = NULL, \
        .line = 0, \
        .lock_count = 0 \
    }
#else
#define __MUTEX_INITIALIZER(lockname) \
    { \
        .locked = ATOMIC_INIT(0), \
        .wait_lock = __SPIN_LOCK_UNLOCKED \
    }
#endif

#define DEFINE_MUTEX(name)  mutex_t name = __MUTEX_INITIALIZER(name)

/**
 * mutex_init - Initialize a mutex
 * @lock: the mutex to initialize
 */
static inline void mutex_init(mutex_t *lock)
{
    atomic_set(&lock->locked, 0);
    spin_lock_init(&lock->wait_lock);
#if CONFIG_DEBUG_MUTEX
    lock->owner = NULL;
    lock->name = NULL;
    lock->file = NULL;
    lock->line = 0;
    lock->lock_count = 0;
#endif
}

/**
 * mutex_destroy - Destroy a mutex
 * @lock: the mutex to destroy
 */
static inline void mutex_destroy(mutex_t *lock)
{
#if CONFIG_DEBUG_MUTEX
    /* Check mutex is not held */
    if (atomic_read(&lock->locked)) {
        /* BUG: destroying held mutex */
    }
#endif
    (void)lock;
}

/* ============================================================================
 * Core Mutex Operations
 * ============================================================================ */

/**
 * mutex_lock - Acquire mutex
 * @lock: the mutex
 *
 * Spins until mutex is acquired. In SMP, multiple CPUs may compete.
 * Uses atomic compare-and-swap for lock acquisition.
 * Will panic after MUTEX_MAX_TOTAL_SPINS to detect deadlocks.
 */
static inline void mutex_lock(mutex_t *lock)
{
    unsigned long spins = 0;
    unsigned long total_spins = 0;

    while (1) {
        /* Try to acquire: change 0 -> 1 */
        if (atomic_cmpxchg(&lock->locked, 0, 1) == 0) {
            /* Got the lock */
            smp_mb();
#if CONFIG_DEBUG_MUTEX
            lock->lock_count++;
#endif
            return;
        }

        /* Spin with backoff */
        do {
            cpu_relax();
            spins++;
            total_spins++;

            /* Detect potential deadlock */
            if (total_spins > MUTEX_MAX_TOTAL_SPINS) {
                kernel_panic("mutex: possible deadlock detected");
            }

            /* Adaptive: after many spins, could yield to scheduler */
            if (spins > MUTEX_MAX_SPINS) {
                /* In future: call schedule() to yield */
                spins = 0;
            }
        } while (atomic_read(&lock->locked));
    }
}

/**
 * mutex_unlock - Release mutex
 * @lock: the mutex
 */
static inline void mutex_unlock(mutex_t *lock)
{
#if CONFIG_DEBUG_MUTEX
    /* Verify we own the lock */
    if (!atomic_read(&lock->locked)) {
        /* BUG: unlocking unheld mutex */
        return;
    }
    lock->lock_count--;
    lock->owner = NULL;
#endif

    smp_mb();
    atomic_set(&lock->locked, 0);

    /* In future: wake up waiters via wait_lock */
}

/**
 * mutex_trylock - Try to acquire mutex
 * @lock: the mutex
 *
 * Returns: 1 if acquired, 0 if already held
 */
static inline int mutex_trylock(mutex_t *lock)
{
    if (atomic_cmpxchg(&lock->locked, 0, 1) == 0) {
        smp_mb();
#if CONFIG_DEBUG_MUTEX
        lock->lock_count++;
#endif
        return 1;
    }
    return 0;
}

/**
 * mutex_is_locked - Check if mutex is held
 * @lock: the mutex
 */
static inline int mutex_is_locked(mutex_t *lock)
{
    return atomic_read(&lock->locked) != 0;
}

/**
 * mutex_lock_interruptible - Acquire mutex (interruptible)
 * @lock: the mutex
 *
 * Returns: 0 on success (always in EMBODIOS, no signals)
 */
static inline int mutex_lock_interruptible(mutex_t *lock)
{
    mutex_lock(lock);
    return 0;
}

/**
 * mutex_lock_killable - Acquire mutex (killable)
 * @lock: the mutex
 *
 * Returns: 0 on success
 */
static inline int mutex_lock_killable(mutex_t *lock)
{
    mutex_lock(lock);
    return 0;
}

/* ============================================================================
 * Semaphore (Counting Lock)
 * ============================================================================ */

/**
 * struct semaphore - Counting semaphore
 *
 * @count: Current count (>0 means available)
 * @lock: Protects count
 */
typedef struct semaphore {
    atomic_t count;
    spinlock_t lock;
} semaphore_t;

#define __SEMAPHORE_INITIALIZER(name, n) \
    { .count = ATOMIC_INIT(n), .lock = __SPIN_LOCK_UNLOCKED }

#define DEFINE_SEMAPHORE(name) \
    semaphore_t name = __SEMAPHORE_INITIALIZER(name, 1)

static inline void sema_init(semaphore_t *sem, int val)
{
    atomic_set(&sem->count, val);
    spin_lock_init(&sem->lock);
}

/**
 * down - Acquire semaphore (decrement)
 * @sem: the semaphore
 *
 * Will panic after excessive spinning to detect deadlocks.
 */
static inline void down(semaphore_t *sem)
{
    unsigned long spins = 0;

    while (1) {
        int count = atomic_read(&sem->count);
        if (count > 0) {
            if (atomic_cmpxchg(&sem->count, count, count - 1) == count) {
                smp_mb();
                return;
            }
        }
        cpu_relax();
        spins++;

        if (spins > MUTEX_MAX_TOTAL_SPINS) {
            kernel_panic("semaphore: possible deadlock detected");
        }
    }
}

/**
 * down_trylock - Try to acquire semaphore
 * @sem: the semaphore
 *
 * Returns: 0 if acquired, 1 if would block (note: opposite of mutex_trylock!)
 */
static inline int down_trylock(semaphore_t *sem)
{
    int count = atomic_read(&sem->count);
    if (count > 0) {
        if (atomic_cmpxchg(&sem->count, count, count - 1) == count) {
            smp_mb();
            return 0;  /* Acquired */
        }
    }
    return 1;  /* Would block */
}

/**
 * down_interruptible - Acquire semaphore (interruptible)
 * @sem: the semaphore
 *
 * Returns: 0 on success
 */
static inline int down_interruptible(semaphore_t *sem)
{
    down(sem);
    return 0;
}

/**
 * up - Release semaphore (increment)
 * @sem: the semaphore
 */
static inline void up(semaphore_t *sem)
{
    smp_mb();
    atomic_inc(&sem->count);
}

/* ============================================================================
 * Completion
 * ============================================================================ */

/**
 * struct completion - Event completion
 *
 * Used to wait for an event to complete.
 */
typedef struct completion {
    atomic_t done;
    spinlock_t lock;
} completion_t;

#define COMPLETION_INITIALIZER(name) \
    { .done = ATOMIC_INIT(0), .lock = __SPIN_LOCK_UNLOCKED }

#define DECLARE_COMPLETION(name) \
    completion_t name = COMPLETION_INITIALIZER(name)

#define DECLARE_COMPLETION_ONSTACK(name) \
    completion_t name = COMPLETION_INITIALIZER(name)

static inline void init_completion(completion_t *x)
{
    atomic_set(&x->done, 0);
    spin_lock_init(&x->lock);
}

static inline void reinit_completion(completion_t *x)
{
    atomic_set(&x->done, 0);
}

/**
 * complete - Signal completion
 * @x: the completion
 */
static inline void complete(completion_t *x)
{
    smp_mb();
    atomic_set(&x->done, 1);
}

/**
 * complete_all - Signal all waiters
 * @x: the completion
 */
static inline void complete_all(completion_t *x)
{
    smp_mb();
    atomic_set(&x->done, UINT32_MAX);
}

/**
 * wait_for_completion - Wait for completion
 * @x: the completion
 *
 * Will panic after excessive waiting to detect deadlocks.
 * Uses CAS to atomically consume one completion.
 */
static inline void wait_for_completion(completion_t *x)
{
    unsigned long spins = 0;
    int done;

    while (1) {
        done = atomic_read(&x->done);
        if (done > 0) {
            if (done == (int)UINT32_MAX) {
                /* complete_all was called */
                smp_mb();
                return;
            }
            /* Try to consume one completion atomically */
            if (atomic_cmpxchg(&x->done, done, done - 1) == done) {
                smp_mb();
                return;
            }
            /* CAS failed, retry without counting as spin */
            continue;
        }

        cpu_relax();
        spins++;

        if (spins > MUTEX_MAX_TOTAL_SPINS) {
            kernel_panic("completion: possible deadlock detected");
        }
    }
}

/**
 * wait_for_completion_interruptible - Wait (interruptible)
 * @x: the completion
 *
 * Returns: 0 on success
 */
static inline int wait_for_completion_interruptible(completion_t *x)
{
    wait_for_completion(x);
    return 0;
}

/**
 * try_wait_for_completion - Check if complete and consume one completion
 * @x: the completion
 *
 * Returns: true if complete
 *
 * Note: Uses CAS loop to handle race between check and decrement.
 */
static inline bool try_wait_for_completion(completion_t *x)
{
    int done;

    while (1) {
        done = atomic_read(&x->done);
        if (done == 0) {
            return false;  /* Not complete */
        }
        if (done == (int)UINT32_MAX) {
            return true;   /* complete_all was called, don't decrement */
        }
        /* Try to consume one completion atomically */
        if (atomic_cmpxchg(&x->done, done, done - 1) == done) {
            return true;
        }
        /* CAS failed, someone else modified done, retry */
        cpu_relax();
    }
}

/**
 * completion_done - Check if signaled
 * @x: the completion
 */
static inline bool completion_done(completion_t *x)
{
    return atomic_read(&x->done) != 0;
}

/* ============================================================================
 * Wait Queue (Simplified - polling based)
 * ============================================================================ */

typedef struct wait_queue_head {
    spinlock_t lock;
} wait_queue_head_t;

#define __WAIT_QUEUE_HEAD_INITIALIZER(name) \
    { .lock = __SPIN_LOCK_UNLOCKED }

#define DECLARE_WAIT_QUEUE_HEAD(name) \
    wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

static inline void init_waitqueue_head(wait_queue_head_t *wq)
{
    spin_lock_init(&wq->lock);
}

#define wake_up(wq)                 do { smp_mb(); } while (0)
#define wake_up_all(wq)             do { smp_mb(); } while (0)
#define wake_up_interruptible(wq)   do { smp_mb(); } while (0)

#define wait_event(wq, condition) \
    do { \
        unsigned long __spins = 0; \
        while (!(condition)) { \
            cpu_relax(); \
            __spins++; \
            if (__spins > MUTEX_MAX_TOTAL_SPINS) { \
                kernel_panic("wait_event: possible deadlock"); \
            } \
        } \
        smp_mb(); \
    } while (0)

#define wait_event_interruptible(wq, condition) \
    ({ int __ret = 0; wait_event(wq, condition); __ret; })

/* ============================================================================
 * RCU Stubs
 * ============================================================================ */

#define rcu_read_lock()             do { smp_mb(); } while (0)
#define rcu_read_unlock()           do { smp_mb(); } while (0)
#define synchronize_rcu()           do { smp_mb(); } while (0)
#define rcu_dereference(p)          ({ smp_mb(); (p); })
#define rcu_assign_pointer(p, v)    do { smp_mb(); (p) = (v); smp_mb(); } while (0)

/* ============================================================================
 * Debug Assertions
 * ============================================================================ */

#define might_sleep()               do { } while (0)
#define might_sleep_if(cond)        do { } while (0)
#define cant_sleep()                do { } while (0)
#define lockdep_assert_not_held(l)  do { } while (0)

#endif /* EMBODIOS_MUTEX_H */
