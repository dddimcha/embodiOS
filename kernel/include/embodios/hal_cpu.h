#ifndef EMBODIOS_HAL_CPU_H
#define EMBODIOS_HAL_CPU_H

#include <embodios/types.h>

/* Forward declaration */
struct cpu_info;

/* HAL CPU operations structure */
struct hal_cpu_ops {
    /* Initialization */
    void (*init)(void);

    /* CPU information */
    struct cpu_info* (*get_info)(void);
    uint32_t (*get_features)(void);
    bool (*has_feature)(uint32_t feature);

    /* CPU ID */
    uint32_t (*get_id)(void);
    uint64_t (*get_timestamp)(void);

    /* Cache control */
    void (*flush_cache)(void);
    void (*invalidate_cache)(void);

    /* Architecture-specific support */
    bool (*sse2_available)(void);
    const char* (*get_sse_status)(void);
};

/* HAL CPU interface functions */
void hal_cpu_register(const struct hal_cpu_ops *ops);
const struct hal_cpu_ops* hal_cpu_get_ops(void);

/* HAL CPU wrapper functions */
void hal_cpu_init(void);
struct cpu_info* hal_cpu_get_info(void);
uint32_t hal_cpu_get_features(void);
bool hal_cpu_has_feature(uint32_t feature);
uint32_t hal_cpu_get_id(void);
uint64_t hal_cpu_get_timestamp(void);
void hal_cpu_flush_cache(void);
void hal_cpu_invalidate_cache(void);
bool hal_cpu_sse2_available(void);
const char* hal_cpu_get_sse_status(void);

#endif /* EMBODIOS_HAL_CPU_H */
