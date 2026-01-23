/* EMBODIOS HAL CPU Implementation */
#include <embodios/hal_cpu.h>
#include <embodios/types.h>

/* Global HAL CPU operations pointer */
static const struct hal_cpu_ops *cpu_ops = NULL;

/* Register HAL CPU operations */
void hal_cpu_register(const struct hal_cpu_ops *ops)
{
    cpu_ops = ops;
}

/* Get HAL CPU operations */
const struct hal_cpu_ops* hal_cpu_get_ops(void)
{
    return cpu_ops;
}

/* HAL CPU wrapper functions */
void hal_cpu_init(void)
{
    if (cpu_ops && cpu_ops->init) {
        cpu_ops->init();
    }
}

struct cpu_info* hal_cpu_get_info(void)
{
    if (cpu_ops && cpu_ops->get_info) {
        return cpu_ops->get_info();
    }
    return NULL;
}

uint32_t hal_cpu_get_features(void)
{
    if (cpu_ops && cpu_ops->get_features) {
        return cpu_ops->get_features();
    }
    return 0;
}

bool hal_cpu_has_feature(uint32_t feature)
{
    if (cpu_ops && cpu_ops->has_feature) {
        return cpu_ops->has_feature(feature);
    }
    return false;
}

uint32_t hal_cpu_get_id(void)
{
    if (cpu_ops && cpu_ops->get_id) {
        return cpu_ops->get_id();
    }
    return 0;
}

uint64_t hal_cpu_get_timestamp(void)
{
    if (cpu_ops && cpu_ops->get_timestamp) {
        return cpu_ops->get_timestamp();
    }
    return 0;
}

void hal_cpu_flush_cache(void)
{
    if (cpu_ops && cpu_ops->flush_cache) {
        cpu_ops->flush_cache();
    }
}

void hal_cpu_invalidate_cache(void)
{
    if (cpu_ops && cpu_ops->invalidate_cache) {
        cpu_ops->invalidate_cache();
    }
}

bool hal_cpu_sse2_available(void)
{
    if (cpu_ops && cpu_ops->sse2_available) {
        return cpu_ops->sse2_available();
    }
    return false;
}

const char* hal_cpu_get_sse_status(void)
{
    if (cpu_ops && cpu_ops->get_sse_status) {
        return cpu_ops->get_sse_status();
    }
    return "SSE status unavailable";
}
