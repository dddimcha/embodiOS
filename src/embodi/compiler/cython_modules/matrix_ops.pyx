# cython: language_level=3
# distutils: language = c
# distutils: extra_compile_args = -O3 -march=native -ffast-math

"""
Optimized matrix operations for EMBODIOS
"""

import numpy as np
cimport numpy as np
cimport cython
from libc.math cimport sqrt
from libc.string cimport memset

# For SIMD operations (if available)
cdef extern from *:
    """
    #ifdef __AVX2__
    #include <immintrin.h>
    #endif
    """
    pass

# Initialize numpy C API
np.import_array()

ctypedef np.float32_t float32_t
ctypedef np.int32_t int32_t

@cython.boundscheck(False)
@cython.wraparound(False)
@cython.cdivision(True)
cpdef np.ndarray[float32_t, ndim=2] gemm_optimized(
    np.ndarray[float32_t, ndim=2] A,
    np.ndarray[float32_t, ndim=2] B,
    float alpha=1.0,
    float beta=0.0,
    np.ndarray[float32_t, ndim=2] C=None):
    """
    Optimized General Matrix Multiply (GEMM)
    C = alpha * A @ B + beta * C
    """
    cdef int M = A.shape[0]
    cdef int K = A.shape[1]
    cdef int N = B.shape[1]
    cdef int i, j, k, ii, jj, kk
    cdef float sum_val
    cdef int block_size = 64  # Cache blocking size
    
    # Allocate C if not provided
    if C is None:
        C = np.zeros((M, N), dtype=np.float32)
    else:
        # Scale existing C by beta
        if beta != 1.0:
            for i in range(M):
                for j in range(N):
                    C[i, j] *= beta
    
    # Blocked matrix multiplication
    for ii in range(0, M, block_size):
        for jj in range(0, N, block_size):
            for kk in range(0, K, block_size):
                # Multiply block
                for i in range(ii, min(ii + block_size, M)):
                    for j in range(jj, min(jj + block_size, N)):
                        sum_val = 0.0
                        for k in range(kk, min(kk + block_size, K)):
                            sum_val += A[i, k] * B[k, j]
                        C[i, j] += alpha * sum_val
    
    return C

@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=1] matvec_optimized(
    np.ndarray[float32_t, ndim=2] A,
    np.ndarray[float32_t, ndim=1] x):
    """
    Optimized matrix-vector multiplication
    y = A @ x
    """
    cdef int m = A.shape[0]
    cdef int n = A.shape[1]
    cdef int i, j
    cdef float sum_val
    
    cdef np.ndarray[float32_t, ndim=1] y = np.zeros(m, dtype=np.float32)
    
    for i in range(m):
        sum_val = 0.0
        for j in range(n):
            sum_val += A[i, j] * x[j]
        y[i] = sum_val
    
    return y

@cython.boundscheck(False)
@cython.wraparound(False)
cpdef float dot_product(np.ndarray[float32_t, ndim=1] a, np.ndarray[float32_t, ndim=1] b):
    """
    Optimized dot product
    """
    cdef int n = a.shape[0]
    cdef int i
    cdef float result = 0.0
    
    # Unroll loop for better performance
    cdef int n_unroll = n - (n % 4)
    
    for i in range(0, n_unroll, 4):
        result += a[i] * b[i]
        result += a[i+1] * b[i+1]
        result += a[i+2] * b[i+2]
        result += a[i+3] * b[i+3]
    
    # Handle remainder
    for i in range(n_unroll, n):
        result += a[i] * b[i]
    
    return result

@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=2] outer_product(
    np.ndarray[float32_t, ndim=1] a,
    np.ndarray[float32_t, ndim=1] b):
    """
    Optimized outer product
    C = a @ b^T
    """
    cdef int m = a.shape[0]
    cdef int n = b.shape[0]
    cdef int i, j
    
    cdef np.ndarray[float32_t, ndim=2] C = np.zeros((m, n), dtype=np.float32)
    
    for i in range(m):
        for j in range(n):
            C[i, j] = a[i] * b[j]
    
    return C

@cython.boundscheck(False)
@cython.wraparound(False)
@cython.cdivision(True)
cpdef tuple qr_decomposition(np.ndarray[float32_t, ndim=2] A):
    """
    QR decomposition using Gram-Schmidt
    """
    cdef int m = A.shape[0]
    cdef int n = A.shape[1]
    cdef int i, j, k
    cdef float norm, dot_prod
    
    cdef np.ndarray[float32_t, ndim=2] Q = np.zeros((m, n), dtype=np.float32)
    cdef np.ndarray[float32_t, ndim=2] R = np.zeros((n, n), dtype=np.float32)
    
    # Copy A to Q
    for i in range(m):
        for j in range(n):
            Q[i, j] = A[i, j]
    
    # Gram-Schmidt process
    for j in range(n):
        # Compute norm
        norm = 0.0
        for i in range(m):
            norm += Q[i, j] * Q[i, j]
        norm = sqrt(norm)
        
        if norm > 1e-10:
            R[j, j] = norm
            
            # Normalize column j of Q
            for i in range(m):
                Q[i, j] /= norm
            
            # Orthogonalize remaining columns
            for k in range(j + 1, n):
                dot_prod = 0.0
                for i in range(m):
                    dot_prod += Q[i, j] * Q[i, k]
                
                R[j, k] = dot_prod
                
                for i in range(m):
                    Q[i, k] -= dot_prod * Q[i, j]
    
    return Q, R

@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=2] transpose(np.ndarray[float32_t, ndim=2] A):
    """
    Cache-friendly matrix transpose
    """
    cdef int m = A.shape[0]
    cdef int n = A.shape[1]
    cdef int i, j, ii, jj
    cdef int block_size = 32
    
    cdef np.ndarray[float32_t, ndim=2] AT = np.zeros((n, m), dtype=np.float32)
    
    # Blocked transpose for better cache usage
    for ii in range(0, m, block_size):
        for jj in range(0, n, block_size):
            for i in range(ii, min(ii + block_size, m)):
                for j in range(jj, min(jj + block_size, n)):
                    AT[j, i] = A[i, j]
    
    return AT

@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=2] elementwise_ops(
    np.ndarray[float32_t, ndim=2] A,
    np.ndarray[float32_t, ndim=2] B,
    str op='add'):
    """
    Optimized element-wise operations
    """
    cdef int m = A.shape[0]
    cdef int n = A.shape[1]
    cdef int i, j
    
    cdef np.ndarray[float32_t, ndim=2] C = np.zeros((m, n), dtype=np.float32)
    
    if op == 'add':
        for i in range(m):
            for j in range(n):
                C[i, j] = A[i, j] + B[i, j]
    elif op == 'sub':
        for i in range(m):
            for j in range(n):
                C[i, j] = A[i, j] - B[i, j]
    elif op == 'mul':
        for i in range(m):
            for j in range(n):
                C[i, j] = A[i, j] * B[i, j]
    elif op == 'div':
        for i in range(m):
            for j in range(n):
                if B[i, j] != 0:
                    C[i, j] = A[i, j] / B[i, j]
    
    return C

# Specialized operations for transformer models
@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=3] batch_matmul(
    np.ndarray[float32_t, ndim=3] A,
    np.ndarray[float32_t, ndim=3] B):
    """
    Batched matrix multiplication for transformers
    """
    cdef int batch_size = A.shape[0]
    cdef int m = A.shape[1]
    cdef int k = A.shape[2]
    cdef int n = B.shape[2]
    cdef int b, i, j, kk
    cdef float sum_val
    
    cdef np.ndarray[float32_t, ndim=3] C = np.zeros((batch_size, m, n), dtype=np.float32)
    
    for b in range(batch_size):
        for i in range(m):
            for j in range(n):
                sum_val = 0.0
                for kk in range(k):
                    sum_val += A[b, i, kk] * B[b, kk, j]
                C[b, i, j] = sum_val
    
    return C