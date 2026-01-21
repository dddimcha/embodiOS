/*
 * Quantized Matrix Multiplication with ARM NEON SIMD
 * Direct Q4_K matmul without full dequantization for maximum speed
 */

#include <embodios/types.h>

#define QK_K 256
#define FIXED_SHIFT 16

#ifdef __aarch64__
#include <arm_neon.h>

/* Q4_K block structure - ARM version with float16_t */
struct block_q4_k {
    float16_t d;           /* Delta (scale) */
    float16_t dmin;        /* Min delta */
    uint8_t scales[12];    /* Quantized scales */
    uint8_t qs[QK_K/2];    /* 4-bit quantized values */
} __attribute__((packed));
/* SIMD-optimized Q4_K matrix-vector multiply */
void q4_k_matvec_neon(const void* weight_data, const fixed_t* input,
                      fixed_t* output, size_t rows, size_t cols) {
    const struct block_q4_k* blocks = (const struct block_q4_k*)weight_data;
    size_t n_blocks_per_row = (cols + QK_K - 1) / QK_K;

    for (size_t row = 0; row < rows; row++) {
        int64_t sum = 0;
        const struct block_q4_k* row_blocks = &blocks[row * n_blocks_per_row];

        for (size_t block_idx = 0; block_idx < n_blocks_per_row; block_idx++) {
            const struct block_q4_k* block = &row_blocks[block_idx];
            size_t input_offset = block_idx * QK_K;

            /* Extract scale */
            fixed_t d = ((int32_t)block->d) << (FIXED_SHIFT - 8);

            /* Process 8 values at a time with NEON */
            for (size_t i = 0; i < QK_K && (input_offset + i) < cols; i += 8) {
                /* Load 8 input values */
                int32x4_t vinput_low = vld1q_s32(&input[input_offset + i]);
                int32x4_t vinput_high = vld1q_s32(&input[input_offset + i + 4]);

                /* Unpack 4 quantized nibbles */
                uint8_t packed_low = block->qs[(i/2)];
                uint8_t packed_high = block->qs[(i/2) + 1];

                int8_t q[8];
                q[0] = packed_low & 0x0F;
                q[1] = (packed_low >> 4) & 0x0F;
                q[2] = packed_high & 0x0F;
                q[3] = (packed_high >> 4) & 0x0F;

                packed_low = block->qs[(i/2) + 2];
                packed_high = block->qs[(i/2) + 3];
                q[4] = packed_low & 0x0F;
                q[5] = (packed_low >> 4) & 0x0F;
                q[6] = packed_high & 0x0F;
                q[7] = (packed_high >> 4) & 0x0F;

                /* Convert to 32-bit */
                int32x4_t vq_low = {q[0], q[1], q[2], q[3]};
                int32x4_t vq_high = {q[4], q[5], q[6], q[7]};

                /* Multiply: weight * input */
                int64x2_t prod_low = vmull_s32(vget_low_s32(vq_low), vget_low_s32(vinput_low));
                int64x2_t prod_high = vmull_s32(vget_high_s32(vq_low), vget_high_s32(vinput_low));

                /* Accumulate */
                sum += vgetq_lane_s64(prod_low, 0) + vgetq_lane_s64(prod_low, 1);
                sum += vgetq_lane_s64(prod_high, 0) + vgetq_lane_s64(prod_high, 1);

                prod_low = vmull_s32(vget_low_s32(vq_high), vget_low_s32(vinput_high));
                prod_high = vmull_s32(vget_high_s32(vq_high), vget_high_s32(vinput_high));

                sum += vgetq_lane_s64(prod_low, 0) + vgetq_lane_s64(prod_low, 1);
                sum += vgetq_lane_s64(prod_high, 0) + vgetq_lane_s64(prod_high, 1);
            }

            /* Apply block scale */
            sum = (sum * d) >> FIXED_SHIFT;
        }

        output[row] = (fixed_t)(sum >> FIXED_SHIFT);
    }
}
#elif defined(__x86_64__) || defined(_M_X64)

/*
 * x86_64 SSE2/AVX2 implementations for Q4_K matrix-vector multiply
 */

/* Prevent mm_malloc.h from being included (needs stdlib.h) */
#define _MM_MALLOC_H_INCLUDED
#define __MM_MALLOC_H

#include <emmintrin.h>  /* SSE2 */

#ifdef __AVX2__
#include <immintrin.h>  /* AVX2 */
#endif

/* Q4_K block structure - x86_64 version */
struct block_q4_k {
    uint16_t d;            /* Delta (scale) as uint16 */
    uint16_t dmin;         /* Min delta as uint16 */
    uint8_t scales[12];    /* Quantized scales */
    uint8_t qs[QK_K/2];    /* 4-bit quantized values */
} __attribute__((packed));

/* Runtime AVX2 detection */
static int x86_has_avx2_checked = 0;
static int x86_has_avx2 = 0;

static inline int check_x86_avx2(void) {
    if (x86_has_avx2_checked) return x86_has_avx2;
    x86_has_avx2_checked = 1;
#ifdef __AVX2__
    unsigned int eax, ebx, ecx, edx;
    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0)
    );
    x86_has_avx2 = (ebx & (1 << 5)) != 0;
#endif
    return x86_has_avx2;
}

#ifdef __AVX2__
/* AVX2-optimized Q4_K matrix-vector multiply */
static void q4_k_matvec_avx2(const void* weight_data, const fixed_t* input,
                              fixed_t* output, size_t rows, size_t cols) {
    const struct block_q4_k* blocks = (const struct block_q4_k*)weight_data;
    size_t n_blocks_per_row = (cols + QK_K - 1) / QK_K;

    for (size_t row = 0; row < rows; row++) {
        __m256i vsum = _mm256_setzero_si256();
        int64_t scalar_sum = 0;
        const struct block_q4_k* row_blocks = &blocks[row * n_blocks_per_row];

        for (size_t block_idx = 0; block_idx < n_blocks_per_row; block_idx++) {
            const struct block_q4_k* block = &row_blocks[block_idx];
            size_t input_offset = block_idx * QK_K;
            fixed_t d = ((int32_t)block->d) << (FIXED_SHIFT - 8);

            /* Process 16 quantized values (8 bytes) at a time */
            size_t i = 0;
            for (; i + 16 <= QK_K && (input_offset + i + 15) < cols; i += 16) {
                /* Load 8 packed bytes (16 nibbles) */
                __m128i packed = _mm_loadl_epi64((const __m128i*)&block->qs[i/2]);

                /* Unpack low and high nibbles */
                __m128i lo_mask = _mm_set1_epi8(0x0F);
                __m128i lo_nibbles = _mm_and_si128(packed, lo_mask);
                __m128i hi_nibbles = _mm_and_si128(_mm_srli_epi16(packed, 4), lo_mask);

                /* Interleave to get proper order */
                __m128i unpacked = _mm_unpacklo_epi8(lo_nibbles, hi_nibbles);

                /* Extend to 32-bit for multiplication */
                __m256i q32 = _mm256_cvtepi8_epi32(unpacked);

                /* Load input values */
                __m256i vinput = _mm256_loadu_si256((const __m256i*)&input[input_offset + i]);

                /* Multiply and accumulate */
                __m256i prod = _mm256_mullo_epi32(q32, vinput);
                vsum = _mm256_add_epi32(vsum, prod);

                /* Second 8 values */
                __m128i unpacked_hi = _mm_unpackhi_epi8(lo_nibbles, hi_nibbles);
                q32 = _mm256_cvtepi8_epi32(unpacked_hi);
                vinput = _mm256_loadu_si256((const __m256i*)&input[input_offset + i + 8]);
                prod = _mm256_mullo_epi32(q32, vinput);
                vsum = _mm256_add_epi32(vsum, prod);
            }

            /* Handle remainder with scalar code */
            for (; i < QK_K && (input_offset + i) < cols; i++) {
                uint8_t packed_byte = block->qs[i/2];
                int8_t q = (i % 2 == 0) ? (packed_byte & 0x0F) : (packed_byte >> 4);
                scalar_sum += (int64_t)q * (int64_t)input[input_offset + i];
            }

            /* Horizontal sum of SIMD accumulator */
            __m128i lo = _mm256_castsi256_si128(vsum);
            __m128i hi = _mm256_extracti128_si256(vsum, 1);
            __m128i sum128 = _mm_add_epi32(lo, hi);
            sum128 = _mm_hadd_epi32(sum128, sum128);
            sum128 = _mm_hadd_epi32(sum128, sum128);
            scalar_sum += _mm_cvtsi128_si32(sum128);
            vsum = _mm256_setzero_si256();

            /* Apply block scale */
            scalar_sum = (scalar_sum * d) >> FIXED_SHIFT;
        }

        output[row] = (fixed_t)(scalar_sum >> FIXED_SHIFT);
    }
}
#endif

/* SSE2-optimized Q4_K matrix-vector multiply */
static void q4_k_matvec_sse2(const void* weight_data, const fixed_t* input,
                              fixed_t* output, size_t rows, size_t cols) {
    const struct block_q4_k* blocks = (const struct block_q4_k*)weight_data;
    size_t n_blocks_per_row = (cols + QK_K - 1) / QK_K;

    for (size_t row = 0; row < rows; row++) {
        int64_t sum = 0;
        const struct block_q4_k* row_blocks = &blocks[row * n_blocks_per_row];

        for (size_t block_idx = 0; block_idx < n_blocks_per_row; block_idx++) {
            const struct block_q4_k* block = &row_blocks[block_idx];
            size_t input_offset = block_idx * QK_K;
            fixed_t d = ((int32_t)block->d) << (FIXED_SHIFT - 8);

            /* Process 8 values at a time with SSE2 */
            size_t i = 0;
            for (; i + 8 <= QK_K && (input_offset + i + 7) < cols; i += 8) {
                /* Manually unpack 4 bytes (8 nibbles) */
                int32_t q[8];
                q[0] = block->qs[i/2] & 0x0F;
                q[1] = (block->qs[i/2] >> 4) & 0x0F;
                q[2] = block->qs[i/2 + 1] & 0x0F;
                q[3] = (block->qs[i/2 + 1] >> 4) & 0x0F;
                q[4] = block->qs[i/2 + 2] & 0x0F;
                q[5] = (block->qs[i/2 + 2] >> 4) & 0x0F;
                q[6] = block->qs[i/2 + 3] & 0x0F;
                q[7] = (block->qs[i/2 + 3] >> 4) & 0x0F;

                /* Load into SSE registers */
                __m128i vq_lo = _mm_set_epi32(q[3], q[2], q[1], q[0]);
                __m128i vq_hi = _mm_set_epi32(q[7], q[6], q[5], q[4]);

                __m128i vin_lo = _mm_loadu_si128((const __m128i*)&input[input_offset + i]);
                __m128i vin_hi = _mm_loadu_si128((const __m128i*)&input[input_offset + i + 4]);

                /* Multiply using 64-bit results */
                __m128i prod1 = _mm_mul_epu32(vq_lo, vin_lo);
                __m128i prod2 = _mm_mul_epu32(_mm_srli_si128(vq_lo, 4),
                                             _mm_srli_si128(vin_lo, 4));
                __m128i prod3 = _mm_mul_epu32(vq_hi, vin_hi);
                __m128i prod4 = _mm_mul_epu32(_mm_srli_si128(vq_hi, 4),
                                             _mm_srli_si128(vin_hi, 4));

                /* Accumulate */
                sum += _mm_cvtsi128_si64(prod1);
                sum += _mm_cvtsi128_si64(_mm_srli_si128(prod1, 8));
                sum += _mm_cvtsi128_si64(prod2);
                sum += _mm_cvtsi128_si64(_mm_srli_si128(prod2, 8));
                sum += _mm_cvtsi128_si64(prod3);
                sum += _mm_cvtsi128_si64(_mm_srli_si128(prod3, 8));
                sum += _mm_cvtsi128_si64(prod4);
                sum += _mm_cvtsi128_si64(_mm_srli_si128(prod4, 8));
            }

            /* Scalar remainder */
            for (; i < QK_K && (input_offset + i) < cols; i++) {
                uint8_t packed_byte = block->qs[i/2];
                int8_t q_val = (i % 2 == 0) ? (packed_byte & 0x0F) : (packed_byte >> 4);
                sum += (int64_t)q_val * (int64_t)input[input_offset + i];
            }

            sum = (sum * d) >> FIXED_SHIFT;
        }

        output[row] = (fixed_t)(sum >> FIXED_SHIFT);
    }
}

/* Public API - dispatches to best available implementation */
void q4_k_matvec_neon(const void* weight_data, const fixed_t* input,
                      fixed_t* output, size_t rows, size_t cols) {
#ifdef __AVX2__
    if (check_x86_avx2()) {
        q4_k_matvec_avx2(weight_data, input, output, rows, cols);
        return;
    }
#endif
    q4_k_matvec_sse2(weight_data, input, output, rows, cols);
}

#else /* Scalar fallback for other platforms */

/* Q4_K block structure - generic version */
struct block_q4_k {
    uint16_t d;            /* Delta (scale) as uint16 */
    uint16_t dmin;         /* Min delta as uint16 */
    uint8_t scales[12];    /* Quantized scales */
    uint8_t qs[QK_K/2];    /* 4-bit quantized values */
} __attribute__((packed));

void q4_k_matvec_neon(const void* weight_data, const fixed_t* input,
                      fixed_t* output, size_t rows, size_t cols) {
    const struct block_q4_k* blocks = (const struct block_q4_k*)weight_data;
    size_t n_blocks_per_row = (cols + QK_K - 1) / QK_K;

    for (size_t row = 0; row < rows; row++) {
        int64_t sum = 0;
        const struct block_q4_k* row_blocks = &blocks[row * n_blocks_per_row];

        for (size_t block_idx = 0; block_idx < n_blocks_per_row; block_idx++) {
            const struct block_q4_k* block = &row_blocks[block_idx];
            size_t input_offset = block_idx * QK_K;
            fixed_t d = ((int32_t)block->d) << (FIXED_SHIFT - 8);

            for (size_t i = 0; i < QK_K && (input_offset + i) < cols; i++) {
                uint8_t packed = block->qs[i/2];
                int8_t q = (i % 2 == 0) ? (packed & 0x0F) : (packed >> 4);
                sum += (int64_t)q * (int64_t)input[input_offset + i];
            }

            sum = (sum * d) >> FIXED_SHIFT;
        }

        output[row] = (fixed_t)(sum >> FIXED_SHIFT);
    }
}

#endif /* __aarch64__ / __x86_64__ */
