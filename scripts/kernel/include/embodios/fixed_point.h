/* EMBODIOS Fixed-Point Math Optimization
 *
 * High-performance fixed-point arithmetic for AI inference.
 * Uses Q16.16 format (32-bit with 16 fractional bits).
 *
 * Features:
 * - Fast multiply-accumulate
 * - Lookup tables for exp/sigmoid/tanh approximations
 * - SIMD-optimized operations
 * - No floating-point required
 */

#ifndef _EMBODIOS_FIXED_POINT_H
#define _EMBODIOS_FIXED_POINT_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Q16.16 fixed-point format */
typedef int32_t fixed_t;
typedef int64_t fixed64_t;

/* Fixed-point constants */
#define FIXED_SHIFT     16
#define FIXED_ONE       (1 << FIXED_SHIFT)          /* 1.0 in Q16.16 */
#define FIXED_HALF      (1 << (FIXED_SHIFT - 1))    /* 0.5 in Q16.16 */
#define FIXED_MAX       0x7FFFFFFF                   /* Max positive value */
#define FIXED_MIN       0x80000000                   /* Min negative value */

/* Conversion macros */
#define INT_TO_FIXED(x)     ((fixed_t)(x) << FIXED_SHIFT)
#define FIXED_TO_INT(x)     ((x) >> FIXED_SHIFT)
#define FLOAT_TO_FIXED(x)   ((fixed_t)((x) * (float)FIXED_ONE))
#define FIXED_TO_FLOAT(x)   ((float)(x) / (float)FIXED_ONE)

/* Basic fixed-point arithmetic with saturation */

/**
 * Saturating fixed-point multiply
 * Returns (a * b) >> FIXED_SHIFT with saturation
 */
static inline fixed_t fixed_mul(fixed_t a, fixed_t b)
{
    fixed64_t result = ((fixed64_t)a * (fixed64_t)b) >> FIXED_SHIFT;
    if (result > FIXED_MAX) return FIXED_MAX;
    if (result < FIXED_MIN) return FIXED_MIN;
    return (fixed_t)result;
}

/**
 * Fast fixed-point multiply (no saturation, for inner loops)
 */
static inline fixed_t fixed_mul_fast(fixed_t a, fixed_t b)
{
    return (fixed_t)(((fixed64_t)a * (fixed64_t)b) >> FIXED_SHIFT);
}

/**
 * Fixed-point multiply-accumulate
 * Returns acc + (a * b)
 */
static inline fixed64_t fixed_mac(fixed64_t acc, fixed_t a, fixed_t b)
{
    return acc + ((fixed64_t)a * (fixed64_t)b);
}

/**
 * Fixed-point divide
 * Returns (a << FIXED_SHIFT) / b
 */
static inline fixed_t fixed_div(fixed_t a, fixed_t b)
{
    if (b == 0) return (a >= 0) ? FIXED_MAX : FIXED_MIN;
    return (fixed_t)(((fixed64_t)a << FIXED_SHIFT) / b);
}

/**
 * Fixed-point absolute value
 */
static inline fixed_t fixed_abs(fixed_t x)
{
    return (x >= 0) ? x : -x;
}

/* ============================================================================
 * Lookup Table Approximations
 *
 * These use 256-entry lookup tables for fast approximations of transcendental
 * functions. Input is scaled to [0, 255] range for table lookup.
 * ============================================================================ */

/* Lookup table sizes */
#define LUT_SIZE        256
#define LUT_SHIFT       8       /* Bits for table index */

/* Sigmoid lookup table (precomputed 1/(1+exp(-x)) for x in [-8, 8]) */
extern const int16_t sigmoid_lut[LUT_SIZE];

/* Tanh lookup table (precomputed tanh(x) for x in [-4, 4]) */
extern const int16_t tanh_lut[LUT_SIZE];

/* Exp lookup table (precomputed exp(x) for x in [-8, 0]) */
extern const int16_t exp_lut[LUT_SIZE];

/**
 * Fast sigmoid approximation using lookup table
 * Input: Q16.16 fixed-point
 * Output: Q16.16 fixed-point in range [0, 1]
 */
static inline fixed_t fixed_sigmoid(fixed_t x)
{
    /* Scale x from [-8, 8] to [0, 255] */
    /* x in Q16.16, range [-8, 8] maps to [-524288, 524288] */
    fixed_t scaled = (x >> 12) + 128;  /* Shift by 12 and add 128 */

    /* Clamp to table bounds */
    if (scaled < 0) return 0;
    if (scaled >= LUT_SIZE) return FIXED_ONE;

    /* Lookup and convert to Q16.16 */
    return ((fixed_t)sigmoid_lut[scaled]) << (FIXED_SHIFT - 15);
}

/**
 * Fast tanh approximation using lookup table
 * Input: Q16.16 fixed-point
 * Output: Q16.16 fixed-point in range [-1, 1]
 */
static inline fixed_t fixed_tanh(fixed_t x)
{
    /* Scale x from [-4, 4] to [0, 255] */
    fixed_t scaled = (x >> 11) + 128;  /* Different scale for tanh range */

    /* Clamp to table bounds */
    if (scaled < 0) return -FIXED_ONE;
    if (scaled >= LUT_SIZE) return FIXED_ONE;

    /* Lookup and convert to Q16.16 (preserving sign) */
    return ((fixed_t)tanh_lut[scaled]) << (FIXED_SHIFT - 15);
}

/**
 * Fast exp approximation using lookup table
 * Input: Q16.16 fixed-point (should be <= 0 for best accuracy)
 * Output: Q16.16 fixed-point
 */
static inline fixed_t fixed_exp(fixed_t x)
{
    /* For x > 0, use exp(x) = 1/exp(-x) */
    if (x > 0) {
        fixed_t neg_exp = fixed_exp(-x);
        if (neg_exp <= 0) return FIXED_MAX;
        return fixed_div(FIXED_ONE, neg_exp);
    }

    /* Scale x from [-8, 0] to [0, 255] */
    fixed_t scaled = (x >> 13) + 255;

    /* Clamp to table bounds */
    if (scaled < 0) return 0;
    if (scaled >= LUT_SIZE) return FIXED_ONE;

    /* Lookup and convert to Q16.16 */
    return ((fixed_t)exp_lut[scaled]) << (FIXED_SHIFT - 15);
}

/**
 * Fast ReLU (max(0, x))
 */
static inline fixed_t fixed_relu(fixed_t x)
{
    return (x > 0) ? x : 0;
}

/**
 * GELU approximation using tanh: 0.5 * x * (1 + tanh(sqrt(2/pi) * (x + 0.044715 * x^3)))
 * Simplified approximation: x * sigmoid(1.702 * x)
 */
static inline fixed_t fixed_gelu(fixed_t x)
{
    /* 1.702 in Q16.16 = 111543 */
    fixed_t scaled = fixed_mul_fast(x, 111543);
    fixed_t sig = fixed_sigmoid(scaled);
    return fixed_mul_fast(x, sig);
}

/**
 * SiLU (Swish): x * sigmoid(x)
 */
static inline fixed_t fixed_silu(fixed_t x)
{
    return fixed_mul_fast(x, fixed_sigmoid(x));
}

/* ============================================================================
 * Vector Operations
 * ============================================================================ */

/**
 * Fixed-point dot product
 * Returns sum(a[i] * b[i]) for i in [0, n)
 */
fixed_t fixed_dot(const fixed_t* a, const fixed_t* b, size_t n);

/**
 * Fixed-point vector add: dst[i] = a[i] + b[i]
 */
void fixed_vadd(fixed_t* dst, const fixed_t* a, const fixed_t* b, size_t n);

/**
 * Fixed-point vector multiply: dst[i] = a[i] * b[i]
 */
void fixed_vmul(fixed_t* dst, const fixed_t* a, const fixed_t* b, size_t n);

/**
 * Fixed-point vector scale: dst[i] = a[i] * scale
 */
void fixed_vscale(fixed_t* dst, const fixed_t* a, fixed_t scale, size_t n);

/**
 * Fixed-point softmax (in-place)
 * Computes softmax over n elements
 */
void fixed_softmax(fixed_t* x, size_t n);

/**
 * Fixed-point RMSNorm
 * Computes RMS normalization with given weights
 */
void fixed_rmsnorm(fixed_t* output, const fixed_t* x, const fixed_t* weight, size_t n);

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize fixed-point lookup tables
 * Call once at startup
 */
void fixed_point_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMBODIOS_FIXED_POINT_H */
