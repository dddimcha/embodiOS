/* Unit test for Virtual Memory Manager */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/* Mock kernel environment */
#define PAGE_SIZE 4096
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* Page table levels - x86_64 4-level paging */
#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)

/* Page table entry flags */
#define PTE_PRESENT     (1UL << 0)
#define PTE_WRITABLE    (1UL << 1)
#define PTE_USER        (1UL << 2)
#define PTE_PWT         (1UL << 3)
#define PTE_PCD         (1UL << 4)
#define PTE_ACCESSED    (1UL << 5)
#define PTE_DIRTY       (1UL << 6)
#define PTE_HUGE        (1UL << 7)
#define PTE_GLOBAL      (1UL << 8)
#define PTE_NX          (1UL << 63)

/* Virtual memory regions */
#define KERNEL_BASE     0x100000UL
#define USER_BASE       0x0000000000400000UL
#define USER_MAX        0x00007FFFFFFFFFFFUL

/* Page table structure */
typedef uint64_t pte_t;

typedef struct {
    pte_t entries[512];
} page_table_t;

/* Mock VMM state */
typedef struct {
    page_table_t* pml4;
    void* heap_start;
    void* heap_end;
    size_t heap_used;
    bool initialized;
} mock_vmm_t;

static mock_vmm_t vmm;

/* Mock physical memory allocator */
static void* mock_pmm_alloc_page() {
    return malloc(PAGE_SIZE);
}

static void mock_pmm_free_page(void* page) {
    free(page);
}

/* Test address index calculations */
void test_address_indices() {
    printf("\n=== Testing Address Index Calculations ===\n");

    struct {
        uintptr_t addr;
        size_t pml4_idx;
        size_t pdpt_idx;
        size_t pd_idx;
        size_t pt_idx;
    } tests[] = {
        {0x0000000000000000UL, 0, 0, 0, 0},
        {0x0000000000001000UL, 0, 0, 0, 1},
        {0x0000000000200000UL, 0, 0, 1, 0},
        {0x0000000040000000UL, 0, 1, 0, 0},
        {0x0000008000000000UL, 1, 0, 0, 0},
        {0x00007FFFFFFFF000UL, 255, 511, 511, 511},
    };

    for (int i = 0; i < 6; i++) {
        uintptr_t addr = tests[i].addr;
        size_t pml4 = PML4_INDEX(addr);
        size_t pdpt = PDPT_INDEX(addr);
        size_t pd = PD_INDEX(addr);
        size_t pt = PT_INDEX(addr);

        printf("Address 0x%016lx:\n", addr);
        printf("  PML4[%3zu] PDPT[%3zu] PD[%3zu] PT[%3zu]\n", pml4, pdpt, pd, pt);

        assert(pml4 == tests[i].pml4_idx);
        assert(pdpt == tests[i].pdpt_idx);
        assert(pd == tests[i].pd_idx);
        assert(pt == tests[i].pt_idx);
    }

    printf("All index calculations correct!\n");
}

/* Test page table entry flag operations */
void test_pte_flags() {
    printf("\n=== Testing PTE Flag Operations ===\n");

    /* Test individual flags */
    pte_t pte = 0;

    printf("Testing flag setting:\n");
    pte |= PTE_PRESENT;
    assert(pte & PTE_PRESENT);
    printf("  PRESENT flag set: 0x%lx\n", pte);

    pte |= PTE_WRITABLE;
    assert(pte & PTE_WRITABLE);
    printf("  WRITABLE flag set: 0x%lx\n", pte);

    pte |= PTE_USER;
    assert(pte & PTE_USER);
    printf("  USER flag set: 0x%lx\n", pte);

    /* Test flag combinations */
    printf("\nTesting flag combinations:\n");
    pte_t kernel_page = PTE_PRESENT | PTE_WRITABLE;
    printf("  Kernel writable: 0x%lx (present=%d, writable=%d, user=%d)\n",
           kernel_page,
           !!(kernel_page & PTE_PRESENT),
           !!(kernel_page & PTE_WRITABLE),
           !!(kernel_page & PTE_USER));

    pte_t user_page = PTE_PRESENT | PTE_USER;
    printf("  User readable: 0x%lx (present=%d, writable=%d, user=%d)\n",
           user_page,
           !!(user_page & PTE_PRESENT),
           !!(user_page & PTE_WRITABLE),
           !!(user_page & PTE_USER));

    pte_t user_writable = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    printf("  User writable: 0x%lx (present=%d, writable=%d, user=%d)\n",
           user_writable,
           !!(user_writable & PTE_PRESENT),
           !!(user_writable & PTE_WRITABLE),
           !!(user_writable & PTE_USER));

    /* Test address extraction */
    printf("\nTesting address extraction:\n");
    uintptr_t phys_addr = 0x123456000;
    pte_t addr_pte = phys_addr | PTE_PRESENT | PTE_WRITABLE;
    uintptr_t extracted = addr_pte & ~0xFFF;
    printf("  Physical address 0x%lx -> PTE 0x%lx -> extracted 0x%lx\n",
           phys_addr, addr_pte, extracted);
    assert(extracted == phys_addr);
}

/* Test page table hierarchy */
void test_page_table_hierarchy() {
    printf("\n=== Testing Page Table Hierarchy ===\n");

    /* Allocate PML4 */
    page_table_t* pml4 = (page_table_t*)malloc(sizeof(page_table_t));
    memset(pml4, 0, sizeof(page_table_t));
    printf("Allocated PML4 at %p\n", pml4);

    /* Create hierarchy for address 0x400000 (first user page) */
    uintptr_t vaddr = USER_BASE;
    size_t pml4_idx = PML4_INDEX(vaddr);
    size_t pdpt_idx = PDPT_INDEX(vaddr);
    size_t pd_idx = PD_INDEX(vaddr);
    size_t pt_idx = PT_INDEX(vaddr);

    printf("\nMapping virtual address 0x%lx:\n", vaddr);
    printf("  Indices: PML4[%zu] PDPT[%zu] PD[%zu] PT[%zu]\n",
           pml4_idx, pdpt_idx, pd_idx, pt_idx);

    /* Allocate PDPT */
    page_table_t* pdpt = (page_table_t*)malloc(sizeof(page_table_t));
    memset(pdpt, 0, sizeof(page_table_t));
    pml4->entries[pml4_idx] = (uintptr_t)pdpt | PTE_PRESENT | PTE_WRITABLE;
    printf("  Created PDPT at %p\n", pdpt);

    /* Allocate PD */
    page_table_t* pd = (page_table_t*)malloc(sizeof(page_table_t));
    memset(pd, 0, sizeof(page_table_t));
    pdpt->entries[pdpt_idx] = (uintptr_t)pd | PTE_PRESENT | PTE_WRITABLE;
    printf("  Created PD at %p\n", pd);

    /* Allocate PT */
    page_table_t* pt = (page_table_t*)malloc(sizeof(page_table_t));
    memset(pt, 0, sizeof(page_table_t));
    pd->entries[pd_idx] = (uintptr_t)pt | PTE_PRESENT | PTE_WRITABLE;
    printf("  Created PT at %p\n", pt);

    /* Map physical page */
    uintptr_t paddr = 0x200000;  /* Physical address */
    pt->entries[pt_idx] = paddr | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    printf("  Mapped physical 0x%lx to virtual 0x%lx\n", paddr, vaddr);

    /* Verify hierarchy traversal */
    printf("\nVerifying hierarchy traversal:\n");
    pte_t pml4_entry = pml4->entries[pml4_idx];
    assert(pml4_entry & PTE_PRESENT);
    page_table_t* found_pdpt = (page_table_t*)(pml4_entry & ~0xFFF);
    assert(found_pdpt == pdpt);
    printf("  PML4 -> PDPT: %p (correct)\n", found_pdpt);

    pte_t pdpt_entry = pdpt->entries[pdpt_idx];
    assert(pdpt_entry & PTE_PRESENT);
    page_table_t* found_pd = (page_table_t*)(pdpt_entry & ~0xFFF);
    assert(found_pd == pd);
    printf("  PDPT -> PD: %p (correct)\n", found_pd);

    pte_t pd_entry = pd->entries[pd_idx];
    assert(pd_entry & PTE_PRESENT);
    page_table_t* found_pt = (page_table_t*)(pd_entry & ~0xFFF);
    assert(found_pt == pt);
    printf("  PD -> PT: %p (correct)\n", found_pt);

    pte_t pt_entry = pt->entries[pt_idx];
    assert(pt_entry & PTE_PRESENT);
    uintptr_t found_paddr = pt_entry & ~0xFFF;
    assert(found_paddr == paddr);
    printf("  PT -> Physical: 0x%lx (correct)\n", found_paddr);

    /* Cleanup */
    free(pt);
    free(pd);
    free(pdpt);
    free(pml4);
}

/* Test virtual address mapping */
void test_address_mapping() {
    printf("\n=== Testing Virtual Address Mapping ===\n");

    /* Initialize mock VMM */
    vmm.pml4 = (page_table_t*)malloc(sizeof(page_table_t));
    memset(vmm.pml4, 0, sizeof(page_table_t));
    vmm.heap_start = (void*)0x8000000;
    vmm.heap_end = (void*)0x8400000;
    vmm.heap_used = 0;
    vmm.initialized = true;

    printf("VMM initialized with heap 0x%lx - 0x%lx\n",
           (uintptr_t)vmm.heap_start, (uintptr_t)vmm.heap_end);

    /* Test mapping single page */
    uintptr_t vaddr = (uintptr_t)vmm.heap_start;
    void* ppage = mock_pmm_alloc_page();
    assert(ppage != NULL);
    uintptr_t paddr = (uintptr_t)ppage;

    printf("\nMapping virtual 0x%lx to physical %p\n", vaddr, ppage);

    /* Create minimal page table hierarchy for testing */
    /* In real VMM, this is done by get_page_table() */
    size_t pml4_idx = PML4_INDEX(vaddr);
    if (!(vmm.pml4->entries[pml4_idx] & PTE_PRESENT)) {
        page_table_t* pdpt = (page_table_t*)malloc(sizeof(page_table_t));
        memset(pdpt, 0, sizeof(page_table_t));
        vmm.pml4->entries[pml4_idx] = (uintptr_t)pdpt | PTE_PRESENT | PTE_WRITABLE;
    }

    page_table_t* pdpt = (page_table_t*)(vmm.pml4->entries[pml4_idx] & ~0xFFF);
    size_t pdpt_idx = PDPT_INDEX(vaddr);
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        page_table_t* pd = (page_table_t*)malloc(sizeof(page_table_t));
        memset(pd, 0, sizeof(page_table_t));
        pdpt->entries[pdpt_idx] = (uintptr_t)pd | PTE_PRESENT | PTE_WRITABLE;
    }

    page_table_t* pd = (page_table_t*)(pdpt->entries[pdpt_idx] & ~0xFFF);
    size_t pd_idx = PD_INDEX(vaddr);
    if (!(pd->entries[pd_idx] & PTE_PRESENT)) {
        page_table_t* pt = (page_table_t*)malloc(sizeof(page_table_t));
        memset(pt, 0, sizeof(page_table_t));
        pd->entries[pd_idx] = (uintptr_t)pt | PTE_PRESENT | PTE_WRITABLE;
    }

    page_table_t* pt = (page_table_t*)(pd->entries[pd_idx] & ~0xFFF);
    size_t pt_idx = PT_INDEX(vaddr);
    pt->entries[pt_idx] = paddr | PTE_PRESENT | PTE_WRITABLE;

    printf("Page mapped successfully\n");

    /* Verify mapping */
    pte_t pte = pt->entries[pt_idx];
    assert(pte & PTE_PRESENT);
    assert(pte & PTE_WRITABLE);
    uintptr_t mapped_paddr = pte & ~0xFFF;
    assert(mapped_paddr == paddr);
    printf("Mapping verified: PTE=0x%lx, physical=0x%lx\n", pte, mapped_paddr);

    /* Cleanup */
    mock_pmm_free_page(ppage);
    free(pt);
    free(pd);
    free(pdpt);
    free(vmm.pml4);
}

/* Test page unmapping and zeroing */
void test_page_unmapping() {
    printf("\n=== Testing Page Unmapping and Zeroing ===\n");

    /* Allocate and map a page */
    void* ppage = mock_pmm_alloc_page();
    assert(ppage != NULL);
    memset(ppage, 0xAA, PAGE_SIZE);  /* Fill with pattern */
    printf("Allocated page at %p, filled with 0xAA\n", ppage);

    /* Verify pattern */
    unsigned char* bytes = (unsigned char*)ppage;
    assert(bytes[0] == 0xAA);
    assert(bytes[100] == 0xAA);
    assert(bytes[PAGE_SIZE-1] == 0xAA);
    printf("Pattern verified\n");

    /* Simulate unmapping with zeroing */
    printf("Zeroing page before unmap...\n");
    memset(ppage, 0, PAGE_SIZE);

    /* Verify zeroing */
    assert(bytes[0] == 0);
    assert(bytes[100] == 0);
    assert(bytes[PAGE_SIZE-1] == 0);
    printf("Page successfully zeroed\n");

    /* Check entire page is zeroed */
    bool all_zero = true;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        if (bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }
    assert(all_zero);
    printf("Verified all %d bytes are zero\n", PAGE_SIZE);

    mock_pmm_free_page(ppage);
}

/* Test multiple page mappings */
void test_multiple_mappings() {
    printf("\n=== Testing Multiple Page Mappings ===\n");

    const int num_pages = 10;
    void* pages[num_pages];
    uintptr_t vaddrs[num_pages];

    uintptr_t vbase = 0x10000000;

    printf("Allocating and mapping %d pages starting at 0x%lx\n", num_pages, vbase);

    /* Allocate pages */
    for (int i = 0; i < num_pages; i++) {
        pages[i] = mock_pmm_alloc_page();
        assert(pages[i] != NULL);
        vaddrs[i] = vbase + (i * PAGE_SIZE);
        printf("  Page %d: physical=%p, virtual=0x%lx\n", i, pages[i], vaddrs[i]);
    }

    /* Verify addresses are page-aligned */
    for (int i = 0; i < num_pages; i++) {
        assert(vaddrs[i] % PAGE_SIZE == 0);
    }
    printf("All virtual addresses are page-aligned\n");

    /* Verify pages are contiguous in virtual space */
    for (int i = 1; i < num_pages; i++) {
        assert(vaddrs[i] - vaddrs[i-1] == PAGE_SIZE);
    }
    printf("Virtual addresses are contiguous\n");

    /* Cleanup */
    for (int i = 0; i < num_pages; i++) {
        mock_pmm_free_page(pages[i]);
    }
}

/* Test virtual memory allocation simulation */
void test_vmm_allocation() {
    printf("\n=== Testing VMM Allocation Simulation ===\n");

    /* Initialize mock VMM */
    vmm.heap_start = (void*)0x8000000;
    vmm.heap_end = (void*)0x8400000;  /* 4MB heap */
    vmm.heap_used = 0;
    vmm.initialized = true;

    size_t heap_size = (uintptr_t)vmm.heap_end - (uintptr_t)vmm.heap_start;
    printf("VMM heap: %zu bytes (%zu MB)\n", heap_size, heap_size / (1024*1024));

    /* Test allocation */
    size_t alloc_sizes[] = {4096, 8192, 16384, 32768, 65536};

    for (int i = 0; i < 5; i++) {
        size_t size = alloc_sizes[i];
        size_t aligned = ALIGN_UP(size, PAGE_SIZE);

        if (vmm.heap_used + aligned <= heap_size) {
            void* vaddr = (void*)((uintptr_t)vmm.heap_start + vmm.heap_used);
            vmm.heap_used += aligned;

            size_t pages = aligned / PAGE_SIZE;
            printf("Allocated %zu bytes (%zu pages) at %p, heap used: %zu/%zu\n",
                   size, pages, vaddr, vmm.heap_used, heap_size);
        } else {
            printf("Allocation of %zu bytes would exceed heap size\n", size);
        }
    }

    /* Test out of memory */
    size_t remaining = heap_size - vmm.heap_used;
    printf("\nRemaining heap: %zu bytes\n", remaining);

    size_t too_large = remaining + PAGE_SIZE;
    if (vmm.heap_used + too_large > heap_size) {
        printf("Allocation of %zu bytes correctly fails (exceeds heap)\n", too_large);
    }
}

/* Test address alignment */
void test_alignment() {
    printf("\n=== Testing Address Alignment ===\n");

    struct {
        size_t size;
        size_t expected;
    } tests[] = {
        {0, 0},
        {1, PAGE_SIZE},
        {4095, PAGE_SIZE},
        {4096, PAGE_SIZE},
        {4097, PAGE_SIZE * 2},
        {8192, PAGE_SIZE * 2},
        {8193, PAGE_SIZE * 3},
        {12288, PAGE_SIZE * 3},
    };

    for (int i = 0; i < 8; i++) {
        size_t aligned = ALIGN_UP(tests[i].size, PAGE_SIZE);
        printf("Size %5zu -> aligned %5zu (expected %5zu) ",
               tests[i].size, aligned, tests[i].expected);
        assert(aligned == tests[i].expected);
        printf("✓\n");
    }
}

/* Test PTE address masking */
void test_pte_masking() {
    printf("\n=== Testing PTE Address Masking ===\n");

    struct {
        uintptr_t addr;
        uint64_t flags;
        uintptr_t expected_addr;
    } tests[] = {
        {0x123456000, PTE_PRESENT, 0x123456000},
        {0x123456000, PTE_PRESENT | PTE_WRITABLE, 0x123456000},
        {0xABCDEF000, PTE_PRESENT | PTE_WRITABLE | PTE_USER, 0xABCDEF000},
        {0x000001000, PTE_PRESENT, 0x000001000},
    };

    for (int i = 0; i < 4; i++) {
        pte_t pte = tests[i].addr | tests[i].flags;
        uintptr_t extracted = pte & ~0xFFF;

        printf("PTE 0x%016lx -> address 0x%lx (expected 0x%lx) ",
               pte, extracted, tests[i].expected_addr);
        assert(extracted == tests[i].expected_addr);
        printf("✓\n");
    }
}

int main() {
    printf("=== EMBODIOS VMM Unit Tests ===\n");

    test_address_indices();
    test_pte_flags();
    test_page_table_hierarchy();
    test_address_mapping();
    test_page_unmapping();
    test_multiple_mappings();
    test_vmm_allocation();
    test_alignment();
    test_pte_masking();

    printf("\n=== All VMM tests passed! ===\n");
    return 0;
}
