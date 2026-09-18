/* EMBODIOS Fixed-Point Math Implementation
 *
 * Lookup tables and vector operations for fixed-point AI inference.
 */

#include <embodios/fixed_point.h>
#include <embodios/mm.h>

#ifdef __x86_64__
#define _MM_MALLOC_H_INCLUDED
#define __MM_MALLOC_H
#include <emmintrin.h>  /* SSE2 */
#ifdef __AVX2__
#include <immintrin.h>  /* AVX2 */
#endif
#endif

/* ============================================================================
 * Precomputed Lookup Tables (Q1.15 format for efficiency)
 *
 * Values are scaled to [-32768, 32767] range (Q1.15)
 * To convert to Q16.16: value << (16 - 15) = value << 1
 * ============================================================================ */

/* Sigmoid: 1/(1+exp(-x)) for x in [-8, 8] mapped to indices [0, 255]
 * sigmoid_lut[i] = sigmoid((i - 128) * 8.0 / 128.0) * 32767 */
const int16_t sigmoid_lut[LUT_SIZE] = {
        1,     1,     2,     2,     3,     3,     4,     5,
        6,     7,     8,     9,    11,    13,    15,    17,
       20,    23,    27,    31,    36,    42,    49,    57,
       66,    77,    89,   103,   120,   139,   161,   186,
      216,   250,   289,   335,   387,   448,   518,   599,
      692,   800,   924,  1067,  1232,  1422,  1640,  1891,
     2180,  2513,  2896,  3336,  3843,  4425,  5093,  5859,
     6735,  7736,  8878,  10176, 11645, 13299, 15152, 17213,
     19490, 21983, 24689, 27598, 30692, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     /* Mirror for positive x (sigmoid is symmetric around 0.5) */
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
     32766, 32763, 32758, 32750, 32739, 32723, 32702, 32674,
     32638, 32592, 32534, 32461, 32370, 32258, 32121, 31954,
     31752, 31510, 31219, 30872, 30459, 29970, 29394, 28718,
     27932, 27024, 25985, 24809, 23491, 22029, 20426, 18690,
     16831, 14866, 12816, 10707,  8571,  6443,  4365,  2383,
       548,   -66,  -548, -1130, -1711, -2291, -2870, -3448,
     -4025, -4600, -5174, -5746, -6316, -6884, -7450, -8013
};

/* Tanh: tanh(x) for x in [-4, 4] mapped to indices [0, 255]
 * tanh_lut[i] = tanh((i - 128) * 4.0 / 128.0) * 32767 */
const int16_t tanh_lut[LUT_SIZE] = {
    -32767, -32767, -32767, -32767, -32766, -32765, -32763, -32760,
    -32756, -32750, -32743, -32733, -32721, -32706, -32687, -32664,
    -32636, -32602, -32562, -32514, -32458, -32392, -32315, -32225,
    -32121, -32001, -31863, -31705, -31524, -31319, -31086, -30822,
    -30525, -30191, -29818, -29401, -28937, -28423, -27854, -27228,
    -26540, -25788, -24968, -24078, -23116, -22080, -20970, -19785,
    -18528, -17200, -15805, -14348, -12835, -11274, -9671,  -8036,
    -6379,  -4711,  -3043,  -1386,   248,   1856,  3427,   4952,
     6420,   7823,   9154,  10408,  11581,  12671,  13678,  14601,
    15443,  16206,  16895,  17512,  18062,  18551,  18983,  19364,
    19697,  19988,  20241,  20460,  20648,  20810,  20949,  21067,
    21168,  21253,  21325,  21385,  21436,  21478,  21513,  21542,
    21565,  21585,  21601,  21614,  21625,  21633,  21640,  21646,
    21650,  21654,  21656,  21659,  21661,  21662,  21663,  21664,
    21665,  21665,  21666,  21666,  21666,  21667,  21667,  21667,
    21667,  21667,  21667,  21667,  21668,  21668,  21668,  21668,
    /* Center point (0) and positive side mirror */
    21668,  21668,  21668,  21668,  21667,  21667,  21667,  21667,
    21667,  21667,  21667,  21666,  21666,  21666,  21665,  21665,
    21664,  21663,  21662,  21661,  21659,  21656,  21654,  21650,
    21646,  21640,  21633,  21625,  21614,  21601,  21585,  21565,
    21542,  21513,  21478,  21436,  21385,  21325,  21253,  21168,
    21067,  20949,  20810,  20648,  20460,  20241,  19988,  19697,
    19364,  18983,  18551,  18062,  17512,  16895,  16206,  15443,
    14601,  13678,  12671,  11581,  10408,  9154,   7823,   6420,
     4952,   3427,   1856,    248,  -1386,  -3043,  -4711,  -6379,
    -8036,  -9671, -11274, -12835, -14348, -15805, -17200, -18528,
   -19785, -20970, -22080, -23116, -24078, -24968, -25788, -26540,
   -27228, -27854, -28423, -28937, -29401, -29818, -30191, -30525,
   -30822, -31086, -31319, -31524, -31705, -31863, -32001, -32121,
   -32225, -32315, -32392, -32458, -32514, -32562, -32602, -32636,
   -32664, -32687, -32706, -32721, -32733, -32743, -32750, -32756,
   -32760, -32763, -32765, -32766, -32767, -32767, -32767, -32767
};

/* Exp: exp(x) for x in [-8, 0] mapped to indices [0, 255]
 * exp_lut[i] = exp((i - 255) * 8.0 / 255.0) * 32767 */
const int16_t exp_lut[LUT_SIZE] = {
       11,    12,    13,    14,    15,    16,    17,    18,
       20,    21,    23,    24,    26,    28,    30,    32,
       34,    37,    39,    42,    45,    48,    52,    55,
       59,    64,    68,    73,    78,    84,    90,    96,
      103,   110,   118,   126,   135,   145,   155,   166,
      178,   191,   204,   219,   234,   251,   269,   288,
      309,   331,   355,   380,   407,   436,   468,   501,
      537,   576,   617,   661,   709,   759,   814,   872,
      935,  1002,  1074,  1151,  1234,  1322,  1417,  1519,
     1628,  1745,  1870,  2004,  2148,  2302,  2468,  2645,
     2835,  3038,  3257,  3491,  3742,  4011,  4300,  4609,
     4941,  5297,  5678,  6086,  6524,  6993,  7496,  8035,
     8613,  9232,  9896, 10607, 11370, 12187, 13064, 14003,
    15011, 16092, 17250, 18492, 19823, 21249, 22778, 24417,
    26173, 28055, 30072, 32233, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    /* Values saturate to max for positive inputs */
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767
};

/* ============================================================================
 * Vector Operations
 * ============================================================================ */

/**
 * Fixed-point dot product with SIMD optimization
 */
fixed_t fixed_dot(const fixed_t* a, const fixed_t* b, size_t n)
{
    fixed64_t sum = 0;
    size_t i = 0;

#if defined(__x86_64__) && defined(__AVX2__)
    /* AVX2: process 8 elements at a time */
    __m256i vsum = _mm256_setzero_si256();

    for (; i + 8 <= n; i += 8) {
        __m256i va = _mm256_loadu_si256((const __m256i*)&a[i]);
        __m256i vb = _mm256_loadu_si256((const __m256i*)&b[i]);

        /* 32-bit multiply to 64-bit result, then accumulate */
        __m256i prod = _mm256_mullo_epi32(va, vb);
        vsum = _mm256_add_epi32(vsum, prod);
    }

    /* Horizontal sum */
    __m128i lo = _mm256_castsi256_si128(vsum);
    __m128i hi = _mm256_extracti128_si256(vsum, 1);
    __m128i sum128 = _mm_add_epi32(lo, hi);
    sum128 = _mm_hadd_epi32(sum128, sum128);
    sum128 = _mm_hadd_epi32(sum128, sum128);
    sum = _mm_cvtsi128_si32(sum128);
#elif defined(__x86_64__)
    /* SSE2: process 4 elements at a time */
    __m128i vsum = _mm_setzero_si128();

    for (; i + 4 <= n; i += 4) {
        __m128i va = _mm_loadu_si128((const __m128i*)&a[i]);
        __m128i vb = _mm_loadu_si128((const __m128i*)&b[i]);

        /* SSE2 doesn't have 32-bit multiply, use 16-bit path */
        __m128i lo_a = _mm_unpacklo_epi32(va, va);
        __m128i lo_b = _mm_unpacklo_epi32(vb, vb);
        __m128i hi_a = _mm_unpackhi_epi32(va, va);
        __m128i hi_b = _mm_unpackhi_epi32(vb, vb);

        /* Multiply and accumulate */
        vsum = _mm_add_epi32(vsum, _mm_mullo_epi16(lo_a, lo_b));
        vsum = _mm_add_epi32(vsum, _mm_mullo_epi16(hi_a, hi_b));
    }

    /* Horizontal sum for SSE2 */
    vsum = _mm_add_epi32(vsum, _mm_shuffle_epi32(vsum, _MM_SHUFFLE(2, 3, 0, 1)));
    vsum = _mm_add_epi32(vsum, _mm_shuffle_epi32(vsum, _MM_SHUFFLE(1, 0, 3, 2)));
    sum = _mm_cvtsi128_si32(vsum);
#endif

    /* Handle remainder with scalar code */
    for (; i < n; i++) {
        sum += (fixed64_t)a[i] * (fixed64_t)b[i];
    }

    return (fixed_t)(sum >> FIXED_SHIFT);
}

/**
 * Fixed-point vector add
 */
void fixed_vadd(fixed_t* dst, const fixed_t* a, const fixed_t* b, size_t n)
{
    size_t i = 0;

#if defined(__x86_64__) && defined(__AVX2__)
    for (; i + 8 <= n; i += 8) {
        __m256i va = _mm256_loadu_si256((const __m256i*)&a[i]);
        __m256i vb = _mm256_loadu_si256((const __m256i*)&b[i]);
        __m256i vr = _mm256_add_epi32(va, vb);
        _mm256_storeu_si256((__m256i*)&dst[i], vr);
    }
#elif defined(__x86_64__)
    for (; i + 4 <= n; i += 4) {
        __m128i va = _mm_loadu_si128((const __m128i*)&a[i]);
        __m128i vb = _mm_loadu_si128((const __m128i*)&b[i]);
        __m128i vr = _mm_add_epi32(va, vb);
        _mm_storeu_si128((__m128i*)&dst[i], vr);
    }
#endif

    for (; i < n; i++) {
        dst[i] = a[i] + b[i];
    }
}

/**
 * Fixed-point vector multiply
 */
void fixed_vmul(fixed_t* dst, const fixed_t* a, const fixed_t* b, size_t n)
{
    size_t i = 0;

#if defined(__x86_64__) && defined(__AVX2__)
    for (; i + 8 <= n; i += 8) {
        __m256i va = _mm256_loadu_si256((const __m256i*)&a[i]);
        __m256i vb = _mm256_loadu_si256((const __m256i*)&b[i]);
        __m256i vr = _mm256_mullo_epi32(va, vb);
        vr = _mm256_srai_epi32(vr, FIXED_SHIFT);
        _mm256_storeu_si256((__m256i*)&dst[i], vr);
    }
#endif

    for (; i < n; i++) {
        dst[i] = fixed_mul_fast(a[i], b[i]);
    }
}

/**
 * Fixed-point vector scale
 */
void fixed_vscale(fixed_t* dst, const fixed_t* a, fixed_t scale, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[i] = fixed_mul_fast(a[i], scale);
    }
}

/**
 * Fixed-point softmax
 * Uses the max-subtraction trick for numerical stability
 */
void fixed_softmax(fixed_t* x, size_t n)
{
    if (n == 0) return;

    /* Find max */
    fixed_t max_val = x[0];
    for (size_t i = 1; i < n; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    /* Compute exp(x - max) and sum */
    fixed64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        x[i] = fixed_exp(x[i] - max_val);
        sum += x[i];
    }

    /* Normalize */
    if (sum > 0) {
        fixed_t inv_sum = fixed_div(FIXED_ONE, (fixed_t)(sum >> FIXED_SHIFT));
        for (size_t i = 0; i < n; i++) {
            x[i] = fixed_mul_fast(x[i], inv_sum);
        }
    }
}

/**
 * Fixed-point RMSNorm
 * output[i] = (x[i] / rms) * weight[i]
 * where rms = sqrt(mean(x^2))
 */
void fixed_rmsnorm(fixed_t* output, const fixed_t* x, const fixed_t* weight, size_t n)
{
    if (n == 0) return;

    /* Compute sum of squares */
    fixed64_t sum_sq = 0;
    for (size_t i = 0; i < n; i++) {
        fixed64_t val = (fixed64_t)x[i];
        sum_sq += (val * val) >> FIXED_SHIFT;
    }

    /* Mean of squares */
    fixed_t mean_sq = (fixed_t)(sum_sq / (fixed64_t)n);

    /* Approximate 1/sqrt using Newton-Raphson iteration
     * Start with initial estimate and refine */
    fixed_t rms_inv = FIXED_ONE;
    if (mean_sq > 0) {
        /* Initial estimate: 1/sqrt(x) ~= 1/(x+1) for small positive values */
        rms_inv = fixed_div(FIXED_ONE << 1, mean_sq + FIXED_ONE);

        /* One Newton-Raphson iteration: y = y * (3 - x*y*y) / 2 */
        fixed_t y2 = fixed_mul_fast(rms_inv, rms_inv);
        fixed_t xy2 = fixed_mul_fast(mean_sq, y2);
        fixed_t factor = INT_TO_FIXED(3) - xy2;
        rms_inv = (fixed_mul_fast(rms_inv, factor)) >> 1;
    }

    /* Normalize and apply weights */
    for (size_t i = 0; i < n; i++) {
        fixed_t normalized = fixed_mul_fast(x[i], rms_inv);
        output[i] = fixed_mul_fast(normalized, weight[i]);
    }
}

/**
 * Initialize fixed-point subsystem
 */
void fixed_point_init(void)
{
    /* Lookup tables are statically initialized, nothing to do */
}
