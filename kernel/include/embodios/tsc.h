/* EMBODIOS Time Stamp Counter (TSC) Module
 *
 * High-resolution timing infrastructure using CPU cycle counters.
 * Primary purpose: Microsecond-accurate timing for AI inference and benchmarking.
 *
 * Features:
 * - Architecture-specific cycle counter reading (x86_64 TSC, ARM64 Generic Timer)
 * - TSC feature detection and validation
 * - Frequency calibration support
 * - Memory barrier variants for precise measurement
 */

#ifndef EMBODIOS_TSC_H
#define EMBODIOS_TSC_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TSC Features (x86_64)
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)

/* TSC feature flags from CPUID */
#define TSC_FEATURE_PRESENT     (1 << 0)  /* TSC is present */
#define TSC_FEATURE_INVARIANT   (1 << 1)  /* TSC rate is invariant */
#define TSC_FEATURE_DEADLINE    (1 << 2)  /* TSC deadline mode */
#define TSC_FEATURE_RDTSCP      (1 << 3)  /* RDTSCP instruction available */

/**
 * Detect TSC features using CPUID
 * @return Bitmask of TSC_FEATURE_* flags
 */
static inline uint32_t tsc_detect_features(void)
{
    uint32_t features = 0;
    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 0x1: Basic features */
    eax = 1;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "0"(eax)
        : "memory");

    if (edx & (1 << 4)) {
        features |= TSC_FEATURE_PRESENT;
    }

    /* CPUID leaf 0x80000001: Extended features */
    eax = 0x80000001;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "0"(eax)
        : "memory");

    if (edx & (1 << 27)) {
        features |= TSC_FEATURE_RDTSCP;
    }

    /* CPUID leaf 0x80000007: Advanced power management */
    eax = 0x80000007;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "0"(eax)
        : "memory");

    if (edx & (1 << 8)) {
        features |= TSC_FEATURE_INVARIANT;
    }

    return features;
}

#endif /* x86_64 */

/* ============================================================================
 * High-Resolution Counter Reading (Architecture-specific)
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)

/* x86: Read Time Stamp Counter */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* x86: Read TSC with memory barrier (serializing) */
static inline uint64_t rdtscp(void)
{
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

/* x86: Read TSC with full serialization (most precise, slowest) */
static inline uint64_t rdtsc_fence(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence" ::: "memory");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    __asm__ volatile("lfence" ::: "memory");
    return ((uint64_t)hi << 32) | lo;
}

#elif defined(__aarch64__)

/* ARM64: Read CNTVCT_EL0 (Virtual Counter) */
static inline uint64_t rdtsc(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

/* ARM64: Read counter with memory barrier */
static inline uint64_t rdtscp(void)
{
    uint64_t val;
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

/* ARM64: Read counter with full serialization */
static inline uint64_t rdtsc_fence(void)
{
    uint64_t val;
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    __asm__ volatile("dsb sy" ::: "memory");
    return val;
}

/* ARM64: Read counter frequency */
static inline uint64_t arm_get_counter_frequency(void)
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

#else

/* Fallback: no high-resolution timer */
static inline uint64_t rdtsc(void) { return 0; }
static inline uint64_t rdtscp(void) { return 0; }
static inline uint64_t rdtsc_fence(void) { return 0; }

#endif

/* ============================================================================
 * TSC Calibration and Frequency
 * ============================================================================ */

/**
 * Initialize TSC subsystem
 * Calibrates TSC frequency on x86_64
 * @return 0 on success, -1 on error
 */
int tsc_init(void);

/**
 * Get TSC frequency in Hz
 * @return Frequency in Hz, or 0 if not calibrated
 */
uint64_t tsc_get_frequency(void);

/**
 * Calibrate TSC frequency
 * Uses multiple methods: CPUID leaf 0x15, MSR, PIT-based fallback
 * @return Calibrated frequency in Hz
 */
uint64_t tsc_calibrate(void);

/**
 * Convert TSC cycles to microseconds
 * @param cycles Number of TSC cycles
 * @return Time in microseconds
 */
uint64_t tsc_to_microseconds(uint64_t cycles);

/**
 * Convert TSC cycles to nanoseconds
 * @param cycles Number of TSC cycles
 * @return Time in nanoseconds
 */
uint64_t tsc_to_nanoseconds(uint64_t cycles);

/**
 * Convert microseconds to TSC cycles
 * @param us Time in microseconds
 * @return Number of TSC cycles
 */
uint64_t microseconds_to_tsc(uint64_t us);

/**
 * Check if TSC is stable (invariant)
 * @return true if TSC is invariant and reliable for timekeeping
 */
bool tsc_is_stable(void);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_TSC_H */
