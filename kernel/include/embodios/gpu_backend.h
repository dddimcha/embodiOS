/* GPU Backend API for EMBODIOS
 *
 * Provides GPU acceleration for inference operations using Vulkan.
 * Supports cross-vendor GPUs (AMD, NVIDIA, Intel) with graceful CPU fallback.
 */

#ifndef _EMBODIOS_GPU_BACKEND_H
#define _EMBODIOS_GPU_BACKEND_H

#include <embodios/types.h>

/* GPU backend types */
typedef enum {
    GPU_BACKEND_NONE = 0,       /* No GPU acceleration (CPU fallback mode) */
    GPU_BACKEND_VULKAN,         /* Vulkan compute backend */
    GPU_BACKEND_AUTO,           /* Auto-detect best available backend */
} gpu_backend_type_t;

/* ============================================================================
 * CPU Fallback Behavior
 * ============================================================================
 *
 * AUTOMATIC CPU FALLBACK:
 *
 * When GPU initialization fails for ANY reason (missing device, driver error,
 * insufficient VRAM, etc.), the inference engine automatically falls back to
 * CPU-only execution without error. This ensures inference always works
 * regardless of hardware availability.
 *
 * GPU initialization can fail due to:
 * - No compatible GPU device found
 * - Vulkan driver not available or incompatible
 * - GPU device initialization error
 * - Insufficient VRAM for model
 * - Device busy or in error state
 *
 * The fallback mechanism:
 * 1. gpu_backend_init() returns negative error code on GPU failure
 * 2. gpu_backend_is_available() returns 0 (false)
 * 3. gpu_backend_get_type() returns GPU_BACKEND_NONE
 * 4. Inference engine uses CPU integer-only operations
 * 5. Execution continues normally with CPU performance characteristics
 *
 * Applications should:
 * - Call gpu_backend_init() and check result
 * - Use gpu_backend_is_available() to determine active backend
 * - Continue execution regardless of GPU availability
 * - Log GPU unavailability for diagnostics but do not fail
 *
 * ============================================================================ */

/* GPU device information */
typedef struct {
    gpu_backend_type_t type;    /* Backend type in use */
    char device_name[256];      /* GPU device name */
    uint32_t vendor_id;         /* PCI vendor ID (0x1002=AMD, 0x10DE=NVIDIA, 0x8086=Intel) */
    uint32_t device_id;         /* PCI device ID */
    size_t vram_size;           /* VRAM size in bytes */
    int available;              /* 1 if GPU is available and initialized */
} gpu_device_info_t;

/* GPU backend context (opaque) */
typedef struct gpu_backend_ctx gpu_backend_ctx_t;

/* ============================================================================
 * Initialization & Cleanup
 * ============================================================================ */

/* Initialize GPU backend
 * type: Backend type to use (GPU_BACKEND_AUTO for auto-detection)
 * Returns: 0 on success, negative on error
 *
 * If GPU initialization fails, the system gracefully falls back to CPU.
 * This function is safe to call multiple times (idempotent).
 */
int gpu_backend_init(gpu_backend_type_t type);

/* Shutdown GPU backend and free resources */
void gpu_backend_shutdown(void);

/* Check if GPU backend is available and initialized */
int gpu_backend_is_available(void);

/* Get current GPU backend type */
gpu_backend_type_t gpu_backend_get_type(void);

/* Get GPU device information
 * info: Output buffer for device info
 * Returns: 0 on success, negative if no GPU available
 */
int gpu_backend_get_device_info(gpu_device_info_t* info);

/* ============================================================================
 * Backend Selection & Configuration
 * ============================================================================ */

/* Select specific GPU device by index
 * device_index: GPU device index (0 = first GPU, 1 = second, etc.)
 * Returns: 0 on success, negative on error
 */
int gpu_backend_select_device(int device_index);

/* Enumerate available GPU devices
 * devices: Output array of device info
 * max_devices: Maximum number of devices to enumerate
 * Returns: Number of devices found, or negative on error
 */
int gpu_backend_enumerate_devices(gpu_device_info_t* devices, int max_devices);

/* ============================================================================
 * Runtime probe & compute dispatch (v0.5.0 "Tesla", WS-GPU)
 * ============================================================================ */

/* Probe for a usable Vulkan compute GPU (PCI scan + feature check).
 * Returns >0 if a GPU is present AND a kernel Vulkan driver can drive it,
 * 0 otherwise (expected under QEMU TCG: no Venus/virtio-gpu-vulkan).
 * Idempotent and cached; safe to call on every matmul — the first call logs
 * a one-line report ("GPU: none (...)" when nothing usable was found). */
int gpu_backend_probe(void);

/* GPU matmul with in-shader Q8_0 dequantization: C[m*n] = dequant(A) * B.
 * A: ggml Q8_0 blocks (34 bytes per 32 values, fp16 scale + int8 quants);
 * k must be a multiple of 32. B and C are fp32, row-major.
 * Returns 0 on success; any negative value means "not computed" and the
 * caller MUST fall back to the SIMD/scalar CPU path. */
int gpu_matmul_q8_0(const void *A, const void *B, float *C, int m, int k, int n);

/* Static name of the probed backend/device ("none" when unavailable). */
const char *gpu_backend_name(void);

/* ============================================================================
 * Testing
 * ============================================================================ */

/* Run end-to-end GPU inference tests
 * Tests full pipeline with GPU vs CPU comparison
 */
void run_e2e_gpu_tests(void);

#endif /* _EMBODIOS_GPU_BACKEND_H */
