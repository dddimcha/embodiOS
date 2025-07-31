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

/* Cache control */
void cpu_flush_cache(void);
void cpu_invalidate_cache(void);

#endif /* EMBODIOS_CPU_H */