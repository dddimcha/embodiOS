#ifndef EMBODIOS_SIMD_H
#define EMBODIOS_SIMD_H

#include <embodios/types.h>

/* ARM NEON optimized operations */
fixed_t vec_dot_neon(const fixed_t* a, const fixed_t* b, size_t n);
void matvec_neon(const fixed_t* mat, const fixed_t* vec, fixed_t* out, size_t rows, size_t cols);
void rms_norm_neon(fixed_t* out, const fixed_t* x, const fixed_t* weight, size_t size);
void softmax_neon(fixed_t* x, size_t size);
void elem_mul_neon(fixed_t* out, const fixed_t* a, const fixed_t* b, size_t n);
void elem_add_neon(fixed_t* out, const fixed_t* a, const fixed_t* b, size_t n);

#endif
