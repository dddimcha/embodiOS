/* EMBODIOS Quantized Operations - Pure Integer Math
 * Dequantize Q4_K, Q5_K, Q6_K, and Q8_0 blocks to fixed-point values
 * NO FLOATING-POINT - Uses Q16.16 fixed-point arithmetic
 *
 * Supports runtime backend switching: GPU (Vulkan) or CPU fallback
 *
 * Reference: llama.cpp/ggml-quants.c
 */

#include <embodios/types.h>
#include <embodios/quantized_ops.h>
#include <embodios/gpu_backend.h>

/* ============================================================================
 * Block Size Information
 * ============================================================================ */

static const size_t block_sizes[] = {
    [QUANT_TYPE_F32]   = 4,
    [QUANT_TYPE_F16]   = 2,
    [QUANT_TYPE_Q4_0]  = 18,   /* 2 + 16 */
    [QUANT_TYPE_Q4_1]  = 20,   /* 4 + 16 */
    [QUANT_TYPE_Q5_0]  = 22,   /* 2 + 4 + 16 */
    [QUANT_TYPE_Q5_1]  = 24,   /* 4 + 4 + 16 */
    [QUANT_TYPE_Q8_0]  = 34,   /* 2 + 32 */
    [QUANT_TYPE_Q8_1]  = 36,   /* 4 + 32 */
    [QUANT_TYPE_Q2_K]  = 84,
    [QUANT_TYPE_Q3_K]  = 110,
    [QUANT_TYPE_Q4_K]  = 144,  /* 4 + 12 + 128 */
    [QUANT_TYPE_Q5_K]  = 176,  /* 4 + 12 + 32 + 128 */
    [QUANT_TYPE_Q6_K]  = 210,  /* 128 + 64 + 16 + 2 */
    [QUANT_TYPE_Q8_K]  = 292,
};

static const size_t block_elements[] = {
    [QUANT_TYPE_F32]   = 1,
    [QUANT_TYPE_F16]   = 1,
    [QUANT_TYPE_Q4_0]  = 32,
    [QUANT_TYPE_Q4_1]  = 32,
    [QUANT_TYPE_Q5_0]  = 32,
    [QUANT_TYPE_Q5_1]  = 32,
    [QUANT_TYPE_Q8_0]  = 32,
    [QUANT_TYPE_Q8_1]  = 32,
    [QUANT_TYPE_Q2_K]  = 256,
    [QUANT_TYPE_Q3_K]  = 256,
    [QUANT_TYPE_Q4_K]  = 256,
    [QUANT_TYPE_Q5_K]  = 256,
    [QUANT_TYPE_Q6_K]  = 256,
    [QUANT_TYPE_Q8_K]  = 256,
};

/* ============================================================================
 * Scale Extraction for K-quants
 *
 * K-quants store 6-bit scales and mins in a packed 12-byte format.
 * Layout: 8 scales + 4 mins in first 6 bytes, then 8 more scales + 4 mins
 * ============================================================================ */

/**
 * decode_k_scales - Extract 6-bit scales and mins from K-quant packed format
 * @scales_raw: Pointer to 12-byte packed scale data
 * @scales: Output array for 16 scale values
 * @mins: Output array for 16 min values
 */
static void decode_k_scales(const uint8_t* scales_raw,
                            int16_t* scales, int16_t* mins)
{
    /*
     * 6-bit scales are packed as follows in 12 bytes:
     * Bytes 0-3: lower 4 bits of scales 0-7
     * Bytes 4-5: lower 4 bits of scales 8-11, upper 2 bits of scales 0-3
     * Bytes 6-7: lower 4 bits of scales 12-15, upper 2 bits of scales 4-7
     * Bytes 8-9: lower 4 bits of mins 0-7
     * Bytes 10-11: lower 4 bits of mins 8-15, upper 2 bits of scales 8-15
     *
     * Simplified extraction (handles most common patterns):
     */

    /* Extract scales (groups 0-7) */
    for (int i = 0; i < 8; i++) {
        int byte_idx = i / 2;
        int nibble = (i % 2 == 0) ? (scales_raw[byte_idx] & 0x0F)
                                  : (scales_raw[byte_idx] >> 4);
        /* Combine with upper bits from bytes 4-5 */
        int upper_bits = 0;
        if (i < 4) {
            upper_bits = (scales_raw[4 + i/4] >> ((i % 4) * 2)) & 0x03;
        } else {
            upper_bits = (scales_raw[5 + (i-4)/4] >> (((i-4) % 4) * 2)) & 0x03;
        }
        scales[i] = nibble | (upper_bits << 4);
    }

    /* Extract scales (groups 8-15) */
    for (int i = 0; i < 8; i++) {
        int byte_idx = 6 + i / 2;
        int nibble = (i % 2 == 0) ? (scales_raw[byte_idx] & 0x0F)
                                  : (scales_raw[byte_idx] >> 4);
        int upper_bits = (scales_raw[10 + i/4] >> ((i % 4) * 2)) & 0x03;
        scales[8 + i] = nibble | (upper_bits << 4);
    }

    /* Extract mins (same packed format in remaining bytes) */
    for (int i = 0; i < 8; i++) {
        int byte_idx = i / 2;
        mins[i] = (i % 2 == 0) ? (scales_raw[byte_idx] & 0x0F)
                               : (scales_raw[byte_idx] >> 4);
    }
    for (int i = 0; i < 8; i++) {
        mins[8 + i] = (i % 2 == 0) ? (scales_raw[6 + i/2] & 0x0F)
                                   : (scales_raw[6 + i/2] >> 4);
    }
}

/* ============================================================================
 * Q4_K Dequantization (4-bit K-quant, 256 values per block)
 * ============================================================================ */

/**
 * dequantize_block_q4_k - Dequantize a single Q4_K block to fixed-point
 * @block: Pointer to quantized Q4_K block (256 values, 4-bit each)
 * @output: Output buffer for 256 fixed-point values
 *
 * Extracts 6-bit scales and mins, then dequantizes each 4-bit value using
 * the formula: value = scale * q - min
 */
void dequantize_block_q4_k(const struct block_q4_k* block, fixed_t* output)
{
    int16_t scales[16];
    int16_t mins[16];

    decode_k_scales(block->scales, scales, mins);

    /* Convert global scales to fixed-point */
    fixed_t d_fixed = FIXED8_TO_FIXED16(block->d);
    fixed_t dmin_fixed = FIXED8_TO_FIXED16(block->dmin);

    /* Process 16 groups of 16 values each */
    for (int group = 0; group < 16; group++) {
        /* Scale and min for this group */
        fixed_t sc = (d_fixed * scales[group]) >> 6;
        fixed_t mn = (dmin_fixed * mins[group]) >> 6;

        /* Dequantize 16 values */
        for (int j = 0; j < 16; j++) {
            int idx = group * 16 + j;
            int byte_idx = idx / 2;
            int nibble_shift = (idx % 2) * 4;

            /* Extract 4-bit value (0-15) */
            uint8_t q = (block->qs[byte_idx] >> nibble_shift) & 0x0F;

            /* Dequantize: value = sc * q - mn */
            output[idx] = ((sc * (int32_t)q) >> 4) - mn;
        }
    }
}

/* ============================================================================
 * Q5_K Dequantization (5-bit K-quant, 256 values per block)
 * ============================================================================ */

/**
 * dequantize_block_q5_k - Dequantize a single Q5_K block to fixed-point
 * @block: Pointer to quantized Q5_K block (256 values, 5-bit each)
 * @output: Output buffer for 256 fixed-point values
 *
 * Extracts 6-bit scales and mins, combines low 4-bits with high bit from qh,
 * then dequantizes each 5-bit value using: value = scale * q - min
 */
void dequantize_block_q5_k(const struct block_q5_k* block, fixed_t* output)
{
    int16_t scales[16];
    int16_t mins[16];

    decode_k_scales(block->scales, scales, mins);

    fixed_t d_fixed = FIXED8_TO_FIXED16(block->d);
    fixed_t dmin_fixed = FIXED8_TO_FIXED16(block->dmin);

    /* Process 16 groups of 16 values each */
    for (int group = 0; group < 16; group++) {
        fixed_t sc = (d_fixed * scales[group]) >> 6;
        fixed_t mn = (dmin_fixed * mins[group]) >> 6;

        for (int j = 0; j < 16; j++) {
            int idx = group * 16 + j;
            int byte_idx = idx / 2;
            int nibble_shift = (idx % 2) * 4;

            /* Extract low 4 bits */
            uint8_t q_low = (block->qs[byte_idx] >> nibble_shift) & 0x0F;

            /* Extract high bit from qh array */
            /* qh is packed: 8 high bits per byte */
            int qh_byte = idx / 8;
            int qh_bit = idx % 8;
            uint8_t q_high = (block->qh[qh_byte] >> qh_bit) & 0x01;

            /* Combine to 5-bit value (0-31) */
            uint8_t q = q_low | (q_high << 4);

            /* Dequantize: value = sc * q - mn */
            output[idx] = ((sc * (int32_t)q) >> 5) - mn;
        }
    }
}

/* ============================================================================
 * Q6_K Dequantization (6-bit K-quant, 256 values per block)
 * ============================================================================ */

/**
 * dequantize_block_q6_k - Dequantize a single Q6_K block to fixed-point
 * @block: Pointer to quantized Q6_K block (256 values, 6-bit each)
 * @output: Output buffer for 256 fixed-point values
 *
 * Uses 8-bit signed scales per group. Extracts low 4-bits from ql and high
 * 2-bits from qh, centers at 32 for signed representation, then dequantizes.
 */
void dequantize_block_q6_k(const struct block_q6_k* block, fixed_t* output)
{
    fixed_t d_fixed = FIXED8_TO_FIXED16(block->d);

    /* Q6_K has 16 groups of 16 values, each with its own 8-bit scale */
    for (int group = 0; group < 16; group++) {
        /* 8-bit signed scale for this group */
        int8_t scale = block->scales[group];
        fixed_t sc = (d_fixed * (int32_t)scale) >> 7;

        for (int j = 0; j < 16; j++) {
            int idx = group * 16 + j;

            /* Extract low 4 bits from ql */
            int ql_byte = idx / 2;
            int ql_shift = (idx % 2) * 4;
            uint8_t q_low = (block->ql[ql_byte] >> ql_shift) & 0x0F;

            /* Extract high 2 bits from qh */
            /* qh packs 4 values per byte (2 bits each) */
            int qh_byte = idx / 4;
            int qh_shift = (idx % 4) * 2;
            uint8_t q_high = (block->qh[qh_byte] >> qh_shift) & 0x03;

            /* Combine to 6-bit value, then center at 32 for signed */
            int8_t q = (int8_t)((q_low | (q_high << 4)) - 32);

            /* Dequantize */
            output[idx] = (sc * (int32_t)q) >> 5;
        }
    }
}

/* ============================================================================
 * Q8_0 Dequantization (8-bit, 32 values per block)
 * ============================================================================ */

/**
 * dequantize_block_q8_0 - Dequantize a single Q8_0 block to fixed-point
 * @block: Pointer to quantized Q8_0 block (32 values, 8-bit each)
 * @output: Output buffer for 32 fixed-point values
 *
 * Simplest quantization format: each value is 8-bit signed integer scaled
 * by a single block delta. Formula: value = delta * q
 */
void dequantize_block_q8_0(const struct block_q8_0* block, fixed_t* output)
{
    fixed_t d_fixed = FIXED8_TO_FIXED16(block->d);

    for (int i = 0; i < QK8_0; i++) {
        int32_t q = (int32_t)block->qs[i];
        output[i] = (d_fixed * q) >> 7;
    }
}

/* ============================================================================
 * Tensor Dequantization Functions
 * ============================================================================ */

/**
 * dequantize_q4_k - Dequantize a Q4_K tensor to fixed-point
 * @quantized_data: Pointer to quantized tensor data
 * @quantized_size: Size of quantized data in bytes
 * @output: Output buffer for fixed-point values
 * @n_values: Number of values to dequantize
 *
 * Returns: 0 on success, -1 if quantized_size is too small
 */
int dequantize_q4_k(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values)
{
    const struct block_q4_k* blocks = (const struct block_q4_k*)quantized_data;
    size_t n_blocks = (n_values + QK_K - 1) / QK_K;

    if (quantized_size < n_blocks * sizeof(struct block_q4_k)) {
        return -1;
    }

    for (size_t i = 0; i < n_blocks; i++) {
        size_t values_in_block = (i == n_blocks - 1) ?
                                 (n_values - i * QK_K) : QK_K;

        fixed_t temp[QK_K];
        dequantize_block_q4_k(&blocks[i], temp);

        for (size_t j = 0; j < values_in_block; j++) {
            output[i * QK_K + j] = temp[j];
        }
    }

    return 0;
}

/**
 * dequantize_q5_k - Dequantize a Q5_K tensor to fixed-point
 * @quantized_data: Pointer to quantized tensor data
 * @quantized_size: Size of quantized data in bytes
 * @output: Output buffer for fixed-point values
 * @n_values: Number of values to dequantize
 *
 * Returns: 0 on success, -1 if quantized_size is too small
 */
int dequantize_q5_k(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values)
{
    const struct block_q5_k* blocks = (const struct block_q5_k*)quantized_data;
    size_t n_blocks = (n_values + QK_K - 1) / QK_K;

    if (quantized_size < n_blocks * sizeof(struct block_q5_k)) {
        return -1;
    }

    for (size_t i = 0; i < n_blocks; i++) {
        size_t values_in_block = (i == n_blocks - 1) ?
                                 (n_values - i * QK_K) : QK_K;

        fixed_t temp[QK_K];
        dequantize_block_q5_k(&blocks[i], temp);

        for (size_t j = 0; j < values_in_block; j++) {
            output[i * QK_K + j] = temp[j];
        }
    }

    return 0;
}

/**
 * dequantize_q6_k - Dequantize a Q6_K tensor to fixed-point
 * @quantized_data: Pointer to quantized tensor data
 * @quantized_size: Size of quantized data in bytes
 * @output: Output buffer for fixed-point values
 * @n_values: Number of values to dequantize
 *
 * Returns: 0 on success, -1 if quantized_size is too small
 */
int dequantize_q6_k(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values)
{
    const struct block_q6_k* blocks = (const struct block_q6_k*)quantized_data;
    size_t n_blocks = (n_values + QK_K - 1) / QK_K;

    if (quantized_size < n_blocks * sizeof(struct block_q6_k)) {
        return -1;
    }

    for (size_t i = 0; i < n_blocks; i++) {
        size_t values_in_block = (i == n_blocks - 1) ?
                                 (n_values - i * QK_K) : QK_K;

        fixed_t temp[QK_K];
        dequantize_block_q6_k(&blocks[i], temp);

        for (size_t j = 0; j < values_in_block; j++) {
            output[i * QK_K + j] = temp[j];
        }
    }

    return 0;
}

/**
 * dequantize_q8_0 - Dequantize a Q8_0 tensor to fixed-point
 * @quantized_data: Pointer to quantized tensor data
 * @quantized_size: Size of quantized data in bytes
 * @output: Output buffer for fixed-point values
 * @n_values: Number of values to dequantize
 *
 * Returns: 0 on success, -1 if quantized_size is too small
 */
int dequantize_q8_0(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values)
{
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
 * Unified Dequantization Dispatcher
 * ============================================================================ */

/**
 * dequantize_tensor - Unified dequantization dispatcher
 * @type: Quantization type (Q4_K, Q5_K, Q6_K, or Q8_0)
 * @quantized_data: Pointer to quantized tensor data
 * @quantized_size: Size of quantized data in bytes
 * @output: Output buffer for fixed-point values
 * @n_values: Number of values to dequantize
 *
 * Returns: 0 on success, -1 on invalid size, -2 on unsupported type
 */
int dequantize_tensor(quant_type_t type, const void* quantized_data,
                      size_t quantized_size, fixed_t* output, size_t n_values)
{
    switch (type) {
        case QUANT_TYPE_Q4_K:
            return dequantize_q4_k(quantized_data, quantized_size, output, n_values);
        case QUANT_TYPE_Q5_K:
            return dequantize_q5_k(quantized_data, quantized_size, output, n_values);
        case QUANT_TYPE_Q6_K:
            return dequantize_q6_k(quantized_data, quantized_size, output, n_values);
        case QUANT_TYPE_Q8_0:
            return dequantize_q8_0(quantized_data, quantized_size, output, n_values);
        default:
            return -2;  /* Unsupported type */
    }
}

/* ============================================================================
 * Quantization Info Functions
 * ============================================================================ */

/**
 * get_block_size - Get block size in bytes for a quantization type
 * @type: Quantization type
 *
 * Returns: Block size in bytes, or 0 for invalid type
 */
size_t get_block_size(quant_type_t type)
{
    if (type >= QUANT_TYPE_COUNT) return 0;
    return block_sizes[type];
}

/**
 * get_block_elements - Get number of elements per block for a quantization type
 * @type: Quantization type
 *
 * Returns: Number of elements per block, or 0 for invalid type
 */
size_t get_block_elements(quant_type_t type)
{
    if (type >= QUANT_TYPE_COUNT) return 0;
    return block_elements[type];
}

/**
 * get_type_name - Get human-readable name for quantization type
 * @type: Quantization type
 *
 * Returns: String name (e.g., "Q4_K"), or "UNKNOWN" for invalid type
 */
const char* get_type_name(quant_type_t type)
{
    static const char* names[] = {
        [QUANT_TYPE_F32]   = "F32",
        [QUANT_TYPE_F16]   = "F16",
        [QUANT_TYPE_Q4_0]  = "Q4_0",
        [QUANT_TYPE_Q4_1]  = "Q4_1",
        [QUANT_TYPE_Q5_0]  = "Q5_0",
        [QUANT_TYPE_Q5_1]  = "Q5_1",
        [QUANT_TYPE_Q8_0]  = "Q8_0",
        [QUANT_TYPE_Q8_1]  = "Q8_1",
        [QUANT_TYPE_Q2_K]  = "Q2_K",
        [QUANT_TYPE_Q3_K]  = "Q3_K",
        [QUANT_TYPE_Q4_K]  = "Q4_K",
        [QUANT_TYPE_Q5_K]  = "Q5_K",
        [QUANT_TYPE_Q6_K]  = "Q6_K",
        [QUANT_TYPE_Q8_K]  = "Q8_K",
    };
    if (type >= QUANT_TYPE_COUNT) return "UNKNOWN";
    return names[type] ? names[type] : "UNKNOWN";
}

/* ============================================================================
 * Quantized Matrix-Vector Multiplication
 *
 * RUNTIME BACKEND SWITCHING:
 * Each matmul function checks if GPU backend is available and dispatches to
 * the appropriate implementation:
 * - GPU backend: Uses Vulkan compute shaders for acceleration
 * - CPU backend: Uses integer-only fixed-point arithmetic
 * ============================================================================ */

/* Forward declarations for GPU backend matmul operations */
#ifdef GGML_USE_VULKAN
extern int ggml_backend_vk_matmul_q4_k(const void* A_quantized, size_t A_quant_size,
                                        const fixed_t* x, fixed_t* y, size_t m, size_t n);
extern int ggml_backend_vk_matmul_q5_k(const void* A_quantized, size_t A_quant_size,
                                        const fixed_t* x, fixed_t* y, size_t m, size_t n);
extern int ggml_backend_vk_matmul_q6_k(const void* A_quantized, size_t A_quant_size,
                                        const fixed_t* x, fixed_t* y, size_t m, size_t n);
extern int ggml_backend_vk_matmul_q8_0(const void* A_quantized, size_t A_quant_size,
                                        const fixed_t* x, fixed_t* y, size_t m, size_t n);
#endif

/**
 * matmul_q4_k - Matrix-vector multiply with Q4_K quantized matrix
 * @A_quantized: Pointer to quantized matrix data (m x n)
 * @A_quant_size: Size of quantized data in bytes (unused, for validation)
 * @x: Input vector of length n
 * @y: Output vector of length m
 * @m: Number of rows in matrix
 * @n: Number of columns in matrix
 *
 * Computes y = A * x where A is Q4_K quantized.
 * Runtime backend switching: GPU (Vulkan) if available, else CPU fallback.
 * Returns: 0 on success
 */
int matmul_q4_k(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y,
                size_t m, size_t n)
{
    /* Runtime backend selection: Try GPU first, fallback to CPU */
#ifdef GGML_USE_VULKAN
    if (gpu_backend_is_available()) {
        /* GPU backend available - use Vulkan compute shaders */
        return ggml_backend_vk_matmul_q4_k(A_quantized, A_quant_size, x, y, m, n);
    }
#endif

    /* CPU backend - integer-only fixed-point implementation */
    const struct block_q4_k* A_blocks = (const struct block_q4_k*)A_quantized;
    size_t blocks_per_row = (n + QK_K - 1) / QK_K;

    (void)A_quant_size;  /* Size validation done externally */

    for (size_t i = 0; i < m; i++) {
        int64_t sum = 0;

        for (size_t block_idx = 0; block_idx < blocks_per_row; block_idx++) {
            const struct block_q4_k* block = &A_blocks[i * blocks_per_row + block_idx];

            fixed_t block_values[QK_K];
            dequantize_block_q4_k(block, block_values);

            size_t values_in_block = (block_idx == blocks_per_row - 1) ?
                                     (n - block_idx * QK_K) : QK_K;

            for (size_t j = 0; j < values_in_block; j++) {
                size_t x_idx = block_idx * QK_K + j;
                int64_t product = ((int64_t)block_values[j] * (int64_t)x[x_idx]);
                sum += product;
            }
        }

        y[i] = (fixed_t)(sum >> FIXED_SHIFT);
    }

    return 0;
}

/**
 * matmul_q5_k - Matrix-vector multiply with Q5_K quantized matrix
 * @A_quantized: Pointer to quantized matrix data (m x n)
 * @A_quant_size: Size of quantized data in bytes (unused, for validation)
 * @x: Input vector of length n
 * @y: Output vector of length m
 * @m: Number of rows in matrix
 * @n: Number of columns in matrix
 *
 * Computes y = A * x where A is Q5_K quantized.
 * Runtime backend switching: GPU (Vulkan) if available, else CPU fallback.
 * Returns: 0 on success
 */
int matmul_q5_k(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y,
                size_t m, size_t n)
{
    /* Runtime backend selection: Try GPU first, fallback to CPU */
#ifdef GGML_USE_VULKAN
    if (gpu_backend_is_available()) {
        /* GPU backend available - use Vulkan compute shaders */
        return ggml_backend_vk_matmul_q5_k(A_quantized, A_quant_size, x, y, m, n);
    }
#endif

    /* CPU backend - integer-only fixed-point implementation */
    const struct block_q5_k* A_blocks = (const struct block_q5_k*)A_quantized;
    size_t blocks_per_row = (n + QK_K - 1) / QK_K;

    (void)A_quant_size;

    for (size_t i = 0; i < m; i++) {
        int64_t sum = 0;

        for (size_t block_idx = 0; block_idx < blocks_per_row; block_idx++) {
            const struct block_q5_k* block = &A_blocks[i * blocks_per_row + block_idx];

            fixed_t block_values[QK_K];
            dequantize_block_q5_k(block, block_values);

            size_t values_in_block = (block_idx == blocks_per_row - 1) ?
                                     (n - block_idx * QK_K) : QK_K;

            for (size_t j = 0; j < values_in_block; j++) {
                size_t x_idx = block_idx * QK_K + j;
                int64_t product = ((int64_t)block_values[j] * (int64_t)x[x_idx]);
                sum += product;
            }
        }

        y[i] = (fixed_t)(sum >> FIXED_SHIFT);
    }

    return 0;
}

/**
 * matmul_q6_k - Matrix-vector multiply with Q6_K quantized matrix
 * @A_quantized: Pointer to quantized matrix data (m x n)
 * @A_quant_size: Size of quantized data in bytes (unused, for validation)
 * @x: Input vector of length n
 * @y: Output vector of length m
 * @m: Number of rows in matrix
 * @n: Number of columns in matrix
 *
 * Computes y = A * x where A is Q6_K quantized.
 * Runtime backend switching: GPU (Vulkan) if available, else CPU fallback.
 * Returns: 0 on success
 */
int matmul_q6_k(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y,
                size_t m, size_t n)
{
    /* Runtime backend selection: Try GPU first, fallback to CPU */
#ifdef GGML_USE_VULKAN
    if (gpu_backend_is_available()) {
        /* GPU backend available - use Vulkan compute shaders */
        return ggml_backend_vk_matmul_q6_k(A_quantized, A_quant_size, x, y, m, n);
    }
#endif

    /* CPU backend - integer-only fixed-point implementation */
    const struct block_q6_k* A_blocks = (const struct block_q6_k*)A_quantized;
    size_t blocks_per_row = (n + QK_K - 1) / QK_K;

    (void)A_quant_size;

    for (size_t i = 0; i < m; i++) {
        int64_t sum = 0;

        for (size_t block_idx = 0; block_idx < blocks_per_row; block_idx++) {
            const struct block_q6_k* block = &A_blocks[i * blocks_per_row + block_idx];

            fixed_t block_values[QK_K];
            dequantize_block_q6_k(block, block_values);

            size_t values_in_block = (block_idx == blocks_per_row - 1) ?
                                     (n - block_idx * QK_K) : QK_K;

            for (size_t j = 0; j < values_in_block; j++) {
                size_t x_idx = block_idx * QK_K + j;
                int64_t product = ((int64_t)block_values[j] * (int64_t)x[x_idx]);
                sum += product;
            }
        }

        y[i] = (fixed_t)(sum >> FIXED_SHIFT);
    }

    return 0;
}

/**
 * matmul_q8_0 - Matrix-vector multiply with Q8_0 quantized matrix
 * @A_quantized: Pointer to quantized matrix data (m x n)
 * @A_quant_size: Size of quantized data in bytes (unused, for validation)
 * @x: Input vector of length n
 * @y: Output vector of length m
 * @m: Number of rows in matrix
 * @n: Number of columns in matrix
 *
 * Computes y = A * x where A is Q8_0 quantized.
 * Runtime backend switching: GPU (Vulkan) if available, else CPU fallback.
 * Returns: 0 on success
 */
int matmul_q8_0(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y,
                size_t m, size_t n)
{
    /* Runtime backend selection: Try GPU first, fallback to CPU */
#ifdef GGML_USE_VULKAN
    if (gpu_backend_is_available()) {
        /* GPU backend available - use Vulkan compute shaders */
        return ggml_backend_vk_matmul_q8_0(A_quantized, A_quant_size, x, y, m, n);
    }
#endif

    /* CPU backend - integer-only fixed-point implementation */
    const struct block_q8_0* A_blocks = (const struct block_q8_0*)A_quantized;
    size_t blocks_per_row = (n + QK8_0 - 1) / QK8_0;

    (void)A_quant_size;

    for (size_t i = 0; i < m; i++) {
        int64_t sum = 0;

        for (size_t block_idx = 0; block_idx < blocks_per_row; block_idx++) {
            const struct block_q8_0* block = &A_blocks[i * blocks_per_row + block_idx];

            fixed_t block_values[QK8_0];
            dequantize_block_q8_0(block, block_values);

            size_t values_in_block = (block_idx == blocks_per_row - 1) ?
                                     (n - block_idx * QK8_0) : QK8_0;

            for (size_t j = 0; j < values_in_block; j++) {
                size_t x_idx = block_idx * QK8_0 + j;
                int64_t product = ((int64_t)block_values[j] * (int64_t)x[x_idx]);
                sum += product;
            }
        }

        y[i] = (fixed_t)(sum >> FIXED_SHIFT);
    }

    return 0;
}

/**
 * matmul_quantized - Unified matrix-vector multiply dispatcher
 * @type: Quantization type (Q4_K, Q5_K, Q6_K, or Q8_0)
 * @A_quantized: Pointer to quantized matrix data (m x n)
 * @A_quant_size: Size of quantized data in bytes
 * @x: Input vector of length n
 * @y: Output vector of length m
 * @m: Number of rows in matrix
 * @n: Number of columns in matrix
 *
 * Computes y = A * x where A is quantized in the specified format.
 * Runtime backend switching: Each type-specific function automatically selects
 * GPU backend (Vulkan) if available, otherwise falls back to CPU implementation.
 * Returns: 0 on success, -2 on unsupported type
 */
int matmul_quantized(quant_type_t type, const void* A_quantized, size_t A_quant_size,
                     const fixed_t* x, fixed_t* y, size_t m, size_t n)
{
    /* Dispatch to type-specific matmul (each handles GPU/CPU backend selection) */
    switch (type) {
        case QUANT_TYPE_Q4_K:
            return matmul_q4_k(A_quantized, A_quant_size, x, y, m, n);
        case QUANT_TYPE_Q5_K:
            return matmul_q5_k(A_quantized, A_quant_size, x, y, m, n);
        case QUANT_TYPE_Q6_K:
            return matmul_q6_k(A_quantized, A_quant_size, x, y, m, n);
        case QUANT_TYPE_Q8_0:
            return matmul_q8_0(A_quantized, A_quant_size, x, y, m, n);
        default:
            return -2;
    }
}
