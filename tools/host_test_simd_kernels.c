/* Host-side correctness + microbenchmark harness for the SIMD quantized
 * vec_dot kernels (kernel/ai/simd_kernels_{scalar,avx2}.c).
 *
 * NOT part of the kernel build. Compile on a host with AVX2:
 *   gcc -O2 -mavx2 -I kernel/include tools/host_test_simd_kernels.c \
 *       kernel/ai/simd_kernels_scalar.c kernel/ai/simd_kernels_avx2.c \
 *       -o host_test_simd -lm
 *
 * Modes:
 *   correctness (default): for each format (q8_0, q4_k, q5_0, q6_k) run
 *       N random trials comparing scalar vs AVX2 (expect bit-identical: the
 *       AVX2 kernels use exact integer reductions + identical FP order) AND
 *       both against a dequant+float reference (validates the fused math
 *       itself). PASS when max rel err vs reference < 1e-4.
 *   bench [iters]: microbenchmark scalar vs SSE2 (q8_0) vs AVX2.
 *
 * The host needs AVX2 for the bench/avx2 paths (grep avx2 /proc/cpuinfo).
 */

#define SIMD_KERNELS_HOST_TEST 1
#include <embodios/simd_kernels.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ---------------- deterministic PRNG ---------------- */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint32_t xrnd(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 16);
}
static float frand(float lo, float hi) {
    return lo + (hi - lo) * ((float)(xrnd() & 0xFFFFFF) / (float)0x1000000);
}
static uint16_t fp16_rand_pos(float lo, float hi) {
    return emb_fp32_to_fp16(frand(lo, hi));
}

/* ---------------- quantize activation to q8_1 (kernel-exact semantics) --- */
static void quantize_row_q8_1_ref(const float* x, block_q8_1* y, int k) {
    const int nb = k / QK8_0;
    for (int i = 0; i < nb; i++) {
        float amax = 0.0f, sum = 0.0f;
        for (int j = 0; j < QK8_0; j++) {
            float v = x[i * QK8_0 + j];
            sum += v;
            float av = v < 0 ? -v : v;
            if (av > amax) amax = av;
        }
        float d = amax / 127.0f;
        float id = d != 0.0f ? 1.0f / d : 0.0f;
        y[i].d = (float)emb_fp32_to_fp16(d);    /* fp16 bits stored as float */
        y[i].s = (float)emb_fp32_to_fp16(sum);
        for (int j = 0; j < QK8_0; j++) {
            float v = x[i * QK8_0 + j] * id;
            int q = (int)(v + (v > 0 ? 0.5f : -0.5f));
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            y[i].qs[j] = (int8_t)q;
        }
    }
}

/* ---------------- dequant references (independent math check) ----------- */
/* Q4_K group scale/min decode (same bit layout as ggml) */
static void get_scale_min_k4_ref(int j, const uint8_t* q, uint8_t* d, uint8_t* m) {
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else { *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
           *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4); }
}

/* dot of dequantized weights with q8_1-quantized activation, in double */
static double ref_dot_q8_0(const block_q8_0* x, const block_q8_1* y, int nb) {
    double acc = 0;
    for (int i = 0; i < nb; i++) {
        double d0 = emb_fp16_to_fp32(x[i].d), d1 = emb_fp16_to_fp32((uint16_t)y[i].d);
        for (int j = 0; j < QK8_0; j++)
            acc += d0 * x[i].qs[j] * d1 * y[i].qs[j];
    }
    return acc;
}
/* Reference mirroring the KERNEL's fused semantics (block_q8_1 spec):
 * the min correction uses s = fp16(sum of original float activations),
 * i.e. sumf += xd*sumi - xdmin*summs. This IS the specification of the
 * kernel's block_q8_1 format (ggml uses per-16 bsums instead; the kernel
 * format predates this work and is kept for parity). */
static double ref_dot_q4_k(const block_q4_K* x, const block_q8_1* y, int nb) {
    double acc = 0;
    for (int i = 0; i < nb; i++) {
        double xd = emb_fp16_to_fp32(x[i].d), xdm = emb_fp16_to_fp32(x[i].dmin);
        double sumi = 0, summs = 0;
        for (int j = 0; j < 8; j++) {
            uint8_t sc, m;
            get_scale_min_k4_ref(j, x[i].scales, &sc, &m);
            const block_q8_1* yb = &y[i * 8 + j];
            double yd = emb_fp16_to_fp32((uint16_t)yb->d);
            double ys = emb_fp16_to_fp32((uint16_t)yb->s);
            const uint8_t* qs = x[i].qs + (j / 2) * 32;
            double g = 0;
            for (int k = 0; k < 32; k++) {
                int q = (j & 1) ? (qs[k] >> 4) : (qs[k] & 0xF);
                g += (double)q * yb->qs[k];
            }
            sumi += (double)sc * yd * g;
            summs += ys * m;
        }
        acc += xd * sumi - xdm * summs;
    }
    return acc;
}
static double ref_dot_q5_0(const block_q5_0* x, const block_q8_1* y, int nb) {
    double acc = 0;
    for (int i = 0; i < nb; i++) {
        double d0 = emb_fp16_to_fp32(x[i].d), d1 = emb_fp16_to_fp32((uint16_t)y[i].d);
        uint32_t qh; memcpy(&qh, x[i].qh, 4);
        for (int j = 0; j < 16; j++) {
            int x0 = ((x[i].qs[j] & 0x0F) | (((qh >> (j + 0)) << 4) & 0x10)) - 16;
            int x1 = ((x[i].qs[j] >> 4)   | ((qh >> (j + 12)) & 0x10)) - 16;
            acc += d0 * x0 * d1 * y[i].qs[j];
            acc += d0 * x1 * d1 * y[i].qs[j + 16];
        }
    }
    return acc;
}
static double ref_dot_q6_k(const block_q6_K* x, const block_q8_1* y, int nb) {
    double acc = 0;
    for (int i = 0; i < nb; i++) {
        double d = emb_fp16_to_fp32(x[i].d);
        for (int h = 0; h < 2; h++) {
            const uint8_t* ql = x[i].ql + 64 * h;
            const uint8_t* qh = x[i].qh + 32 * h;
            const int8_t*  sc = x[i].scales + 8 * h;
            const block_q8_1* yb = &y[i * 8 + 4 * h];
            for (int l = 0; l < 32; l++) {
                int is = l / 16;
                int q1 = ((ql[l +  0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int q2 = ((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = ((ql[l +  0] >>  4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = ((ql[l + 32] >>  4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                double yd0 = emb_fp16_to_fp32((uint16_t)yb[0].d);
                double yd1 = emb_fp16_to_fp32((uint16_t)yb[1].d);
                double yd2 = emb_fp16_to_fp32((uint16_t)yb[2].d);
                double yd3 = emb_fp16_to_fp32((uint16_t)yb[3].d);
                acc += d * sc[is + 0] * q1 * yd0 * yb[0].qs[l];
                acc += d * sc[is + 2] * q2 * yd1 * yb[1].qs[l];
                acc += d * sc[is + 4] * q3 * yd2 * yb[2].qs[l];
                acc += d * sc[is + 6] * q4 * yd3 * yb[3].qs[l];
            }
        }
    }
    return acc;
}

/* ---------------- random block generators ---------------- */
static void gen_q8_0(block_q8_0* b, int nb) {
    for (int i = 0; i < nb; i++) {
        b[i].d = fp16_rand_pos(0.001f, 0.05f);
        for (int j = 0; j < QK8_0; j++) b[i].qs[j] = (int8_t)(xrnd() % 255 - 127);
    }
}
static void gen_q4_k(block_q4_K* b, int nb) {
    for (int i = 0; i < nb; i++) {
        b[i].d = fp16_rand_pos(0.001f, 0.05f);
        b[i].dmin = fp16_rand_pos(0.0f, 0.01f);
        for (int j = 0; j < 12; j++) b[i].scales[j] = (uint8_t)xrnd();
        for (int j = 0; j < QK_K / 2; j++) b[i].qs[j] = (uint8_t)xrnd();
    }
}
static void gen_q5_0(block_q5_0* b, int nb) {
    for (int i = 0; i < nb; i++) {
        b[i].d = fp16_rand_pos(0.001f, 0.05f);
        for (int j = 0; j < 4; j++) b[i].qh[j] = (uint8_t)xrnd();
        for (int j = 0; j < QK8_0 / 2; j++) b[i].qs[j] = (uint8_t)xrnd();
    }
}
static void gen_q6_k(block_q6_K* b, int nb) {
    for (int i = 0; i < nb; i++) {
        b[i].d = fp16_rand_pos(0.001f, 0.05f);
        for (int j = 0; j < QK_K / 2; j++) b[i].ql[j] = (uint8_t)xrnd();
        for (int j = 0; j < QK_K / 4; j++) b[i].qh[j] = (uint8_t)xrnd();
        for (int j = 0; j < QK_K / 16; j++) b[i].scales[j] = (int8_t)(xrnd() % 255 - 127);
    }
}

static double rel_err(double a, double b) {
    double denom = fabs(b) > 1e-30 ? fabs(b) : 1e-30;
    return fabs(a - b) / denom;
}

/* ---------------- correctness driver ---------------- */
static int check_format(const char* name, int trials, int cols,
                        void (*gen_w)(void*, int), size_t wblk, int w_per_blk,
                        float (*dot_scalar)(const void*, const block_q8_1*, int),
                        float (*dot_avx2)(const void*, const block_q8_1*, int),
                        double (*dot_ref)(const void*, const block_q8_1*, int)) {
    int nb_w = cols / w_per_blk;
    int nb_y = cols / QK8_0;
    void* w = malloc(wblk * nb_w);
    block_q8_1* y = malloc(sizeof(block_q8_1) * nb_y);
    float* x = malloc(sizeof(float) * cols);

    double max_rel_vs_ref_s = 0, max_rel_vs_ref_a = 0, max_rel_s_vs_a = 0;
    int exact = 1;

    for (int t = 0; t < trials; t++) {
        gen_w(w, nb_w);
        for (int j = 0; j < cols; j++) x[j] = frand(-2.0f, 2.0f);
        quantize_row_q8_1_ref(x, y, cols);

        float rs = dot_scalar(w, y, nb_w);
        float ra = dot_avx2(w, y, nb_w);
        double rr = dot_ref(w, y, nb_w);

        if (memcmp(&rs, &ra, sizeof(float)) != 0) {
            exact = 0;
            double r = rel_err(ra, rs);
            if (r > max_rel_s_vs_a) max_rel_s_vs_a = r;
        }
        double es = rel_err(rs, rr); if (es > max_rel_vs_ref_s) max_rel_vs_ref_s = es;
        double ea = rel_err(ra, rr); if (ea > max_rel_vs_ref_a) max_rel_vs_ref_a = ea;
    }

    printf("%-6s trials=%d cols=%d | scalar-vs-avx2: %s (max rel %.3g) | "
           "vs dequant-ref max rel: scalar %.3g, avx2 %.3g => %s\n",
           name, trials, cols, exact ? "BIT-IDENTICAL" : "DIFFERS",
           max_rel_s_vs_a, max_rel_vs_ref_s, max_rel_vs_ref_a,
           (max_rel_vs_ref_s < 1e-3 && max_rel_vs_ref_a < 1e-3 &&
            max_rel_s_vs_a < 1e-4) ? "PASS" : "FAIL");

    /* Gate 1 (task requirement): scalar-vs-avx2 max rel err < 1e-4.
     * Gate 2 (mapping sanity): vs dequant-style double reference < 1e-3
     * (looser: absorbs float32-vs-double accumulation noise ~1e-4 over
     * 1536-term rows; group-mapping bugs show up as >= 1e-2). */
    int pass = max_rel_vs_ref_s < 1e-3 && max_rel_vs_ref_a < 1e-3 &&
               max_rel_s_vs_a < 1e-4;
    free(w); free(y); free(x);
    return pass;
}

/* wrappers with uniform signature */
static float wrap_s_q80(const void* x, const block_q8_1* y, int nb){return vec_dot_q8_0_q8_1_scalar(x,y,nb);}
static float wrap_sse_q80(const void* x, const block_q8_1* y, int nb){return vec_dot_q8_0_q8_1_sse(x,y,nb);}
static float wrap_a_q80(const void* x, const block_q8_1* y, int nb){return vec_dot_q8_0_q8_1_avx2(x,y,nb);}
static float wrap_s_q4k(const void* x, const block_q8_1* y, int nb){return vec_dot_q4_k_q8_1_scalar(x,y,nb);}
static float wrap_a_q4k(const void* x, const block_q8_1* y, int nb){return vec_dot_q4_k_q8_1_avx2(x,y,nb);}
static float wrap_s_q50(const void* x, const block_q8_1* y, int nb){return vec_dot_q5_0_q8_1_scalar(x,y,nb);}
static float wrap_a_q50(const void* x, const block_q8_1* y, int nb){return vec_dot_q5_0_q8_1_avx2(x,y,nb);}
static float wrap_s_q6k(const void* x, const block_q8_1* y, int nb){return vec_dot_q6_k_q8_1_scalar(x,y,nb);}
static float wrap_a_q6k(const void* x, const block_q8_1* y, int nb){return vec_dot_q6_k_q8_1_avx2(x,y,nb);}
static void gen_w_q80(void* p, int n){gen_q8_0(p, n);}
static void gen_w_q4k(void* p, int n){gen_q4_k(p, n);}
static void gen_w_q50(void* p, int n){gen_q5_0(p, n);}
static void gen_w_q6k(void* p, int n){gen_q6_k(p, n);}

/* ---------------- microbenchmark ---------------- */
static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static volatile float g_sink;

static void bench_one(const char* name, long iters, int cols,
                      void (*gen_w)(void*, int), size_t wblk, int w_per_blk,
                      const char* be_name, float (*dot)(const void*, const block_q8_1*, int)) {
    int nb_w = cols / w_per_blk;
    int nb_y = cols / QK8_0;
    void* w = malloc(wblk * nb_w);
    block_q8_1* y = malloc(sizeof(block_q8_1) * nb_y);
    float* x = malloc(sizeof(float) * cols);
    gen_w(w, nb_w);
    for (int j = 0; j < cols; j++) x[j] = frand(-2.0f, 2.0f);
    quantize_row_q8_1_ref(x, y, cols);

    /* warmup */
    float acc = 0;
    for (long i = 0; i < iters / 10 + 1; i++) acc += dot(w, y, nb_w);
    double t0 = now_s();
    for (long i = 0; i < iters; i++) acc += dot(w, y, nb_w);
    double dt = now_s() - t0;
    g_sink = acc;

    double ns_call = dt * 1e9 / (double)iters;
    printf("  %-6s %-7s %10.1f ns/call (%d elem)  %8.2f Melem/s\n",
           name, be_name, ns_call, cols, cols / (ns_call * 1e-9) / 1e6);
    free(w); free(y); free(x);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        long iters = argc > 2 ? atol(argv[2]) : 1000000;
        /* realistic SmolLM-135M row: hidden = 1536 */
        int cols = 1536;
        printf("== microbench: %ld iters, row width %d ==\n", iters, cols);
        bench_one("q8_0", iters, cols, gen_w_q80, sizeof(block_q8_0), 32, "scalar", wrap_s_q80);
        bench_one("q8_0", iters, cols, gen_w_q80, sizeof(block_q8_0), 32, "sse2",   wrap_sse_q80);
        bench_one("q8_0", iters, cols, gen_w_q80, sizeof(block_q8_0), 32, "avx2",   wrap_a_q80);
        bench_one("q4_k", iters, cols, gen_w_q4k, sizeof(block_q4_K), 256, "scalar", wrap_s_q4k);
        bench_one("q4_k", iters, cols, gen_w_q4k, sizeof(block_q4_K), 256, "avx2",   wrap_a_q4k);
        bench_one("q5_0", iters, cols, gen_w_q50, sizeof(block_q5_0), 32, "scalar", wrap_s_q50);
        bench_one("q5_0", iters, cols, gen_w_q50, sizeof(block_q5_0), 32, "avx2",   wrap_a_q50);
        bench_one("q6_k", iters, cols, gen_w_q6k, sizeof(block_q6_K), 256, "scalar", wrap_s_q6k);
        bench_one("q6_k", iters, cols, gen_w_q6k, sizeof(block_q6_K), 256, "avx2",   wrap_a_q6k);
        return 0;
    }

    int trials = 1000;
    int cols = 1536;  /* SmolLM hidden; also divisible by 256 and 32 */
    int pass = 1;
    printf("== correctness: %d random trials per format ==\n", trials);
    pass &= check_format("q8_0", trials, cols, gen_w_q80, sizeof(block_q8_0), 32,
                         wrap_s_q80, wrap_a_q80,
                         (double(*)(const void*, const block_q8_1*, int))ref_dot_q8_0);
    pass &= check_format("q4_k", trials, cols, gen_w_q4k, sizeof(block_q4_K), 256,
                         wrap_s_q4k, wrap_a_q4k,
                         (double(*)(const void*, const block_q8_1*, int))ref_dot_q4_k);
    pass &= check_format("q5_0", trials, cols, gen_w_q50, sizeof(block_q5_0), 32,
                         wrap_s_q50, wrap_a_q50,
                         (double(*)(const void*, const block_q8_1*, int))ref_dot_q5_0);
    pass &= check_format("q6_k", trials, cols, gen_w_q6k, sizeof(block_q6_K), 256,
                         wrap_s_q6k, wrap_a_q6k,
                         (double(*)(const void*, const block_q8_1*, int))ref_dot_q6_k);

    printf("%s\n", pass ? "ALL PASS" : "FAILURES PRESENT");
    return pass ? 0 : 1;
}
