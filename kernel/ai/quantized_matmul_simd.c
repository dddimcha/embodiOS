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
#else
/* Scalar fallback for non-ARM platforms */

/* Q4_K block structure - generic version using uint16_t instead of float16_t */
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
#endif /* __aarch64__ */
