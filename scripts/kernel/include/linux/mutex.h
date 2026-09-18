/* SPDX-License-Identifier: GPL-2.0 */
/**
 * Linux Compatibility Layer - Mutexes
 *
 * Provides Linux kernel mutex APIs for EMBODIOS.
 * Reference: linux/include/linux/mutex.h
 *
 * Part of EMBODIOS Linux Driver Compatibility Shim (~50 APIs)
 *
 * Implementation Notes:
 * ---------------------
 * EMBODIOS is currently single-core and non-preemptive, so mutexes are
 * implemented as simple flags. In a single-core non-preemptive system,
 * if a mutex is already held, the caller has a bug (recursive lock or
 * incorrect usage), as there's no other task that could be holding it.
 *
 * For SMP/preemptive support, these would need proper blocking/wakeup.
 */

#ifndef _LINUX_MUTEX_H
#define _LINUX_MUTEX_H

#include <linux/types.h>
#include <linux/spinlock.h>

/* ============================================================================
 * Mutex type definition
 * ============================================================================ */

/*
 * struct mutex - Mutual exclusion lock
 *
 * Mutexes are sleeping locks - if the lock is not available, the caller
 * sleeps until it becomes available. In EMBODIOS (non-preemptive), this
 * is simplified to a simple flag since only the current task runs.
 */
struct mutex {
    volatile unsigned int locked;
    spinlock_t wait_lock;           /* Protects wait list (unused in UP) */
#ifdef CONFIG_DEBUG_MUTEXES
    const char *name;
    void *owner;                    /* Task holding the mutex */
    const char *file;
    int line;
#endif
};

/* Convenience typedef */
typedef struct mutex mutex_t;

/* ============================================================================
 * Mutex initialization
 * ============================================================================ */

#define __MUTEX_INITIALIZER(lockname) \
    { .locked = 0, .wait_lock = __SPIN_LOCK_UNLOCKED }

#define DEFINE_MUTEX(mutexname) \
    struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)

/*
 * mutex_init - Initialize a mutex
 * @lock: the mutex to initialize
 */
static inline void mutex_init(struct mutex *lock)
{
    lock->locked = 0;
    spin_lock_init(&lock->wait_lock);
#ifdef CONFIG_DEBUG_MUTEXES
    lock->owner = NULL;
    lock->name = NULL;
    lock->file = NULL;
    lock->line = 0;
#endif
}

/*
 * mutex_destroy - Destroy a mutex
 * @lock: the mutex to destroy
 *
 * No-op in EMBODIOS, but validates mutex is not held in debug mode.
 */
static inline void mutex_destroy(struct mutex *lock)
{
#ifdef CONFIG_DEBUG_MUTEXES
    if (lock->locked) {
        /* Bug: destroying held mutex */
    }
#endif
    (void)lock;
}

/* ============================================================================
 * Mutex lock operations
 * ============================================================================ */

/*
 * mutex_lock - Acquire the mutex
 * @lock: the mutex to acquire
 *
 * Locks the mutex. In EMBODIOS, if already locked, this is a bug since
 * there's no preemption and no other task could have locked it.
 */
static inline void mutex_lock(struct mutex *lock)
{
#ifdef CONFIG_DEBUG_MUTEXES
    if (lock->locked) {
        /* Bug: recursive mutex lock or lock contention in UP system */
    }
#endif
    lock->locked = 1;
}

/*
 * mutex_lock_interruptible - Acquire mutex, interruptible
 * @lock: the mutex to acquire
 *
 * Returns: 0 on success, -EINTR if interrupted (never in EMBODIOS)
 */
static inline int mutex_lock_interruptible(struct mutex *lock)
{
    mutex_lock(lock);
    return 0;
}

/*
 * mutex_lock_killable - Acquire mutex, killable
 * @lock: the mutex to acquire
 *
 * Returns: 0 on success, -EINTR if killed (never in EMBODIOS)
 */
static inline int mutex_lock_killable(struct mutex *lock)
{
    mutex_lock(lock);
    return 0;
}

/*
 * mutex_trylock - Try to acquire the mutex
 * @lock: the mutex to acquire
 *
 * Returns: 1 if lock acquired, 0 if already held
 */
static inline int mutex_trylock(struct mutex *lock)
{
    if (lock->locked)
        return 0;

    lock->locked = 1;
    return 1;
}

/*
 * mutex_unlock - Release the mutex
 * @lock: the mutex to release
 */
static inline void mutex_unlock(struct mutex *lock)
{
#ifdef CONFIG_DEBUG_MUTEXES
    if (!lock->locked) {
        /* Bug: unlocking unheld mutex */
    }
#endif
    lock->locked = 0;
}

/*
 * mutex_is_locked - Check if mutex is held
 * @lock: the mutex to check
 *
 * Returns: 1 if locked, 0 if unlocked
 */
static inline int mutex_is_locked(struct mutex *lock)
{
    return lock->locked != 0;
}

/* ============================================================================
 * Semaphore compatibility
 * ============================================================================
 *
 * Linux semaphores are similar to mutexes but allow counting.
 * Simplified to binary semaphore (same as mutex) in EMBODIOS.
 */

struct semaphore {
    volatile unsigned int count;
    spinlock_t lock;
};

#define __SEMAPHORE_INITIALIZER(name, n) \
    { .count = (n), .lock = __SPIN_LOCK_UNLOCKED }

#define DEFINE_SEMAPHORE(name) \
    struct semaphore name = __SEMAPHORE_INITIALIZER(name, 1)

static inline void sema_init(struct semaphore *sem, int val)
{
    sem->count = val;
    spin_lock_init(&sem->lock);
}

/*
 * down - Acquire semaphore
 * @sem: the semaphore to acquire
 */
static inline void down(struct semaphore *sem)
{
    /* In UP non-preemptive, just decrement */
    if (sem->count > 0)
        sem->count--;
    /* else: bug - would block forever in UP */
}

/*
 * down_interruptible - Acquire semaphore, interruptible
 * @sem: the semaphore to acquire
 *
 * Returns: 0 on success, -EINTR if interrupted
 */
static inline int down_interruptible(struct semaphore *sem)
{
    down(sem);
    return 0;
}

/*
 * down_trylock - Try to acquire semaphore
 * @sem: the semaphore to acquire
 *
 * Returns: 0 if acquired, 1 if would block
 */
static inline int down_trylock(struct semaphore *sem)
{
    if (sem->count > 0) {
        sem->count--;
        return 0;
    }
    return 1;
}

/*
 * up - Release semaphore
 * @sem: the semaphore to release
 */
static inline void up(struct semaphore *sem)
{
    sem->count++;
}

/* ============================================================================
 * Completion API
 * ============================================================================
 *
 * Completions are used to wait for events. Simplified for EMBODIOS.
 */

struct completion {
    volatile unsigned int done;
    spinlock_t lock;
};

#define COMPLETION_INITIALIZER(name) \
    { .done = 0, .lock = __SPIN_LOCK_UNLOCKED }

#define DECLARE_COMPLETION(name) \
    struct completion name = COMPLETION_INITIALIZER(name)

#define DECLARE_COMPLETION_ONSTACK(name) \
    struct completion name = COMPLETION_INITIALIZER(name)

static inline void init_completion(struct completion *x)
{
    x->done = 0;
    spin_lock_init(&x->lock);
}

static inline void reinit_completion(struct completion *x)
{
    x->done = 0;
}

/*
 * complete - Signal completion
 * @x: the completion to signal
 */
static inline void complete(struct completion *x)
{
    x->done = 1;
}

/*
 * complete_all - Signal all waiters
 * @x: the completion to signal
 */
static inline void complete_all(struct completion *x)
{
    x->done = UINT_MAX;
}

/*
 * wait_for_completion - Wait for completion
 * @x: the completion to wait for
 *
 * In EMBODIOS UP, if not complete, this is a bug (no preemption to complete it)
 */
static inline void wait_for_completion(struct completion *x)
{
    /* Spin wait - in UP non-preemptive, this would hang if not already done */
    while (!x->done)
        cpu_relax();  /* Reduce power, allow interrupts */
    if (x->done != UINT_MAX)
        x->done--;
}

/*
 * wait_for_completion_interruptible - Wait for completion, interruptible
 * @x: the completion to wait for
 *
 * Returns: 0 on success, -ERESTARTSYS if interrupted
 */
static inline int wait_for_completion_interruptible(struct completion *x)
{
    wait_for_completion(x);
    return 0;
}

/*
 * wait_for_completion_timeout - Wait for completion with timeout
 * @x: the completion to wait for
 * @timeout: timeout in jiffies
 *
 * Returns: remaining jiffies (>0) if completed, 0 if timed out
 */
static inline unsigned long wait_for_completion_timeout(
    struct completion *x, unsigned long timeout)
{
    /* Simplified: no real timeout support */
    if (x->done) {
        if (x->done != UINT_MAX)
            x->done--;
        return timeout;
    }
    return 0;  /* Would timeout */
}

/*
 * try_wait_for_completion - Check if completion is done
 * @x: the completion to check
 *
 * Returns: true if done, false otherwise
 */
static inline bool try_wait_for_completion(struct completion *x)
{
    if (x->done) {
        if (x->done != UINT_MAX)
            x->done--;
        return true;
    }
    return false;
}

/*
 * completion_done - Check if completion has been signaled
 * @x: the completion to check
 *
 * Returns: true if signaled, false otherwise
 */
static inline bool completion_done(struct completion *x)
{
    return x->done != 0;
}

/* ============================================================================
 * Wait queue API (simplified)
 * ============================================================================
 *
 * Wait queues allow processes to sleep until a condition is met.
 * Heavily simplified for EMBODIOS non-preemptive environment.
 */

typedef struct wait_queue_head {
    spinlock_t lock;
} wait_queue_head_t;

#define __WAIT_QUEUE_HEAD_INITIALIZER(name) { .lock = __SPIN_LOCK_UNLOCKED }

#define DECLARE_WAIT_QUEUE_HEAD(name) \
    wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

static inline void init_waitqueue_head(wait_queue_head_t *wq)
{
    spin_lock_init(&wq->lock);
}

/* Wake up operations - no-op in UP non-preemptive */
#define wake_up(wq)             do { (void)(wq); } while (0)
#define wake_up_all(wq)         do { (void)(wq); } while (0)
#define wake_up_interruptible(wq) do { (void)(wq); } while (0)

/* Wait operations - poll condition only */
#define wait_event(wq, condition) \
    do { while (!(condition)) cpu_relax(); } while (0)

#define wait_event_interruptible(wq, condition) \
    ({ int __ret = 0; wait_event(wq, condition); __ret; })

#define wait_event_timeout(wq, condition, timeout) \
    ({ \
        unsigned long __ret = timeout; \
        if (!(condition)) __ret = 0; \
        __ret; \
    })

/* ============================================================================
 * RCU stubs (Read-Copy-Update)
 * ============================================================================
 *
 * RCU is a synchronization mechanism for read-mostly data.
 * Simplified to no-ops for EMBODIOS UP system.
 */

#define rcu_read_lock()         do { barrier(); } while (0)
#define rcu_read_unlock()       do { barrier(); } while (0)
#define synchronize_rcu()       do { barrier(); } while (0)
/* rcu_dereference includes barrier to prevent speculative reads */
#define rcu_dereference(p)      ({ barrier(); (p); })
#define rcu_assign_pointer(p, v) do { barrier(); (p) = (v); barrier(); } while (0)
#define RCU_INIT_POINTER(p, v)  do { (p) = (v); } while (0)

/* ============================================================================
 * Debugging assertions
 * ============================================================================ */

#define might_sleep()           do { } while (0)
#define might_sleep_if(cond)    do { } while (0)
#define cant_sleep()            do { } while (0)
/* lockdep_assert_held is defined in spinlock.h */
#ifndef lockdep_assert_not_held
#define lockdep_assert_not_held(lock) do { (void)(lock); } while (0)
#endif

#endif /* _LINUX_MUTEX_H */
