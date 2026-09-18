/* EMBODIOS Test Framework - In-kernel unit testing */
#include <embodios/test.h>
#include <embodios/console.h>
#include <embodios/types.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* Architecture-specific I/O port operations for shutdown */
#ifdef __x86_64__
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
#endif

/* Maximum snapshot size in bytes */
#define MAX_SNAPSHOT_SIZE (1024 * 1024)  /* 1MB snapshot buffer */

/* QEMU shutdown ports */
#define QEMU_EXIT_PORT    0x604
#define QEMU_EXIT_SUCCESS 0x10  /* Exit code 0 */
#define QEMU_EXIT_FAILURE 0x11  /* Exit code 1 */

/* Memory snapshot structure for test isolation */
struct test_snapshot {
    void* data;
    size_t size;
    bool valid;
};

/* Test isolation state */
static struct {
    struct test_snapshot snapshot;
    bool isolation_enabled;
    void* snapshot_buffer;
} isolation_state = {
    .snapshot = {
        .data = NULL,
        .size = 0,
        .valid = false
    },
    .isolation_enabled = false,
    .snapshot_buffer = NULL
};

/* Test registry - linked list of all registered tests */
static struct test_case* test_registry_head = NULL;
static struct test_case* test_registry_tail = NULL;

/* Test statistics */
static struct test_stats current_stats = {0, 0, 0};

/* Setup/teardown hooks */
static test_hook_t setup_hook = NULL;
static test_hook_t teardown_hook = NULL;

/* Shutdown kernel after test completion
 * Performs platform-specific shutdown to exit QEMU cleanly
 * exit_code: 0 for success (all tests passed), 1 for failure
 */
static void kernel_shutdown(int exit_code)
{
    /* Disable interrupts before shutdown */
    arch_disable_interrupts();

    console_printf("\n[INFO] Shutting down kernel...\n");

#ifdef __x86_64__
    /* QEMU ISA debug exit device
     * Port 0x604: Exit status = (value >> 1)
     * Value 0x10 -> exit code 0 (success)
     * Value 0x11 -> exit code 1 (failure)
     */
    uint16_t qemu_exit_code = (exit_code == 0) ? QEMU_EXIT_SUCCESS : QEMU_EXIT_FAILURE;
    outw(QEMU_EXIT_PORT, qemu_exit_code);

    /* If QEMU debug exit didn't work, try ACPI shutdown (PM1a control block)
     * Most QEMU configurations use port 0x604, but some use 0xB004
     */
    outw(0xB004, 0x2000);

#elif defined(__aarch64__)
    /* ARM PSCI (Power State Coordination Interface) shutdown
     * PSCI call convention:
     *   X0 = function ID (0x84000008 for SYSTEM_OFF)
     *   Uses SMC (Secure Monitor Call) or HVC (Hypervisor Call)
     */
    register uint64_t x0 __asm__("x0") = 0x84000008;  /* PSCI_SYSTEM_OFF */
    __asm__ volatile("hvc #0" : : "r"(x0) : "memory");

    /* If HVC didn't work, try SMC */
    __asm__ volatile("smc #0" : : "r"(x0) : "memory");
#endif

    /* If shutdown methods failed, halt indefinitely */
    console_printf("[WARN] Shutdown failed, halting system\n");
    while (1) {
        arch_halt();
    }
}

/* Initialize test isolation system
 * Allocates buffer for memory snapshots
 */
static void test_isolation_init(void)
{
    if (isolation_state.isolation_enabled) {
        return;
    }

    /* Allocate snapshot buffer */
    isolation_state.snapshot_buffer = kmalloc(MAX_SNAPSHOT_SIZE);
    if (!isolation_state.snapshot_buffer) {
        console_printf("[WARN] Failed to allocate snapshot buffer, test isolation disabled\n");
        return;
    }

    isolation_state.isolation_enabled = true;
}

/* Create a memory snapshot before test execution
 * Saves current kernel state for potential restoration
 * Returns: true on success, false on failure
 */
static bool test_snapshot_save(void)
{
    if (!isolation_state.isolation_enabled) {
        return false;
    }

    /* Get PMM statistics to capture memory state */
    size_t available_pages = pmm_available_pages();
    size_t total_pages = pmm_total_pages();

    /* Calculate snapshot data size */
    size_t snapshot_size = sizeof(size_t) * 2;  /* available_pages + total_pages */

    if (snapshot_size > MAX_SNAPSHOT_SIZE) {
        console_printf("[WARN] Snapshot too large: %zu bytes\n", snapshot_size);
        return false;
    }

    /* Save snapshot data */
    size_t* snapshot_data = (size_t*)isolation_state.snapshot_buffer;
    snapshot_data[0] = available_pages;
    snapshot_data[1] = total_pages;

    /* Mark snapshot as valid */
    isolation_state.snapshot.data = isolation_state.snapshot_buffer;
    isolation_state.snapshot.size = snapshot_size;
    isolation_state.snapshot.valid = true;

    return true;
}

/* Restore kernel state from memory snapshot
 * Called after test execution to restore state
 * Returns: true on success, false on failure
 */
static bool test_snapshot_restore(void)
{
    if (!isolation_state.isolation_enabled || !isolation_state.snapshot.valid) {
        return false;
    }

    /* Get snapshot data */
    size_t* snapshot_data = (size_t*)isolation_state.snapshot.data;
    size_t saved_available_pages = snapshot_data[0];
    size_t saved_total_pages = snapshot_data[1];

    /* Verify total pages hasn't changed (sanity check) */
    size_t current_total = pmm_total_pages();
    if (current_total != saved_total_pages) {
        console_printf("[WARN] Total pages changed during test: %zu -> %zu\n",
                       saved_total_pages, current_total);
    }

    /* Check if pages were leaked during test */
    size_t current_available = pmm_available_pages();
    if (current_available != saved_available_pages) {
        size_t leaked_pages = saved_available_pages - current_available;
        console_printf("[WARN] Memory leak detected: %zu pages (%zu bytes)\n",
                       leaked_pages, leaked_pages * 4096);
    }

    /* Invalidate snapshot */
    isolation_state.snapshot.valid = false;

    return true;
}

/* Test registration function
 * Called by __attribute__((constructor)) functions generated by TEST() macro
 * Adds test to the global test registry linked list
 */
void test_register(struct test_case* test)
{
    if (!test) {
        return;
    }

    /* Initialize test's next pointer */
    test->next = NULL;

    /* Add to linked list */
    if (!test_registry_head) {
        /* First test */
        test_registry_head = test;
        test_registry_tail = test;
    } else {
        /* Append to tail */
        test_registry_tail->next = test;
        test_registry_tail = test;
    }
}

/* Set setup hook
 * Called by __attribute__((constructor)) function generated by TEST_SETUP() macro
 * Registers a function to be called before each test
 */
void test_set_setup_hook(test_hook_t setup)
{
    setup_hook = setup;
}

/* Set teardown hook
 * Called by __attribute__((constructor)) function generated by TEST_TEARDOWN() macro
 * Registers a function to be called after each test
 */
void test_set_teardown_hook(test_hook_t teardown)
{
    teardown_hook = teardown;
}

/* Run a single test case
 * Returns: TEST_PASS (0) on success, TEST_FAIL (1) on failure
 */
static int run_test(struct test_case* test)
{
    int result;
    bool snapshot_saved = false;

    if (!test || !test->func) {
        console_printf("[ERROR] Invalid test case\n");
        return TEST_FAIL;
    }

    console_printf("[TEST] %s (%s:%d)... ", test->name, test->file, test->line);

    /* Save memory snapshot before test execution */
    if (isolation_state.isolation_enabled) {
        snapshot_saved = test_snapshot_save();
    }

    /* Call setup hook if registered */
    if (setup_hook) {
        setup_hook();
    }

    /* Run the test function */
    result = test->func();

    /* Call teardown hook if registered */
    if (teardown_hook) {
        teardown_hook();
    }

    /* Restore memory snapshot after test execution */
    if (snapshot_saved) {
        test_snapshot_restore();
    }

    /* Update statistics */
    current_stats.total++;
    if (result == TEST_PASS) {
        current_stats.passed++;
        console_printf("[PASS]\n");
    } else {
        current_stats.failed++;
        console_printf("[FAIL]\n");
    }

    return result;
}

/* Run all registered tests
 * Returns: 0 if all tests passed, non-zero if any test failed
 */
int test_run_all(void)
{
    struct test_case* current;
    int total_failures = 0;

    /* Initialize test isolation system */
    test_isolation_init();

    /* Reset statistics */
    current_stats.total = 0;
    current_stats.passed = 0;
    current_stats.failed = 0;

    console_printf("\n");
    console_printf("========================================\n");
    console_printf("  EMBODIOS Kernel Test Framework\n");
    console_printf("========================================\n");
    console_printf("\n");

    /* Check if any tests are registered */
    if (!test_registry_head) {
        console_printf("[WARN] No tests registered\n");
        console_printf("\n");
        return 0;
    }

    /* Run all tests in the registry */
    for (current = test_registry_head; current != NULL; current = current->next) {
        if (run_test(current) != TEST_PASS) {
            total_failures++;
        }
    }

    /* Print summary */
    console_printf("\n");
    console_printf("========================================\n");
    console_printf("  Test Summary\n");
    console_printf("========================================\n");
    console_printf("  Total:  %d\n", current_stats.total);
    console_printf("  Passed: %d\n", current_stats.passed);
    console_printf("  Failed: %d\n", current_stats.failed);
    console_printf("========================================\n");
    console_printf("\n");

    /* Shutdown kernel with appropriate exit code */
    int exit_code = (total_failures > 0) ? 1 : 0;
    kernel_shutdown(exit_code);

    /* Should never reach here */
    return exit_code;
}

/* Run a specific test by name
 * Returns: 0 if test passed, 1 if test failed, -1 if test not found
 */
int test_run_single(const char* name)
{
    struct test_case* current;
    int result;

    if (!name) {
        console_printf("[ERROR] Invalid test name\n");
        return -1;
    }

    /* Initialize test isolation system */
    test_isolation_init();

    /* Reset statistics */
    current_stats.total = 0;
    current_stats.passed = 0;
    current_stats.failed = 0;

    console_printf("\n");
    console_printf("========================================\n");
    console_printf("  EMBODIOS Kernel Test Framework\n");
    console_printf("  Running: %s\n", name);
    console_printf("========================================\n");
    console_printf("\n");

    /* Search for test by name */
    for (current = test_registry_head; current != NULL; current = current->next) {
        if (strcmp(current->name, name) == 0) {
            result = run_test(current);

            /* Print summary */
            console_printf("\n");
            console_printf("========================================\n");
            console_printf("  Result: %s\n", result == TEST_PASS ? "PASS" : "FAIL");
            console_printf("========================================\n");
            console_printf("\n");

            /* Shutdown kernel with appropriate exit code */
            kernel_shutdown(result);

            /* Should never reach here */
            return result;
        }
    }

    /* Test not found */
    console_printf("[ERROR] Test '%s' not found\n", name);
    console_printf("\n");

    /* Shutdown with error code for test not found */
    kernel_shutdown(1);

    /* Should never reach here */
    return -1;
}

/* Get current test statistics
 * Copies current stats to provided structure
 */
void test_get_stats(struct test_stats* stats)
{
    if (!stats) {
        return;
    }

    stats->total = current_stats.total;
    stats->passed = current_stats.passed;
    stats->failed = current_stats.failed;
}
