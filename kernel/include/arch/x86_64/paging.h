#ifndef ARCH_X86_64_PAGING_H
#define ARCH_X86_64_PAGING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Physical memory region detected from firmware (usable RAM) */
struct boot_mem_region {
    uint64_t base;
    uint64_t size;
};

#define BOOT_MEM_MAX_REGIONS 16

/* Highest physical address the boot page-table pool can identity-map.
 * Boot code statically maps the first 1GB; the dynamic pool in paging.c
 * adds BOOT_PD_POOL PD pages (1GB each) on top of that. */
#define BOOT_MAP_MAX_ADDR   (32ULL << 30)   /* 32 GB */

/* Detect usable RAM regions from the boot environment.
 * Sources, in order of preference:
 *   1. multiboot2 memory map tag (GRUB / ISO boot)
 *   2. Xen PVH hvm_start_info memmap (QEMU -kernel direct boot)
 *   3. Fallback: a single conservative region [1MB, 1GB)
 * Returns the number of regions stored in 'regions' (>= 1). */
size_t arch_detect_memory(struct boot_mem_region* regions, size_t max_regions);

/* Extend the boot-time identity map (first 1GB, set up in boot.S) to cover
 * all given RAM regions using 2MB pages from a static PD pool in .bss.
 * Must be called before any access to physical addresses >= 1GB.
 * Returns the highest address guaranteed to be identity-mapped
 * (regions above the returned ceiling may be only partially mapped if the
 * PD pool was exhausted). */
uint64_t arch_identity_map_ram(const struct boot_mem_region* regions, size_t count);

/* Identity-map an arbitrary physical range (e.g. a PCI MMIO BAR) using
 * 2MB pages from the same static PD pool. Needed because PCI BARs often
 * sit just below 4GB (e.g. 0xFEB80000), far past the boot-time 1GB map.
 * Returns true if the whole range is mapped. */
bool arch_identity_map_mmio(uint64_t base, uint64_t size);

/* Initialize paging subsystem */
void paging_init(void);

/* Map a virtual page to a physical page */
int paging_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* Unmap a virtual page */
void paging_unmap_page(uint64_t vaddr);

/* Get physical address for virtual address */
uint64_t paging_get_physical(uint64_t vaddr);

/* Flush TLB entry */
void paging_flush_tlb(uint64_t vaddr);

/* Switch page directory */
void paging_switch_directory(uint64_t pml4_phys);

#endif /* ARCH_X86_64_PAGING_H */
