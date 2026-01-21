/*
 * SIMD Operations for ARM NEON
 * Optimized matrix operations using ARM NEON intrinsics
 */

#include <embodios/types.h>

#ifdef __aarch64__
#include <arm_neon.h>

/* Vector dot product using NEON (4x faster than scalar) */
fixed_t vec_dot_neon(const fixed_t* a, const fixed_t* b, size_t n) {
    int64_t sum = 0;
    size_t i = 0;

    /* Process 4 elements at a time with NEON */
    for (; i + 4 <= n; i += 4) {
        int32x4_t va = vld1q_s32((const int32_t*)&a[i]);
        int32x4_t vb = vld1q_s32((const int32_t*)&b[i]);

        /* Multiply and accumulate */
        int64x2_t prod_low = vmull_s32(vget_low_s32(va), vget_low_s32(vb));
        int64x2_t prod_high = vmull_s32(vget_high_s32(va), vget_high_s32(vb));

        sum += vgetq_lane_s64(prod_low, 0) + vgetq_lane_s64(prod_low, 1);
        sum += vgetq_lane_s64(prod_high, 0) + vgetq_lane_s64(prod_high, 1);
    }

    /* Handle remaining elements */
    for (; i < n; i++) {
        sum += (int64_t)a[i] * (int64_t)b[i];
    }

    return (fixed_t)(sum >> 16);
}

/* Matrix-vector multiplication with NEON */
void matvec_neon(const fixed_t* mat, const fixed_t* vec, fixed_t* out,
                 size_t rows, size_t cols) {
    for (size_t r = 0; r < rows; r++) {
        out[r] = vec_dot_neon(&mat[r * cols], vec, cols);
    }
}

/* RMS normalization with NEON */
void rms_norm_neon(fixed_t* out, const fixed_t* x, const fixed_t* weight, size_t size) {
    /* Compute sum of squares */
    int64_t sum_sq = 0;
    size_t i = 0;

    for (; i + 4 <= size; i += 4) {
        int32x4_t vx = vld1q_s32((const int32_t*)&x[i]);
        int64x2_t sq_low = vmull_s32(vget_low_s32(vx), vget_low_s32(vx));
        int64x2_t sq_high = vmull_s32(vget_high_s32(vx), vget_high_s32(vx));

        sum_sq += vgetq_lane_s64(sq_low, 0) + vgetq_lane_s64(sq_low, 1);
        sum_sq += vgetq_lane_s64(sq_high, 0) + vgetq_lane_s64(sq_high, 1);
    }

    for (; i < size; i++) {
        sum_sq += ((int64_t)x[i] * (int64_t)x[i]) >> 16;
    }

    /* Compute RMS */
    fixed_t rms = (fixed_t)(sum_sq / size);

    /* Normalize */
    for (i = 0; i < size; i++) {
        int64_t normalized = ((int64_t)x[i] << 16) / (rms + (1 << 10));
        out[i] = ((normalized * weight[i]) >> 16);
    }
}

/* Softmax with NEON (for attention) */
void softmax_neon(fixed_t* x, size_t size) {
    /* Find max */
    fixed_t max_val = x[0];
    for (size_t i = 1; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    /* Exp and sum (approximated with fixed-point) */
    int64_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        fixed_t shifted = x[i] - max_val;
        /* Approximate exp with Taylor series for fixed-point */
        fixed_t exp_val = (1 << 16) + shifted + ((shifted * shifted) >> 17);
        x[i] = exp_val;
        sum += exp_val;
    }

    /* Normalize */
    for (size_t i = 0; i < size; i++) {
        x[i] = (fixed_t)(((int64_t)x[i] << 16) / sum);
    }
}

/* Element-wise multiply with NEON */
void elem_mul_neon(fixed_t* out, const fixed_t* a, const fixed_t* b, size_t n) {
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        int32x4_t va = vld1q_s32((const int32_t*)&a[i]);
        int32x4_t vb = vld1q_s32((const int32_t*)&b[i]);

        /* Multiply with proper fixed-point scaling */
        int64x2_t prod_low = vmull_s32(vget_low_s32(va), vget_low_s32(vb));
        int64x2_t prod_high = vmull_s32(vget_high_s32(va), vget_high_s32(vb));

        int32x2_t result_low = vshrn_n_s64(prod_low, 16);
        int32x2_t result_high = vshrn_n_s64(prod_high, 16);

        vst1q_s32((int32_t*)&out[i], vcombine_s32(result_low, result_high));
    }

    for (; i < n; i++) {
        out[i] = (fixed_t)(((int64_t)a[i] * (int64_t)b[i]) >> 16);
    }
}

/* Element-wise add with NEON */
void elem_add_neon(fixed_t* out, const fixed_t* a, const fixed_t* b, size_t n) {
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        int32x4_t va = vld1q_s32((const int32_t*)&a[i]);
        int32x4_t vb = vld1q_s32((const int32_t*)&b[i]);
        int32x4_t vsum = vaddq_s32(va, vb);
        vst1q_s32((int32_t*)&out[i], vsum);
    }

    for (; i < n; i++) {
        out[i] = a[i] + b[i];
    }
}

#elif defined(__x86_64__) || defined(_M_X64)

/*
 * x86_64 SIMD implementations - SSE2 and AVX2
 * SSE2 is baseline for x86_64, AVX2 provides ~2x speedup
 */

/* Prevent mm_malloc.h from being included (needs stdlib.h) */
#define _MM_MALLOC_H_INCLUDED
#define __MM_MALLOC_H

#include <emmintrin.h>  /* SSE2 */

#ifdef __AVX2__
#include <immintrin.h>  /* AVX2 + FMA */
#endif

/* Runtime CPU feature detection */
static int cpu_has_avx2_checked = 0;
static int cpu_has_avx2 = 0;

static inline int check_avx2_support(void) {
    if (cpu_has_avx2_checked) return cpu_has_avx2;
    cpu_has_avx2_checked = 1;
#ifdef __AVX2__
    /* Check CPUID for AVX2 support */
    unsigned int eax, ebx, ecx, edx;
    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0)
    );
    cpu_has_avx2 = (ebx & (1 << 5)) != 0;  /* AVX2 bit */
#endif
    return cpu_has_avx2;
}

/* AVX2 vector dot product (8x float per iteration) */
#ifdef __AVX2__
static fixed_t vec_dot_avx2(const fixed_t* a, const fixed_t* b, size_t n) {
    int64_t sum = 0;
    size_t i = 0;

    /* Process 8 elements at a time with AVX2 */
    for (; i + 8 <= n; i += 8) {
        __m256i va = _mm256_loadu_si256((const __m256i*)&a[i]);
        __m256i vb = _mm256_loadu_si256((const __m256i*)&b[i]);

        /* Multiply pairs: low 4 and high 4 */
        __m256i prod = _mm256_mullo_epi32(va, vb);

        /* Sum horizontally - extract and add */
        __m128i lo = _mm256_castsi256_si128(prod);
        __m128i hi = _mm256_extracti128_si256(prod, 1);
        __m128i sum128 = _mm_add_epi32(lo, hi);

        /* Continue horizontal sum */
        sum128 = _mm_hadd_epi32(sum128, sum128);
        sum128 = _mm_hadd_epi32(sum128, sum128);
        sum += (int64_t)_mm_cvtsi128_si32(sum128);
    }

    /* Handle remaining elements */
    for (; i < n; i++) {
        sum += (int64_t)a[i] * (int64_t)b[i];
    }

    return (fixed_t)(sum >> 16);
}
#endif

/* SSE2 vector dot product (4x float per iteration) */
static fixed_t vec_dot_sse2(const fixed_t* a, const fixed_t* b, size_t n) {
    int64_t sum = 0;
    size_t i = 0;

    /* Process 4 elements at a time with SSE2 */
    for (; i + 4 <= n; i += 4) {
        __m128i va = _mm_loadu_si128((const __m128i*)&a[i]);
        __m128i vb = _mm_loadu_si128((const __m128i*)&b[i]);

        /* SSE2 doesn't have mullo_epi32, use pmulld workaround */
        __m128i tmp1 = _mm_mul_epu32(va, vb);                      /* a0*b0, a2*b2 */
        __m128i tmp2 = _mm_mul_epu32(_mm_srli_si128(va, 4),
                                     _mm_srli_si128(vb, 4));       /* a1*b1, a3*b3 */

        /* Extract and accumulate */
        sum += (int64_t)_mm_cvtsi128_si64(tmp1);
        sum += (int64_t)_mm_cvtsi128_si64(_mm_srli_si128(tmp1, 8));
        sum += (int64_t)_mm_cvtsi128_si64(tmp2);
        sum += (int64_t)_mm_cvtsi128_si64(_mm_srli_si128(tmp2, 8));
    }

    /* Handle remaining elements */
    for (; i < n; i++) {
        sum += (int64_t)a[i] * (int64_t)b[i];
    }

    return (fixed_t)(sum >> 16);
}

/* Public API - dispatches to best available implementation */
fixed_t vec_dot_neon(const fixed_t* a, const fixed_t* b, size_t n) {
#ifdef __AVX2__
    if (check_avx2_support()) {
        return vec_dot_avx2(a, b, n);
    }
#endif
    return vec_dot_sse2(a, b, n);
}

void matvec_neon(const fixed_t* mat, const fixed_t* vec, fixed_t* out,
                 size_t rows, size_t cols) {
    for (size_t r = 0; r < rows; r++) {
        out[r] = vec_dot_neon(&mat[r * cols], vec, cols);
    }
}

/* RMS normalization with SSE2/AVX2 */
void rms_norm_neon(fixed_t* out, const fixed_t* x, const fixed_t* weight, size_t size) {
    int64_t sum_sq = 0;
    size_t i = 0;

#ifdef __AVX2__
    if (check_avx2_support()) {
        /* AVX2 path: process 8 at a time */
        for (; i + 8 <= size; i += 8) {
            __m256i vx = _mm256_loadu_si256((const __m256i*)&x[i]);
            __m256i sq = _mm256_mullo_epi32(vx, vx);

            /* Horizontal sum */
            __m128i lo = _mm256_castsi256_si128(sq);
            __m128i hi = _mm256_extracti128_si256(sq, 1);
            __m128i sum128 = _mm_add_epi32(lo, hi);
            sum128 = _mm_hadd_epi32(sum128, sum128);
            sum128 = _mm_hadd_epi32(sum128, sum128);
            sum_sq += ((int64_t)_mm_cvtsi128_si32(sum128)) >> 16;
        }
    }
#endif

    /* Scalar remainder */
    for (; i < size; i++) {
        sum_sq += ((int64_t)x[i] * (int64_t)x[i]) >> 16;
    }

    fixed_t rms = (fixed_t)(sum_sq / size);

    for (i = 0; i < size; i++) {
        int64_t normalized = ((int64_t)x[i] << 16) / (rms + (1 << 10));
        out[i] = ((normalized * weight[i]) >> 16);
    }
}

void softmax_neon(fixed_t* x, size_t size) {
    fixed_t max_val = x[0];
    for (size_t i = 1; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    int64_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        fixed_t shifted = x[i] - max_val;
        fixed_t exp_val = (1 << 16) + shifted + ((shifted * shifted) >> 17);
        x[i] = exp_val;
        sum += exp_val;
    }

    for (size_t i = 0; i < size; i++) {
        x[i] = (fixed_t)(((int64_t)x[i] << 16) / sum);
    }
}

/* Element-wise multiply with AVX2/SSE2 */
void elem_mul_neon(fixed_t* out, const fixed_t* a, const fixed_t* b, size_t n) {
    size_t i = 0;

#ifdef __AVX2__
    if (check_avx2_support()) {
        for (; i + 8 <= n; i += 8) {
            __m256i va = _mm256_loadu_si256((const __m256i*)&a[i]);
            __m256i vb = _mm256_loadu_si256((const __m256i*)&b[i]);
            __m256i prod = _mm256_mullo_epi32(va, vb);
            /* Shift right by 16 for fixed-point */
            prod = _mm256_srai_epi32(prod, 16);
            _mm256_storeu_si256((__m256i*)&out[i], prod);
        }
    }
#endif

    for (; i < n; i++) {
        out[i] = (fixed_t)(((int64_t)a[i] * (int64_t)b[i]) >> 16);
    }
}

/* Element-wise add with AVX2/SSE2 */
void elem_add_neon(fixed_t* out, const fixed_t* a, const fixed_t* b, size_t n) {
    size_t i = 0;

#ifdef __AVX2__
    if (check_avx2_support()) {
        for (; i + 8 <= n; i += 8) {
            __m256i va = _mm256_loadu_si256((const __m256i*)&a[i]);
            __m256i vb = _mm256_loadu_si256((const __m256i*)&b[i]);
            __m256i vsum = _mm256_add_epi32(va, vb);
            _mm256_storeu_si256((__m256i*)&out[i], vsum);
        }
    }
#endif

    /* SSE2 path for remainder or if AVX2 not available */
    for (; i + 4 <= n; i += 4) {
        __m128i va = _mm_loadu_si128((const __m128i*)&a[i]);
        __m128i vb = _mm_loadu_si128((const __m128i*)&b[i]);
        __m128i vsum = _mm_add_epi32(va, vb);
        _mm_storeu_si128((__m128i*)&out[i], vsum);
    }

    for (; i < n; i++) {
        out[i] = a[i] + b[i];
    }
}

/*
 * AVX2 Matrix-Vector multiplication for dense layers
 * Processes 8 output elements per iteration
 */
#ifdef __AVX2__
void matvec_avx2(const fixed_t* mat, const fixed_t* vec, fixed_t* out,
                 size_t rows, size_t cols) {
    for (size_t r = 0; r < rows; r++) {
        __m256i vsum = _mm256_setzero_si256();
        const fixed_t* row = &mat[r * cols];
        size_t c = 0;

        /* Process 8 columns at a time */
        for (; c + 8 <= cols; c += 8) {
            __m256i vm = _mm256_loadu_si256((const __m256i*)&row[c]);
            __m256i vv = _mm256_loadu_si256((const __m256i*)&vec[c]);
            __m256i prod = _mm256_mullo_epi32(vm, vv);
            vsum = _mm256_add_epi32(vsum, prod);
        }

        /* Horizontal sum */
        __m128i lo = _mm256_castsi256_si128(vsum);
        __m128i hi = _mm256_extracti128_si256(vsum, 1);
        __m128i sum128 = _mm_add_epi32(lo, hi);
        sum128 = _mm_hadd_epi32(sum128, sum128);
        sum128 = _mm_hadd_epi32(sum128, sum128);
        int64_t sum = (int64_t)_mm_cvtsi128_si32(sum128);

        /* Handle remainder */
        for (; c < cols; c++) {
            sum += (int64_t)row[c] * (int64_t)vec[c];
        }

        out[r] = (fixed_t)(sum >> 16);
    }
}
#endif

#else /* Scalar fallback for other platforms */

/* Pure scalar implementations */
fixed_t vec_dot_neon(const fixed_t* a, const fixed_t* b, size_t n) {
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += (int64_t)a[i] * (int64_t)b[i];
    }
    return (fixed_t)(sum >> 16);
}

void matvec_neon(const fixed_t* mat, const fixed_t* vec, fixed_t* out,
                 size_t rows, size_t cols) {
    for (size_t r = 0; r < rows; r++) {
        out[r] = vec_dot_neon(&mat[r * cols], vec, cols);
    }
}

void rms_norm_neon(fixed_t* out, const fixed_t* x, const fixed_t* weight, size_t size) {
    int64_t sum_sq = 0;
    for (size_t i = 0; i < size; i++) {
        sum_sq += ((int64_t)x[i] * (int64_t)x[i]) >> 16;
    }
    fixed_t rms = (fixed_t)(sum_sq / size);

    for (size_t i = 0; i < size; i++) {
        int64_t normalized = ((int64_t)x[i] << 16) / (rms + (1 << 10));
        out[i] = ((normalized * weight[i]) >> 16);
    }
}

void softmax_neon(fixed_t* x, size_t size) {
    fixed_t max_val = x[0];
    for (size_t i = 1; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    int64_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        fixed_t shifted = x[i] - max_val;
        fixed_t exp_val = (1 << 16) + shifted + ((shifted * shifted) >> 17);
        x[i] = exp_val;
        sum += exp_val;
    }

    for (size_t i = 0; i < size; i++) {
        x[i] = (fixed_t)(((int64_t)x[i] << 16) / sum);
    }
}

void elem_mul_neon(fixed_t* out, const fixed_t* a, const fixed_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] = (fixed_t)(((int64_t)a[i] * (int64_t)b[i]) >> 16);
    }
}

void elem_add_neon(fixed_t* out, const fixed_t* a, const fixed_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] = a[i] + b[i];
    }
}

#endif /* __aarch64__ / __x86_64__ */
