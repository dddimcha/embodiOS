/* x86_64 Early Architecture Initialization */
#include <embodios/kernel.h>
#include <embodios/types.h>
#include <embodios/mm.h>

/* GDT entry structure */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __packed;

struct gdt_entry64 {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __packed;

struct gdt_descriptor {
    uint16_t limit;
    uint64_t base;
} __packed;

/* GDT */
static struct gdt_entry gdt[5];
static struct gdt_descriptor gdt_desc;

/* TSS structure for x86_64 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __packed;

static struct tss tss;
extern char kernel_stack_top[];

/* Set GDT entry */
static void gdt_set_entry(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[num].base_low = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    gdt[num].limit_low = limit & 0xFFFF;
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access = access;
}

/* Initialize GDT */
static void init_gdt(void)
{
    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);
    
    /* Kernel code segment */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xAF);
    
    /* Kernel data segment */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);
    
    /* User code segment */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xAF);
    
    /* User data segment */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);
    
    /* Load GDT */
    gdt_desc.limit = sizeof(gdt) - 1;
    gdt_desc.base = (uint64_t)&gdt;
    
    __asm__ volatile("lgdt %0" : : "m"(gdt_desc));
    
    /* Reload segments */
    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "pushq $0x08\n"
        "pushq $1f\n"
        "lretq\n"
        "1:\n"
        : : : "ax"
    );
}

/* Initialize TSS */
static void init_tss(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (uint64_t)kernel_stack_top;
    tss.iopb_offset = sizeof(tss);
    
    /* Install TSS descriptor in GDT */
    uint64_t tss_base = (uint64_t)&tss;
    uint32_t tss_limit = sizeof(tss) - 1;
    
    /* TSS descriptor spans two GDT entries in long mode */
    struct gdt_entry64* tss_entry = (struct gdt_entry64*)&gdt[5];
    
    tss_entry->limit_low = tss_limit & 0xFFFF;
    tss_entry->base_low = tss_base & 0xFFFF;
    tss_entry->base_middle = (tss_base >> 16) & 0xFF;
    tss_entry->access = 0x89;  /* Present, TSS */
    tss_entry->granularity = ((tss_limit >> 16) & 0x0F) | 0x00;
    tss_entry->base_high = (tss_base >> 24) & 0xFF;
    tss_entry->base_upper = (tss_base >> 32) & 0xFFFFFFFF;
    tss_entry->reserved = 0;
    
    /* Load TSS */
    __asm__ volatile("ltr %%ax" : : "a"(0x28));
}

/* Early architecture initialization */
void arch_early_init(void)
{
    /* Initialize GDT */
    init_gdt();
    
    /* Initialize TSS */
    init_tss();
    
    /* Enable write protection in kernel mode */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1 << 16);  /* WP bit */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    
    /* Enable SMEP if available */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 20);  /* SMEP bit */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

/* Enable interrupts */
void arch_enable_interrupts(void)
{
    __asm__ volatile("sti");
}

/* Disable interrupts */
void arch_disable_interrupts(void)
{
    __asm__ volatile("cli");
}

/* Halt CPU */
void arch_halt(void)
{
    __asm__ volatile("hlt");
}