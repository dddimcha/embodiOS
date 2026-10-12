/* EMBODIOS SIMD parity self-test (simdtest) — v0.8.0 WS-2 (aarch64 bring-up)
 *
 * Runs a deterministic, LCG-generated set of Q8_0 x Q8_1 and Q4_K x Q8_1
 * fused vec_dot workloads (the matvec row-dots that dominate GGUF inference)
 * through:
 *   1. the scalar reference kernels  (vec_dot_*_scalar)
 *   2. the dispatched backend        (g_vec_dot_* — NEON on aarch64,
 *                                     SSE2/AVX2 on x86_64)
 *   3. the explicit NEON kernels     (vec_dot_*_neon, aarch64 only)
 * and FNV-1a-64-hashes the raw bytes of the float outputs. Scalar and SIMD
 * hashes MUST be bit-identical: both paths do exact integer accumulation
 * (order-independent) and apply the per-block float scaling with the
 * identical C expression in the same order.
 *
 * Timing is measured with cpu_get_timestamp() (cntvct_el0 on aarch64,
 * TSC on x86_64) and reported as ticks per pass.
 *
 * Registration in core/stubs.c is intentionally NOT done here (stubs.c is
 * owned by WS-1). The integrator adds ONE hunk to the command dispatcher:
 *
 *   } else if (strncmp(command, "simdtest", 8) == 0 &&
 *              (command[8] == '\0' || command[8] == ' ')) {
 *       cmd_simdtest_dispatch(command + 8);   (* core/cmd_simdtest.c *)
 *   }
 *
 * Author: Dmitry Dimcha <dddimcha@gmail.com>
 * License: MIT
 */

#include <embodios/console.h>
#include <embodios/cpu.h>           /* cpu_get_timestamp() */
#include <embodios/kernel.h>        /* strncmp */
#include <embodios/simd_kernels.h>
#include <embodios/types.h>

/* ---------------------------------------------------------------------------
 * Test geometry (static BSS — no heap, fully deterministic)
 * ------------------------------------------------------------------------- */
#define ST_Q8_NB      64    /* Q8_0 blocks per dot  = 2048 elements */
#define ST_Q8_ROWS     8    /* independent dot products ("logit rows") */
#define ST_Q4K_NB      8    /* Q4_K superblocks per dot = 2048 elements */
#define ST_Q4K_ROWS    8
#define ST_Q4K_Q8NB   (ST_Q4K_NB * (QK_K / QK8_0))  /* Q8_1 blocks per row */
#define ST_REPS       32    /* inner reps for the cycle measurement */

static block_q8_0 st_w_q8[ST_Q8_ROWS * ST_Q8_NB];
static block_q8_1 st_x_q8[ST_Q8_NB];

static block_q4_K st_w_q4k[ST_Q4K_ROWS * ST_Q4K_NB];
static block_q8_1 st_x_q4k[ST_Q4K_Q8NB];

static float st_out_scalar[ST_Q8_ROWS > ST_Q4K_ROWS ? ST_Q8_ROWS : ST_Q4K_ROWS];
static float st_out_simd[ST_Q8_ROWS > ST_Q4K_ROWS ? ST_Q8_ROWS : ST_Q4K_ROWS];

/* ---------------------------------------------------------------------------
 * Deterministic LCG (Knuth MMIX constants) — NOT rand(), same fill on every
 * boot, every arch, every optimization level.
 * ------------------------------------------------------------------------- */
static uint64_t g_st_lcg;

static void st_lcg_seed(uint64_t seed)
{
    g_st_lcg = seed;
}

static uint32_t st_lcg_next(void)
{
    g_st_lcg = g_st_lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(g_st_lcg >> 33);
}

/* ---------------------------------------------------------------------------
 * FNV-1a 64 over raw bytes of the result buffer
 * ------------------------------------------------------------------------- */
static uint64_t st_fnv1a64(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 14695981039346656037ULL;   /* offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;              /* FNV prime */
    }
    return h;
}

/* A few small sane fp16 scales (bit patterns); varying d per block exercises
 * the scale path while keeping every dot product finite. */
static const uint16_t st_fp16_scales[4] = {
    0x2E66,   /* ~0.0742  */
    0x3555,   /* ~0.3333  */
    0x15B5,   /* ~0.00853 */
    0x3C00,   /*  1.0     */
};

/* ---------------------------------------------------------------------------
 * Deterministic buffer fills
 * ------------------------------------------------------------------------- */
static void st_fill_q8_0_weights(block_q8_0* w, int nb)
{
    for (int i = 0; i < nb; i++) {
        w[i].d = st_fp16_scales[st_lcg_next() & 3];
        for (int j = 0; j < QK8_0; j++) {
            w[i].qs[j] = (int8_t)(st_lcg_next() & 0xFF);
        }
    }
}

static void st_fill_q8_1_input(block_q8_1* x, int nb)
{
    for (int i = 0; i < nb; i++) {
        /* d/s are fp16 bit patterns stored in float fields (kernel ABI,
         * see embodios/simd_kernels.h) */
        x[i].d = (float)st_fp16_scales[st_lcg_next() & 3];
        x[i].s = (float)(uint16_t)st_lcg_next();
        for (int j = 0; j < QK8_0; j++) {
            x[i].qs[j] = (int8_t)(st_lcg_next() & 0xFF);
        }
    }
}

static void st_fill_q4_k_weights(block_q4_K* w, int nb)
{
    uint8_t* bytes = (uint8_t*)w;
    size_t total = (size_t)nb * sizeof(block_q4_K);
    for (size_t i = 0; i < total; i++) {
        bytes[i] = (uint8_t)st_lcg_next();
    }
    /* Pin d/dmin to small sane fp16 values regardless of the random fill */
    for (int i = 0; i < nb; i++) {
        w[i].d = st_fp16_scales[st_lcg_next() & 3];
        w[i].dmin = 0x1000;   /* fp16 ~= 0.00195 */
    }
}

/* ---------------------------------------------------------------------------
 * Runners: one "matvec" = ROWS independent row-dots through fn
 * ------------------------------------------------------------------------- */
typedef float (*st_dot_q8_fn)(const block_q8_0*, const block_q8_1*, int);
typedef float (*st_dot_q4k_fn)(const block_q4_K*, const block_q8_1*, int);

static void st_run_q8(st_dot_q8_fn fn, float* out)
{
    for (int r = 0; r < ST_Q8_ROWS; r++) {
        out[r] = fn(&st_w_q8[r * ST_Q8_NB], st_x_q8, ST_Q8_NB);
    }
}

static void st_run_q4k(st_dot_q4k_fn fn, float* out)
{
    for (int r = 0; r < ST_Q4K_ROWS; r++) {
        out[r] = fn(&st_w_q4k[r * ST_Q4K_NB], st_x_q4k, ST_Q4K_NB);
    }
}

static uint64_t st_time_q8(st_dot_q8_fn fn, float* out)
{
    uint64_t t0 = cpu_get_timestamp();
    for (int rep = 0; rep < ST_REPS; rep++) {
        st_run_q8(fn, out);
    }
    return (cpu_get_timestamp() - t0) / ST_REPS;
}

static uint64_t st_time_q4k(st_dot_q4k_fn fn, float* out)
{
    uint64_t t0 = cpu_get_timestamp();
    for (int rep = 0; rep < ST_REPS; rep++) {
        st_run_q4k(fn, out);
    }
    return (cpu_get_timestamp() - t0) / ST_REPS;
}

/* ---------------------------------------------------------------------------
 * The test itself. Returns 0 on full parity, 1 on any mismatch.
 * ------------------------------------------------------------------------- */
static int cmd_simdtest(void)
{
    int fails = 0;

    /* Make sure the dispatch pointers reflect the boot-time selection */
    simd_kernels_init();

    console_printf("\nsimdtest: scalar vs %s fused vec_dot parity (bit-exact)\n",
                   simd_backend_name());
    console_printf("timer: cpu_get_timestamp ticks (%s)\n",
#if defined(__aarch64__)
                   "cntvct_el0"
#else
                   "TSC"
#endif
                   );

    /* ----- Q8_0 x Q8_1 ----- */
    st_lcg_seed(0x9E3779B97F4A7C15ULL);
    st_fill_q8_0_weights(st_w_q8, ST_Q8_ROWS * ST_Q8_NB);
    st_lcg_seed(0x243F6A8885A308D3ULL);
    st_fill_q8_1_input(st_x_q8, ST_Q8_NB);

    uint64_t t_s = st_time_q8(vec_dot_q8_0_q8_1_scalar, st_out_scalar);
    uint64_t h_s = st_fnv1a64(st_out_scalar, ST_Q8_ROWS * sizeof(float));

    uint64_t t_d = st_time_q8(g_vec_dot_q8_0_q8_1, st_out_simd);
    uint64_t h_d = st_fnv1a64(st_out_simd, ST_Q8_ROWS * sizeof(float));

    console_printf("Q8_0 x Q8_1: %d blocks/dot x %d rows\n", ST_Q8_NB, ST_Q8_ROWS);
    console_printf("  scalar   : hash=0x%016llx  ticks/pass=%llu\n",
                   (unsigned long long)h_s, (unsigned long long)t_s);
    console_printf("  %-8s : hash=0x%016llx  ticks/pass=%llu  %s\n",
                   simd_backend_name(),
                   (unsigned long long)h_d, (unsigned long long)t_d,
                   h_d == h_s ? "PASS" : "FAIL <-- hash mismatch");
    if (h_d != h_s) fails++;

#if defined(__aarch64__)
    uint64_t t_n = st_time_q8(vec_dot_q8_0_q8_1_neon, st_out_simd);
    uint64_t h_n = st_fnv1a64(st_out_simd, ST_Q8_ROWS * sizeof(float));
    console_printf("  NEON(exp): hash=0x%016llx  ticks/pass=%llu  %s\n",
                   (unsigned long long)h_n, (unsigned long long)t_n,
                   h_n == h_s ? "PASS" : "FAIL <-- hash mismatch");
    if (h_n != h_s) fails++;
#endif

    /* ----- Q4_K x Q8_1 ----- */
    st_lcg_seed(0xB7E151628AED2A6BULL);
    st_fill_q4_k_weights(st_w_q4k, ST_Q4K_ROWS * ST_Q4K_NB);
    st_lcg_seed(0xBF7158809CF4F3C7ULL);
    st_fill_q8_1_input(st_x_q4k, ST_Q4K_Q8NB);

    t_s = st_time_q4k(vec_dot_q4_k_q8_1_scalar, st_out_scalar);
    h_s = st_fnv1a64(st_out_scalar, ST_Q4K_ROWS * sizeof(float));

    t_d = st_time_q4k(g_vec_dot_q4_k_q8_1, st_out_simd);
    h_d = st_fnv1a64(st_out_simd, ST_Q4K_ROWS * sizeof(float));

    console_printf("Q4_K x Q8_1: %d superblocks/dot x %d rows\n",
                   ST_Q4K_NB, ST_Q4K_ROWS);
    console_printf("  scalar   : hash=0x%016llx  ticks/pass=%llu\n",
                   (unsigned long long)h_s, (unsigned long long)t_s);
    console_printf("  %-8s : hash=0x%016llx  ticks/pass=%llu  %s\n",
                   simd_backend_name(),
                   (unsigned long long)h_d, (unsigned long long)t_d,
                   h_d == h_s ? "PASS" : "FAIL <-- hash mismatch");
    if (h_d != h_s) fails++;

#if defined(__aarch64__)
    t_n = st_time_q4k(vec_dot_q4_k_q8_1_neon, st_out_simd);
    h_n = st_fnv1a64(st_out_simd, ST_Q4K_ROWS * sizeof(float));
    console_printf("  NEON(exp): hash=0x%016llx  ticks/pass=%llu  %s\n",
                   (unsigned long long)h_n, (unsigned long long)t_n,
                   h_n == h_s ? "PASS" : "FAIL <-- hash mismatch");
    if (h_n != h_s) fails++;
#endif

    console_printf("simdtest: %s\n\n", fails == 0
                   ? "PASS — scalar and SIMD outputs are bit-identical"
                   : "FAIL — SIMD backend diverges from scalar reference");
    return fails ? 1 : 0;
}

void cmd_simdtest_dispatch(const char* args)
{
    (void)args;   /* no sub-options yet; reserved for future filters */
    (void)cmd_simdtest();
}
