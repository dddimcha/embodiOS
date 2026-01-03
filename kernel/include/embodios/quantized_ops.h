/**
 * EMBODIOS Quantized Operations Header
 *
 * Pure integer dequantization for GGUF quantization types.
 * Supports Q4_K, Q5_K, Q6_K, and Q8_0.
 *
 * Uses Q16.16 fixed-point arithmetic (no floating-point).
 */

#ifndef EMBODIOS_QUANTIZED_OPS_H
#define EMBODIOS_QUANTIZED_OPS_H

#include <embodios/types.h>

/* ============================================================================
 * Fixed-Point Types
 * ============================================================================ */

typedef int32_t fixed_t;   /* Q16.16 fixed-point */
typedef int16_t fixed16_t; /* Q8.8 fixed-point */

#define FIXED_SHIFT 16
#define FIXED_ONE (1 << FIXED_SHIFT)
#define FIXED8_SHIFT 8
#define FIXED8_ONE (1 << FIXED8_SHIFT)

/* Conversion macros */
#define FIXED8_TO_FIXED16(x) (((int32_t)(x)) << (FIXED_SHIFT - FIXED8_SHIFT))
#define INT_TO_FIXED(x) ((fixed_t)(x) << FIXED_SHIFT)
#define FIXED_TO_INT(x) ((x) >> FIXED_SHIFT)

/* ============================================================================
 * Quantization Constants
 * ============================================================================ */

#define QK_K 256            /* Super-block size for K-quants */
#define K_SCALE_SIZE 12     /* Scale bytes in K-quant blocks */
#define QK8_0 32            /* Block size for Q8_0 */

/* ============================================================================
 * Quantization Types
 * ============================================================================ */

typedef enum {
    QUANT_TYPE_F32     = 0,
    QUANT_TYPE_F16     = 1,
    QUANT_TYPE_Q4_0    = 2,
    QUANT_TYPE_Q4_1    = 3,
    QUANT_TYPE_Q5_0    = 6,
    QUANT_TYPE_Q5_1    = 7,
    QUANT_TYPE_Q8_0    = 8,
    QUANT_TYPE_Q8_1    = 9,
    QUANT_TYPE_Q2_K    = 10,
    QUANT_TYPE_Q3_K    = 11,
    QUANT_TYPE_Q4_K    = 12,
    QUANT_TYPE_Q5_K    = 13,
    QUANT_TYPE_Q6_K    = 14,
    QUANT_TYPE_Q8_K    = 15,
    QUANT_TYPE_COUNT
} quant_type_t;

/* ============================================================================
 * Block Structures (for direct access if needed)
 * ============================================================================ */

struct block_q4_k {
    fixed16_t d;
    fixed16_t dmin;
    uint8_t scales[K_SCALE_SIZE];
    uint8_t qs[QK_K/2];
} __attribute__((packed));

struct block_q5_k {
    fixed16_t d;
    fixed16_t dmin;
    uint8_t scales[K_SCALE_SIZE];
    uint8_t qh[QK_K/8];
    uint8_t qs[QK_K/2];
} __attribute__((packed));

struct block_q6_k {
    uint8_t ql[QK_K/2];
    uint8_t qh[QK_K/4];
    int8_t scales[QK_K/16];
    fixed16_t d;
} __attribute__((packed));

struct block_q8_0 {
    fixed16_t d;
    int8_t qs[QK8_0];
} __attribute__((packed));

/* ============================================================================
 * Block-level Dequantization
 * ============================================================================ */

/**
 * Dequantize a single Q4_K block (256 values)
 */
void dequantize_block_q4_k(const struct block_q4_k* block, fixed_t* output);

/**
 * Dequantize a single Q5_K block (256 values)
 */
void dequantize_block_q5_k(const struct block_q5_k* block, fixed_t* output);

/**
 * Dequantize a single Q6_K block (256 values)
 */
void dequantize_block_q6_k(const struct block_q6_k* block, fixed_t* output);

/**
 * Dequantize a single Q8_0 block (32 values)
 */
void dequantize_block_q8_0(const struct block_q8_0* block, fixed_t* output);

/* ============================================================================
 * Tensor-level Dequantization
 * ============================================================================ */

/**
 * Dequantize Q4_K tensor
 * @return 0 on success, -1 on invalid size
 */
int dequantize_q4_k(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values);

/**
 * Dequantize Q5_K tensor
 */
int dequantize_q5_k(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values);

/**
 * Dequantize Q6_K tensor
 */
int dequantize_q6_k(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values);

/**
 * Dequantize Q8_0 tensor
 */
int dequantize_q8_0(const void* quantized_data, size_t quantized_size,
                    fixed_t* output, size_t n_values);

/**
 * Unified dequantization dispatcher
 * @param type Quantization type
 * @return 0 on success, -1 on invalid size, -2 on unsupported type
 */
int dequantize_tensor(quant_type_t type, const void* quantized_data,
                      size_t quantized_size, fixed_t* output, size_t n_values);

/* ============================================================================
 * Quantized Matrix-Vector Multiplication
 * Computes y = A * x where A is quantized
 * ============================================================================ */

int matmul_q4_k(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y, size_t m, size_t n);

int matmul_q5_k(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y, size_t m, size_t n);

int matmul_q6_k(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y, size_t m, size_t n);

int matmul_q8_0(const void* A_quantized, size_t A_quant_size,
                const fixed_t* x, fixed_t* y, size_t m, size_t n);

/**
 * Unified matmul dispatcher
 */
int matmul_quantized(quant_type_t type, const void* A_quantized, size_t A_quant_size,
                     const fixed_t* x, fixed_t* y, size_t m, size_t n);

/* ============================================================================
 * Quantization Info Functions
 * ============================================================================ */

/**
 * Get block size in bytes for a quantization type
 */
size_t get_block_size(quant_type_t type);

/**
 * Get number of elements per block for a quantization type
 */
size_t get_block_elements(quant_type_t type);

/**
 * Get human-readable name for quantization type
 */
const char* get_type_name(quant_type_t type);

/**
 * Check if a quantization type is supported
 */
static inline bool is_quant_type_supported(quant_type_t type) {
    return type == QUANT_TYPE_Q4_K || type == QUANT_TYPE_Q5_K ||
           type == QUANT_TYPE_Q6_K || type == QUANT_TYPE_Q8_0;
}

/**
 * Calculate bytes needed for a quantized tensor
 */
static inline size_t calc_quant_size(quant_type_t type, size_t n_values) {
    size_t block_elems = get_block_elements(type);
    if (block_elems == 0) return 0;
    size_t n_blocks = (n_values + block_elems - 1) / block_elems;
    return n_blocks * get_block_size(type);
}

/* ============================================================================
 * Testing and Benchmarks
 * ============================================================================ */

/**
 * Run quantization test suite
 * @return Number of failed tests (0 = all passed)
 */
int run_quantized_tests(void);

/**
 * Run quantization benchmarks only
 * @return 0 on success
 */
int run_quantized_benchmarks(void);

#endif /* EMBODIOS_QUANTIZED_OPS_H */
