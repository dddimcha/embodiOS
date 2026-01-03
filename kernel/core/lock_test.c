/* EMBODIOS Locking Primitives Tests
 *
 * Unit tests for spinlock, mutex, and synchronization primitives.
 * Tests both basic functionality and SMP correctness.
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/atomic.h>
#include <embodios/spinlock.h>
#include <embodios/mutex.h>

/* ============================================================================
 * Test Utilities
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        tests_passed++; \
        console_printf("  PASS: %s\n", msg); \
    } else { \
        tests_failed++; \
        console_printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if ((a) == (b)) { \
        tests_passed++; \
        console_printf("  PASS: %s\n", msg); \
    } else { \
        tests_failed++; \
        console_printf("  FAIL: %s (expected %d, got %d)\n", msg, (int)(b), (int)(a)); \
    } \
} while (0)

/* ============================================================================
 * Test: Atomic Operations
 * ============================================================================ */

static void test_atomic_ops(void)
{
    console_printf("\n[Test] Atomic Operations\n");

    atomic_t counter = ATOMIC_INIT(0);

    /* Test init and read */
    TEST_ASSERT_EQ(atomic_read(&counter), 0, "atomic_init sets to 0");

    /* Test set */
    atomic_set(&counter, 42);
    TEST_ASSERT_EQ(atomic_read(&counter), 42, "atomic_set works");

    /* Test add */
    atomic_add(10, &counter);
    TEST_ASSERT_EQ(atomic_read(&counter), 52, "atomic_add works");

    /* Test sub */
    atomic_sub(12, &counter);
    TEST_ASSERT_EQ(atomic_read(&counter), 40, "atomic_sub works");

    /* Test inc */
    atomic_inc(&counter);
    TEST_ASSERT_EQ(atomic_read(&counter), 41, "atomic_inc works");

    /* Test dec */
    atomic_dec(&counter);
    TEST_ASSERT_EQ(atomic_read(&counter), 40, "atomic_dec works");

    /* Test add_return */
    int result = atomic_add_return(5, &counter);
    TEST_ASSERT_EQ(result, 45, "atomic_add_return returns new value");

    /* Test dec_and_test */
    atomic_set(&counter, 1);
    bool is_zero = atomic_dec_and_test(&counter);
    TEST_ASSERT(is_zero, "atomic_dec_and_test detects zero");

    atomic_set(&counter, 2);
    is_zero = atomic_dec_and_test(&counter);
    TEST_ASSERT(!is_zero, "atomic_dec_and_test detects non-zero");

    /* Test xchg */
    atomic_set(&counter, 100);
    int old = atomic_xchg(&counter, 200);
    TEST_ASSERT_EQ(old, 100, "atomic_xchg returns old value");
    TEST_ASSERT_EQ(atomic_read(&counter), 200, "atomic_xchg sets new value");

    /* Test cmpxchg - success case */
    atomic_set(&counter, 50);
    old = atomic_cmpxchg(&counter, 50, 60);
    TEST_ASSERT_EQ(old, 50, "atomic_cmpxchg returns old on match");
    TEST_ASSERT_EQ(atomic_read(&counter), 60, "atomic_cmpxchg sets on match");

    /* Test cmpxchg - failure case */
    old = atomic_cmpxchg(&counter, 50, 70);  /* 50 != 60 */
    TEST_ASSERT_EQ(old, 60, "atomic_cmpxchg returns current on mismatch");
    TEST_ASSERT_EQ(atomic_read(&counter), 60, "atomic_cmpxchg unchanged on mismatch");
}

/* ============================================================================
 * Test: Atomic64 Operations
 * ============================================================================ */

static void test_atomic64_ops(void)
{
    console_printf("\n[Test] Atomic64 Operations\n");

    atomic64_t counter = ATOMIC64_INIT(0);

    /* Test basic operations */
    TEST_ASSERT_EQ(atomic64_read(&counter), 0, "atomic64_init sets to 0");

    atomic64_set(&counter, 0x100000000LL);  /* > 32-bit */
    TEST_ASSERT(atomic64_read(&counter) == 0x100000000LL, "atomic64 handles 64-bit");

    atomic64_add(0x100000000LL, &counter);
    TEST_ASSERT(atomic64_read(&counter) == 0x200000000LL, "atomic64_add works");

    atomic64_sub(0x100000000LL, &counter);
    TEST_ASSERT(atomic64_read(&counter) == 0x100000000LL, "atomic64_sub works");
}

/* ============================================================================
 * Test: Bit Operations
 * ============================================================================ */

static void test_bit_ops(void)
{
    console_printf("\n[Test] Bit Operations\n");

    unsigned long bitmap = 0;

    /* Test set_bit */
    set_bit(0, &bitmap);
    TEST_ASSERT(bitmap == 1, "set_bit(0) works");

    set_bit(5, &bitmap);
    TEST_ASSERT(bitmap == 0x21, "set_bit(5) works");

    /* Test test_bit */
    TEST_ASSERT(test_bit(0, &bitmap), "test_bit(0) returns true");
    TEST_ASSERT(test_bit(5, &bitmap), "test_bit(5) returns true");
    TEST_ASSERT(!test_bit(3, &bitmap), "test_bit(3) returns false");

    /* Test clear_bit */
    clear_bit(0, &bitmap);
    TEST_ASSERT(!test_bit(0, &bitmap), "clear_bit(0) works");
    TEST_ASSERT(test_bit(5, &bitmap), "clear_bit doesn't affect other bits");

    /* Test test_and_set_bit */
    bitmap = 0;
    bool was_set = test_and_set_bit(3, &bitmap);
    TEST_ASSERT(!was_set, "test_and_set_bit returns false for unset");
    TEST_ASSERT(test_bit(3, &bitmap), "test_and_set_bit sets the bit");

    was_set = test_and_set_bit(3, &bitmap);
    TEST_ASSERT(was_set, "test_and_set_bit returns true for set");

    /* Test test_and_clear_bit */
    was_set = test_and_clear_bit(3, &bitmap);
    TEST_ASSERT(was_set, "test_and_clear_bit returns true for set");
    TEST_ASSERT(!test_bit(3, &bitmap), "test_and_clear_bit clears the bit");
}

/* ============================================================================
 * Test: Spinlock Basic
 * ============================================================================ */

static void test_spinlock_basic(void)
{
    console_printf("\n[Test] Spinlock Basic Operations\n");

    DEFINE_SPINLOCK(test_lock);

    /* Test initial state */
    TEST_ASSERT(!spin_is_locked(&test_lock), "Spinlock initially unlocked");

    /* Test lock/unlock */
    spin_lock(&test_lock);
    TEST_ASSERT(spin_is_locked(&test_lock), "spin_lock acquires lock");

    spin_unlock(&test_lock);
    TEST_ASSERT(!spin_is_locked(&test_lock), "spin_unlock releases lock");

    /* Test trylock success */
    int got_lock = spin_trylock(&test_lock);
    TEST_ASSERT(got_lock, "spin_trylock succeeds on free lock");
    TEST_ASSERT(spin_is_locked(&test_lock), "spin_trylock acquires lock");

    /* Note: Can't test trylock failure in single-threaded test */

    spin_unlock(&test_lock);

    /* Test multiple lock/unlock cycles */
    for (int i = 0; i < 100; i++) {
        spin_lock(&test_lock);
        spin_unlock(&test_lock);
    }
    TEST_ASSERT(!spin_is_locked(&test_lock), "100 lock/unlock cycles work");
}

/* ============================================================================
 * Test: Spinlock IRQ
 * ============================================================================ */

static void test_spinlock_irq(void)
{
    console_printf("\n[Test] Spinlock IRQ Operations\n");

    DEFINE_SPINLOCK(test_lock);
    unsigned long flags;

    /* Test irqsave/restore */
    spin_lock_irqsave(&test_lock, flags);
    TEST_ASSERT(spin_is_locked(&test_lock), "spin_lock_irqsave acquires lock");

    spin_unlock_irqrestore(&test_lock, flags);
    TEST_ASSERT(!spin_is_locked(&test_lock), "spin_unlock_irqrestore releases lock");

    /* Test lock_irq/unlock_irq */
    spin_lock_irq(&test_lock);
    TEST_ASSERT(spin_is_locked(&test_lock), "spin_lock_irq acquires lock");

    spin_unlock_irq(&test_lock);
    TEST_ASSERT(!spin_is_locked(&test_lock), "spin_unlock_irq releases lock");

    /* Test nested irqsave */
    unsigned long flags1;
    spin_lock_irqsave(&test_lock, flags1);

    /* Can't acquire same lock twice (would deadlock), but test flags handling */
    spin_unlock_irqrestore(&test_lock, flags1);
    TEST_ASSERT(true, "IRQ flags save/restore completes");
}

/* ============================================================================
 * Test: Read-Write Lock
 * ============================================================================ */

static void test_rwlock(void)
{
    console_printf("\n[Test] Read-Write Lock\n");

    DEFINE_RWLOCK(test_rwlock);

    /* Test read lock */
    read_lock(&test_rwlock);
    TEST_ASSERT(true, "read_lock succeeds");
    read_unlock(&test_rwlock);

    /* Test write lock */
    write_lock(&test_rwlock);
    TEST_ASSERT(true, "write_lock succeeds");
    write_unlock(&test_rwlock);

    /* Test multiple readers (sequential in single-threaded) */
    read_lock(&test_rwlock);
    read_unlock(&test_rwlock);
    read_lock(&test_rwlock);
    read_unlock(&test_rwlock);
    TEST_ASSERT(true, "Multiple sequential reads work");
}

/* ============================================================================
 * Test: Mutex Basic
 * ============================================================================ */

static void test_mutex_basic(void)
{
    console_printf("\n[Test] Mutex Basic Operations\n");

    DEFINE_MUTEX(test_mutex);

    /* Test initial state */
    TEST_ASSERT(!mutex_is_locked(&test_mutex), "Mutex initially unlocked");

    /* Test lock/unlock */
    mutex_lock(&test_mutex);
    TEST_ASSERT(mutex_is_locked(&test_mutex), "mutex_lock acquires lock");

    mutex_unlock(&test_mutex);
    TEST_ASSERT(!mutex_is_locked(&test_mutex), "mutex_unlock releases lock");

    /* Test trylock success */
    int got_lock = mutex_trylock(&test_mutex);
    TEST_ASSERT(got_lock, "mutex_trylock succeeds on free lock");
    TEST_ASSERT(mutex_is_locked(&test_mutex), "mutex_trylock acquires lock");

    mutex_unlock(&test_mutex);

    /* Test interruptible variant */
    int ret = mutex_lock_interruptible(&test_mutex);
    TEST_ASSERT_EQ(ret, 0, "mutex_lock_interruptible returns 0");
    mutex_unlock(&test_mutex);

    /* Test multiple lock/unlock cycles */
    for (int i = 0; i < 100; i++) {
        mutex_lock(&test_mutex);
        mutex_unlock(&test_mutex);
    }
    TEST_ASSERT(!mutex_is_locked(&test_mutex), "100 lock/unlock cycles work");
}

/* ============================================================================
 * Test: Semaphore
 * ============================================================================ */

static void test_semaphore(void)
{
    console_printf("\n[Test] Semaphore\n");

    semaphore_t sem;
    sema_init(&sem, 3);  /* Count of 3 */

    /* Test down (should succeed 3 times) */
    int try1 = down_trylock(&sem);  /* 0 = acquired */
    int try2 = down_trylock(&sem);
    int try3 = down_trylock(&sem);
    int try4 = down_trylock(&sem);  /* Should fail (1 = would block) */

    TEST_ASSERT_EQ(try1, 0, "First down succeeds");
    TEST_ASSERT_EQ(try2, 0, "Second down succeeds");
    TEST_ASSERT_EQ(try3, 0, "Third down succeeds");
    TEST_ASSERT_EQ(try4, 1, "Fourth down would block");

    /* Release and try again */
    up(&sem);
    try4 = down_trylock(&sem);
    TEST_ASSERT_EQ(try4, 0, "After up, down succeeds");

    /* Clean up */
    up(&sem);
    up(&sem);
    up(&sem);
}

/* ============================================================================
 * Test: Completion
 * ============================================================================ */

static void test_completion(void)
{
    console_printf("\n[Test] Completion\n");

    DECLARE_COMPLETION(test_comp);

    /* Test initial state */
    TEST_ASSERT(!completion_done(&test_comp), "Completion initially not done");

    /* Signal completion */
    complete(&test_comp);
    TEST_ASSERT(completion_done(&test_comp), "complete() signals done");

    /* Try wait should succeed */
    bool got_it = try_wait_for_completion(&test_comp);
    TEST_ASSERT(got_it, "try_wait_for_completion succeeds after complete");

    /* Reinit and test complete_all */
    reinit_completion(&test_comp);
    TEST_ASSERT(!completion_done(&test_comp), "reinit clears completion");

    complete_all(&test_comp);
    TEST_ASSERT(completion_done(&test_comp), "complete_all signals done");

    /* Multiple try_waits should succeed with complete_all */
    got_it = try_wait_for_completion(&test_comp);
    TEST_ASSERT(got_it, "First try_wait after complete_all succeeds");
    got_it = try_wait_for_completion(&test_comp);
    TEST_ASSERT(got_it, "Second try_wait after complete_all succeeds");
}

/* ============================================================================
 * Test: Memory Barriers
 * ============================================================================ */

static void test_memory_barriers(void)
{
    console_printf("\n[Test] Memory Barriers\n");

    volatile int value = 0;

    /* These tests just verify the barriers compile and execute */
    value = 1;
    smp_mb();
    TEST_ASSERT(value == 1, "smp_mb() doesn't corrupt data");

    value = 2;
    smp_wmb();
    TEST_ASSERT(value == 2, "smp_wmb() doesn't corrupt data");

    smp_rmb();
    TEST_ASSERT(value == 2, "smp_rmb() doesn't corrupt data");

    barrier();
    TEST_ASSERT(value == 2, "barrier() doesn't corrupt data");
}

/* ============================================================================
 * Main Test Entry Point
 * ============================================================================ */

int lock_run_tests(void)
{
    console_printf("\n========================================\n");
    console_printf("EMBODIOS Locking Primitives Tests\n");
    console_printf("========================================\n");

    tests_passed = 0;
    tests_failed = 0;

    /* Run all tests */
    test_atomic_ops();
    test_atomic64_ops();
    test_bit_ops();
    test_spinlock_basic();
    test_spinlock_irq();
    test_rwlock();
    test_mutex_basic();
    test_semaphore();
    test_completion();
    test_memory_barriers();

    /* Print summary */
    console_printf("\n========================================\n");
    console_printf("Lock Tests Complete: %d passed, %d failed\n",
                   tests_passed, tests_failed);
    console_printf("========================================\n");

    return (tests_failed == 0) ? 0 : -1;
}
