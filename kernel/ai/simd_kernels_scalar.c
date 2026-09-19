/*
 * Scalar (and baseline-SIMD) reference implementations of the fused
 * quantized vec_dot kernels.
 *
 * These are the dispatch fallbacks when AVX2 is not available (e.g. QEMU
 * TCG). The Q8_0 and Q4_K x86_64/aarch64 variants are moved verbatim from
 * kernel/ai/streaming_inference.c so that fallback behavior is bit-identical
 * to master. The Q5_0 and Q6_K fused variants are new (on master those types
 * fall back to dequantize+float-dot; they are only selected explicitly).
 *
 * Host-compilable: only depends on embodios/simd_kernels.h (+ arm_neon.h on
 * aarch64 / emmintrin.h on x86_64 for the baseline variants).
 */

#include <embodios/simd_kernels.h>

#if defined(__x86_64__) || defined(_M_X64)
/* Prevent mm_malloc.h from being included (needs stdlib.h, absent freestanding) */
#define _MM_MALLOC_H_INCLUDED
#define __MM_MALLOC_H
#include <emmintrin.h>  /* SSE2 */
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

/* Extract Q4_K scale and min for group j (identical to
 * get_scale_min_k4_fused() in streaming_inference.c) */
static inline void get_scale_min_k4(int j, const uint8_t* q, uint8_t* d, uint8_t* m) {
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

/* ============================================================================
 * Q8_0 x Q8_1
 * ========================================================================== */

#if defined(__aarch64__)
/* ARM NEON fused Q8_0 x Q8_1 dot product (moved from streaming_inference.c) */
float vec_dot_q8_0_q8_1_scalar(const block_q8_0* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = emb_fp16_to_fp32(x[i].d);
        float d1 = emb_fp16_to_fp32((uint16_t)y[i].d);

        int32x4_t sum_vec = vdupq_n_s32(0);

        for (int j = 0; j < QK8_0; j += 16) {
            int8x16_t ax = vld1q_s8(x[i].qs + j);
            int8x16_t ay = vld1q_s8(y[i].qs + j);

            int16x8_t prod_lo = vmull_s8(vget_low_s8(ax), vget_low_s8(ay));
            int16x8_t prod_hi = vmull_s8(vget_high_s8(ax), vget_high_s8(ay));

            sum_vec = vpadalq_s16(sum_vec, prod_lo);
            sum_vec = vpadalq_s16(sum_vec, prod_hi);
        }

        int32_t isum = vaddvq_s32(sum_vec);
        sumf += d0 * d1 * (float)isum;
    }

    return sumf;
}
#else
/* Scalar fallback */
float vec_dot_q8_0_q8_1_scalar(const block_q8_0* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = emb_fp16_to_fp32(x[i].d);
        float d1 = emb_fp16_to_fp32((uint16_t)y[i].d);
        int32_t isum = 0;

        for (int j = 0; j < QK8_0; j++) {
            isum += (int32_t)x[i].qs[j] * (int32_t)y[i].qs[j];
        }

        sumf += d0 * d1 * (float)isum;
    }

    return sumf;
}
#endif

#if defined(__x86_64__) || defined(_M_X64)
/* SSE2 baseline (moved from streaming_inference.c — the fastest Q8_0 path
 * that shipped on master) */
float vec_dot_q8_0_q8_1_sse(const block_q8_0* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = emb_fp16_to_fp32(x[i].d);
        float d1 = emb_fp16_to_fp32((uint16_t)y[i].d);

        __m128i sum_vec = _mm_setzero_si128();

        for (int j = 0; j < QK8_0; j += 16) {
            __m128i ax = _mm_loadu_si128((const __m128i*)(x[i].qs + j));
            __m128i ay = _mm_loadu_si128((const __m128i*)(y[i].qs + j));

            __m128i ax_lo = _mm_srai_epi16(_mm_unpacklo_epi8(ax, ax), 8);
            __m128i ax_hi = _mm_srai_epi16(_mm_unpackhi_epi8(ax, ax), 8);
            __m128i ay_lo = _mm_srai_epi16(_mm_unpacklo_epi8(ay, ay), 8);
            __m128i ay_hi = _mm_srai_epi16(_mm_unpackhi_epi8(ay, ay), 8);

            __m128i prod_lo = _mm_madd_epi16(ax_lo, ay_lo);
            __m128i prod_hi = _mm_madd_epi16(ax_hi, ay_hi);

            sum_vec = _mm_add_epi32(sum_vec, prod_lo);
            sum_vec = _mm_add_epi32(sum_vec, prod_hi);
        }

        __m128i sum_hi = _mm_shuffle_epi32(sum_vec, _MM_SHUFFLE(1, 0, 3, 2));
        sum_vec = _mm_add_epi32(sum_vec, sum_hi);
        sum_hi = _mm_shuffle_epi32(sum_vec, _MM_SHUFFLE(2, 3, 0, 1));
        sum_vec = _mm_add_epi32(sum_vec, sum_hi);

        int32_t isum = _mm_cvtsi128_si32(sum_vec);
        sumf += d0 * d1 * (float)isum;
    }

    return sumf;
}
#endif

/* ============================================================================
 * Q4_K x Q8_1
 *
 * Q4_K layout (128 bytes qs for 256 elements):
 * - Bytes 0-31: groups 0,1 (low nibbles=group0, high nibbles=group1)
 * - Bytes 32-63: groups 2,3 ... etc.
 * For each Q4_K block we consume 8 Q8_1 blocks (256/32 = 8).
 * ========================================================================== */

#if defined(__aarch64__)
/* ARM NEON Q4_K x Q8_1 (moved from streaming_inference.c) */
float vec_dot_q4_k_q8_1_scalar(const block_q4_K* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        const block_q4_K* xb = &x[i];
        float xd = emb_fp16_to_fp32(xb->d);
        float xdmin = emb_fp16_to_fp32(xb->dmin);

        float sumi = 0.0f;
        float summs = 0.0f;

        for (int j = 0; j < QK_K / 32; j++) {  /* 8 groups: 0-7 */
            const block_q8_1* yb = &y[i * 8 + j];
            float yd = emb_fp16_to_fp32((uint16_t)yb->d);

            uint8_t sc, m;
            get_scale_min_k4(j, xb->scales, &sc, &m);

            summs += emb_fp16_to_fp32((uint16_t)yb->s) * (float)m;

            const uint8_t* qs = xb->qs + (j / 2) * 32;
            int use_high = j & 1;

            uint8x16_t xq0 = vld1q_u8(qs);
            uint8x16_t xq1 = vld1q_u8(qs + 16);

            int8x16_t x0, x1;
            if (use_high) {
                x0 = vreinterpretq_s8_u8(vshrq_n_u8(xq0, 4));
                x1 = vreinterpretq_s8_u8(vshrq_n_u8(xq1, 4));
            } else {
                x0 = vreinterpretq_s8_u8(vandq_u8(xq0, vdupq_n_u8(0xF)));
                x1 = vreinterpretq_s8_u8(vandq_u8(xq1, vdupq_n_u8(0xF)));
            }

            int8x16_t yq0 = vld1q_s8(yb->qs);
            int8x16_t yq1 = vld1q_s8(yb->qs + 16);

            int16x8_t prod0_lo = vmull_s8(vget_low_s8(x0), vget_low_s8(yq0));
            int16x8_t prod0_hi = vmull_s8(vget_high_s8(x0), vget_high_s8(yq0));
            int16x8_t prod1_lo = vmull_s8(vget_low_s8(x1), vget_low_s8(yq1));
            int16x8_t prod1_hi = vmull_s8(vget_high_s8(x1), vget_high_s8(yq1));

            int32x4_t sum32 = vpaddlq_s16(prod0_lo);
            sum32 = vpadalq_s16(sum32, prod0_hi);
            sum32 = vpadalq_s16(sum32, prod1_lo);
            sum32 = vpadalq_s16(sum32, prod1_hi);

            int32_t group_sum = vaddvq_s32(sum32);

            sumi += (float)sc * yd * (float)group_sum;
        }

        sumf += xd * sumi - xdmin * summs;
    }

    return sumf;
}
#else
/* Scalar fallback for Q4_K x Q8_1 (moved from streaming_inference.c) */
float vec_dot_q4_k_q8_1_scalar(const block_q4_K* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        const block_q4_K* xb = &x[i];
        float xd = emb_fp16_to_fp32(xb->d);
        float xdmin = emb_fp16_to_fp32(xb->dmin);

        float sumi = 0.0f;
        float summs = 0.0f;

        for (int j = 0; j < QK_K / 32; j++) {
            const block_q8_1* yb = &y[i * 8 + j];
            float yd = emb_fp16_to_fp32((uint16_t)yb->d);

            uint8_t sc, m;
            get_scale_min_k4(j, xb->scales, &sc, &m);

            summs += emb_fp16_to_fp32((uint16_t)yb->s) * (float)m;

            /* Q4_K byte offset: groups 0,1 use bytes 0-31, groups 2,3 use 32-63, etc */
            const uint8_t* qs = xb->qs + (j / 2) * 32;
            int use_high = j & 1;

            int32_t group_sum = 0;
            for (int k = 0; k < 32; k++) {
                int x_val = use_high ? (qs[k] >> 4) : (qs[k] & 0xF);
                group_sum += x_val * yb->qs[k];
            }

            sumi += (float)sc * yd * (float)group_sum;
        }

        sumf += xd * sumi - xdmin * summs;
    }

    return sumf;
}
#endif

/* ============================================================================
 * Q5_0 x Q8_1 (new fused path)
 *
 * block_q5_0: qh[4] = 32 high bits (element e <-> bit e), qs[16] nibbles
 * (element j = low nibble of qs[j], element j+16 = high nibble of qs[j]).
 * dequant: w = d * ((nibble | (bit << 4)) - 16)
 * dot(w, x) = d0*d1 * ( sum(u_e * y8_e) - 16 * sum(y8_e) ),  u in [0,31]
 * ========================================================================== */
float vec_dot_q5_0_q8_1_scalar(const block_q5_0* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = emb_fp16_to_fp32(x[i].d);
        float d1 = emb_fp16_to_fp32((uint16_t)y[i].d);

        uint32_t qh;
        __builtin_memcpy(&qh, x[i].qh, sizeof(qh));

        int32_t usum = 0;  /* sum(u_e * y8_e) */
        int32_t ysum = 0;  /* sum(y8_e)      */
        for (int j = 0; j < QK8_0 / 2; j++) {
            const int u0 = (x[i].qs[j] & 0x0F) | (((qh >> (j + 0)) << 4) & 0x10);
            const int u1 = (x[i].qs[j] >> 4)   | ((qh >> (j + 12)) & 0x10);
            usum += u0 * (int32_t)y[i].qs[j];
            usum += u1 * (int32_t)y[i].qs[j + QK8_0 / 2];
            ysum += (int32_t)y[i].qs[j] + (int32_t)y[i].qs[j + QK8_0 / 2];
        }

        sumf += d0 * d1 * (float)(usum - 16 * ysum);
    }

    return sumf;
}

/* ============================================================================
 * Q6_K x Q8_1 (new fused path)
 *
 * Per 128-element half h (h = 0,1):
 *   ql += 64*h, qh += 32*h, scales += 8*h, y blocks += 4*h
 *   l = 0..31:
 *     q[l+ 0] = (ql[l]    & 0xF) | ((qh[l] >> 0 & 3) << 4) - 32   scale sc[0/1]
 *     q[l+32] = (ql[l+32] & 0xF) | ((qh[l] >> 2 & 3) << 4) - 32   scale sc[2/3]
 *     q[l+64] = (ql[l]    >>  4) | ((qh[l] >> 4 & 3) << 4) - 32   scale sc[4/5]
 *     q[l+96] = (ql[l+32] >>  4) | ((qh[l] >> 6 & 3) << 4) - 32   scale sc[6/7]
 *   scale index within each 32-group: sc[2*g + l/16]
 * dot = d * sum_g sc_g * yd_{g/2} * (sum_{e in g} q_e * y8_e)
 * ========================================================================== */
float vec_dot_q6_k_q8_1_scalar(const block_q6_K* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        const block_q6_K* xb = &x[i];
        float d = emb_fp16_to_fp32(xb->d);

        float sumi = 0.0f;
        int32_t g6_acc[8] = {0};

        for (int h = 0; h < 2; h++) {              /* two 128-element halves */
            const uint8_t* ql = xb->ql + 64 * h;
            const uint8_t* qh = xb->qh + 32 * h;
            const int8_t*  sc = xb->scales + 8 * h;
            const block_q8_1* yb = &y[i * 8 + 4 * h];

            /* 4 groups of 32 elements per half */
            for (int l = 0; l < 32; l++) {
                int is = l / 16;

                int q1 = (int8_t)((ql[l +  0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int q2 = (int8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = (int8_t)((ql[l +  0] >>  4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = (int8_t)((ql[l + 32] >>  4) | (((qh[l] >> 6) & 3) << 4)) - 32;

                /* accumulate into per-16-element group sums (exact int math) */
                g6_acc[is + 0] += q1 * (int32_t)yb[0].qs[l];
                g6_acc[is + 2] += q2 * (int32_t)yb[1].qs[l];
                g6_acc[is + 4] += q3 * (int32_t)yb[2].qs[l];
                g6_acc[is + 6] += q4 * (int32_t)yb[3].qs[l];
            }

            /* apply scales in group order 0..7, yd per 32-element q8_1 block */
            for (int g = 0; g < 8; g++) {
                float yd = emb_fp16_to_fp32((uint16_t)yb[g / 2].d);
                sumi += (float)sc[g] * yd * (float)g6_acc[g];
                g6_acc[g] = 0;
            }
        }

        sumf += d * sumi;
    }

    return sumf;
}
