/*
 * EMBODIOS SIMD quantized vec_dot kernels — shared types and dispatch API
 *
 * This header is intentionally freestanding and host-compilable: it is used
 * by the kernel (kernel/ai/simd_kernels_*.c) AND by the host-side correctness
 * / microbenchmark harness (tools/host_test_simd_kernels.c).
 *
 * Layout of all block structures matches the in-kernel GGUF fused-matmul
 * path (kernel/ai/streaming_inference.c).
 *
 * NOTE on block_q8_1: `d` and `s` are stored in `float` fields but hold the
 * *bit pattern of an fp16 value* (converted via emb_fp32_to_fp16 and back
 * with emb_fp16_to_fp32). This quirk is part of the kernel ABI and must be
 * preserved for bit-exact parity with the scalar fallback.
 */
#ifndef EMBODIOS_SIMD_KERNELS_H
#define EMBODIOS_SIMD_KERNELS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef QK8_0
#define QK8_0 32
#endif
#ifndef QK_K
#define QK_K 256
#endif

/* Q8_0 weight block: 32 x int8 + fp16 scale */
typedef struct __attribute__((packed)) {
    uint16_t d;
    int8_t   qs[QK8_0];
} block_q8_0;

/* Q8_1 activation block (kernel-private format).
 * d = fp16(amax/127) bits-as-float, s = fp16(sum of original floats)
 * bits-as-float. */
typedef struct __attribute__((packed)) {
    float  d;
    float  s;
    int8_t qs[QK8_0];
} block_q8_1;

/* Q4_K weight super-block: 256 x 4-bit, 8 groups of 32 */
typedef struct __attribute__((packed)) {
    uint16_t d;
    uint16_t dmin;
    uint8_t  scales[12];
    uint8_t  qs[QK_K / 2];
} block_q4_K;

/* Q5_0 weight block: 32 x 5-bit (4-bit qs + 1 high bit plane) */
typedef struct __attribute__((packed)) {
    uint16_t d;
    uint8_t  qh[4];
    uint8_t  qs[QK8_0 / 2];
} block_q5_0;

/* Q6_K weight super-block: 256 x 6-bit, 16 groups of 16 */
typedef struct __attribute__((packed)) {
    uint8_t  ql[QK_K / 2];
    uint8_t  qh[QK_K / 4];
    int8_t   scales[QK_K / 16];
    uint16_t d;
} block_q6_K;

/* ---------------------------------------------------------------------------
 * fp16 <-> fp32 helpers (pure bit manipulation, no F16C/SSE required)
 * ------------------------------------------------------------------------- */
static inline float emb_fp16_to_fp32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t f;

    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3ff;
            f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7f800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }

    union { uint32_t i; float f; } u;
    u.i = f;
    return u.f;
}

static inline uint16_t emb_fp32_to_fp16(float f) {
    union { float f; uint32_t i; } u;
    u.f = f;
    uint32_t x = u.i;

    uint32_t sign = (x >> 16) & 0x8000;
    int exp = ((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = (x >> 13) & 0x3ff;

    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;      /* too small: flush to zero */
        mant = (mant | 0x400) >> (1 - exp);
        return (uint16_t)(sign | mant);
    } else if (exp >= 31) {
        return (uint16_t)(sign | 0x7c00);          /* overflow: inf */
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | mant);
}

/* ---------------------------------------------------------------------------
 * Scalar reference kernels (simd_kernels_scalar.c).
 * True scalar C on every architecture — these are the dispatch fallback AND
 * the parity-test reference (core/cmd_simdtest.c).
 * ------------------------------------------------------------------------- */
float vec_dot_q8_0_q8_1_scalar(const block_q8_0* x, const block_q8_1* y, int nb);
float vec_dot_q4_k_q8_1_scalar(const block_q4_K* x, const block_q8_1* y, int nb);
float vec_dot_q5_0_q8_1_scalar(const block_q5_0* x, const block_q8_1* y, int nb);
float vec_dot_q6_k_q8_1_scalar(const block_q6_K* x, const block_q8_1* y, int nb);

/* ---------------------------------------------------------------------------
 * ARM NEON kernels (aarch64 only, simd_kernels_scalar.c).
 * Bit-exact vs the scalar references above (exact int accumulation + the
 * identical per-block float scaling expression in the same order).
 * Selected by simd_kernels_init() on __aarch64__.
 * ------------------------------------------------------------------------- */
#if defined(__aarch64__) || defined(SIMD_KERNELS_HOST_TEST)
float vec_dot_q8_0_q8_1_neon(const block_q8_0* x, const block_q8_1* y, int nb);
float vec_dot_q4_k_q8_1_neon(const block_q4_K* x, const block_q8_1* y, int nb);
#endif

/* x86_64 SSE2 baseline for Q8_0 (was the fastest shipping path on master) */
#if defined(__x86_64__) || defined(_M_X64)
float vec_dot_q8_0_q8_1_sse(const block_q8_0* x, const block_q8_1* y, int nb);
#endif

/* ---------------------------------------------------------------------------
 * AVX2 kernels (simd_kernels_avx2.c, compiled with -mavx2 for that TU only)
 * ------------------------------------------------------------------------- */
#if defined(__x86_64__) || defined(_M_X64) || defined(SIMD_KERNELS_HOST_TEST)
float vec_dot_q8_0_q8_1_avx2(const block_q8_0* x, const block_q8_1* y, int nb);
float vec_dot_q4_k_q8_1_avx2(const block_q4_K* x, const block_q8_1* y, int nb);
float vec_dot_q5_0_q8_1_avx2(const block_q5_0* x, const block_q8_1* y, int nb);
float vec_dot_q6_k_q8_1_avx2(const block_q6_K* x, const block_q8_1* y, int nb);
#endif

/* ---------------------------------------------------------------------------
 * Runtime dispatch (kernel only — simd_dispatch.c)
 * ------------------------------------------------------------------------- */
typedef float (*vec_dot_q8_0_fn)(const block_q8_0*, const block_q8_1*, int);
typedef float (*vec_dot_q4_k_fn)(const block_q4_K*, const block_q8_1*, int);
typedef float (*vec_dot_q5_0_fn)(const block_q5_0*, const block_q8_1*, int);
typedef float (*vec_dot_q6_k_fn)(const block_q6_K*, const block_q8_1*, int);

extern vec_dot_q8_0_fn g_vec_dot_q8_0_q8_1;
extern vec_dot_q4_k_fn g_vec_dot_q4_k_q8_1;
extern vec_dot_q5_0_fn g_vec_dot_q5_0_q8_1;
extern vec_dot_q6_k_fn g_vec_dot_q6_k_q8_1;

/* Probe CPU (CPUID + XCR0), select kernels, print chosen path to boot log.
 * Safe to call multiple times; idempotent. */
void simd_kernels_init(void);

/* Non-zero when AVX2 kernels were selected (OS XCR0 verified, not just CPUID) */
int simd_avx2_active(void);

/* Name of the active backend for logging: "AVX2", "SSE2", "NEON", "scalar" */
const char* simd_backend_name(void);

/* ---------------------------------------------------------------------------
 * Shell self-test (kernel/core/cmd_simdtest.c, v0.8.0 WS-2)
 *
 * "simdtest" runs a deterministic LCG-generated set of Q8_0/Q4_K vec_dot
 * workloads through the scalar reference and the dispatched SIMD backend,
 * FNV-1a-64-hashes both outputs (must be bit-identical) and prints
 * PASS/FAIL + cycle counts (cpu_get_timestamp: cntvct_el0 on aarch64).
 *
 * Registration in core/stubs.c is done by the integrator (one line):
 *   } else if (strncmp(command, "simdtest", 8) == 0 &&
 *              (command[8] == '\0' || command[8] == ' ')) {
 *       cmd_simdtest_dispatch(command + 8);  (* core/cmd_simdtest.c *)
 *   }
 * ------------------------------------------------------------------------- */
void cmd_simdtest_dispatch(const char* args);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_SIMD_KERNELS_H */
