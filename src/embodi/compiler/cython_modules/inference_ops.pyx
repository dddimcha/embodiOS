# cython: language_level=3
# distutils: language = c
# distutils: extra_compile_args = -O3 -march=native -ffast-math

"""
Optimized inference operations for EMBODIOS using Cython
"""

import numpy as np
cimport numpy as np
cimport cython
from libc.math cimport exp, log, sqrt, tanh
from libc.string cimport memcpy
from libc.stdlib cimport malloc, free

# Initialize numpy C API
np.import_array()

# Type definitions
ctypedef np.float32_t float32_t
ctypedef np.int32_t int32_t
ctypedef np.int64_t int64_t

@cython.boundscheck(False)
@cython.wraparound(False)
@cython.cdivision(True)
cpdef np.ndarray[float32_t, ndim=2] attention_forward(
    np.ndarray[float32_t, ndim=2] q,
    np.ndarray[float32_t, ndim=2] k,
    np.ndarray[float32_t, ndim=2] v,
    float scale):
    """
    Optimized self-attention forward pass
    
    Args:
        q: Query matrix [seq_len, d_model]
        k: Key matrix [seq_len, d_model]
        v: Value matrix [seq_len, d_model]
        scale: Scaling factor (typically 1/sqrt(d_model))
    
    Returns:
        Attention output [seq_len, d_model]
    """
    cdef int seq_len = q.shape[0]
    cdef int d_model = q.shape[1]
    cdef int i, j, k_idx
    cdef float max_score, sum_exp, score
    
    # Allocate output arrays
    cdef np.ndarray[float32_t, ndim=2] scores = np.zeros((seq_len, seq_len), dtype=np.float32)
    cdef np.ndarray[float32_t, ndim=2] attention_weights = np.zeros((seq_len, seq_len), dtype=np.float32)
    cdef np.ndarray[float32_t, ndim=2] output = np.zeros((seq_len, d_model), dtype=np.float32)
    
    # Compute attention scores: Q @ K^T * scale
    for i in range(seq_len):
        for j in range(seq_len):
            score = 0.0
            for k_idx in range(d_model):
                score += q[i, k_idx] * k[j, k_idx]
            scores[i, j] = score * scale
    
    # Compute softmax row-wise with numerical stability
    for i in range(seq_len):
        # Find max for numerical stability
        max_score = scores[i, 0]
        for j in range(1, seq_len):
            if scores[i, j] > max_score:
                max_score = scores[i, j]
        
        # Compute exp and sum
        sum_exp = 0.0
        for j in range(seq_len):
            attention_weights[i, j] = exp(scores[i, j] - max_score)
            sum_exp += attention_weights[i, j]
        
        # Normalize
        for j in range(seq_len):
            attention_weights[i, j] /= sum_exp
    
    # Apply attention to values: attention_weights @ V
    for i in range(seq_len):
        for k_idx in range(d_model):
            output[i, k_idx] = 0.0
            for j in range(seq_len):
                output[i, k_idx] += attention_weights[i, j] * v[j, k_idx]
    
    return output

@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=1] layer_norm_forward(
    np.ndarray[float32_t, ndim=1] x,
    np.ndarray[float32_t, ndim=1] gamma,
    np.ndarray[float32_t, ndim=1] beta,
    float eps=1e-5):
    """
    Optimized layer normalization
    """
    cdef int size = x.shape[0]
    cdef int i
    cdef float mean = 0.0
    cdef float variance = 0.0
    cdef float std_inv
    
    # Compute mean
    for i in range(size):
        mean += x[i]
    mean /= size
    
    # Compute variance
    for i in range(size):
        variance += (x[i] - mean) * (x[i] - mean)
    variance /= size
    
    # Compute standard deviation inverse
    std_inv = 1.0 / sqrt(variance + eps)
    
    # Normalize and scale
    cdef np.ndarray[float32_t, ndim=1] output = np.zeros(size, dtype=np.float32)
    for i in range(size):
        output[i] = gamma[i] * (x[i] - mean) * std_inv + beta[i]
    
    return output

@cython.boundscheck(False)
@cython.wraparound(False)
@cython.cdivision(True)
cpdef np.ndarray[float32_t, ndim=2] gelu_activation(np.ndarray[float32_t, ndim=2] x):
    """
    Optimized GELU activation function
    GELU(x) = 0.5 * x * (1 + tanh(sqrt(2/pi) * (x + 0.044715 * x^3)))
    """
    cdef int rows = x.shape[0]
    cdef int cols = x.shape[1]
    cdef int i, j
    cdef float val, x_val
    cdef float sqrt_2_pi = 0.7978845608  # sqrt(2/pi)
    cdef float coeff = 0.044715
    
    cdef np.ndarray[float32_t, ndim=2] output = np.zeros((rows, cols), dtype=np.float32)
    
    for i in range(rows):
        for j in range(cols):
            x_val = x[i, j]
            val = sqrt_2_pi * (x_val + coeff * x_val * x_val * x_val)
            output[i, j] = 0.5 * x_val * (1.0 + tanh(val))
    
    return output

@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[int32_t, ndim=1] argmax_sampling(
    np.ndarray[float32_t, ndim=1] logits,
    int top_k=0):
    """
    Optimized argmax or top-k sampling
    """
    cdef int vocab_size = logits.shape[0]
    cdef int i
    cdef float max_val = logits[0]
    cdef int max_idx = 0
    
    if top_k == 0 or top_k == 1:
        # Simple argmax
        for i in range(1, vocab_size):
            if logits[i] > max_val:
                max_val = logits[i]
                max_idx = i
        
        return np.array([max_idx], dtype=np.int32)
    
    # Top-k sampling would go here
    # For now, return argmax
    return np.array([max_idx], dtype=np.int32)

# Matrix multiplication optimized for different sizes
@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=2] matmul_optimized(
    np.ndarray[float32_t, ndim=2] a,
    np.ndarray[float32_t, ndim=2] b):
    """
    Optimized matrix multiplication with loop tiling
    """
    cdef int m = a.shape[0]
    cdef int k = a.shape[1]
    cdef int n = b.shape[1]
    cdef int i, j, kk, ii, jj
    cdef int tile_size = 64  # Cache-friendly tile size
    cdef float sum_val
    
    cdef np.ndarray[float32_t, ndim=2] c = np.zeros((m, n), dtype=np.float32)
    
    # Tiled matrix multiplication for better cache usage
    for ii in range(0, m, tile_size):
        for jj in range(0, n, tile_size):
            for kk in range(0, k, tile_size):
                # Process tile
                for i in range(ii, min(ii + tile_size, m)):
                    for j in range(jj, min(jj + tile_size, n)):
                        sum_val = c[i, j]
                        for k_idx in range(kk, min(kk + tile_size, k)):
                            sum_val += a[i, k_idx] * b[k_idx, j]
                        c[i, j] = sum_val
    
    return c

# Embedding lookup optimized
@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=2] embedding_lookup(
    np.ndarray[int32_t, ndim=1] input_ids,
    np.ndarray[float32_t, ndim=2] embedding_table):
    """
    Optimized embedding lookup
    """
    cdef int seq_len = input_ids.shape[0]
    cdef int hidden_size = embedding_table.shape[1]
    cdef int i, j
    cdef int token_id
    
    cdef np.ndarray[float32_t, ndim=2] output = np.zeros((seq_len, hidden_size), dtype=np.float32)
    
    for i in range(seq_len):
        token_id = input_ids[i]
        # Direct memory copy for speed
        memcpy(&output[i, 0], &embedding_table[token_id, 0], hidden_size * sizeof(float32_t))
    
    return output

# Positional encoding
@cython.boundscheck(False)
@cython.wraparound(False)
cpdef np.ndarray[float32_t, ndim=2] positional_encoding(int seq_len, int d_model):
    """
    Generate sinusoidal positional encodings
    """
    cdef int pos, i
    cdef float angle
    cdef np.ndarray[float32_t, ndim=2] pos_encoding = np.zeros((seq_len, d_model), dtype=np.float32)
    
    for pos in range(seq_len):
        for i in range(0, d_model, 2):
            angle = pos / (10000.0 ** (2.0 * i / d_model))
            pos_encoding[pos, i] = np.sin(angle)
            if i + 1 < d_model:
                pos_encoding[pos, i + 1] = np.cos(angle)
    
    return pos_encoding

# Hardware token processing
@cython.boundscheck(False)
cpdef list extract_hardware_tokens(np.ndarray[int32_t, ndim=1] tokens, dict hardware_token_map):
    """
    Extract hardware control tokens from token stream
    """
    cdef int i = 0
    cdef int token
    cdef list commands = []
    cdef dict cmd
    
    while i < tokens.shape[0]:
        token = tokens[i]
        
        # Check if it's a hardware token
        if token in hardware_token_map:
            token_type = hardware_token_map[token]
            
            if token_type == "GPIO_WRITE":
                if i + 2 < tokens.shape[0]:
                    cmd = {
                        'type': 'gpio_write',
                        'pin': tokens[i + 1],
                        'value': tokens[i + 2]
                    }
                    commands.append(cmd)
                    i += 3
                    continue
            elif token_type == "I2C_READ":
                if i + 2 < tokens.shape[0]:
                    cmd = {
                        'type': 'i2c_read',
                        'device': tokens[i + 1],
                        'register': tokens[i + 2]
                    }
                    commands.append(cmd)
                    i += 3
                    continue
        
        i += 1
    
    return commands

# Fast tokenization helpers
@cython.boundscheck(False)
cpdef np.ndarray[int32_t, ndim=1] fast_encode(str text, dict vocab):
    """
    Fast text to token encoding
    """
    cdef list tokens = text.lower().split()
    cdef int n_tokens = len(tokens)
    cdef np.ndarray[int32_t, ndim=1] encoded = np.zeros(n_tokens, dtype=np.int32)
    cdef int i
    cdef str token
    
    for i in range(n_tokens):
        token = tokens[i]
        if token in vocab:
            encoded[i] = vocab[token]
        else:
            encoded[i] = vocab.get('<unk>', 0)
    
    return encoded