#ifndef EMBODIOS_CPU_H
#define EMBODIOS_CPU_H

#include <embodios/types.h>

/* CPU features */
#define CPU_FEATURE_FPU     (1 << 0)
#define CPU_FEATURE_SSE     (1 << 1)
#define CPU_FEATURE_SSE2    (1 << 2)
#define CPU_FEATURE_SSE3    (1 << 3)
#define CPU_FEATURE_SSSE3   (1 << 4)
#define CPU_FEATURE_SSE41   (1 << 5)
#define CPU_FEATURE_SSE42   (1 << 6)
#define CPU_FEATURE_AVX     (1 << 7)
#define CPU_FEATURE_AVX2    (1 << 8)
#define CPU_FEATURE_AVX512  (1 << 9)
#define CPU_FEATURE_NEON    (1 << 10)  /* ARM */

/* CPU info structure */
struct cpu_info {
    char vendor[16];
    char model[64];
    uint32_t family;
    uint32_t model_id;
    uint32_t stepping;
    uint32_t features;
    uint32_t cores;
    uint64_t frequency;
};

/* CPU operations */
void cpu_init(void);
struct cpu_info* cpu_get_info(void);
uint32_t cpu_get_features(void);
bool cpu_has_feature(uint32_t feature);

/* CPU ID */
uint32_t cpu_get_id(void);
uint64_t cpu_get_timestamp(void);

/* SMP support */
uint32_t smp_num_cpus(void);
uint32_t smp_get_num_online(void);
uint32_t cpu_count(void);

/* Per-CPU information for the 'cpus' shell command (x86_64 SMP) */
struct smp_cpu_info {
    uint32_t cpu_id;        /* Sequential CPU number (0 = BSP) */
    uint32_t apic_id;       /* Local APIC ID */
    int online;             /* CPU is online */
    int bsp;                /* Bootstrap processor */
    uint64_t work_count;    /* Mailbox work items executed on this CPU */
    uint64_t work_cycles;   /* TSC cycles spent in mailbox work */
    uint64_t polls;         /* Mailbox poll iterations (AP liveness) */
};
int smp_get_cpu_info(uint32_t cpu, struct smp_cpu_info *out);

/* Cross-CPU work queue: run fn(arg) on an AP's mailbox loop.
 * smp_work_dispatch posts work (spins if the previous item on that CPU is
 * still in flight); smp_work_wait blocks until the AP finished it.
 * CPU 0 (BSP) is not a valid dispatch target - run locally instead. */
typedef void (*smp_work_fn_t)(void *arg);
int smp_work_dispatch(uint32_t cpu, smp_work_fn_t fn, void *arg);
int smp_work_wait(uint32_t cpu);

/* Cache control */
void cpu_flush_cache(void);
void cpu_invalidate_cache(void);

/* SSE/FPU support (x86_64) */
bool arch_sse2_available(void);
const char* arch_get_sse_status(void);

#endif /* EMBODIOS_CPU_H */