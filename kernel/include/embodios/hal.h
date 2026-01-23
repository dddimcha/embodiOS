#ifndef EMBODIOS_HAL_H
#define EMBODIOS_HAL_H

#include <embodios/types.h>

/*
 * Hardware Abstraction Layer (HAL)
 *
 * The HAL provides a clean interface for platform-specific operations,
 * enabling EMBODIOS to run on multiple architectures with minimal porting effort.
 *
 * Architecture Support:
 *   - x86_64
 *   - aarch64 (ARM64)
 *
 * HAL Components:
 *   - CPU: Processor initialization, feature detection, cache control
 *   - Timer: High-precision timing, delays, tick counting
 *   - I/O: Port I/O and memory-mapped I/O operations
 *   - Interrupts: Interrupt controller setup and management
 */

/* HAL version */
#define HAL_VERSION_MAJOR   1
#define HAL_VERSION_MINOR   0
#define HAL_VERSION_PATCH   0

/* HAL capabilities */
#define HAL_CAP_CPU         (1 << 0)
#define HAL_CAP_TIMER       (1 << 1)
#define HAL_CAP_IO          (1 << 2)
#define HAL_CAP_INTERRUPT   (1 << 3)
#define HAL_CAP_DMA         (1 << 4)
#define HAL_CAP_CACHE       (1 << 5)

/* HAL status codes */
#define HAL_SUCCESS         0
#define HAL_ERROR          -1
#define HAL_NOT_SUPPORTED  -2
#define HAL_BUSY           -3
#define HAL_TIMEOUT        -4

/* HAL initialization */
void hal_init(void);
void hal_early_init(void);

/* HAL information */
const char* hal_get_arch_name(void);
uint32_t hal_get_capabilities(void);
bool hal_has_capability(uint32_t cap);

/* HAL component initialization */
void hal_cpu_init(void);
void hal_timer_init(void);
void hal_io_init(void);
void hal_interrupt_init(void);

/* Platform-specific hooks */
void hal_platform_init(void);
void hal_platform_shutdown(void);

#endif /* EMBODIOS_HAL_H */
