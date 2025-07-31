/* EMBODIOS x86_64 CPU Detection and Management */
#include <embodios/cpu.h>
#include <embodios/types.h>
#include <embodios/console.h>
#include "vga_io.h"

/* CPUID functions */
#define CPUID_VENDOR        0x00000000
#define CPUID_FEATURES      0x00000001
#define CPUID_EXT_MAX       0x80000000
#define CPUID_EXT_FEATURES  0x80000001
#define CPUID_BRAND_STRING  0x80000002

/* Feature bits */
#define CPUID_FEAT_EDX_FPU      (1 << 0)
#define CPUID_FEAT_EDX_SSE      (1 << 25)
#define CPUID_FEAT_EDX_SSE2     (1 << 26)
#define CPUID_FEAT_ECX_SSE3     (1 << 0)
#define CPUID_FEAT_ECX_SSSE3    (1 << 9)
#define CPUID_FEAT_ECX_SSE41    (1 << 19)
#define CPUID_FEAT_ECX_SSE42    (1 << 20)
#define CPUID_FEAT_ECX_AVX      (1 << 28)
#define CPUID_FEAT7_EBX_AVX2    (1 << 5)
#define CPUID_FEAT7_EBX_AVX512F (1 << 16)

static struct cpu_info cpu_info;

/* Execute CPUID instruction */
static inline void cpuid(uint32_t func, uint32_t* eax, uint32_t* ebx, 
                        uint32_t* ecx, uint32_t* edx)
{
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(func), "c"(0));
}

/* Read timestamp counter */
uint64_t cpu_get_timestamp(void)
{
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

/* Get CPU ID (APIC ID) */
uint32_t cpu_get_id(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ebx >> 24) & 0xFF;
}

/* Initialize CPU detection */
void cpu_init(void)
{
    uint32_t eax, ebx, ecx, edx;
    
    /* Get vendor string */
    cpuid(CPUID_VENDOR, &eax, &ebx, &ecx, &edx);
    *((uint32_t*)&cpu_info.vendor[0]) = ebx;
    *((uint32_t*)&cpu_info.vendor[4]) = edx;
    *((uint32_t*)&cpu_info.vendor[8]) = ecx;
    cpu_info.vendor[12] = '\0';
    
    /* Get CPU features */
    cpuid(CPUID_FEATURES, &eax, &ebx, &ecx, &edx);
    
    cpu_info.stepping = eax & 0xF;
    cpu_info.model_id = (eax >> 4) & 0xF;
    cpu_info.family = (eax >> 8) & 0xF;
    
    if (cpu_info.family == 0xF) {
        cpu_info.family += (eax >> 20) & 0xFF;
    }
    if (cpu_info.family >= 0x6) {
        cpu_info.model_id += ((eax >> 16) & 0xF) << 4;
    }
    
    /* Detect features */
    cpu_info.features = 0;
    
    if (edx & CPUID_FEAT_EDX_FPU)
        cpu_info.features |= CPU_FEATURE_FPU;
    if (edx & CPUID_FEAT_EDX_SSE)
        cpu_info.features |= CPU_FEATURE_SSE;
    if (edx & CPUID_FEAT_EDX_SSE2)
        cpu_info.features |= CPU_FEATURE_SSE2;
    if (ecx & CPUID_FEAT_ECX_SSE3)
        cpu_info.features |= CPU_FEATURE_SSE3;
    if (ecx & CPUID_FEAT_ECX_SSSE3)
        cpu_info.features |= CPU_FEATURE_SSSE3;
    if (ecx & CPUID_FEAT_ECX_SSE41)
        cpu_info.features |= CPU_FEATURE_SSE41;
    if (ecx & CPUID_FEAT_ECX_SSE42)
        cpu_info.features |= CPU_FEATURE_SSE42;
    if (ecx & CPUID_FEAT_ECX_AVX)
        cpu_info.features |= CPU_FEATURE_AVX;
    
    /* Check extended features */
    cpuid(7, &eax, &ebx, &ecx, &edx);
    if (ebx & CPUID_FEAT7_EBX_AVX2)
        cpu_info.features |= CPU_FEATURE_AVX2;
    if (ebx & CPUID_FEAT7_EBX_AVX512F)
        cpu_info.features |= CPU_FEATURE_AVX512;
    
    /* Get brand string */
    cpuid(CPUID_EXT_MAX, &eax, &ebx, &ecx, &edx);
    if (eax >= CPUID_BRAND_STRING + 2) {
        uint32_t* model = (uint32_t*)cpu_info.model;
        for (int i = 0; i < 3; i++) {
            cpuid(CPUID_BRAND_STRING + i, &eax, &ebx, &ecx, &edx);
            model[i*4 + 0] = eax;
            model[i*4 + 1] = ebx;
            model[i*4 + 2] = ecx;
            model[i*4 + 3] = edx;
        }
        cpu_info.model[47] = '\0';
    }
    
    /* Count logical processors */
    cpuid(1, &eax, &ebx, &ecx, &edx);
    cpu_info.cores = (ebx >> 16) & 0xFF;
    if (cpu_info.cores == 0) cpu_info.cores = 1;
}

/* Architecture-specific initialization */
void arch_cpu_init(void)
{
    cpu_init();
    
    console_printf("CPU: %s\n", cpu_info.vendor);
    console_printf("Model: %s\n", cpu_info.model);
    console_printf("Family: %u, Model: %u, Stepping: %u\n", 
                   cpu_info.family, cpu_info.model_id, cpu_info.stepping);
    console_printf("Cores: %u\n", cpu_info.cores);
    console_printf("Features:");
    
    if (cpu_info.features & CPU_FEATURE_FPU) console_printf(" FPU");
    if (cpu_info.features & CPU_FEATURE_SSE) console_printf(" SSE");
    if (cpu_info.features & CPU_FEATURE_SSE2) console_printf(" SSE2");
    if (cpu_info.features & CPU_FEATURE_SSE3) console_printf(" SSE3");
    if (cpu_info.features & CPU_FEATURE_SSSE3) console_printf(" SSSE3");
    if (cpu_info.features & CPU_FEATURE_SSE41) console_printf(" SSE4.1");
    if (cpu_info.features & CPU_FEATURE_SSE42) console_printf(" SSE4.2");
    if (cpu_info.features & CPU_FEATURE_AVX) console_printf(" AVX");
    if (cpu_info.features & CPU_FEATURE_AVX2) console_printf(" AVX2");
    if (cpu_info.features & CPU_FEATURE_AVX512) console_printf(" AVX-512");
    console_printf("\n");
}

/* Get CPU info */
struct cpu_info* cpu_get_info(void)
{
    return &cpu_info;
}

/* Get CPU features */
uint32_t cpu_get_features(void)
{
    return cpu_info.features;
}

/* Check if CPU has feature */
bool cpu_has_feature(uint32_t feature)
{
    return (cpu_info.features & feature) != 0;
}

/* Flush CPU cache */
void cpu_flush_cache(void)
{
    __asm__ volatile("wbinvd" ::: "memory");
}

/* Invalidate CPU cache */
void cpu_invalidate_cache(void)
{
    __asm__ volatile("invd" ::: "memory");
}

/* Reboot the system */
void arch_reboot(void)
{
    /* Disable interrupts */
    __asm__ volatile("cli");
    
    /* Try keyboard controller reset */
    outb(0x64, 0xFE);
    
    /* If that doesn't work, halt */
    while (1) {
        __asm__ volatile("hlt");
    }
}