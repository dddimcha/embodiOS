/* EMBODIOS K-quants Dequantization (GGUF/ggml-compatible, float output)
 *
 * Pure-C reference ports of dequantize_row_q2_K / q3_K / q5_K from
 * llama.cpp ggml-quants.c (no SIMD intrinsics, freestanding-friendly).
 *
 * Shared by the kernel inference paths (streaming_inference.c,
 * gguf_inference.c) and by the host-side accuracy test
 * (tools/host_test_kquants.c) so the exact same code is verified.
 *
 * All K-quant blocks hold QK_K = 256 values per super-block.
 *
 * Reference: kernel/llama_cpp/ggml/ggml-quants.c, ggml-common.h
 */

#ifndef EMBODIOS_KQUANTS_DEQUANT_H
#define EMBODIOS_KQUANTS_DEQUANT_H

#include <embodios/types.h>

#define KQUANTS_QK_K 256

/* ============================================================================
 * Block structures (match ggml-common.h layouts byte-for-byte)
 * ============================================================================ */

/* Q2_K: 2-bit, weight = d*sc*q - dmin*m, 4-bit packed scales+mins
 * Size: 16 + 64 + 2 + 2 = 84 bytes */
typedef struct __attribute__((packed)) {
    uint8_t scales[KQUANTS_QK_K / 16]; /* scales and mins, quantized with 4 bits */
    uint8_t qs[KQUANTS_QK_K / 4];      /* quants, 2 bits each */
    uint16_t d;                        /* super-block scale (fp16) */
    uint16_t dmin;                     /* super-block min scale (fp16) */
} kquant_block_q2_K;

/* Q3_K: 3-bit, weight = d * (sc-32) * q (signed, no mins)
 * Size: 32 + 64 + 12 + 2 = 110 bytes */
typedef struct __attribute__((packed)) {
    uint8_t hmask[KQUANTS_QK_K / 8];   /* quants - high bit */
    uint8_t qs[KQUANTS_QK_K / 4];      /* quants - low 2 bits */
    uint8_t scales[12];                /* scales, quantized with 6 bits */
    uint16_t d;                        /* super-block scale (fp16) */
} kquant_block_q3_K;

/* Q5_K: 5-bit, weight = d*sc*q - dmin*m, 6-bit packed scales+mins
 * Size: 2 + 2 + 12 + 32 + 128 = 176 bytes */
typedef struct __attribute__((packed)) {
    uint16_t d;                        /* super-block scale (fp16) */
    uint16_t dmin;                     /* super-block min scale (fp16) */
    uint8_t scales[12];                /* scales and mins, quantized with 6 bits */
    uint8_t qh[KQUANTS_QK_K / 8];      /* quants, high bit */
    uint8_t qs[KQUANTS_QK_K / 2];      /* quants, low 4 bits */
} kquant_block_q5_K;

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Convert IEEE-754 half-precision bits to float32 (integer-only decode) */
static inline float kquant_fp16_to_fp32(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t f;

    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            /* Denormal */
            exp = 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3ff;
            f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        /* Inf/NaN */
        f = (sign << 31) | 0x7f800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }

    union { uint32_t i; float f; } u;
    u.i = f;
    return u.f;
}

/* Decode 6-bit scale and min for sub-block j from the packed 12-byte
 * Q4_K/Q5_K format - matches ggml get_scale_min_k4() */
static inline void kquant_get_scale_min_k4(int j, const uint8_t* q,
                                           uint8_t* d, uint8_t* m)
{
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

/* Unpack the 16 signed 6-bit Q3_K scales (biased by 32) from the packed
 * 12-byte format into out[16] - matches ggml dequantize_row_q3_K() */
static inline void kquant_unpack_q3_scales(const uint8_t* packed, int8_t* out)
{
    const uint32_t kmask1 = 0x03030303;
    const uint32_t kmask2 = 0x0f0f0f0f;
    uint32_t aux[4];

    /* memcpy(aux, packed, 12) without string.h dependency */
    {
        uint8_t* dst8 = (uint8_t*)aux;
        for (int i = 0; i < 12; i++) dst8[i] = packed[i];
    }

    uint32_t tmp = aux[2];
    aux[2] = ((aux[0] >> 4) & kmask2) | (((tmp >> 4) & kmask1) << 4);
    aux[3] = ((aux[1] >> 4) & kmask2) | (((tmp >> 6) & kmask1) << 4);
    aux[0] = (aux[0] & kmask2) | (((tmp >> 0) & kmask1) << 4);
    aux[1] = (aux[1] & kmask2) | (((tmp >> 2) & kmask1) << 4);

    const int8_t* s = (const int8_t*)aux;
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

/* ============================================================================
 * Dequantization (ggml-exact ports)
 * ============================================================================ */

/* Q2_K: port of ggml dequantize_row_q2_K() */
static inline void kquant_dequant_q2_K(const void* src, float* dst, int64_t n)
{
    const kquant_block_q2_K* blocks = (const kquant_block_q2_K*)src;
    int64_t nb = n / KQUANTS_QK_K;

    for (int64_t i = 0; i < nb; i++) {
        const float d = kquant_fp16_to_fp32(blocks[i].d);
        const float min = kquant_fp16_to_fp32(blocks[i].dmin);

        const uint8_t* q = blocks[i].qs;
        float* y = dst + i * KQUANTS_QK_K;

        int is = 0;
        float dl, ml;
        for (int nn = 0; nn < KQUANTS_QK_K; nn += 128) {
            int shift = 0;
            for (int j = 0; j < 4; j++) {
                uint8_t sc = blocks[i].scales[is++];
                dl = d * (sc & 0xF);
                ml = min * (sc >> 4);
                for (int l = 0; l < 16; l++) {
                    *y++ = dl * ((int8_t)((q[l] >> shift) & 3)) - ml;
                }

                sc = blocks[i].scales[is++];
                dl = d * (sc & 0xF);
                ml = min * (sc >> 4);
                for (int l = 0; l < 16; l++) {
                    *y++ = dl * ((int8_t)((q[l + 16] >> shift) & 3)) - ml;
                }

                shift += 2;
            }
            q += 32;
        }
    }
}

/* Q3_K: port of ggml dequantize_row_q3_K() */
static inline void kquant_dequant_q3_K(const void* src, float* dst, int64_t n)
{
    const kquant_block_q3_K* blocks = (const kquant_block_q3_K*)src;
    int64_t nb = n / KQUANTS_QK_K;

    for (int64_t i = 0; i < nb; i++) {
        const float d_all = kquant_fp16_to_fp32(blocks[i].d);

        int8_t scales[16];
        kquant_unpack_q3_scales(blocks[i].scales, scales);

        const uint8_t* q = blocks[i].qs;
        const uint8_t* hm = blocks[i].hmask;
        uint8_t m = 1;
        float* y = dst + i * KQUANTS_QK_K;

        int is = 0;
        float dl;
        for (int nn = 0; nn < KQUANTS_QK_K; nn += 128) {
            int shift = 0;
            for (int j = 0; j < 4; j++) {
                dl = d_all * (scales[is++] - 32);
                for (int l = 0; l < 16; l++) {
                    *y++ = dl * ((int8_t)((q[l + 0] >> shift) & 3) - ((hm[l + 0] & m) ? 0 : 4));
                }

                dl = d_all * (scales[is++] - 32);
                for (int l = 0; l < 16; l++) {
                    *y++ = dl * ((int8_t)((q[l + 16] >> shift) & 3) - ((hm[l + 16] & m) ? 0 : 4));
                }

                shift += 2;
                m <<= 1;
            }
            q += 32;
        }
    }
}

/* Q5_K: port of ggml dequantize_row_q5_K() */
static inline void kquant_dequant_q5_K(const void* src, float* dst, int64_t n)
{
    const kquant_block_q5_K* blocks = (const kquant_block_q5_K*)src;
    int64_t nb = n / KQUANTS_QK_K;

    for (int64_t i = 0; i < nb; i++) {
        const uint8_t* ql = blocks[i].qs;
        const uint8_t* qh = blocks[i].qh;
        const float d = kquant_fp16_to_fp32(blocks[i].d);
        const float min = kquant_fp16_to_fp32(blocks[i].dmin);
        float* y = dst + i * KQUANTS_QK_K;

        int is = 0;
        uint8_t sc, m;
        uint8_t u1 = 1, u2 = 2;
        for (int j = 0; j < KQUANTS_QK_K; j += 64) {
            kquant_get_scale_min_k4(is + 0, blocks[i].scales, &sc, &m);
            const float d1 = d * sc;
            const float m1 = min * m;
            kquant_get_scale_min_k4(is + 1, blocks[i].scales, &sc, &m);
            const float d2 = d * sc;
            const float m2 = min * m;
            for (int l = 0; l < 32; l++) {
                *y++ = d1 * ((ql[l] & 0xF) + (qh[l] & u1 ? 16 : 0)) - m1;
            }
            for (int l = 0; l < 32; l++) {
                *y++ = d2 * ((ql[l] >> 4) + (qh[l] & u2 ? 16 : 0)) - m2;
            }
            ql += 32;
            is += 2;
            u1 <<= 2;
            u2 <<= 2;
        }
    }
}

/* ============================================================================
 * IQ4_NL (type 20) - 4-bit non-linear lookup quants, 32 values per block.
 *
 * Not a K-quant, but modern llama.cpp K-mixes (Q3_K_S/Q4_K_S etc.) use
 * IQ4_NL for attention/FFN tensors, so real Q3_K_S GGUF files cannot run
 * without it. Port of ggml dequantize_row_iq4_nl().
 * Block: d(fp16) + qs[16] = 18 bytes.
 * ============================================================================ */

#define KQUANTS_QK4_NL 32

typedef struct __attribute__((packed)) {
    uint16_t d;                            /* scale (fp16) */
    uint8_t qs[KQUANTS_QK4_NL / 2];        /* 4-bit quants (table indices) */
} kquant_block_iq4_nl;

static const int8_t kquant_kvalues_iq4nl[16] = {
    -127, -104, -83, -65, -49, -35, -22, -10, 1, 13, 25, 38, 53, 69, 89, 113,
};

static inline void kquant_dequant_iq4_nl(const void* src, float* dst, int64_t n)
{
    const kquant_block_iq4_nl* blocks = (const kquant_block_iq4_nl*)src;
    int64_t nb = n / KQUANTS_QK4_NL;

    for (int64_t i = 0; i < nb; i++) {
        const uint8_t* qs = blocks[i].qs;
        const float d = kquant_fp16_to_fp32(blocks[i].d);
        float* y = dst + i * KQUANTS_QK4_NL;
        for (int j = 0; j < KQUANTS_QK4_NL / 2; j++) {
            y[j + 0]               = d * kquant_kvalues_iq4nl[qs[j] & 0xf];
            y[j + KQUANTS_QK4_NL / 2] = d * kquant_kvalues_iq4nl[qs[j] >> 4];
        }
    }
}

#endif /* EMBODIOS_KQUANTS_DEQUANT_H */
