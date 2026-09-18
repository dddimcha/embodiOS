/* Host-side accuracy harness for the K-quants dequantization code.
 *
 * NOT part of the kernel build. Compiled on the host together with the
 * actual kernel sources:
 *   gcc -I kernel/include tools/host_test_kquants.c kernel/ai/quantized_ops.c
 *
 * It exercises:
 *   1. the shared float ports (include/embodios/kquants_dequant.h) used by
 *      streaming_inference.c / gguf_inference.c, and
 *   2. the fixed-point Q16.16 ports in kernel/ai/quantized_ops.c.
 *
 * Usage: host_test_kquants <q2k|q3k|q5k> <blocks.bin> <n_blocks> <out_prefix>
 * Writes <out_prefix>.float.bin and <out_prefix>.fixed.bin, each
 * n_blocks*256 little-endian float32 values.
 */

#include <embodios/kquants_dequant.h>
#include <embodios/quantized_ops.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Link stub: quantized_ops.c matmul paths reference vec_dot_neon.
 * The dequant paths under test never call it. */
fixed_t vec_dot_neon(const fixed_t* a, const fixed_t* b, size_t n)
{
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += ((int64_t)a[i] * b[i]) >> 16;
    }
    return (fixed_t)sum;
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <q2k|q3k|q5k> <blocks.bin> <n_blocks> <out_prefix>\n",
                argv[0]);
        return 2;
    }

    const char* mode = argv[1];
    const char* in_path = argv[2];
    long n_blocks = atol(argv[3]);
    const char* out_prefix = argv[4];

    size_t block_size;
    int elems = KQUANTS_QK_K;
    int has_fixed = 1;
    if (strcmp(mode, "q2k") == 0)      block_size = sizeof(kquant_block_q2_K);
    else if (strcmp(mode, "q3k") == 0) block_size = sizeof(kquant_block_q3_K);
    else if (strcmp(mode, "q5k") == 0) block_size = sizeof(kquant_block_q5_K);
    else if (strcmp(mode, "iq4nl") == 0) {
        block_size = sizeof(kquant_block_iq4_nl);
        elems = KQUANTS_QK4_NL;
        has_fixed = 0;  /* no fixed-point IQ4_NL port in quantized_ops.c */
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    FILE* fin = fopen(in_path, "rb");
    if (!fin) { perror("fopen input"); return 1; }

    char out_float[512], out_fixed[512];
    snprintf(out_float, sizeof(out_float), "%s.float.bin", out_prefix);
    snprintf(out_fixed, sizeof(out_fixed), "%s.fixed.bin", out_prefix);
    FILE* ff = fopen(out_float, "wb");
    FILE* fx = fopen(out_fixed, "wb");
    if (!ff || !fx) { perror("fopen output"); return 1; }

    uint8_t* block = malloc(block_size);
    float* out_f = malloc(KQUANTS_QK_K * sizeof(float));
    fixed_t* out_x = malloc(KQUANTS_QK_K * sizeof(fixed_t));
    float* out_xf = malloc(KQUANTS_QK_K * sizeof(float));

    for (long b = 0; b < n_blocks; b++) {
        if (fread(block, 1, block_size, fin) != block_size) {
            fprintf(stderr, "short read at block %ld\n", b);
            return 1;
        }

        /* Float path: the exact code used by the streaming kernel */
        if (strcmp(mode, "q2k") == 0)      kquant_dequant_q2_K(block, out_f, elems);
        else if (strcmp(mode, "q3k") == 0) kquant_dequant_q3_K(block, out_f, elems);
        else if (strcmp(mode, "q5k") == 0) kquant_dequant_q5_K(block, out_f, elems);
        else                               kquant_dequant_iq4_nl(block, out_f, elems);

        if (has_fixed) {
            /* Fixed-point path: kernel/ai/quantized_ops.c */
            int ret;
            if (strcmp(mode, "q2k") == 0)
                ret = dequantize_q2_k(block, block_size, out_x, elems);
            else if (strcmp(mode, "q3k") == 0)
                ret = dequantize_q3_k(block, block_size, out_x, elems);
            else
                ret = dequantize_q5_k(block, block_size, out_x, elems);
            if (ret != 0) {
                fprintf(stderr, "fixed dequant failed: %d\n", ret);
                return 1;
            }

            for (int i = 0; i < elems; i++) {
                out_xf[i] = (float)out_x[i] / 65536.0f;
            }
            fwrite(out_xf, sizeof(float), elems, fx);
        }

        fwrite(out_f, sizeof(float), elems, ff);
    }

    fclose(fin);
    fclose(ff);
    fclose(fx);
    return 0;
}
