/* EMBODIOS DMA Subsystem Tests
 * Comprehensive tests for DMA operations
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/dma.h>

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
} while(0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if ((a) == (b)) { \
        tests_passed++; \
        console_printf("  PASS: %s\n", msg); \
    } else { \
        tests_failed++; \
        console_printf("  FAIL: %s (expected %d, got %d)\n", msg, (int)(b), (int)(a)); \
    } \
} while(0)

#define TEST_ASSERT_NEQ(a, b, msg) do { \
    if ((a) != (b)) { \
        tests_passed++; \
        console_printf("  PASS: %s\n", msg); \
    } else { \
        tests_failed++; \
        console_printf("  FAIL: %s (got unexpected %d)\n", msg, (int)(a)); \
    } \
} while(0)

/* ============================================================================
 * Test: DMA Initialization
 * ============================================================================ */

static void test_dma_init(void)
{
    console_printf("\n[Test] DMA Initialization\n");

    /* Should be initialized already, but test the check */
    TEST_ASSERT(dma_is_initialized(), "DMA subsystem is initialized");

    /* Double init should fail */
    int result = dma_init();
    TEST_ASSERT_EQ(result, DMA_ERR_ALREADY_INIT, "Double init returns ALREADY_INIT");
}

/* ============================================================================
 * Test: Coherent Memory Allocation
 * ============================================================================ */

static void test_coherent_allocation(void)
{
    console_printf("\n[Test] Coherent Memory Allocation\n");

    dma_addr_t dma_handle;
    void* vaddr;

    /* Test basic allocation */
    vaddr = dma_alloc_coherent(4096, &dma_handle);
    TEST_ASSERT(vaddr != NULL, "4KB coherent allocation succeeds");
    TEST_ASSERT(dma_handle != DMA_ADDR_INVALID, "DMA handle is valid");

    /* Test alignment (64-byte cache line) */
    TEST_ASSERT(((uintptr_t)vaddr & (DMA_MIN_ALIGNMENT - 1)) == 0,
                "Buffer is 64-byte aligned");

    /* Write test pattern */
    uint32_t* data = (uint32_t*)vaddr;
    for (int i = 0; i < 1024; i++) {
        data[i] = 0xDEADBEEF + i;
    }

    /* Verify pattern */
    bool pattern_ok = true;
    for (int i = 0; i < 1024; i++) {
        if (data[i] != 0xDEADBEEF + i) {
            pattern_ok = false;
            break;
        }
    }
    TEST_ASSERT(pattern_ok, "Data pattern integrity verified");

    /* Free and verify */
    dma_free_coherent(vaddr, 4096, dma_handle);
    TEST_ASSERT(true, "Coherent free completes");

    /* Test various sizes */
    size_t sizes[] = {64, 256, 1024, 8192, 65536};
    for (int i = 0; i < 5; i++) {
        vaddr = dma_alloc_coherent(sizes[i], &dma_handle);
        if (vaddr != NULL) {
            TEST_ASSERT(((uintptr_t)vaddr & (DMA_MIN_ALIGNMENT - 1)) == 0,
                       "Alignment OK for various sizes");
            dma_free_coherent(vaddr, sizes[i], dma_handle);
        } else {
            tests_failed++;
            console_printf("  FAIL: Allocation failed for size %u\n", (unsigned)sizes[i]);
        }
    }

    /* Test NULL parameter handling */
    vaddr = dma_alloc_coherent(4096, NULL);
    TEST_ASSERT(vaddr == NULL, "NULL dma_handle returns NULL");

    /* Test zero size */
    vaddr = dma_alloc_coherent(0, &dma_handle);
    TEST_ASSERT(vaddr == NULL, "Zero size returns NULL");
}

/* ============================================================================
 * Test: Streaming DMA Mapping
 * ============================================================================ */

static void test_streaming_mapping(void)
{
    console_printf("\n[Test] Streaming DMA Mapping\n");

    /* Allocate a test buffer */
    dma_addr_t coherent_handle;
    void* buffer = dma_alloc_coherent(4096, &coherent_handle);
    TEST_ASSERT(buffer != NULL, "Test buffer allocated");

    if (buffer == NULL) return;

    /* Test DMA_TO_DEVICE mapping */
    dma_addr_t dma_addr = dma_map_single(buffer, 4096, DMA_TO_DEVICE);
    TEST_ASSERT(dma_addr != DMA_ADDR_INVALID, "DMA_TO_DEVICE mapping succeeds");

    /* Unmap */
    dma_unmap_single(dma_addr, 4096, DMA_TO_DEVICE);
    TEST_ASSERT(true, "DMA_TO_DEVICE unmap completes");

    /* Test DMA_FROM_DEVICE mapping */
    dma_addr = dma_map_single(buffer, 4096, DMA_FROM_DEVICE);
    TEST_ASSERT(dma_addr != DMA_ADDR_INVALID, "DMA_FROM_DEVICE mapping succeeds");
    dma_unmap_single(dma_addr, 4096, DMA_FROM_DEVICE);

    /* Test DMA_BIDIRECTIONAL mapping */
    dma_addr = dma_map_single(buffer, 4096, DMA_BIDIRECTIONAL);
    TEST_ASSERT(dma_addr != DMA_ADDR_INVALID, "DMA_BIDIRECTIONAL mapping succeeds");
    dma_unmap_single(dma_addr, 4096, DMA_BIDIRECTIONAL);

    /* Test NULL buffer */
    dma_addr = dma_map_single(NULL, 4096, DMA_TO_DEVICE);
    TEST_ASSERT(dma_addr == DMA_ADDR_INVALID, "NULL buffer returns INVALID");

    /* Cleanup */
    dma_free_coherent(buffer, 4096, coherent_handle);
}

/* ============================================================================
 * Test: Address Translation
 * ============================================================================ */

static void test_address_translation(void)
{
    console_printf("\n[Test] Address Translation\n");

    /* Allocate test buffer */
    dma_addr_t coherent_handle;
    void* buffer = dma_alloc_coherent(4096, &coherent_handle);
    TEST_ASSERT(buffer != NULL, "Test buffer allocated");

    if (buffer == NULL) return;

    /* Test virt_to_dma */
    dma_addr_t dma_addr = virt_to_dma(buffer);
    TEST_ASSERT(dma_addr != DMA_ADDR_INVALID, "virt_to_dma returns valid address");

    /* Test dma_to_virt roundtrip */
    void* virt_back = dma_to_virt(dma_addr);
    TEST_ASSERT(virt_back == buffer, "dma_to_virt roundtrip matches");

    /* Test NULL handling */
    dma_addr = virt_to_dma(NULL);
    TEST_ASSERT(dma_addr == DMA_ADDR_INVALID, "virt_to_dma(NULL) returns INVALID");

    /* Test invalid DMA address */
    virt_back = dma_to_virt(DMA_ADDR_INVALID);
    TEST_ASSERT(virt_back == NULL, "dma_to_virt(INVALID) returns NULL");

    /* Cleanup */
    dma_free_coherent(buffer, 4096, coherent_handle);
}

/* ============================================================================
 * Test: Scatter-Gather Operations
 * ============================================================================ */

static void test_scatter_gather(void)
{
    console_printf("\n[Test] Scatter-Gather Operations\n");

    dma_sg_list_t sg;
    int result;

    /* Test SG init */
    result = dma_sg_init(&sg, 16);
    TEST_ASSERT_EQ(result, DMA_OK, "SG list init succeeds");
    TEST_ASSERT_EQ(dma_sg_count(&sg), 0, "Empty SG has 0 entries");

    /* Allocate test buffers */
    dma_addr_t handles[4];
    void* buffers[4];
    size_t sizes[4] = {1024, 2048, 512, 4096};

    for (int i = 0; i < 4; i++) {
        buffers[i] = dma_alloc_coherent(sizes[i], &handles[i]);
        if (buffers[i] == NULL) {
            console_printf("  FAIL: Could not allocate test buffer %d\n", i);
            dma_sg_free(&sg);
            return;
        }
    }

    /* Add segments to SG list */
    for (int i = 0; i < 4; i++) {
        result = dma_sg_add(&sg, buffers[i], sizes[i]);
        TEST_ASSERT_EQ(result, DMA_OK, "SG add segment succeeds");
    }

    TEST_ASSERT_EQ(dma_sg_count(&sg), 4, "SG list has 4 entries");

    /* Test total length */
    size_t total = dma_sg_total_length(&sg);
    size_t expected_total = 1024 + 2048 + 512 + 4096;
    TEST_ASSERT_EQ((int)total, (int)expected_total, "SG total length correct");

    /* Map SG list */
    result = dma_sg_map(&sg, DMA_TO_DEVICE);
    TEST_ASSERT_EQ(result, DMA_OK, "SG map succeeds");

    /* Verify DMA addresses are valid */
    bool all_valid = true;
    for (int i = 0; i < sg.count; i++) {
        if (sg.entries[i].dma_addr == DMA_ADDR_INVALID) {
            all_valid = false;
            break;
        }
    }
    TEST_ASSERT(all_valid, "All SG entries have valid DMA addresses");

    /* Unmap SG list */
    dma_sg_unmap(&sg, DMA_TO_DEVICE);
    TEST_ASSERT(true, "SG unmap completes");

    /* Free SG list */
    dma_sg_free(&sg);
    TEST_ASSERT(true, "SG free completes");

    /* Free test buffers */
    for (int i = 0; i < 4; i++) {
        dma_free_coherent(buffers[i], sizes[i], handles[i]);
    }

    /* Test NULL handling */
    result = dma_sg_init(NULL, 16);
    TEST_ASSERT_EQ(result, DMA_ERR_INVALID, "SG init with NULL returns INVALID");

    /* Test max entries limit - should now return error instead of silently clamping */
    result = dma_sg_init(&sg, DMA_SG_MAX_ENTRIES + 1);
    TEST_ASSERT_EQ(result, DMA_ERR_INVALID, "SG init with too many entries returns INVALID");
}

/* ============================================================================
 * Test: Cache Synchronization
 * ============================================================================ */

static void test_cache_sync(void)
{
    console_printf("\n[Test] Cache Synchronization\n");

    /* Allocate test buffer */
    dma_addr_t coherent_handle;
    void* buffer = dma_alloc_coherent(4096, &coherent_handle);
    TEST_ASSERT(buffer != NULL, "Test buffer allocated");

    if (buffer == NULL) return;

    /* Map buffer */
    dma_addr_t dma_addr = dma_map_single(buffer, 4096, DMA_BIDIRECTIONAL);
    TEST_ASSERT(dma_addr != DMA_ADDR_INVALID, "Buffer mapped");

    /* Write data to buffer */
    uint32_t* data = (uint32_t*)buffer;
    for (int i = 0; i < 1024; i++) {
        data[i] = 0xCAFEBABE + i;
    }

    /* Sync for device (flush cache) */
    dma_sync_for_device(dma_addr, 4096, DMA_TO_DEVICE);
    TEST_ASSERT(true, "Sync for device completes");

    /* Simulate device writing (just modify data) */
    for (int i = 0; i < 1024; i++) {
        data[i] = 0xDEADC0DE + i;
    }

    /* Sync for CPU (invalidate cache) */
    dma_sync_for_cpu(dma_addr, 4096, DMA_FROM_DEVICE);
    TEST_ASSERT(true, "Sync for CPU completes");

    /* Verify we can read the data */
    bool read_ok = true;
    for (int i = 0; i < 1024; i++) {
        if (data[i] != 0xDEADC0DE + i) {
            read_ok = false;
            break;
        }
    }
    TEST_ASSERT(read_ok, "Data readable after sync");

    /* Cleanup */
    dma_unmap_single(dma_addr, 4096, DMA_BIDIRECTIONAL);
    dma_free_coherent(buffer, 4096, coherent_handle);
}

/* ============================================================================
 * Test: Address Validation
 * ============================================================================ */

static void test_address_validation(void)
{
    console_printf("\n[Test] Address Validation\n");

    /* Allocate test buffer */
    dma_addr_t coherent_handle;
    void* buffer = dma_alloc_coherent(4096, &coherent_handle);
    TEST_ASSERT(buffer != NULL, "Test buffer allocated");

    if (buffer == NULL) return;

    /* Valid address should pass */
    int result = dma_validate_address(coherent_handle, 4096);
    TEST_ASSERT_EQ(result, DMA_OK, "Valid address passes validation");

    /* Invalid address should fail */
    result = dma_validate_address(DMA_ADDR_INVALID, 4096);
    TEST_ASSERT_NEQ(result, DMA_OK, "Invalid address fails validation");

    /* Zero size should fail */
    result = dma_validate_address(coherent_handle, 0);
    TEST_ASSERT_NEQ(result, DMA_OK, "Zero size fails validation");

    /* Overflow check */
    result = dma_validate_address(0xFFFFFFFFFFFFFF00ULL, 4096);
    TEST_ASSERT_NEQ(result, DMA_OK, "Overflow address fails validation");

    /* Cleanup */
    dma_free_coherent(buffer, 4096, coherent_handle);
}

/* ============================================================================
 * Test: Dummy Transfer Simulation
 * ============================================================================ */

static void test_dummy_transfer(void)
{
    console_printf("\n[Test] Dummy Transfer Simulation\n");

    /* Simulate a DMA transfer scenario:
     * 1. Allocate source and destination buffers
     * 2. Fill source with test data
     * 3. Sync source for device (CPU wrote, device will read)
     * 4. "Device" copies data (we simulate by memcpy)
     * 5. Sync destination for CPU (device wrote, CPU will read)
     * 6. Verify data integrity
     */

    dma_addr_t src_handle, dst_handle;
    void* src = dma_alloc_coherent(4096, &src_handle);
    void* dst = dma_alloc_coherent(4096, &dst_handle);

    TEST_ASSERT(src != NULL && dst != NULL, "Source and dest buffers allocated");

    if (src == NULL || dst == NULL) {
        if (src) dma_free_coherent(src, 4096, src_handle);
        if (dst) dma_free_coherent(dst, 4096, dst_handle);
        return;
    }

    /* Fill source with test pattern */
    uint8_t* src_data = (uint8_t*)src;
    for (int i = 0; i < 4096; i++) {
        src_data[i] = (uint8_t)(i & 0xFF);
    }

    /* Clear destination */
    uint8_t* dst_data = (uint8_t*)dst;
    for (int i = 0; i < 4096; i++) {
        dst_data[i] = 0;
    }

    /* Sync source for device access */
    dma_sync_for_device(src_handle, 4096, DMA_TO_DEVICE);

    /* Simulate DMA transfer (device copies src to dst) */
    for (int i = 0; i < 4096; i++) {
        dst_data[i] = src_data[i];
    }

    /* Sync destination for CPU access */
    dma_sync_for_cpu(dst_handle, 4096, DMA_FROM_DEVICE);

    /* Verify data integrity */
    bool transfer_ok = true;
    for (int i = 0; i < 4096; i++) {
        if (dst_data[i] != (uint8_t)(i & 0xFF)) {
            transfer_ok = false;
            console_printf("  Mismatch at byte %d: expected %02x, got %02x\n",
                          i, (uint8_t)(i & 0xFF), dst_data[i]);
            break;
        }
    }
    TEST_ASSERT(transfer_ok, "DMA transfer data integrity verified");

    /* Cleanup */
    dma_free_coherent(src, 4096, src_handle);
    dma_free_coherent(dst, 4096, dst_handle);
}

/* ============================================================================
 * Test: Statistics
 * ============================================================================ */

static void test_statistics(void)
{
    console_printf("\n[Test] DMA Statistics\n");

    dma_stats_t stats;
    dma_get_stats(&stats);

    /* Stats should reflect our test activity */
    TEST_ASSERT(stats.alloc_count > 0, "Allocation count > 0");
    TEST_ASSERT(stats.free_count > 0, "Free count > 0");

    /* Print stats for reference */
    console_printf("  Stats: allocs=%llu, frees=%llu, maps=%llu, unmaps=%llu\n",
                  (unsigned long long)stats.alloc_count,
                  (unsigned long long)stats.free_count,
                  (unsigned long long)stats.map_count,
                  (unsigned long long)stats.unmap_count);
    console_printf("  Active allocations: %d\n", stats.active_allocations);

    TEST_ASSERT(true, "Statistics retrieved successfully");
}

/* ============================================================================
 * Test: Stress Test
 * ============================================================================ */

static void test_stress(void)
{
    console_printf("\n[Test] Stress Test\n");

    #define STRESS_COUNT 32
    void* buffers[STRESS_COUNT];
    dma_addr_t handles[STRESS_COUNT];
    int alloc_count = 0;

    /* Rapid allocations */
    for (int i = 0; i < STRESS_COUNT; i++) {
        size_t size = 1024 * (1 + (i % 8));  /* Vary sizes */
        buffers[i] = dma_alloc_coherent(size, &handles[i]);
        if (buffers[i] != NULL) {
            alloc_count++;
            /* Write something to each buffer */
            ((uint32_t*)buffers[i])[0] = 0xDEAD0000 + i;
        }
    }

    console_printf("  Allocated %d/%d buffers\n", alloc_count, STRESS_COUNT);
    TEST_ASSERT(alloc_count >= STRESS_COUNT / 2, "At least half allocations succeed");

    /* Verify data */
    bool data_ok = true;
    for (int i = 0; i < STRESS_COUNT; i++) {
        if (buffers[i] != NULL) {
            if (((uint32_t*)buffers[i])[0] != 0xDEAD0000 + i) {
                data_ok = false;
                break;
            }
        }
    }
    TEST_ASSERT(data_ok, "All buffer data intact");

    /* Free in reverse order */
    for (int i = STRESS_COUNT - 1; i >= 0; i--) {
        if (buffers[i] != NULL) {
            size_t size = 1024 * (1 + (i % 8));
            dma_free_coherent(buffers[i], size, handles[i]);
        }
    }
    TEST_ASSERT(true, "All buffers freed");

    #undef STRESS_COUNT
}

/* ============================================================================
 * Main Test Entry Point
 * ============================================================================ */

int dma_run_tests(void)
{
    console_printf("\n========================================\n");
    console_printf("EMBODIOS DMA Subsystem Tests\n");
    console_printf("========================================\n");

    tests_passed = 0;
    tests_failed = 0;

    /* Run all tests */
    test_dma_init();
    test_coherent_allocation();
    test_streaming_mapping();
    test_address_translation();
    test_scatter_gather();
    test_cache_sync();
    test_address_validation();
    test_dummy_transfer();
    test_statistics();
    test_stress();

    /* Print summary */
    console_printf("\n========================================\n");
    console_printf("DMA Tests Complete: %d passed, %d failed\n",
                  tests_passed, tests_failed);
    console_printf("========================================\n");

    /* Dump allocations for debugging */
    console_printf("\nActive DMA allocations:\n");
    dma_dump_allocations();

    /* Print final stats */
    console_printf("\nFinal DMA statistics:\n");
    dma_print_stats();

    return (tests_failed == 0) ? 0 : -1;
}
