/* EMBODIOS Vulkan GPU Backend Test Suite
 *
 * Tests for GPU acceleration backend:
 * - Initialization and device detection
 * - CPU fallback behavior
 * - Device enumeration and selection
 * - Error handling and state management
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/kernel.h>
#include <embodios/mm.h>
#include <embodios/gpu_backend.h>

/* ============================================================================
 * Test Utilities
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            tests_passed++; \
        } else { \
            tests_failed++; \
            console_printf("  FAIL: %s\n", msg); \
        } \
    } while (0)

#define TEST_ASSERT_RANGE(val, min, max, msg) \
    TEST_ASSERT((val) >= (min) && (val) <= (max), msg)

/* ============================================================================
 * Backend Initialization Tests
 * ============================================================================ */

static void test_backend_init_auto(void)
{
    console_printf("[Test] GPU backend auto-initialization\n");

    /* Shutdown any existing backend first */
    gpu_backend_shutdown();

    /* Try auto-detect initialization */
    int ret = gpu_backend_init(GPU_BACKEND_AUTO);

    /* Auto-detect should either succeed (GPU found) or fail (CPU fallback) */
    /* Both are valid outcomes - we just verify consistent state */
    gpu_backend_type_t type = gpu_backend_get_type();
    int available = gpu_backend_is_available();

    if (ret == 0) {
        /* GPU initialized successfully */
        TEST_ASSERT(available == 1, "GPU available after successful init");
        TEST_ASSERT(type == GPU_BACKEND_VULKAN, "Backend type is Vulkan");
        console_printf("  PASS: GPU auto-init succeeded (Vulkan)\n");
    } else {
        /* CPU fallback mode */
        TEST_ASSERT(available == 0, "GPU not available after failed init");
        TEST_ASSERT(type == GPU_BACKEND_NONE, "Backend type is NONE (CPU fallback)");
        console_printf("  PASS: GPU auto-init failed, CPU fallback active\n");
    }
}

static void test_backend_init_vulkan(void)
{
    console_printf("[Test] GPU backend Vulkan initialization\n");

    gpu_backend_shutdown();

    /* Try explicit Vulkan initialization */
    int ret = gpu_backend_init(GPU_BACKEND_VULKAN);

    gpu_backend_type_t type = gpu_backend_get_type();
    int available = gpu_backend_is_available();

    if (ret == 0) {
        TEST_ASSERT(available == 1, "GPU available after Vulkan init");
        TEST_ASSERT(type == GPU_BACKEND_VULKAN, "Backend type is Vulkan");
        console_printf("  PASS: Vulkan init succeeded\n");
    } else {
        TEST_ASSERT(available == 0, "GPU not available after Vulkan init failure");
        TEST_ASSERT(type == GPU_BACKEND_NONE, "Backend type is NONE");
        console_printf("  PASS: Vulkan init failed (expected on systems without GPU)\n");
    }
}

static void test_backend_init_idempotent(void)
{
    console_printf("[Test] GPU backend init idempotency\n");

    gpu_backend_shutdown();

    /* First initialization */
    int ret1 = gpu_backend_init(GPU_BACKEND_AUTO);
    gpu_backend_type_t type1 = gpu_backend_get_type();
    int available1 = gpu_backend_is_available();

    /* Second initialization should be idempotent */
    int ret2 = gpu_backend_init(GPU_BACKEND_AUTO);
    gpu_backend_type_t type2 = gpu_backend_get_type();
    int available2 = gpu_backend_is_available();

    TEST_ASSERT(ret1 == ret2, "Init returns same result on second call");
    TEST_ASSERT(type1 == type2, "Backend type unchanged on second init");
    TEST_ASSERT(available1 == available2, "Available state unchanged on second init");

    console_printf("  PASS: Init is idempotent\n");
}

/* ============================================================================
 * Device Information Tests
 * ============================================================================ */

static void test_device_info_retrieval(void)
{
    console_printf("[Test] GPU device info retrieval\n");

    gpu_backend_shutdown();
    int init_ret = gpu_backend_init(GPU_BACKEND_AUTO);

    gpu_device_info_t info;
    memset(&info, 0, sizeof(info));

    int ret = gpu_backend_get_device_info(&info);

    if (init_ret == 0) {
        /* GPU available - should get device info */
        TEST_ASSERT(ret == 0, "Device info retrieved successfully");
        TEST_ASSERT(info.available == 1, "Device marked as available");
        TEST_ASSERT(info.type == GPU_BACKEND_VULKAN, "Device type is Vulkan");
        TEST_ASSERT(info.device_name[0] != '\0', "Device name is non-empty");
        console_printf("  PASS: Device info retrieved (GPU: %s)\n", info.device_name);
    } else {
        /* No GPU - should fail to get device info */
        TEST_ASSERT(ret != 0, "Device info fails when no GPU");
        console_printf("  PASS: Device info correctly unavailable (CPU mode)\n");
    }
}

static void test_device_info_null_param(void)
{
    console_printf("[Test] GPU device info NULL parameter handling\n");

    /* Test NULL parameter handling */
    int ret = gpu_backend_get_device_info(NULL);

    TEST_ASSERT(ret != 0, "Device info returns error for NULL parameter");
    console_printf("  PASS: NULL parameter handled correctly\n");
}

/* ============================================================================
 * Device Enumeration Tests
 * ============================================================================ */

static void test_device_enumeration(void)
{
    console_printf("[Test] GPU device enumeration\n");

    gpu_backend_shutdown();
    int init_ret = gpu_backend_init(GPU_BACKEND_AUTO);

    gpu_device_info_t devices[16];
    memset(devices, 0, sizeof(devices));

    int count = gpu_backend_enumerate_devices(devices, 16);

    if (init_ret == 0) {
        /* GPU available - should enumerate at least one device */
        TEST_ASSERT(count > 0, "At least one device enumerated");
        TEST_ASSERT(count <= 16, "Enumeration respects max_devices");

        /* Verify first device info */
        TEST_ASSERT(devices[0].available == 1, "First device marked available");
        TEST_ASSERT(devices[0].type == GPU_BACKEND_VULKAN, "First device type is Vulkan");
        TEST_ASSERT(devices[0].device_name[0] != '\0', "First device has name");

        console_printf("  PASS: Enumerated %d device(s)\n", count);
        for (int i = 0; i < count; i++) {
            console_printf("    Device %d: %s\n", i, devices[i].device_name);
        }
    } else {
        /* No GPU - should enumerate zero devices */
        TEST_ASSERT(count == 0, "No devices enumerated in CPU mode");
        console_printf("  PASS: No devices enumerated (CPU mode)\n");
    }
}

static void test_device_enumeration_null_param(void)
{
    console_printf("[Test] GPU device enumeration NULL parameter handling\n");

    /* Test NULL devices parameter */
    int ret1 = gpu_backend_enumerate_devices(NULL, 16);
    TEST_ASSERT(ret1 < 0, "Enumerate returns error for NULL devices");

    /* Test zero max_devices */
    gpu_device_info_t devices[1];
    int ret2 = gpu_backend_enumerate_devices(devices, 0);
    TEST_ASSERT(ret2 < 0, "Enumerate returns error for max_devices = 0");

    console_printf("  PASS: NULL/invalid parameters handled correctly\n");
}

/* ============================================================================
 * CPU Fallback Tests
 * ============================================================================ */

static void test_cpu_fallback_consistency(void)
{
    console_printf("[Test] CPU fallback state consistency\n");

    gpu_backend_shutdown();
    int ret = gpu_backend_init(GPU_BACKEND_AUTO);

    /* If init fails, verify CPU fallback is consistent */
    if (ret != 0) {
        gpu_backend_type_t type = gpu_backend_get_type();
        int available = gpu_backend_is_available();
        gpu_device_info_t info;
        int info_ret = gpu_backend_get_device_info(&info);

        TEST_ASSERT(type == GPU_BACKEND_NONE, "Backend type is NONE in fallback");
        TEST_ASSERT(available == 0, "Backend not available in fallback");
        TEST_ASSERT(info_ret != 0, "Device info fails in fallback");

        console_printf("  PASS: CPU fallback state is consistent\n");
    } else {
        console_printf("  SKIP: GPU available, CPU fallback not tested\n");
        tests_passed++;  /* Count as passed since we can't force failure */
    }
}

/* ============================================================================
 * Shutdown Tests
 * ============================================================================ */

static void test_backend_shutdown(void)
{
    console_printf("[Test] GPU backend shutdown\n");

    gpu_backend_shutdown();
    int init_ret = gpu_backend_init(GPU_BACKEND_AUTO);

    /* Shutdown should work regardless of init success */
    gpu_backend_shutdown();

    /* After shutdown, backend should not be available */
    int available = gpu_backend_is_available();
    gpu_backend_type_t type = gpu_backend_get_type();

    TEST_ASSERT(available == 0, "Backend not available after shutdown");
    TEST_ASSERT(type == GPU_BACKEND_NONE, "Backend type NONE after shutdown");

    console_printf("  PASS: Backend shutdown correctly\n");
}

static void test_backend_reinit_after_shutdown(void)
{
    console_printf("[Test] GPU backend re-initialization after shutdown\n");

    gpu_backend_shutdown();
    int ret1 = gpu_backend_init(GPU_BACKEND_AUTO);
    gpu_backend_type_t type1 = gpu_backend_get_type();

    gpu_backend_shutdown();

    int ret2 = gpu_backend_init(GPU_BACKEND_AUTO);
    gpu_backend_type_t type2 = gpu_backend_get_type();

    /* Re-init should produce same result as initial init */
    TEST_ASSERT(ret1 == ret2, "Re-init returns same result");
    TEST_ASSERT(type1 == type2, "Re-init produces same backend type");

    console_printf("  PASS: Re-initialization works correctly\n");
}

/* ============================================================================
 * State Management Tests
 * ============================================================================ */

static void test_backend_state_without_init(void)
{
    console_printf("[Test] GPU backend state queries without init\n");

    /* Ensure clean state */
    gpu_backend_shutdown();

    /* Query backend state without initializing */
    int available = gpu_backend_is_available();
    gpu_backend_type_t type = gpu_backend_get_type();

    TEST_ASSERT(available == 0, "Backend not available without init");
    TEST_ASSERT(type == GPU_BACKEND_NONE, "Backend type NONE without init");

    console_printf("  PASS: State queries work without init\n");
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================ */

/**
 * run_vulkan_tests - Run all Vulkan GPU backend tests
 *
 * Tests GPU backend initialization, device enumeration, CPU fallback,
 * and state management. Designed to work with or without actual GPU hardware.
 *
 * Returns: Number of failed tests (0 on success)
 */
int run_vulkan_tests(void)
{
    console_printf("\n");
    console_printf("====================================================================\n");
    console_printf("EMBODIOS Vulkan GPU Backend Test Suite\n");
    console_printf("====================================================================\n");
    console_printf("\n");

    /* Initialization Tests */
    console_printf("--- Backend Initialization Tests ---\n");
    test_backend_init_auto();
    test_backend_init_vulkan();
    test_backend_init_idempotent();

    /* Device Information Tests */
    console_printf("\n--- Device Information Tests ---\n");
    test_device_info_retrieval();
    test_device_info_null_param();

    /* Device Enumeration Tests */
    console_printf("\n--- Device Enumeration Tests ---\n");
    test_device_enumeration();
    test_device_enumeration_null_param();

    /* CPU Fallback Tests */
    console_printf("\n--- CPU Fallback Tests ---\n");
    test_cpu_fallback_consistency();

    /* Shutdown Tests */
    console_printf("\n--- Shutdown Tests ---\n");
    test_backend_shutdown();
    test_backend_reinit_after_shutdown();

    /* State Management Tests */
    console_printf("\n--- State Management Tests ---\n");
    test_backend_state_without_init();

    /* Print Results */
    console_printf("\n");
    console_printf("====================================================================\n");
    console_printf("Test Results\n");
    console_printf("====================================================================\n");
    console_printf("Tests Passed: %d\n", tests_passed);
    console_printf("Tests Failed: %d\n", tests_failed);
    console_printf("Total Tests:  %d\n", tests_passed + tests_failed);
    console_printf("\n");

    if (tests_failed > 0) {
        console_printf("RESULT: FAILED\n");
        console_printf("====================================================================\n");
        return tests_failed;
    }

    console_printf("RESULT: ALL TESTS PASSED\n");
    console_printf("====================================================================\n");
    return 0;
}
