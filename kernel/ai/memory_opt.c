/*
 * Memory Optimization for Cache Efficiency
 * Tiled matrix operations and cache-friendly memory access
 */

#include <embodios/types.h>
#include <embodios/simd.h>
#include <embodios/mm.h>

#define CACHE_LINE_SIZE 64
#define TILE_SIZE 32

/* Cache-aligned memory allocation */
void* alloc_aligned(size_t size) {
    size_t aligned_size = (size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    return kmalloc(aligned_size);
}

/* Tiled matrix-matrix multiplication for better cache usage */
void matmul_tiled(const fixed_t* A, const fixed_t* B, fixed_t* C,
                  size_t M, size_t N, size_t K) {
    /* C[M,N] = A[M,K] * B[K,N] */

    /* Process in TILE_SIZE x TILE_SIZE blocks */
    for (size_t i = 0; i < M; i += TILE_SIZE) {
        for (size_t j = 0; j < N; j += TILE_SIZE) {
            for (size_t k = 0; k < K; k += TILE_SIZE) {
                /* Compute tile */
                size_t i_end = (i + TILE_SIZE < M) ? i + TILE_SIZE : M;
                size_t j_end = (j + TILE_SIZE < N) ? j + TILE_SIZE : N;
                size_t k_end = (k + TILE_SIZE < K) ? k + TILE_SIZE : K;

                for (size_t ii = i; ii < i_end; ii++) {
                    for (size_t jj = j; jj < j_end; jj++) {
                        int64_t sum = 0;
                        for (size_t kk = k; kk < k_end; kk++) {
                            sum += (int64_t)A[ii * K + kk] * (int64_t)B[kk * N + jj];
                        }
                        if (k == 0) {
                            C[ii * N + jj] = (fixed_t)(sum >> 16);
                        } else {
                            C[ii * N + jj] += (fixed_t)(sum >> 16);
                        }
                    }
                }
            }
        }
    }
}

/* Transpose matrix for better cache locality */
void transpose(const fixed_t* src, fixed_t* dst, size_t rows, size_t cols) {
    for (size_t i = 0; i < rows; i += TILE_SIZE) {
        for (size_t j = 0; j < cols; j += TILE_SIZE) {
            size_t i_end = (i + TILE_SIZE < rows) ? i + TILE_SIZE : rows;
            size_t j_end = (j + TILE_SIZE < cols) ? j + TILE_SIZE : cols;

            for (size_t ii = i; ii < i_end; ii++) {
                for (size_t jj = j; jj < j_end; jj++) {
                    dst[jj * rows + ii] = src[ii * cols + jj];
                }
            }
        }
    }
}

/* Prefetch next cache line (hint to CPU) */
static inline void prefetch(const void* addr) {
#ifdef __aarch64__
    __asm__ __volatile__("prfm pldl1keep, [%0]" : : "r"(addr));
#endif
}

/* Matrix-vector multiply with prefetching */
void matvec_prefetch(const fixed_t* mat, const fixed_t* vec, fixed_t* out,
                     size_t rows, size_t cols) {
    for (size_t r = 0; r < rows; r++) {
        /* Prefetch next row */
        if (r + 1 < rows) {
            prefetch(&mat[(r + 1) * cols]);
        }

        /* Use SIMD for dot product */
        out[r] = vec_dot_neon(&mat[r * cols], vec, cols);
    }
}
