/* EMBODIOS Performance Benchmark Module
 *
 * Restored implementation matching include/embodios/benchmark.h.
 * Uses TSC (rdtsc) for timing and the streaming inference engine for
 * real GGUF inference benchmarks.
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/benchmark.h>
#include <embodios/streaming_inference.h>
#include <embodios/bpe_tokenizer.h>
#include <embodios/gguf_parser.h>

/* libc/freestanding helpers */
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);
extern int strcmp(const char *a, const char *b);

/* Architecture TSC calibration (x86_64: arch/x86_64/tsc.c) */
#if defined(__x86_64__)
extern uint64_t tsc_get_frequency(void);
#endif

/* Fallback TSC frequency: 1 GHz (only used if calibration unavailable) */
#define TSC_FREQ_FALLBACK 1000000000ULL

static uint64_t g_tsc_freq = 0;

int benchmark_init(void)
{
    if (g_tsc_freq != 0) {
        return 0;
    }

#if defined(__x86_64__)
    g_tsc_freq = tsc_get_frequency();
#endif
    if (g_tsc_freq == 0) {
        g_tsc_freq = TSC_FREQ_FALLBACK;
        console_printf("[BENCH] TSC calibration unavailable, assuming %llu Hz\n",
                       (unsigned long long)g_tsc_freq);
    } else {
        console_printf("[BENCH] TSC frequency: %llu MHz\n",
                       (unsigned long long)(g_tsc_freq / 1000000ULL));
    }
    return 0;
}

uint64_t benchmark_get_tsc_freq(void)
{
    if (g_tsc_freq == 0) {
        benchmark_init();
    }
    return g_tsc_freq;
}

uint64_t benchmark_cycles_to_us(uint64_t cycles)
{
    uint64_t freq = benchmark_get_tsc_freq();
    if (freq == 0) {
        return 0;
    }
    /* cycles * 1e6 / freq, avoiding overflow via MHz division first */
    return (cycles / freq) * 1000000ULL +
           ((cycles % freq) * 1000000ULL) / freq;
}

uint64_t benchmark_cycles_to_ms(uint64_t cycles)
{
    return benchmark_cycles_to_us(cycles) / 1000ULL;
}

/* ============================================================================
 * Quick System Performance Check
 * ============================================================================ */

int benchmark_quick_check(void)
{
    benchmark_init();

    console_printf("\n=== Quick Performance Check ===\n");
    console_printf("TSC frequency: %llu MHz\n",
                   (unsigned long long)(benchmark_get_tsc_freq() / 1000000ULL));

    /* CPU: simple integer loop */
    const int cpu_iters = 1000000;
    volatile uint64_t acc = 0;
    uint64_t start = rdtsc();
    for (int i = 0; i < cpu_iters; i++) {
        acc += (uint64_t)i * 2654435761U;
        acc ^= acc >> 13;
    }
    uint64_t cpu_cycles = rdtsc() - start;
    uint64_t cpu_us = benchmark_cycles_to_us(cpu_cycles);
    console_printf("CPU int loop: %d iters in %llu us (%llu cycles)\n",
                   cpu_iters, (unsigned long long)cpu_us,
                   (unsigned long long)cpu_cycles);

    /* Memory: write + read bandwidth over a 4 MiB buffer */
    const size_t buf_size = 4 * 1024 * 1024;
    uint8_t *buf = (uint8_t *)kmalloc(buf_size);
    if (buf) {
        start = rdtsc();
        memset(buf, 0x5A, buf_size);
        uint64_t write_cycles = rdtsc() - start;

        acc = 0;
        start = rdtsc();
        for (size_t i = 0; i < buf_size; i += 64) {
            acc += buf[i];
        }
        uint64_t read_cycles = rdtsc() - start;
        kfree(buf);

        uint64_t write_us = benchmark_cycles_to_us(write_cycles);
        uint64_t read_us = benchmark_cycles_to_us(read_cycles);
        uint64_t write_bw = write_us ? (buf_size / 1024) * 1000000ULL / (write_us * 1024) : 0;
        uint64_t read_bw  = read_us  ? (buf_size / 1024) * 1000000ULL / (read_us * 1024)  : 0;
        console_printf("Memory write: %llu MB/s, read: %llu MB/s\n",
                       (unsigned long long)write_bw, (unsigned long long)read_bw);
    } else {
        console_printf("Memory test skipped (allocation failed)\n");
    }

    console_printf("Quick check complete\n");
    return 0;
}

/* ============================================================================
 * Real GGUF Inference Benchmark
 * ============================================================================ */

int benchmark_gguf_inference(inference_benchmark_t *result,
                             const char *prompt,
                             int max_tokens)
{
    if (!result) {
        return -1;
    }
    memset(result, 0, sizeof(*result));

    benchmark_init();

    /* Model must be loaded already (embedded or via loadmodel) */
    const struct gguf_model_arch *arch = gguf_parser_get_arch();
    if (!arch) {
        console_printf("[BENCH] ERROR: no GGUF model loaded\n");
        return -1;
    }

    /* Ensure inference engine and tokenizer are up */
    if (!streaming_inference_is_ready()) {
        if (streaming_inference_init(false) != 0) {
            console_printf("[BENCH] ERROR: streaming_inference_init failed\n");
            return -1;
        }
    }
    if (!bpe_tokenizer_is_initialized()) {
        if (bpe_tokenizer_init() != 0) {
            console_printf("[BENCH] ERROR: bpe_tokenizer_init failed\n");
            return -1;
        }
    }

    if (!prompt) {
        prompt = "Once upon a time";
    }
    if (max_tokens <= 0) {
        max_tokens = 20;
    }
    if (max_tokens > 256) {
        max_tokens = 256;
    }

    /* Tokenize the prompt */
    int prompt_tokens[256];
    int prompt_len = bpe_tokenizer_encode(prompt, prompt_tokens, 256, true, false);
    if (prompt_len <= 0) {
        console_printf("[BENCH] ERROR: failed to tokenize prompt\n");
        return -1;
    }

    int *output_tokens = (int *)kmalloc((size_t)max_tokens * sizeof(int));
    if (!output_tokens) {
        console_printf("[BENCH] ERROR: out of memory\n");
        return -1;
    }

    console_printf("[BENCH] Generating %d tokens (prompt: %d tokens)...\n",
                   max_tokens, prompt_len);

    uint64_t start = rdtsc();
    int generated = streaming_inference_generate(prompt_tokens, prompt_len,
                                                 output_tokens, max_tokens);
    uint64_t total_cycles = rdtsc() - start;

    kfree(output_tokens);

    if (generated <= 0) {
        console_printf("[BENCH] ERROR: generation failed (%d)\n", generated);
        return -1;
    }

    uint64_t total_us = benchmark_cycles_to_us(total_cycles);

    result->total_tokens = (uint64_t)generated;
    result->total_cycles = total_cycles;
    result->total_time_us = total_us;
    if (total_us > 0) {
        result->tokens_per_sec = (double)generated * 1000000.0 / (double)total_us;
        result->avg_latency_ms = (double)total_us / 1000.0 / (double)generated;
    }
    result->target_met = result->tokens_per_sec >= PERF_TARGET_TOKENS_PER_SEC;

    console_printf("[BENCH] === GGUF Inference Benchmark ===\n");
    console_printf("[BENCH] Tokens generated : %llu\n",
                   (unsigned long long)result->total_tokens);
    console_printf("[BENCH] Total time       : %llu us (%llu ms)\n",
                   (unsigned long long)total_us,
                   (unsigned long long)(total_us / 1000ULL));
    console_printf("[BENCH] Tokens/sec       : %u.%02u\n",
                   (unsigned int)result->tokens_per_sec,
                   (unsigned int)(result->tokens_per_sec * 100.0) % 100);
    console_printf("[BENCH] Target %d tok/s  : %s\n",
                   PERF_TARGET_TOKENS_PER_SEC,
                   result->target_met ? "MET" : "NOT MET");

    return 0;
}

/* ============================================================================
 * GGUF Model Validation
 * ============================================================================ */

int benchmark_validate_gguf_model(const char *model_name)
{
    int passed = 0;
    int total = 0;

    benchmark_init();

    console_printf("\n=== GGUF Model Validation ===\n");
    console_printf("Model: %s\n", model_name ? model_name : "(unknown)");

    /* Test 1: model parsed */
    total++;
    const struct gguf_model_arch *arch = gguf_parser_get_arch();
    if (arch) {
        passed++;
        console_printf("[PASS] Model parsed: arch=%s dim=%u layers=%u vocab=%u\n",
                       arch->general_architecture,
                       arch->embedding_length, arch->block_count,
                       arch->vocab_size);
    } else {
        console_printf("[FAIL] No GGUF model loaded\n");
        console_printf("Validation: %d/%d tests passed\n", passed, total);
        return passed;
    }

    /* Test 2: sane architecture parameters */
    total++;
    if (arch->embedding_length > 0 && arch->block_count > 0 &&
        arch->vocab_size > 0 && arch->attention_head_count > 0) {
        passed++;
        console_printf("[PASS] Architecture parameters sane\n");
    } else {
        console_printf("[FAIL] Architecture parameters invalid\n");
    }

    /* Test 3: tokenizer usable */
    total++;
    if (bpe_tokenizer_is_initialized() || bpe_tokenizer_init() == 0) {
        int toks[16];
        int n = bpe_tokenizer_encode("hello", toks, 16, false, false);
        if (n > 0) {
            passed++;
            console_printf("[PASS] Tokenizer working (%d tokens for test string)\n", n);
        } else {
            console_printf("[FAIL] Tokenizer encode failed\n");
        }
    } else {
        console_printf("[FAIL] Tokenizer init failed\n");
    }

    /* Test 4: inference engine initializes */
    total++;
    if (streaming_inference_is_ready() || streaming_inference_init(false) == 0) {
        passed++;
        console_printf("[PASS] Streaming inference engine ready\n");
    } else {
        console_printf("[FAIL] Streaming inference engine init failed\n");
    }

    /* Test 5: short real inference run */
    total++;
    {
        inference_benchmark_t bench;
        if (benchmark_gguf_inference(&bench, "Hello", 4) == 0 &&
            bench.total_tokens > 0) {
            passed++;
            console_printf("[PASS] Inference run (%llu tokens)\n",
                           (unsigned long long)bench.total_tokens);
        } else {
            console_printf("[FAIL] Inference run failed\n");
        }
    }

    console_printf("Validation: %d/%d tests passed\n", passed, total);
    return passed;
}
