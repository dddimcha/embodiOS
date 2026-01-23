/* EMBODIOS ARM64 CPU Detection and Management */
#include <embodios/cpu.h>
#include <embodios/hal_cpu.h>
#include <embodios/types.h>
#include <embodios/console.h>

static struct cpu_info arm64_cpu_info = {
    .vendor = "ARM",
    .model = "Generic ARMv8",
    .family = 8,
    .model_id = 0,
    .stepping = 0,
    .features = 0,
    .cores = 1
};

/* Forward declarations for HAL interface */
static bool cpu_neon_available(void);
static const char* cpu_get_neon_status(void);

/* Initialize CPU detection */
void cpu_init(void)
{
    uint64_t id_aa64isar0;

    /* Read feature register */
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(id_aa64isar0));

    /* Detect features */
    arm64_cpu_info.features = 0;

    /* ARM64 always has FPU and NEON */
    arm64_cpu_info.features |= CPU_FEATURE_FPU;
    arm64_cpu_info.features |= CPU_FEATURE_NEON;

    /* TODO: Detect more features from ID registers */

    /* Set default core count */
    arm64_cpu_info.cores = 1;
}

/* HAL CPU operations */
static const struct hal_cpu_ops aarch64_cpu_ops = {
    .init = cpu_init,
    .get_info = cpu_get_info,
    .get_features = cpu_get_features,
    .has_feature = cpu_has_feature,
    .get_id = cpu_get_id,
    .get_timestamp = cpu_get_timestamp,
    .flush_cache = cpu_flush_cache,
    .invalidate_cache = cpu_invalidate_cache,
    .sse2_available = cpu_neon_available,
    .get_sse_status = cpu_get_neon_status,
};

/* Architecture-specific timer initialization */
extern void arch_timer_init(void);

/* Architecture-specific initialization */
void arch_cpu_init(void)
{
    cpu_init();

    /* Register HAL operations */
    hal_cpu_register(&aarch64_cpu_ops);

    /* Initialize high-resolution timer HAL */
    arch_timer_init();

    console_printf("CPU: %s\n", arm64_cpu_info.vendor);
    console_printf("Model: %s\n", arm64_cpu_info.model);
    console_printf("Family: %u, Model: %u, Stepping: %u\n",
                   arm64_cpu_info.family, arm64_cpu_info.model_id, arm64_cpu_info.stepping);
    console_printf("Cores: %u\n", arm64_cpu_info.cores);
    console_printf("Features:");

    if (arm64_cpu_info.features & CPU_FEATURE_FPU) console_printf(" FPU");
    if (arm64_cpu_info.features & CPU_FEATURE_NEON) console_printf(" NEON");
    console_printf("\n");
}

void cpu_halt(void)
{
    /* ARM64 halt implementation */
    while (1) {
        __asm__ volatile("wfe");
    }
}

void arch_halt(void)
{
    cpu_halt();
}

/* Get CPU ID (MPIDR) */
uint32_t cpu_get_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

/* Read timestamp counter */
uint64_t cpu_get_timestamp(void)
{
    /* ARM64: Read system counter */
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

/* Get CPU info */
struct cpu_info* cpu_get_info(void)
{
    return &arm64_cpu_info;
}

/* Get CPU features */
uint32_t cpu_get_features(void)
{
    return arm64_cpu_info.features;
}

/* Check if CPU has feature */
bool cpu_has_feature(uint32_t feature)
{
    return (arm64_cpu_info.features & feature) != 0;
}

/* Flush CPU cache */
void cpu_flush_cache(void)
{
    /* Data cache clean and invalidate */
    __asm__ volatile("dc cisw, xzr" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/* Invalidate CPU cache */
void cpu_invalidate_cache(void)
{
    /* Data cache invalidate */
    __asm__ volatile("dc isw, xzr" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/* Check if NEON is available */
static bool cpu_neon_available(void)
{
    return cpu_has_feature(CPU_FEATURE_NEON);
}

/* Get NEON status string */
static const char* cpu_get_neon_status(void)
{
    if (arm64_cpu_info.features & CPU_FEATURE_NEON)
        return "NEON";
    return "None";
}

/* Get number of CPUs for SMP */
uint32_t smp_num_cpus(void)
{
    return arm64_cpu_info.cores;
}

/* Get CPU count (alias for smp_num_cpus) */
uint32_t cpu_count(void)
{
    return arm64_cpu_info.cores;
}

void arch_enable_interrupts(void)
{
    /* Enable interrupts */
    __asm__ volatile("msr daifclr, #2");
}

void arch_disable_interrupts(void)
{
    /* Disable interrupts */
    __asm__ volatile("msr daifset, #2");
}

void arch_reboot(void)
{
    /* ARM64 reboot - system-specific */
    /* TODO: Implement proper reboot sequence */
    while (1) {
        __asm__ volatile("wfe");
    }
}