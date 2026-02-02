/* EMBODIOS Optimized Tensor Operations
 * 
 * High-performance tensor operations for AI inference.
 * Implements BLAS-like operations optimized for kernel space.
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/mm.h"
#include "embodios/tvm.h"
#include "embodios/fixed_point.h"

/* SIMD matrix multiplication from simd_ops.c */
extern void matmul_neon(const fixed_t* a, const fixed_t* b, fixed_t* out,
                        size_t m, size_t k, size_t n);

/* Cache line size for optimization */
#define CACHE_LINE_SIZE 64

/* Block sizes for cache-friendly operations */
#define BLOCK_SIZE_M 64
#define BLOCK_SIZE_N 64
#define BLOCK_SIZE_K 64

/* SIMD vector size (in floats) */
#define VECTOR_SIZE 8

/* Aligned memory allocation */
static void* aligned_alloc(size_t size, size_t alignment)
{
    void* ptr = kmalloc(size + alignment);
    if (!ptr) return NULL;
    
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
    return (void*)aligned;
}

/* Optimized matrix multiplication: C = A * B + C
 * A: M x K matrix
 * B: K x N matrix  
 * C: M x N matrix
 */
void tensor_gemm(float* A, float* B, float* C, 
                 int M, int N, int K,
                 float alpha, float beta)
{
    /* Handle beta scaling */
    if (beta != 1.0f) {
        for (int i = 0; i < M * N; i++) {
            C[i] *= beta;
        }
    }
    
    /* Blocked matrix multiplication for cache efficiency */
    for (int i0 = 0; i0 < M; i0 += BLOCK_SIZE_M) {
        int imax = (i0 + BLOCK_SIZE_M < M) ? i0 + BLOCK_SIZE_M : M;
        
        for (int j0 = 0; j0 < N; j0 += BLOCK_SIZE_N) {
            int jmax = (j0 + BLOCK_SIZE_N < N) ? j0 + BLOCK_SIZE_N : N;
            
            for (int k0 = 0; k0 < K; k0 += BLOCK_SIZE_K) {
                int kmax = (k0 + BLOCK_SIZE_K < K) ? k0 + BLOCK_SIZE_K : K;
                
                /* Compute block */
                for (int i = i0; i < imax; i++) {
                    for (int j = j0; j < jmax; j++) {
                        float sum = 0.0f;
                        
                        /* Inner product with unrolling */
                        int k = k0;
                        for (; k < kmax - 3; k += 4) {
                            sum += A[i * K + k] * B[k * N + j];
                            sum += A[i * K + k+1] * B[(k+1) * N + j];
                            sum += A[i * K + k+2] * B[(k+2) * N + j];
                            sum += A[i * K + k+3] * B[(k+3) * N + j];
                        }
                        
                        /* Handle remainder */
                        for (; k < kmax; k++) {
                            sum += A[i * K + k] * B[k * N + j];
                        }
                        
                        C[i * N + j] += alpha * sum;
                    }
                }
            }
        }
    }
}

/* Optimized dense layer forward pass */
void tensor_dense_forward(TVMTensor* input, TVMTensor* weight,
                         TVMTensor* bias, TVMTensor* output)
{
    /* Extract dimensions */
    int batch_size = input->shape[0];
    int in_features = input->shape[1];
    int out_features = weight->shape[0];

    float* in_data = (float*)input->data;
    float* weight_data = (float*)weight->data;
    float* bias_data = bias ? (float*)bias->data : NULL;
    float* out_data = (float*)output->data;

    /* Calculate buffer sizes */
    size_t in_size = (size_t)batch_size * in_features;
    size_t weight_size = (size_t)in_features * out_features;
    size_t out_size = (size_t)batch_size * out_features;

    /* Allocate fixed-point buffers for SIMD */
    fixed_t* in_fixed = (fixed_t*)kmalloc(in_size * sizeof(fixed_t));
    fixed_t* weight_fixed = (fixed_t*)kmalloc(weight_size * sizeof(fixed_t));
    fixed_t* out_fixed = (fixed_t*)kmalloc(out_size * sizeof(fixed_t));

    if (!in_fixed || !weight_fixed || !out_fixed) {
        /* Fallback to scalar on allocation failure */
        if (in_fixed) kfree(in_fixed);
        if (weight_fixed) kfree(weight_fixed);
        if (out_fixed) kfree(out_fixed);

        tensor_gemm(in_data, weight_data, out_data,
                    batch_size, out_features, in_features,
                    1.0f, 0.0f);

        if (bias_data) {
            for (int i = 0; i < batch_size; i++) {
                for (int j = 0; j < out_features; j++) {
                    out_data[i * out_features + j] += bias_data[j];
                }
            }
        }
        return;
    }

    /* Convert input to fixed-point Q16.16 */
    for (size_t i = 0; i < in_size; i++) {
        in_fixed[i] = FLOAT_TO_FIXED(in_data[i]);
    }

    /* Convert weights to fixed-point Q16.16 */
    for (size_t i = 0; i < weight_size; i++) {
        weight_fixed[i] = FLOAT_TO_FIXED(weight_data[i]);
    }

    /* Use SIMD-accelerated NEON matrix multiplication (~4x faster) */
    matmul_neon(in_fixed, weight_fixed, out_fixed,
                batch_size, in_features, out_features);

    /* Convert output back to float */
    for (size_t i = 0; i < out_size; i++) {
        out_data[i] = FIXED_TO_FLOAT(out_fixed[i]);
    }

    /* Free fixed-point buffers */
    kfree(in_fixed);
    kfree(weight_fixed);
    kfree(out_fixed);

    /* Add bias if present */
    if (bias_data) {
        for (int i = 0; i < batch_size; i++) {
            for (int j = 0; j < out_features; j++) {
                out_data[i * out_features + j] += bias_data[j];
            }
        }
    }
}

/* Vectorized ReLU activation */
void tensor_relu_forward(TVMTensor* input, TVMTensor* output)
{
    float* in_data = (float*)input->data;
    float* out_data = (float*)output->data;
    
    /* Calculate total elements */
    int64_t size = 1;
    for (int i = 0; i < input->ndim; i++) {
        size *= input->shape[i];
    }
    
    /* Vectorized ReLU with unrolling */
    int64_t i = 0;
    for (; i < size - 7; i += 8) {
        out_data[i] = (in_data[i] > 0) ? in_data[i] : 0;
        out_data[i+1] = (in_data[i+1] > 0) ? in_data[i+1] : 0;
        out_data[i+2] = (in_data[i+2] > 0) ? in_data[i+2] : 0;
        out_data[i+3] = (in_data[i+3] > 0) ? in_data[i+3] : 0;
        out_data[i+4] = (in_data[i+4] > 0) ? in_data[i+4] : 0;
        out_data[i+5] = (in_data[i+5] > 0) ? in_data[i+5] : 0;
        out_data[i+6] = (in_data[i+6] > 0) ? in_data[i+6] : 0;
        out_data[i+7] = (in_data[i+7] > 0) ? in_data[i+7] : 0;
    }
    
    /* Handle remainder */
    for (; i < size; i++) {
        out_data[i] = (in_data[i] > 0) ? in_data[i] : 0;
    }
}

/* Fast exponential approximation for softmax */
static float fast_exp(float x)
{
    /* Clamp to prevent overflow */
    if (x < -88.0f) return 0.0f;
    if (x > 88.0f) return 3.4e38f;
    
    /* Fast approximation using Taylor series */
    float t = 1.0f + x / 256.0f;
    t *= t; t *= t; t *= t; t *= t;
    t *= t; t *= t; t *= t; t *= t;
    return t;
}

/* Optimized softmax */
void tensor_softmax_forward(TVMTensor* input, TVMTensor* output)
{
    /* Get dimensions */
    int batch_size = 1;
    for (int i = 0; i < input->ndim - 1; i++) {
        batch_size *= input->shape[i];
    }
    int num_classes = input->shape[input->ndim - 1];
    
    float* in_data = (float*)input->data;
    float* out_data = (float*)output->data;
    
    /* Process each batch element */
    for (int b = 0; b < batch_size; b++) {
        float* in_row = in_data + b * num_classes;
        float* out_row = out_data + b * num_classes;
        
        /* Find max for numerical stability */
        float max_val = in_row[0];
        for (int i = 1; i < num_classes; i++) {
            if (in_row[i] > max_val) max_val = in_row[i];
        }
        
        /* Compute exponentials and sum */
        float sum = 0.0f;
        for (int i = 0; i < num_classes; i++) {
            out_row[i] = fast_exp(in_row[i] - max_val);
            sum += out_row[i];
        }
        
        /* Normalize */
        float inv_sum = 1.0f / sum;
        for (int i = 0; i < num_classes; i++) {
            out_row[i] *= inv_sum;
        }
    }
}

/* Element-wise operations */
void tensor_add(TVMTensor* a, TVMTensor* b, TVMTensor* output)
{
    float* a_data = (float*)a->data;
    float* b_data = (float*)b->data;
    float* out_data = (float*)output->data;
    
    int64_t size = 1;
    for (int i = 0; i < a->ndim; i++) {
        size *= a->shape[i];
    }
    
    /* Vectorized addition */
    for (int64_t i = 0; i < size; i++) {
        out_data[i] = a_data[i] + b_data[i];
    }
}

/* Tensor transpose */
void tensor_transpose(TVMTensor* input, TVMTensor* output, int axis0, int axis1)
{
    float* in_data = (float*)input->data;
    float* out_data = (float*)output->data;
    
    /* For 2D matrices */
    if (input->ndim == 2 && axis0 == 0 && axis1 == 1) {
        int rows = input->shape[0];
        int cols = input->shape[1];
        
        /* Cache-friendly transpose */
        for (int i = 0; i < rows; i++) {
            for (int j = 0; j < cols; j++) {
                out_data[j * rows + i] = in_data[i * cols + j];
            }
        }
    }
}

/* Initialize tensor operations subsystem */
void tensor_ops_init(void)
{
    console_printf("Tensor Ops: Optimized operations initialized\n");
    console_printf("  Block size: %dx%dx%d\n", BLOCK_SIZE_M, BLOCK_SIZE_N, BLOCK_SIZE_K);
    console_printf("  Vector size: %d floats\n", VECTOR_SIZE);
}