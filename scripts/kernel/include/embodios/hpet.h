/* EMBODIOS High Precision Event Timer (HPET) Module
 *
 * High-resolution timing using HPET hardware timer (x86_64).
 * Primary purpose: Alternative to TSC for microsecond-accurate timing.
 *
 * Features:
 * - HPET detection via ACPI or fixed address (0xFED00000)
 * - Hardware register mapping and configuration
 * - High-resolution counter reading (typically 10-100 MHz)
 * - Microsecond/nanosecond conversion utilities
 * - Alternative timer source when TSC is unavailable or unstable
 */

#ifndef EMBODIOS_HPET_H
#define EMBODIOS_HPET_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * HPET Register Offsets
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)

/* HPET base address (typical fixed location) */
#define HPET_DEFAULT_BASE_ADDR  0xFED00000

/* HPET Memory-Mapped Registers */
#define HPET_REG_CAPABILITIES       0x000  /* General Capabilities and ID */
#define HPET_REG_CONFIGURATION      0x010  /* General Configuration */
#define HPET_REG_INTERRUPT_STATUS   0x020  /* General Interrupt Status */
#define HPET_REG_MAIN_COUNTER       0x0F0  /* Main Counter Value */

/* Timer N Configuration and Capability Register (N = 0, 1, 2...) */
#define HPET_REG_TIMER_CONFIG(n)    (0x100 + (n) * 0x20)
#define HPET_REG_TIMER_COMPARATOR(n) (0x108 + (n) * 0x20)
#define HPET_REG_TIMER_FSB_ROUTE(n) (0x110 + (n) * 0x20)

/* ============================================================================
 * HPET Capabilities Register (Offset 0x000)
 * ============================================================================ */

/* Bits 0-7: Revision ID */
#define HPET_CAP_REV_ID_MASK        0xFF

/* Bits 8-12: Number of timers minus 1 */
#define HPET_CAP_NUM_TIMERS_SHIFT   8
#define HPET_CAP_NUM_TIMERS_MASK    0x1F

/* Bit 13: Main counter size (0 = 32-bit, 1 = 64-bit) */
#define HPET_CAP_COUNT_SIZE_64      (1ULL << 13)

/* Bit 15: Legacy replacement route capable */
#define HPET_CAP_LEGACY_ROUTE       (1ULL << 15)

/* Bits 16-31: Vendor ID */
#define HPET_CAP_VENDOR_ID_SHIFT    16
#define HPET_CAP_VENDOR_ID_MASK     0xFFFF

/* Bits 32-63: Counter tick period in femtoseconds (10^-15 seconds) */
#define HPET_CAP_PERIOD_SHIFT       32
#define HPET_CAP_PERIOD_MASK        0xFFFFFFFFULL

/* ============================================================================
 * HPET Configuration Register (Offset 0x010)
 * ============================================================================ */

/* Bit 0: Enable main counter */
#define HPET_CFG_ENABLE             (1ULL << 0)

/* Bit 1: Enable legacy replacement mapping */
#define HPET_CFG_LEGACY_ROUTE       (1ULL << 1)

/* ============================================================================
 * HPET Timer Configuration Register (Offset 0x100 + N*0x20)
 * ============================================================================ */

/* Bit 2: Interrupt type (0 = edge, 1 = level) */
#define HPET_TIMER_CFG_INT_TYPE     (1ULL << 2)

/* Bit 3: Interrupt enable */
#define HPET_TIMER_CFG_INT_ENABLE   (1ULL << 3)

/* Bit 4: Periodic mode enable */
#define HPET_TIMER_CFG_PERIODIC     (1ULL << 4)

/* Bit 5: Periodic mode capable (read-only) */
#define HPET_TIMER_CFG_PERIODIC_CAP (1ULL << 5)

/* Bit 6: Timer size (0 = 32-bit, 1 = 64-bit) */
#define HPET_TIMER_CFG_SIZE_64      (1ULL << 6)

/* Bit 8: Force 32-bit mode for 64-bit timer */
#define HPET_TIMER_CFG_FORCE_32     (1ULL << 8)

/* Bits 9-13: Interrupt routing */
#define HPET_TIMER_CFG_INT_ROUTE_SHIFT 9
#define HPET_TIMER_CFG_INT_ROUTE_MASK  0x1F

/* Bit 14: FSB interrupt delivery enable */
#define HPET_TIMER_CFG_FSB_ENABLE   (1ULL << 14)

/* Bit 15: FSB interrupt delivery capable (read-only) */
#define HPET_TIMER_CFG_FSB_CAP      (1ULL << 15)

/* ============================================================================
 * HPET Feature Flags
 * ============================================================================ */

#define HPET_FEATURE_PRESENT        (1 << 0)  /* HPET is present and accessible */
#define HPET_FEATURE_64BIT          (1 << 1)  /* Main counter is 64-bit */
#define HPET_FEATURE_LEGACY         (1 << 2)  /* Legacy replacement capable */
#define HPET_FEATURE_PERIODIC       (1 << 3)  /* At least one timer supports periodic mode */

/* ============================================================================
 * HPET Initialization and Control
 * ============================================================================ */

/**
 * Initialize HPET subsystem
 * Detects HPET via ACPI or fixed address, maps registers, reads capabilities
 * @return 0 on success, -1 on error (HPET not present or not functional)
 */
int hpet_init(void);

/**
 * Detect HPET features and capabilities
 * @return Bitmask of HPET_FEATURE_* flags
 */
uint32_t hpet_detect_features(void);

/**
 * Get HPET counter frequency in Hz
 * Calculated from counter tick period in capabilities register
 * @return Frequency in Hz, or 0 if HPET not initialized
 */
uint64_t hpet_get_frequency(void);

/**
 * Get HPET counter tick period in femtoseconds
 * @return Period in femtoseconds (10^-15 seconds), or 0 if not initialized
 */
uint64_t hpet_get_period_fs(void);

/**
 * Read HPET main counter value
 * @return Current counter value (64-bit or 32-bit depending on hardware)
 */
uint64_t hpet_read_counter(void);

/**
 * Get current time in microseconds from HPET
 * @return Time in microseconds since HPET initialization
 */
uint64_t hpet_get_microseconds(void);

/**
 * Get current time in nanoseconds from HPET
 * @return Time in nanoseconds since HPET initialization
 */
uint64_t hpet_get_nanoseconds(void);

/**
 * Convert HPET counter ticks to microseconds
 * @param ticks Number of HPET counter ticks
 * @return Time in microseconds
 */
uint64_t hpet_ticks_to_microseconds(uint64_t ticks);

/**
 * Convert HPET counter ticks to nanoseconds
 * @param ticks Number of HPET counter ticks
 * @return Time in nanoseconds
 */
uint64_t hpet_ticks_to_nanoseconds(uint64_t ticks);

/**
 * Convert microseconds to HPET counter ticks
 * @param us Time in microseconds
 * @return Number of HPET counter ticks
 */
uint64_t hpet_microseconds_to_ticks(uint64_t us);

/**
 * Delay for specified microseconds using HPET
 * Busy-wait using HPET counter
 * @param us Microseconds to delay
 */
void hpet_delay_us(uint64_t us);

/**
 * Check if HPET is available and initialized
 * @return 1 if HPET is available, 0 otherwise
 */
int hpet_is_available(void);

/**
 * Enable HPET main counter
 * Starts the HPET counting
 * @return 0 on success, -1 on error
 */
int hpet_enable(void);

/**
 * Disable HPET main counter
 * Stops the HPET counting
 */
void hpet_disable(void);

#endif /* x86_64 */

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_HPET_H */
