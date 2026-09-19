/*
 * AVX2 implementations of the fused quantized vec_dot kernels.
 *
 * Compiled ONLY for x86_64 and ONLY with -mavx2 for this translation unit
 * (see kernel/Makefile: ai/simd_kernels_avx2.o rule). The rest of the kernel
 * stays baseline SSE2 so the scalar/SSE fallback keeps working on CPUs (and
 * emulators, e.g. QEMU TCG) without AVX2.
 *
 * NUMERICS: every kernel below is *bit-identical* to its scalar reference in
 * simd_kernels_scalar.c:
 *   - all integer partial sums are exact (no reassociation-sensitive float),
 *   - the per-group/per-block float accumulation runs in the same order with
 *     the same expressions.
 * This guarantees greedy argmax parity: logits are identical, so generated
 * text cannot diverge between the scalar and AVX2 paths.
 *
 * Host-compilable for tools/host_test_simd_kernels.c (gcc -O2 -mavx2).
 */

#if defined(__x86_64__) || defined(_M_X64) || defined(SIMD_KERNELS_HOST_TEST)

#include <embodios/simd_kernels.h>

/* Prevent mm_malloc.h from being included (needs stdlib.h, absent freestanding) */
#define _MM_MALLOC_H_INCLUDED
#define __MM_MALLOC_H
#include <immintrin.h>

/* Horizontal sum of 8 x int32 — exact */
static inline int32_t hsum_i32_8(__m256i v) {
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(v),
                              _mm256_extracti128_si256(v, 1));
    s = _mm_hadd_epi32(s, s);
    s = _mm_hadd_epi32(s, s);
    return _mm_cvtsi128_si32(s);
}

/* Extract Q4_K scale and min for group j (same as scalar reference) */
static inline void get_scale_min_k4_avx2(int j, const uint8_t* q, uint8_t* d, uint8_t* m) {
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

/* ============================================================================
 * Q8_0 x Q8_1 — 32 int8 products per block
 * ========================================================================== */
float vec_dot_q8_0_q8_1_avx2(const block_q8_0* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = emb_fp16_to_fp32(x[i].d);
        float d1 = emb_fp16_to_fp32((uint16_t)y[i].d);

        __m256i ax = _mm256_loadu_si256((const __m256i*)x[i].qs);
        __m256i ay = _mm256_loadu_si256((const __m256i*)y[i].qs);

        __m256i ax_lo = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(ax));
        __m256i ax_hi = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(ax, 1));
        __m256i ay_lo = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(ay));
        __m256i ay_hi = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(ay, 1));

        __m256i sum32 = _mm256_add_epi32(_mm256_madd_epi16(ax_lo, ay_lo),
                                         _mm256_madd_epi16(ax_hi, ay_hi));

        int32_t isum = hsum_i32_8(sum32);
        sumf += d0 * d1 * (float)isum;
    }

    return sumf;
}

/* ============================================================================
 * Q4_K x Q8_1 — 8 groups of 32 per 256-element super-block
 *
 * maddubs(u4 in [0,15], s8 in [-128,127]): pair sums in [-3840, 3810],
 * no int16 saturation. Group sums are exact integers; scales/yd applied in
 * float in the same order as the scalar reference => bit-identical.
 * ========================================================================== */
float vec_dot_q4_k_q8_1_avx2(const block_q4_K* x, const block_q8_1* y, int nb) {
    const __m256i m4 = _mm256_set1_epi8(0xF);
    const __m256i ones16 = _mm256_set1_epi16(1);
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        const block_q4_K* xb = &x[i];
        float xd = emb_fp16_to_fp32(xb->d);
        float xdmin = emb_fp16_to_fp32(xb->dmin);

        float sumi = 0.0f;
        float summs = 0.0f;

        /* 4 chunks of 32 bytes; chunk c holds groups 2c (low nibbles) and
         * 2c+1 (high nibbles), matching y blocks i*8 + 2c, i*8 + 2c+1 */
        for (int c = 0; c < 4; c++) {
            const block_q8_1* y0 = &y[i * 8 + 2 * c];
            const block_q8_1* y1 = &y[i * 8 + 2 * c + 1];

            __m256i q4bits = _mm256_loadu_si256((const __m256i*)(xb->qs + 32 * c));
            __m256i q4l = _mm256_and_si256(q4bits, m4);
            __m256i q4h = _mm256_and_si256(_mm256_srli_epi16(q4bits, 4), m4);

            __m256i q8l = _mm256_loadu_si256((const __m256i*)y0->qs);
            __m256i q8h = _mm256_loadu_si256((const __m256i*)y1->qs);

            __m256i p16l = _mm256_maddubs_epi16(q4l, q8l);
            __m256i p16h = _mm256_maddubs_epi16(q4h, q8h);

            int32_t g0 = hsum_i32_8(_mm256_madd_epi16(p16l, ones16));
            int32_t g1 = hsum_i32_8(_mm256_madd_epi16(p16h, ones16));

            uint8_t sc0, m0, sc1, m1;
            get_scale_min_k4_avx2(2 * c,     xb->scales, &sc0, &m0);
            get_scale_min_k4_avx2(2 * c + 1, xb->scales, &sc1, &m1);

            /* identical FP order to scalar: group j = 2c, then j = 2c+1 */
            summs += emb_fp16_to_fp32((uint16_t)y0->s) * (float)m0;
            sumi  += (float)sc0 * emb_fp16_to_fp32((uint16_t)y0->d) * (float)g0;
            summs += emb_fp16_to_fp32((uint16_t)y1->s) * (float)m1;
            sumi  += (float)sc1 * emb_fp16_to_fp32((uint16_t)y1->d) * (float)g1;
        }

        sumf += xd * sumi - xdmin * summs;
    }

    return sumf;
}

/* ============================================================================
 * Q5_0 x Q8_1 — 32 elements per block
 *
 * u = nibble | (qh_bit << 4) in [0,31]; dot = d0*d1*(usum - 16*ysum).
 * maddubs(u5 in [0,31], s8): pair sums in [-7936, 7874], no saturation.
 * ========================================================================== */
float vec_dot_q5_0_q8_1_avx2(const block_q5_0* x, const block_q8_1* y, int nb) {
    const __m256i m4 = _mm256_set1_epi8(0xF);
    const __m256i ones16 = _mm256_set1_epi16(1);
    const __m256i onesu8 = _mm256_set1_epi8(1);
    /* byte e of shuffled qh = qh byte (e / 8) */
    const __m256i bshuf = _mm256_setr_epi8(
        0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1,
        2,2,2,2,2,2,2,2, 3,3,3,3,3,3,3,3);
    /* bit-select mask: byte e tests bit (e % 8) */
    const __m256i bmask = _mm256_set1_epi64x(0x8040201008040201LL);
    const __m256i m16 = _mm256_set1_epi8(0x10);
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = emb_fp16_to_fp32(x[i].d);
        float d1 = emb_fp16_to_fp32((uint16_t)y[i].d);

        uint32_t qh;
        __builtin_memcpy(&qh, x[i].qh, sizeof(qh));

        /* qs is only 16 bytes: load 128-bit, build [low nibbles | high nibbles] */
        __m128i qs8 = _mm_loadu_si128((const __m128i*)x[i].qs);
        __m256i q5 = _mm256_set_m128i(_mm_srli_epi16(qs8, 4), qs8);
        q5 = _mm256_and_si256(q5, m4);

        /* expand 32 high bits -> 0x00/0x10 per byte (element e <-> qh bit e) */
        __m256i vbits = _mm256_shuffle_epi8(_mm256_set1_epi32((int32_t)qh), bshuf);
        vbits = _mm256_and_si256(vbits, bmask);
        vbits = _mm256_cmpeq_epi8(vbits, bmask);
        __m256i q5u = _mm256_or_si256(q5, _mm256_and_si256(vbits, m16));

        __m256i q8 = _mm256_loadu_si256((const __m256i*)y[i].qs);

        __m256i p16 = _mm256_maddubs_epi16(q5u, q8);
        int32_t usum = hsum_i32_8(_mm256_madd_epi16(p16, ones16));

        __m256i y16 = _mm256_maddubs_epi16(onesu8, q8);
        int32_t ysum = hsum_i32_8(_mm256_madd_epi16(y16, ones16));

        sumf += d0 * d1 * (float)(usum - 16 * ysum);
    }

    return sumf;
}

/* ============================================================================
 * Q6_K x Q8_1 — 16 groups of 16 per 256-element super-block
 *
 * q in [-32,31] (signed), so maddubs is not applicable directly: use
 * sign-extended madd. Per 32-byte q8_1 block two 16-element scale groups are
 * reduced separately, then scales*yd applied in float in scalar order.
 * ========================================================================== */
float vec_dot_q6_k_q8_1_avx2(const block_q6_K* x, const block_q8_1* y, int nb) {
    const __m256i m4 = _mm256_set1_epi8(0xF);
    const __m256i m32 = _mm256_set1_epi8(32);
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        const block_q6_K* xb = &x[i];
        float d = emb_fp16_to_fp32(xb->d);

        float sumi = 0.0f;

        for (int h = 0; h < 2; h++) {              /* two 128-element halves */
            const uint8_t* ql = xb->ql + 64 * h;
            const uint8_t* qh = xb->qh + 32 * h;
            const int8_t*  sc = xb->scales + 8 * h;
            const block_q8_1* yb = &y[i * 8 + 4 * h];

            __m256i q4bits_0 = _mm256_loadu_si256((const __m256i*)(ql));      /* ql[0..31]  */
            __m256i q4bits_1 = _mm256_loadu_si256((const __m256i*)(ql + 32)); /* ql[32..63] */
            __m256i qhbits   = _mm256_loadu_si256((const __m256i*)(qh));      /* qh[0..31]  */

            /* q1 = elems   0..31 : (ql[l]    & 0xF) | ((qh[l]>>0 & 3)<<4) */
            __m256i q1 = _mm256_or_si256(_mm256_and_si256(q4bits_0, m4),
                                         _mm256_slli_epi16(_mm256_and_si256(qhbits, _mm256_set1_epi8(0x03)), 4));
            /* q2 = elems  32..63 : (ql[l+32] & 0xF) | ((qh[l]>>2 & 3)<<4) */
            __m256i q2 = _mm256_or_si256(_mm256_and_si256(q4bits_1, m4),
                                         _mm256_slli_epi16(_mm256_and_si256(qhbits, _mm256_set1_epi8(0x0C)), 2));
            /* q3 = elems  64..95 : (ql[l]    >>  4) | ((qh[l]>>4 & 3)<<4) */
            __m256i q3 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(q4bits_0, 4), m4),
                                         _mm256_and_si256(qhbits, _mm256_set1_epi8(0x30)));
            /* q4 = elems 96..127 : (ql[l+32] >>  4) | ((qh[l]>>6 & 3)<<4) */
            __m256i q4 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(q4bits_1, 4), m4),
                                         _mm256_srli_epi16(_mm256_and_si256(qhbits, _mm256_set1_epi8(0xC0)), 2));

            q1 = _mm256_sub_epi8(q1, m32);
            q2 = _mm256_sub_epi8(q2, m32);
            q3 = _mm256_sub_epi8(q3, m32);
            q4 = _mm256_sub_epi8(q4, m32);

            const __m256i qs[4] = { q1, q2, q3, q4 };

            /* Each qk covers 32 elements = one q8_1 block, split into two
             * 16-element scale groups: low 128-bit lane -> sc[2k], high ->
             * sc[2k+1] (l<16 in low lane, l>=16 in high lane). */
            for (int k = 0; k < 4; k++) {
                __m256i qv = qs[k];
                __m256i yv = _mm256_loadu_si256((const __m256i*)yb[k].qs);

                __m256i q16lo = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(qv));
                __m256i q16hi = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(qv, 1));
                __m256i y16lo = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(yv));
                __m256i y16hi = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(yv, 1));

                int32_t g_lo = hsum_i32_8(_mm256_madd_epi16(q16lo, y16lo));
                int32_t g_hi = hsum_i32_8(_mm256_madd_epi16(q16hi, y16hi));

                float yd = emb_fp16_to_fp32((uint16_t)yb[k].d);
                sumi += (float)sc[2 * k]     * yd * (float)g_lo;
                sumi += (float)sc[2 * k + 1] * yd * (float)g_hi;
            }
        }

        sumf += d * sumi;
    }

    return sumf;
}

#endif /* x86_64 || SIMD_KERNELS_HOST_TEST */
