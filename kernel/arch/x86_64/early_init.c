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

/* GDT - need 7 entries: null, kernel code, kernel data, user code, user data, TSS (2 entries) */
static struct gdt_entry gdt[7];
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
    
#ifdef __x86_64__
    __asm__ volatile("lgdt %0" : : "m"(gdt_desc));
#endif
    
    /* Reload segments */
#ifdef __x86_64__
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
#endif
}

/* ============================================================================
 * SSE/FPU Initialization
 * ============================================================================ */

/* CR0 bits */
#define CR0_MP  (1 << 1)   /* Monitor Coprocessor */
#define CR0_EM  (1 << 2)   /* Emulation - must be 0 for SSE */
#define CR0_TS  (1 << 3)   /* Task Switched - must be 0 to avoid #NM */
#define CR0_NE  (1 << 5)   /* Numeric Error */
#define CR0_WP  (1 << 16)  /* Write Protect */

/* CR4 bits */
#define CR4_OSFXSR     (1 << 9)   /* OS supports FXSAVE/FXRSTOR */
#define CR4_OSXMMEXCPT (1 << 10)  /* OS supports unmasked SSE exceptions */
#define CR4_SMEP       (1 << 20)  /* Supervisor Mode Execution Prevention */

/* CPUID feature bits */
#define CPUID_FEAT_EDX_FPU   (1 << 0)   /* x87 FPU */
#define CPUID_FEAT_EDX_FXSR  (1 << 24)  /* FXSAVE/FXRSTOR */
#define CPUID_FEAT_EDX_SSE   (1 << 25)  /* SSE support */
#define CPUID_FEAT_EDX_SSE2  (1 << 26)  /* SSE2 support */

/* MXCSR default value:
 * Bits 7-12: Exception masks (all set = masked)
 * Bits 13-14: Rounding mode (00 = round to nearest)
 * Bit 15: Flush-to-zero (0 = disabled)
 */
#define MXCSR_DEFAULT 0x1F80

/* SSE state */
static struct {
    bool fpu_present;
    bool fxsr_present;
    bool sse_present;
    bool sse2_present;
    bool initialized;
} sse_state = {0};

/**
 * Check CPU features via CPUID
 */
static void detect_cpu_features(void)
{
#ifdef __x86_64__
    uint32_t eax, ebx, ecx, edx;

    /* CPUID function 1: Processor Info and Feature Bits */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));

    sse_state.fpu_present = (edx & CPUID_FEAT_EDX_FPU) != 0;
    sse_state.fxsr_present = (edx & CPUID_FEAT_EDX_FXSR) != 0;
    sse_state.sse_present = (edx & CPUID_FEAT_EDX_SSE) != 0;
    sse_state.sse2_present = (edx & CPUID_FEAT_EDX_SSE2) != 0;
#endif
}

/**
 * Initialize FPU and SSE
 *
 * This MUST be called FIRST before any other functions, as the kernel
 * is compiled with -msse2 and any function could use SSE instructions.
 *
 * CR0 configuration:
 *   - Clear EM (bit 2): Disable x87 emulation
 *   - Clear TS (bit 3): Clear task switched flag
 *   - Set MP (bit 1): Monitor coprocessor
 *   - Set NE (bit 5): Native FPU error handling
 *   - Set WP (bit 16): Write protection in kernel mode
 *
 * CR4 configuration:
 *   - Set OSFXSR (bit 9): Enable FXSAVE/FXRSTOR
 *   - Set OSXMMEXCPT (bit 10): Enable SSE exceptions
 */
static void init_fpu_sse(void)
{
#ifdef __x86_64__
    /* Detect CPU features first */
    detect_cpu_features();

    /*
     * SSE and FXSR are REQUIRED for this kernel (compiled with -msse2).
     * If not available, we cannot continue - halt immediately.
     */
    if (!sse_state.sse_present || !sse_state.fxsr_present) {
        /* Fatal: Cannot boot without SSE support */
        __asm__ volatile("cli; hlt");
        __builtin_unreachable();
    }

    /* Configure CR0 for FPU/SSE */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));

    cr0 |= CR0_MP;   /* Monitor coprocessor */
    cr0 |= CR0_NE;   /* Native FPU errors */
    cr0 |= CR0_WP;   /* Write protection */
    cr0 &= ~CR0_EM;  /* Disable emulation (REQUIRED for SSE) */
    cr0 &= ~CR0_TS;  /* Clear task switched */

    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    /* Configure CR4 for SSE */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    cr4 |= CR4_OSFXSR;      /* Enable FXSAVE/FXRSTOR */
    cr4 |= CR4_OSXMMEXCPT;  /* Enable SSE exceptions */

    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    /* Initialize FPU */
    __asm__ volatile("fninit");

    /* Initialize SSE state with default MXCSR */
    uint32_t mxcsr = MXCSR_DEFAULT;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));

    sse_state.initialized = true;
#endif
}

/**
 * Check if SSE2 is available and initialized
 */
bool arch_sse2_available(void)
{
    return sse_state.initialized && sse_state.sse2_present;
}

/**
 * Get SSE status string for diagnostics
 */
const char* arch_get_sse_status(void)
{
    if (!sse_state.initialized) {
        return "NOT INITIALIZED";
    }
    if (sse_state.sse2_present) {
        return "SSE2 ENABLED";
    }
    if (sse_state.sse_present) {
        return "SSE ENABLED (no SSE2)";
    }
    return "NO SSE SUPPORT";
}

/* ============================================================================
 * TSS Initialization
 * ============================================================================ */

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
    tss_entry->granularity = (tss_limit >> 16) & 0x0F;
    tss_entry->base_high = (tss_base >> 24) & 0xFF;
    tss_entry->base_upper = (tss_base >> 32) & 0xFFFFFFFF;
    tss_entry->reserved = 0;
    
    /* Load TSS */
#ifdef __x86_64__
    __asm__ volatile("ltr %%ax" : : "a"(0x28));
#endif
}

/* Early architecture initialization */
void arch_early_init(void)
{
    /*
     * CRITICAL: Initialize FPU/SSE FIRST!
     *
     * The kernel is compiled with -msse2, so ANY function call could
     * potentially use SSE instructions (e.g., memset, memcpy optimized
     * to use SSE). This MUST happen before init_gdt() and init_tss()
     * which use memset internally.
     *
     * init_fpu_sse() also sets CR0.WP (write protection).
     */
    init_fpu_sse();

    /* Now safe to call functions that may use SSE */
    init_gdt();
    init_tss();

#ifdef __x86_64__
    /* Check if SMEP is available before enabling */
    /* CPUID.07H:EBX.SMEP[bit 7] indicates SMEP support */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));

    if (ebx & (1 << 7)) {
        /* SMEP is supported, enable it */
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_SMEP;
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
#endif
}

/* Initialize interrupt system */
void arch_interrupt_init(void)
{
    /* TODO: Initialize IDT (Interrupt Descriptor Table) */
    /* TODO: Initialize PIC or APIC */
    /* For now, just a stub */
}

/* Enable interrupts */
void arch_enable_interrupts(void)
{
#ifdef __x86_64__
    __asm__ volatile("sti");
#endif
}

/* Disable interrupts */
void arch_disable_interrupts(void)
{
#ifdef __x86_64__
    __asm__ volatile("cli");
#endif
}

/* Halt CPU */
void arch_halt(void)
{
#ifdef __x86_64__
    __asm__ volatile("hlt");
#endif
}

/* ============================================================================
 * SMP Initialization
 * ============================================================================ */

/* Forward declaration for SMP initialization (defined in smp.c) */
extern void smp_init(void);

/**
 * Initialize SMP (Symmetric Multi-Processing)
 *
 * This should be called after basic architecture initialization is complete.
 * It will detect and boot secondary CPU cores.
 */
void arch_smp_init(void)
{
#ifdef __x86_64__
    smp_init();
#endif
}