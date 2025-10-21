/* EMBODIOS Quantized Operations - Pure Integer Math
 * Dequantize Q4_K and Q8_0 blocks to fixed-point values
 * NO FLOATING-POINT
 */

#include <embodios/types.h>

/* ============================================================================
 * Fixed-Point Type System
 * ============================================================================ */

typedef int32_t fixed_t;   /* Q16.16 fixed-point */
typedef int16_t fixed16_t; /* Q8.8 fixed-point */

#define FIXED_SHIFT 16
#define FIXED_ONE (1 << FIXED_SHIFT)
#define FIXED8_SHIFT 8
#define FIXED8_ONE (1 << FIXED8_SHIFT)

/* ============================================================================
 * Q4_K Structures
 * ============================================================================ */

#define QK_K 256
#define K_SCALE_SIZE 12

struct block_q4_k {
    uint8_t scales[K_SCALE_SIZE];
    uint8_t qs[QK_K/2];     /* 128 bytes: 2 values per byte */
    fixed16_t d;            /* Global scale (Q8.8) */
    fixed16_t dmin;         /* Min scale (Q8.8) */
} __attribute__((packed));

/* Q8_0 */
#define QK8_0 32

struct block_q8_0 {
    fixed16_t d;
    int8_t qs[QK8_0];
} __attribute__((packed));

/* ============================================================================
 * Dequantization Functions
 * ============================================================================ */

/* Dequantize a single Q4_K block (256 values) to fixed-point */
void dequantize_block_q4_k(const struct block_q4_k* block, fixed_t* output) {
    /* Extract scales from packed format */
    /* Q4_K uses 6-bit scales stored in 12 bytes for 16 groups of 16 values */

    int16_t scales[16];
    int16_t mins[16];

    /* Decode scales (6-bit unsigned, packed) */
    /* First 6 scales in first 9 bytes, next 10 in remaining */
    const uint8_t* sc = block->scales;

    /* Simplified scale extraction (approximation) */
    for (int i = 0; i < 8; i++) {
        scales[i] = (int16_t)((sc[i] & 0x3F));
        scales[i+8] = (int16_t)((sc[i] >> 4) & 0x0F);
    }

    /* Convert scales to fixed-point using global scale d */
    fixed_t d_fixed = ((int32_t)block->d) << (FIXED_SHIFT - FIXED8_SHIFT);
    fixed_t dmin_fixed = ((int32_t)block->dmin) << (FIXED_SHIFT - FIXED8_SHIFT);

    /* Dequantize each group of 16 values */
    for (int group = 0; group < 16; group++) {
        /* Scale for this group */
        int32_t scale = scales[group];
        fixed_t scale_fixed = (d_fixed * scale) >> 6;  /* Divide by 64 (6-bit scale) */

        /* Dequantize 16 values in this group */
        for (int j = 0; j < 16; j++) {
            int idx = group * 16 + j;
            int byte_idx = idx / 2;
            int nibble_idx = idx % 2;

            /* Extract 4-bit value */
            uint8_t byte_val = block->qs[byte_idx];
            uint8_t nibble = (nibble_idx == 0) ? (byte_val & 0x0F) : (byte_val >> 4);

            /* Dequantize: value = scale * (q - 8) */
            /* q is 4-bit unsigned (0-15), center at 8 */
            int32_t q = (int32_t)nibble - 8;
            output[idx] = (scale_fixed * q) >> 3;  /* Multiply by scale, divide by 8 */
        }
    }
}

/* Dequantize Q8_0 block (32 values) to fixed-point */
void dequantize_block_q8_0(const struct block_q8_0* block, fixed_t* output) {
    /* Convert Q8.8 scale to Q16.16 */
    fixed_t d_fixed = ((int32_t)block->d) << (FIXED_SHIFT - FIXED8_SHIFT);

    /* Dequantize each value: output = d * q */
    for (int i = 0; i < QK8_0; i++) {
        int32_t q = (int32_t)block->qs[i];
        output[i] = (d_fixed * q) >> 7;  /* Scale by d, normalize */
    }
}

/* Dequantize Q4_K tensor (multiple blocks) */
int dequantize_q4_k(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values) {
    const struct block_q4_k* blocks = (const struct block_q4_k*)quantized_data;
    size_t n_blocks = (n_values + QK_K - 1) / QK_K;

    /* Check size matches */
    if (quantized_size < n_blocks * sizeof(struct block_q4_k)) {
        return -1;
    }

    /* Dequantize each block */
    for (size_t i = 0; i < n_blocks; i++) {
        size_t values_in_block = (i == n_blocks - 1) ?
                                 (n_values - i * QK_K) : QK_K;

        fixed_t temp[QK_K];
        dequantize_block_q4_k(&blocks[i], temp);

        /* Copy to output */
        for (size_t j = 0; j < values_in_block; j++) {
            output[i * QK_K + j] = temp[j];
        }
    }

    return 0;
}

/* Dequantize Q8_0 tensor */
int dequantize_q8_0(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values) {
    const struct block_q8_0* blocks = (const struct block_q8_0*)quantized_data;
    size_t n_blocks = (n_values + QK8_0 - 1) / QK8_0;

    if (quantized_size < n_blocks * sizeof(struct block_q8_0)) {
        return -1;
    }

    for (size_t i = 0; i < n_blocks; i++) {
        size_t values_in_block = (i == n_blocks - 1) ?
                                 (n_values - i * QK8_0) : QK8_0;

        fixed_t temp[QK8_0];
        dequantize_block_q8_0(&blocks[i], temp);

        for (size_t j = 0; j < values_in_block; j++) {
            output[i * QK8_0 + j] = temp[j];
        }
    }

    return 0;
}

/* ============================================================================
 * Quantized Matrix-Vector Multiplication
 * ============================================================================ */

/* Matrix-vector multiply: y = A * x
 * A is quantized (Q4_K), x and y are fixed-point
 * A is [m x n], x is [n], y is [m]
 */
int matmul_q4_k(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y,
                size_t m, size_t n) {
    /* Each row of A is quantized */
    const struct block_q4_k* A_blocks = (const struct block_q4_k*)A_quantized;

    size_t blocks_per_row = (n + QK_K - 1) / QK_K;

    /* For each output row */
    for (size_t i = 0; i < m; i++) {
        int64_t sum = 0;

        /* Dequantize row i and compute dot product */
        for (size_t block_idx = 0; block_idx < blocks_per_row; block_idx++) {
            const struct block_q4_k* block = &A_blocks[i * blocks_per_row + block_idx];

            /* Dequantize block */
            fixed_t block_values[QK_K];
            dequantize_block_q4_k(block, block_values);

            /* Compute partial dot product */
            size_t values_in_block = (block_idx == blocks_per_row - 1) ?
                                     (n - block_idx * QK_K) : QK_K;

            for (size_t j = 0; j < values_in_block; j++) {
                size_t x_idx = block_idx * QK_K + j;
                /* Fixed-point multiply: (a * b) >> FIXED_SHIFT */
                int64_t product = ((int64_t)block_values[j] * (int64_t)x[x_idx]);
                sum += product;
            }
        }

        /* Store result (already scaled properly from products) */
        y[i] = (fixed_t)(sum >> FIXED_SHIFT);
    }

    return 0;
}
