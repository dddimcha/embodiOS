#ifndef ARCH_X86_64_PAGING_H
#define ARCH_X86_64_PAGING_H

#include <stdint.h>

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
