/* x86_64 Paging Management */
#include <stdint.h>
#include <stddef.h>
#include "../../include/arch/x86_64/paging.h"
#include "../../include/embodios/mm.h"
#include "../../include/embodios/console.h"

/* Page table entry flags */
#define PAGE_PRESENT    0x001
#define PAGE_WRITE      0x002
#define PAGE_USER       0x004
#define PAGE_WRITETHROUGH 0x008
#define PAGE_NOCACHE    0x010
#define PAGE_ACCESSED   0x020
#define PAGE_DIRTY      0x040
#define PAGE_SIZE       0x080
#define PAGE_GLOBAL     0x100
#define PAGE_NX         (1ULL << 63)

/* Page table structure (4KB pages, 4-level paging) */
typedef uint64_t pml4e_t;
typedef uint64_t pdpte_t;
typedef uint64_t pde_t;
typedef uint64_t pte_t;

/* Kernel page tables (identity mapped + higher half) */
static pml4e_t* kernel_pml4 = NULL;

/* Extract page table indices from virtual address */
static inline uint16_t pml4_index(uint64_t vaddr) { return (vaddr >> 39) & 0x1FF; }
static inline uint16_t pdpt_index(uint64_t vaddr) { return (vaddr >> 30) & 0x1FF; }
static inline uint16_t pd_index(uint64_t vaddr)   { return (vaddr >> 21) & 0x1FF; }
static inline uint16_t pt_index(uint64_t vaddr)   { return (vaddr >> 12) & 0x1FF; }

/* Initialize paging */
void paging_init(void)
{
    /* For now, use the boot-time page tables */
    /* In a full implementation, we would:
     * 1. Allocate a new PML4
     * 2. Map kernel to higher half (0xFFFFFFFF80000000)
     * 3. Identity map first few MB for early boot
     * 4. Set up proper kernel/user separation
     */

    /* Get current CR3 */
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_pml4));
}

/* Map a virtual page to a physical page */
int paging_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    (void)vaddr;
    (void)paddr;
    (void)flags;

    /* TODO: Implement page mapping
     * 1. Walk page tables, allocating if needed
     * 2. Set PTE with physical address and flags
     */

    return 0;
}

/* Unmap a virtual page */
void paging_unmap_page(uint64_t vaddr)
{
    (void)vaddr;

    /* TODO: Implement page unmapping
     * 1. Walk page tables
     * 2. Clear PTE
     * 3. Invalidate TLB
     */
}

/* Get physical address for virtual address */
uint64_t paging_get_physical(uint64_t vaddr)
{
    (void)vaddr;

    /* TODO: Implement virtual to physical translation
     * 1. Walk page tables
     * 2. Return physical address from PTE
     */

    return 0;
}

/* Flush TLB entry */
void paging_flush_tlb(uint64_t vaddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/* Switch page directory */
void paging_switch_directory(uint64_t pml4_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys));
}

/* ------------------------------------------------------------------------ */
/* Physical memory map detection (multiboot2 mmap / Xen PVH start info)     */
/* ------------------------------------------------------------------------ */

#define MB2_BOOTLOADER_MAGIC    0x36d76289U
#define MB2_TAG_TYPE_END        0
#define MB2_TAG_TYPE_MMAP       6
#define MB2_MMAP_AVAILABLE      1

#define XEN_HVM_START_MAGIC     0x336ec578U
#define XEN_HVM_MEMMAP_TYPE_RAM 1

/* Detection happens before the identity map is extended past 1GB, so all
 * firmware-provided tables must live in the low 1GB to be readable. */
#define MEMMAP_TABLES_LIMIT     (1ULL << 30)

/* Saved by boot.S: eax/ebx at entry (multiboot2 magic/info under GRUB,
 * ebx = hvm_start_info physical address under QEMU -kernel PVH boot) */
extern uint32_t multiboot_magic;
extern uint32_t multiboot_info;

struct hvm_start_info {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;      /* version >= 1 */
    uint32_t memmap_entries;    /* version >= 1 */
    uint32_t reserved;
};

struct hvm_memmap_table_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
};

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries follow */
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
};

/* Add a usable RAM region, clipped to the address range we can map */
static size_t mem_region_add(struct boot_mem_region* regions, size_t count,
                             size_t max_regions, uint64_t base, uint64_t size)
{
    if (size == 0 || base >= BOOT_MAP_MAX_ADDR) {
        return count;
    }
    if (base + size > BOOT_MAP_MAX_ADDR) {
        size = BOOT_MAP_MAX_ADDR - base;
    }
    if (count >= max_regions) {
        console_printf("Memmap: WARNING - too many regions, dropping [0x%lx, +0x%lx)\n",
                       (unsigned long)base, (unsigned long)size);
        return count;
    }
    regions[count].base = base;
    regions[count].size = size;
    return count + 1;
}

/* Parse multiboot2 memory map (GRUB / ISO boot path) */
static size_t detect_memory_mb2(struct boot_mem_region* regions, size_t max_regions)
{
    uint32_t* mbi = (uint32_t*)(uintptr_t)multiboot_info;
    if ((uintptr_t)mbi >= MEMMAP_TABLES_LIMIT) {
        return 0;
    }
    uint32_t total_size = mbi[0];
    if (total_size < 8 || total_size > 64 * 1024) {
        return 0;
    }

    size_t count = 0;
    uintptr_t end = (uintptr_t)mbi + total_size;
    struct mb2_tag* tag = (struct mb2_tag*)(mbi + 2);

    while ((uintptr_t)tag + 8 <= end && tag->type != MB2_TAG_TYPE_END) {
        if (tag->type == MB2_TAG_TYPE_MMAP) {
            struct mb2_tag_mmap* mmap = (struct mb2_tag_mmap*)tag;
            if (mmap->entry_size < sizeof(struct mb2_mmap_entry)) {
                return 0;
            }
            uintptr_t entries_end = (uintptr_t)tag + tag->size;
            struct mb2_mmap_entry* e =
                (struct mb2_mmap_entry*)((uintptr_t)mmap + sizeof(struct mb2_tag_mmap));
            while ((uintptr_t)e + mmap->entry_size <= entries_end) {
                if (e->type == MB2_MMAP_AVAILABLE) {
                    count = mem_region_add(regions, count, max_regions,
                                           e->addr, e->len);
                }
                e = (struct mb2_mmap_entry*)((uintptr_t)e + mmap->entry_size);
            }
            return count;
        }
        tag = (struct mb2_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7U));
    }
    return 0;
}

/* Parse Xen PVH hvm_start_info memmap (QEMU -kernel direct boot path) */
static size_t detect_memory_pvh(struct boot_mem_region* regions, size_t max_regions)
{
    if (multiboot_info == 0 || multiboot_info >= MEMMAP_TABLES_LIMIT) {
        return 0;
    }
    struct hvm_start_info* info = (struct hvm_start_info*)(uintptr_t)multiboot_info;
    if (info->magic != XEN_HVM_START_MAGIC || info->version < 1) {
        return 0;
    }
    if (info->memmap_entries == 0 || info->memmap_entries > 128) {
        return 0;
    }
    uint64_t memmap_end = info->memmap_paddr +
                          (uint64_t)info->memmap_entries * sizeof(struct hvm_memmap_table_entry);
    if (info->memmap_paddr == 0 || memmap_end > MEMMAP_TABLES_LIMIT) {
        return 0;
    }

    size_t count = 0;
    struct hvm_memmap_table_entry* e =
        (struct hvm_memmap_table_entry*)(uintptr_t)info->memmap_paddr;
    for (uint32_t i = 0; i < info->memmap_entries; i++) {
        if (e[i].type == XEN_HVM_MEMMAP_TYPE_RAM) {
            count = mem_region_add(regions, count, max_regions, e[i].addr, e[i].size);
        }
    }
    return count;
}

size_t arch_detect_memory(struct boot_mem_region* regions, size_t max_regions)
{
    size_t count = 0;
    const char* source = NULL;

    console_printf("Memmap: boot magic=0x%lx info=0x%lx\n",
                   (unsigned long)multiboot_magic, (unsigned long)multiboot_info);

    if (multiboot_magic == MB2_BOOTLOADER_MAGIC) {
        count = detect_memory_mb2(regions, max_regions);
        source = "multiboot2 mmap";
    }
    if (count == 0) {
        count = detect_memory_pvh(regions, max_regions);
        if (count > 0) {
            source = "PVH hvm_start_info memmap";
        }
    }
    if (count == 0) {
        /* Conservative fallback: legacy behaviour, assume 1GB of RAM */
        regions[0].base = 0x100000;             /* 1 MB */
        regions[0].size = 1023ULL * 1024 * 1024; /* up to 1 GB */
        count = 1;
        source = "fallback (assumed 1GB)";
    }

    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += regions[i].size;
    }
    console_printf("Memmap: %lu MB usable RAM in %lu region(s) [%s]\n",
                   (unsigned long)(total / (1024 * 1024)),
                   (unsigned long)count, source);
    return count;
}

/* ------------------------------------------------------------------------ */
/* Extended identity mapping beyond the boot-time 1GB                       */
/* ------------------------------------------------------------------------ */

/* Boot page tables from boot.S (.bss, identity-mapped) */
extern uint64_t pml4_table[512];
extern uint64_t pdpt_table[512];

#define PTE_PRESENT_W        0x03ULL
#define PTE_HUGE_2MB         0x83ULL  /* Present + Writable + PS */
#define PAGE_2MB             (2ULL * 1024 * 1024)
#define BOOT_MAP_FIRST_GB    (1ULL << 30)

/* Pool of extra PD pages (each covers 1GB with 2MB pages). Zeroed by the
 * BSS clearing in boot.S before use. BOOT_PD_POOL pages = BOOT_MAP_MAX_ADDR. */
static uint64_t boot_pd_pool[BOOT_MAP_MAX_ADDR >> 30][512]
    __attribute__((aligned(4096)));
static size_t boot_pd_used = 0;

/* Identity-map [start, end) with 2MB pages (both already 2MB-aligned).
 * Returns false if the static PD pool ran out. */
static bool identity_map_2mb_range(uint64_t start, uint64_t end)
{
    for (uint64_t addr = start; addr < end; addr += PAGE_2MB) {
        if (addr < BOOT_MAP_FIRST_GB) {
            continue;  /* already covered by pdt_table in boot.S */
        }
        uint64_t pdpt_idx = addr >> 30;
        uint64_t pd_idx = (addr >> 21) & 0x1FF;

        if (!(pdpt_table[pdpt_idx] & PTE_PRESENT_W)) {
            if (boot_pd_used >= (BOOT_MAP_MAX_ADDR >> 30)) {
                console_printf("Paging: PD pool exhausted at 0x%lx\n",
                               (unsigned long)addr);
                return false;
            }
            uint64_t* pd = boot_pd_pool[boot_pd_used++];
            /* pd is in .bss (zeroed); identity-mapped so phys == virt */
            pdpt_table[pdpt_idx] = (uint64_t)(uintptr_t)pd | PTE_PRESENT_W;
        }
        uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt_table[pdpt_idx] & ~0xFFFULL);
        if (!(pd[pd_idx] & PTE_PRESENT_W)) {
            pd[pd_idx] = addr | PTE_HUGE_2MB;
        }
    }
    return true;
}

static void identity_map_flush_tlb(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uint64_t arch_identity_map_ram(const struct boot_mem_region* regions, size_t count)
{
    uint64_t ceiling = BOOT_MAP_FIRST_GB;  /* boot.S already maps [0, 1GB) */

    for (size_t r = 0; r < count; r++) {
        uint64_t start = regions[r].base & ~(PAGE_2MB - 1);
        uint64_t end = (regions[r].base + regions[r].size + PAGE_2MB - 1) &
                       ~(PAGE_2MB - 1);

        if (!identity_map_2mb_range(start, end)) {
            console_printf("Paging: memory above 0x%lx unmapped\n",
                           (unsigned long)ceiling);
            break;
        }
        if (end > ceiling) {
            ceiling = end;
        }
    }

    /* Flush TLB so newly populated PDPT entries take effect */
    identity_map_flush_tlb();

    if (ceiling > BOOT_MAP_FIRST_GB) {
        console_printf("Paging: identity map extended to %lu MB\n",
                       (unsigned long)(ceiling >> 20));
    }
    return ceiling;
}

bool arch_identity_map_mmio(uint64_t base, uint64_t size)
{
    if (size == 0) {
        return false;
    }
    uint64_t start = base & ~(PAGE_2MB - 1);
    uint64_t end = (base + size + PAGE_2MB - 1) & ~(PAGE_2MB - 1);

    if (!identity_map_2mb_range(start, end)) {
        return false;
    }
    identity_map_flush_tlb();
    return true;
}
