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

#else /* !__aarch64__ */

/* Fallback scalar implementations for non-ARM platforms */

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

#endif /* __aarch64__ */
