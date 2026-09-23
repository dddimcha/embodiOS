/* EMBODIOS Parallel Matmul Benchmark (parbench) — v0.7.0 "Maxwell" WS-B
 *
 * Runs a fixed 1536x1536 Q4_K-style fused matvec (the same kernel family
 * that dominates SmolLM-135M inference) through the IPI worker pool at
 * 1..N CPUs and prints a cycles + speedup table. Every run must produce a
 * bit-identical output checksum: rows are partitioned across CPUs but each
 * row keeps its exact serial accumulation order.
 *
 * Registered in core/stubs.c with a single cmd_parbench_dispatch() line.
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#include <embodios/cmd_smp.h>       /* cmd_parbench_dispatch prototype */
#include <embodios/console.h>
#include <embodios/cpu.h>
#include <embodios/kernel.h>
#include <embodios/mm.h>
#include <embodios/parallel_inference.h>
#include <embodios/simd_kernels.h>
#include <embodios/types.h>

/* Fixed benchmark geometry: SmolLM-135M FFN-sized, Q4_K blocks */
#define PARBENCH_ROWS     1536
#define PARBENCH_COLS     1536
#define PARBENCH_NB_ROW   (PARBENCH_COLS / QK_K)    /* 6 Q4_K blocks/row */
#define PARBENCH_NB_Q8    (PARBENCH_COLS / QK8_0)   /* 48 Q8_1 input blocks */
#define PARBENCH_REPS     4

/* Deterministic xorshift64 PRNG (same fill on every boot/CPU count) */
static uint64_t g_pb_rng;

static uint64_t pb_rand(void)
{
    uint64_t x = g_pb_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_pb_rng = x;
    return x;
}

/* Fill the Q4_K weight matrix with deterministic data. d/dmin are pinned
 * to small sane fp16 values so the dot products stay finite regardless of
 * the random scales/qs bytes. */
static void pb_fill_weights(block_q4_K* w)
{
    g_pb_rng = 0x9E3779B97F4A7C15ULL;
    uint8_t* bytes = (uint8_t*)w;
    size_t total = (size_t)PARBENCH_ROWS * PARBENCH_NB_ROW * sizeof(block_q4_K);
    for (size_t i = 0; i < total; i++) {
        bytes[i] = (uint8_t)(pb_rand() >> 56);
    }
    for (int i = 0; i < PARBENCH_ROWS * PARBENCH_NB_ROW; i++) {
        w[i].d = 0x2E66;      /* fp16 ~= 0.074 */
        w[i].dmin = 0x1000;   /* fp16 ~= 0.00195 */
    }
}

/* Fill the shared Q8_1-quantized input (d/s are fp16 bits stored as float,
 * part of the kernel ABI — see embodios/simd_kernels.h) */
static void pb_fill_input(block_q8_1* x)
{
    g_pb_rng = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < PARBENCH_NB_Q8; i++) {
        x[i].d = (float)(uint16_t)0x2E66;   /* fp16 ~= 0.074, bits-as-float */
        x[i].s = 0.0f;
        for (int j = 0; j < QK8_0; j++) {
            x[i].qs[j] = (int8_t)((pb_rand() >> 56) & 0x1F) - 16;
        }
    }
}

static uint32_t pb_checksum(const float* out)
{
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < PARBENCH_ROWS; i++) {
        union { float f; uint32_t u; } v;
        v.f = out[i];
        h ^= v.u;
        h *= 16777619u;
    }
    return h;
}

static void cmd_parbench(void)
{
    uint32_t online = smp_get_num_online();
    if (online < 1) online = 1;
    if (online > PARALLEL_MAX_THREADS) online = PARALLEL_MAX_THREADS;

    block_q4_K* w = (block_q4_K*)kmalloc((size_t)PARBENCH_ROWS * PARBENCH_NB_ROW
                                         * sizeof(block_q4_K));
    block_q8_1* x = (block_q8_1*)kmalloc(PARBENCH_NB_Q8 * sizeof(block_q8_1));
    float* out = (float*)kmalloc(PARBENCH_ROWS * sizeof(float));
    if (!w || !x || !out) {
        console_printf("parbench: allocation failed\n");
        if (w) kfree(w);
        if (x) kfree(x);
        if (out) kfree(out);
        return;
    }

    pb_fill_weights(w);
    pb_fill_input(x);

    /* Preserve the pool configuration the inference engine may have set up */
    int was_init = parallel_pool_on_smp();
    int prev_threads = parallel_get_num_threads();

    console_printf("\nparbench: Q4_K fused matvec %dx%d (%d blocks/row), %d reps\n",
                   PARBENCH_ROWS, PARBENCH_COLS, PARBENCH_NB_ROW, PARBENCH_REPS);
    console_printf("pool: %s, %u CPU(s) online\n",
                   online > 1 ? "IPI worker pool (hlt+wakeup)" : "UP, serial only",
                   online);
    console_printf("%-6s %-12s %-13s %s\n", "CPUs", "cycles/rep",
                   "speedup", "checksum");
    console_printf("--------------------------------------------------\n");

    uint64_t base_cycles = 0;
    uint32_t ref_sum = 0;
    int identical = 1;

    for (uint32_t n = 1; n <= online; n++) {
        parallel_set_num_threads((int)n);

        /* Warm-up run (also produces the reference checksum at n == 1) */
        parallel_quant_matvec(out, w, x, PARBENCH_ROWS, PARBENCH_COLS,
                              PARBENCH_NB_ROW, sizeof(block_q4_K),
                              (quant_row_dot_fn)g_vec_dot_q4_k_q8_1);

        uint64_t t0 = cpu_get_timestamp();
        for (int rep = 0; rep < PARBENCH_REPS; rep++) {
            parallel_quant_matvec(out, w, x, PARBENCH_ROWS, PARBENCH_COLS,
                                  PARBENCH_NB_ROW, sizeof(block_q4_K),
                                  (quant_row_dot_fn)g_vec_dot_q4_k_q8_1);
        }
        uint64_t cycles = (cpu_get_timestamp() - t0) / PARBENCH_REPS;

        uint32_t sum = pb_checksum(out);
        if (n == 1) {
            base_cycles = cycles;
            ref_sum = sum;
        } else if (sum != ref_sum) {
            identical = 0;
        }

        if (base_cycles > 0 && cycles > 0) {
            console_printf("%-6u %-12llu %llu.%02llux       0x%08x%s\n",
                           n, (unsigned long long)cycles,
                           (unsigned long long)(base_cycles / cycles),
                           (unsigned long long)((base_cycles * 100 / cycles) % 100),
                           sum, sum == ref_sum ? "" : "  <-- DIFFERS");
        } else {
            console_printf("%-6u %-12llu n/a             0x%08x%s\n",
                           n, (unsigned long long)cycles,
                           sum, sum == ref_sum ? "" : "  <-- DIFFERS");
        }
    }

    console_printf("--------------------------------------------------\n");
    console_printf("bit-identical output at all thread counts: %s\n\n",
                   identical ? "yes" : "NO — BUG");

    /* Restore the pool to its previous state (or full width for chat) */
    parallel_set_num_threads(was_init ? prev_threads : (int)online);

    kfree(w);
    kfree(x);
    kfree(out);
}

int cmd_parbench_dispatch(const char* command)
{
    if (!command) {
        return 0;
    }
    if (strcmp(command, "parbench") == 0) {
        cmd_parbench();
        return 1;
    }
    return 0;
}
