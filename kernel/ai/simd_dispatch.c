/*
 * Runtime dispatch for the quantized vec_dot SIMD kernels.
 *
 * simd_kernels_init() runs once at boot (called from kernel_main right after
 * arch_cpu_init()): it verifies AVX2 usability (CPUID.7:EBX.AVX2 *and* OS
 * support via XCR0 XMM+YMM state — CPUID alone is not enough, executing AVX2
 * without XCR0 configured raises #UD) and installs the fastest safe kernel
 * per format. Callers (streaming_inference matmul paths) always go through
 * the function pointers, so the dispatch is transparent.
 *
 * Fallback ladder per format:
 *   Q8_0: AVX2 -> SSE2 (x86_64) / NEON (aarch64) / scalar
 *   Q4_K: AVX2 -> NEON (aarch64) / scalar
 *   Q5_0: AVX2 -> fused scalar (master used dequant+float-dot; fused scalar
 *         is strictly faster and bit-comparable, see host test)
 *   Q6_K: AVX2 -> fused scalar (same note)
 */

#include <embodios/simd_kernels.h>
#include <embodios/console.h>
#include <embodios/cpu.h>

vec_dot_q8_0_fn g_vec_dot_q8_0_q8_1 = vec_dot_q8_0_q8_1_scalar;
vec_dot_q4_k_fn g_vec_dot_q4_k_q8_1 = vec_dot_q4_k_q8_1_scalar;
vec_dot_q5_0_fn g_vec_dot_q5_0_q8_1 = vec_dot_q5_0_q8_1_scalar;
vec_dot_q6_k_fn g_vec_dot_q6_k_q8_1 = vec_dot_q6_k_q8_1_scalar;

static int g_avx2_active = 0;
static const char* g_backend = "scalar";
static int g_initialized = 0;

int simd_avx2_active(void) { return g_avx2_active; }
const char* simd_backend_name(void) { return g_backend; }

void simd_kernels_init(void)
{
    if (g_initialized) return;
    g_initialized = 1;

#if defined(__x86_64__) || defined(_M_X64)
    /* Baseline x86_64 improvements over plain scalar */
    g_vec_dot_q8_0_q8_1 = vec_dot_q8_0_q8_1_sse;
    g_backend = "SSE2";

    /* cpu_has_feature(CPU_FEATURE_AVX2) is only set after verifying
     * CPUID.1:ECX.OSXSAVE, CPUID.1:ECX.AVX, CPUID.7:EBX.AVX2 and a live
     * xgetbv(XCR0) check for XMM+YMM state (see arch/x86_64/cpu.c). */
    if (cpu_has_feature(CPU_FEATURE_AVX2)) {
        g_vec_dot_q8_0_q8_1 = vec_dot_q8_0_q8_1_avx2;
        g_vec_dot_q4_k_q8_1 = vec_dot_q4_k_q8_1_avx2;
        g_vec_dot_q5_0_q8_1 = vec_dot_q5_0_q8_1_avx2;
        g_vec_dot_q6_k_q8_1 = vec_dot_q6_k_q8_1_avx2;
        g_avx2_active = 1;
        g_backend = "AVX2";
    }
#elif defined(__aarch64__)
    /* NEON entry points are the *_scalar symbols on aarch64 */
    g_backend = "NEON";
#endif

    if (g_avx2_active) {
        console_printf("SIMD: AVX2 enabled (Q4_K/Q5_0/Q6_K/Q8_0 fused vec_dot)\n");
    } else {
        console_printf("SIMD: scalar fallback (AVX2 not available, backend=%s)\n",
                       g_backend);
    }
}
