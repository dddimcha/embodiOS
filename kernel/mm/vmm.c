/* EMBODIOS Virtual Memory Manager */
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <embodios/console.h>

/* Page table levels */
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
#define KERNEL_SIZE     0x40000000UL  /* 1GB */
#define USER_BASE       0x0000000000400000UL
#define USER_MAX        0x00007FFFFFFFFFFFUL

/* Page table structure */
typedef uint64_t pte_t;

struct page_table {
    pte_t entries[512];
} __aligned(4096);

/* VMM state */
static struct {
    struct page_table* kernel_pml4;
    void* vmm_heap_start;
    void* vmm_heap_end;
    size_t vmm_heap_used;
    bool initialized;
} vmm_state = {
    .initialized = false
};

/* Get or create page table */
static struct page_table* get_page_table(struct page_table* parent, size_t index, bool create)
{
    pte_t* entry = &parent->entries[index];
    
    if (!(*entry & PTE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        
        /* Allocate new page table */
        struct page_table* table = (struct page_table*)pmm_alloc_page();
        if (!table) {
            return NULL;
        }
        
        memset(table, 0, sizeof(struct page_table));
        
        /* Set entry */
        *entry = ((uintptr_t)table - KERNEL_BASE) | PTE_PRESENT | PTE_WRITABLE;
    }
    
    /* Return virtual address of page table */
    uintptr_t phys_addr = *entry & ~0xFFF;
    return (struct page_table*)(phys_addr + KERNEL_BASE);
}

/* Map a single page */
static bool map_page(struct page_table* pml4, uintptr_t vaddr, uintptr_t paddr, uint64_t flags)
{
    /* Get page table hierarchy */
    struct page_table* pdpt = get_page_table(pml4, PML4_INDEX(vaddr), true);
    if (!pdpt) return false;
    
    struct page_table* pd = get_page_table(pdpt, PDPT_INDEX(vaddr), true);
    if (!pd) return false;
    
    struct page_table* pt = get_page_table(pd, PD_INDEX(vaddr), true);
    if (!pt) return false;
    
    /* Set page table entry */
    pt->entries[PT_INDEX(vaddr)] = paddr | flags | PTE_PRESENT;
    
    /* Invalidate TLB */
#ifdef __x86_64__
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
#else
    /* ARM64: Invalidate TLB entry */
    /* TODO: Implement ARM64 TLB invalidation */
#endif
    
    return true;
}

/* Unmap a single page */
static void unmap_page(struct page_table* pml4, uintptr_t vaddr)
{
    /* Get page table hierarchy */
    struct page_table* pdpt = get_page_table(pml4, PML4_INDEX(vaddr), false);
    if (!pdpt) return;
    
    struct page_table* pd = get_page_table(pdpt, PDPT_INDEX(vaddr), false);
    if (!pd) return;
    
    struct page_table* pt = get_page_table(pd, PD_INDEX(vaddr), false);
    if (!pt) return;
    
    /* Clear page table entry */
    pt->entries[PT_INDEX(vaddr)] = 0;
    
    /* Invalidate TLB */
#ifdef __x86_64__
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
#else
    /* ARM64: Invalidate TLB entry */
    /* TODO: Implement ARM64 TLB invalidation */
#endif
}

/* Initialize virtual memory manager */
void vmm_init(void)
{
    console_printf("VMM: Initializing virtual memory\n");
    
#ifdef __x86_64__
    /* Get current PML4 from CR3 */
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    vmm_state.kernel_pml4 = (struct page_table*)cr3;
#else
    /* ARM64: Initialize page tables */
    /* TODO: Implement ARM64 page table initialization */
    vmm_state.kernel_pml4 = NULL;
#endif
    
    /* Set up VMM heap - separate from main heap, used for page table allocations */
    /* This is a small region for VMM internal allocations */
    vmm_state.vmm_heap_start = (void*)0x8000000;   /* 128MB */
    vmm_state.vmm_heap_end = (void*)0x8400000;     /* 132MB - 4MB for VMM */
    vmm_state.vmm_heap_used = 0;
    
    vmm_state.initialized = true;
    
    console_printf("VMM: Initialized with kernel heap at %p-%p\n", 
                   vmm_state.vmm_heap_start, vmm_state.vmm_heap_end);
}

/* Allocate virtual memory */
void* vmm_alloc(size_t size)
{
    if (!vmm_state.initialized || size == 0) {
        return NULL;
    }
    
    /* Align size to page boundary */
    size = ALIGN_UP(size, PAGE_SIZE);
    
    /* Check if we have enough space */
    if (vmm_state.vmm_heap_used + size > 
        (size_t)((uintptr_t)vmm_state.vmm_heap_end - (uintptr_t)vmm_state.vmm_heap_start)) {
        return NULL;
    }
    
    /* Calculate virtual address */
    void* vaddr = (void*)((uintptr_t)vmm_state.vmm_heap_start + vmm_state.vmm_heap_used);
    
    /* Allocate physical pages and map them */
    size_t num_pages = size / PAGE_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        void* ppage = pmm_alloc_page();
        if (!ppage) {
            /* Cleanup on failure */
            for (size_t j = 0; j < i; j++) {
                unmap_page(vmm_state.kernel_pml4, 
                          (uintptr_t)vaddr + j * PAGE_SIZE);
            }
            return NULL;
        }
        
        if (!map_page(vmm_state.kernel_pml4,
                     (uintptr_t)vaddr + i * PAGE_SIZE,
                     (uintptr_t)ppage - KERNEL_BASE,
                     PTE_WRITABLE)) {
            /* Cleanup on failure */
            pmm_free_page(ppage);
            for (size_t j = 0; j < i; j++) {
                unmap_page(vmm_state.kernel_pml4, 
                          (uintptr_t)vaddr + j * PAGE_SIZE);
            }
            return NULL;
        }
    }
    
    vmm_state.vmm_heap_used += size;
    return vaddr;
}

/* Free virtual memory */
void vmm_free(void* addr, size_t size)
{
    if (!vmm_state.initialized || !addr || size == 0) {
        return;
    }
    
    /* Align size to page boundary */
    size = ALIGN_UP(size, PAGE_SIZE);
    
    /* Unmap and free pages */
    size_t num_pages = size / PAGE_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        /* Get physical address before unmapping */
        struct page_table* pdpt = get_page_table(vmm_state.kernel_pml4, 
                                                PML4_INDEX((uintptr_t)addr + i * PAGE_SIZE), false);
        if (!pdpt) continue;
        
        struct page_table* pd = get_page_table(pdpt, 
                                              PDPT_INDEX((uintptr_t)addr + i * PAGE_SIZE), false);
        if (!pd) continue;
        
        struct page_table* pt = get_page_table(pd, 
                                              PD_INDEX((uintptr_t)addr + i * PAGE_SIZE), false);
        if (!pt) continue;
        
        pte_t pte = pt->entries[PT_INDEX((uintptr_t)addr + i * PAGE_SIZE)];
        if (pte & PTE_PRESENT) {
            uintptr_t paddr = pte & ~0xFFF;
            pmm_free_page((void*)(paddr + KERNEL_BASE));
        }
        
        unmap_page(vmm_state.kernel_pml4, (uintptr_t)addr + i * PAGE_SIZE);
    }
}

/* Map memory region */
void vmm_map(void* vaddr, void* paddr, size_t size, uint32_t flags)
{
    if (!vmm_state.initialized || !vaddr || !paddr || size == 0) {
        return;
    }
    
    /* Convert flags */
    uint64_t pte_flags = 0;
    if (flags & PAGE_WRITABLE) pte_flags |= PTE_WRITABLE;
    if (flags & PAGE_USER) pte_flags |= PTE_USER;
    
    /* Map pages */
    size_t num_pages = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        map_page(vmm_state.kernel_pml4,
                (uintptr_t)vaddr + i * PAGE_SIZE,
                (uintptr_t)paddr + i * PAGE_SIZE,
                pte_flags);
    }
}

/* Unmap memory region */
void vmm_unmap(void* vaddr, size_t size)
{
    if (!vmm_state.initialized || !vaddr || size == 0) {
        return;
    }
    
    /* Unmap pages */
    size_t num_pages = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        unmap_page(vmm_state.kernel_pml4, (uintptr_t)vaddr + i * PAGE_SIZE);
    }
}