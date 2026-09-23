/* GPU Backend Implementation for EMBODIOS
 *
 * Provides GPU acceleration for inference operations using Vulkan.
 * Implements automatic CPU fallback when GPU is unavailable.
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/gpu_backend.h>
#include <embodios/vk_device.h>

/* ============================================================================
 * Global Backend State
 * ============================================================================ */

static gpu_backend_type_t g_backend_type = GPU_BACKEND_NONE;
static int g_backend_available = 0;
static gpu_device_info_t g_device_info = {0};

/* Cached result of gpu_backend_probe(): -1 = not probed yet,
 * 0 = no usable GPU, 1 = usable Vulkan compute device. */
static int g_probe_result = -1;

/* ============================================================================
 * Forward Declarations for Vulkan Backend
 * ============================================================================ */

#ifdef GGML_USE_VULKAN
/* These will be implemented in ggml-vulkan.cpp when Vulkan backend is active */
extern int ggml_backend_vk_init(void);
extern int ggml_backend_vk_get_device_count(void);
extern int ggml_backend_vk_get_device_description(int device, char* description, size_t size);
#endif

/* ============================================================================
 * Initialization & Cleanup
 * ============================================================================ */

/* Initialize GPU backend
 * Returns: 0 on success, negative on error (triggers CPU fallback)
 */
int gpu_backend_init(gpu_backend_type_t type) {
    /* If already initialized, return success */
    if (g_backend_available) {
        return 0;
    }

    /* Auto-detect: try the kernel Vulkan device layer first */
    if (type == GPU_BACKEND_AUTO || type == GPU_BACKEND_VULKAN) {
        int vk_result = vk_device_init();
        if (vk_result == VK_DEV_OK && vk_device_available()) {
            g_backend_type = GPU_BACKEND_VULKAN;
            g_backend_available = 1;

            g_device_info.type = GPU_BACKEND_VULKAN;
            g_device_info.available = 1;
            const char *name = vk_device_name();
            int i = 0;
            while (name[i] && i < (int)sizeof(g_device_info.device_name) - 1) {
                g_device_info.device_name[i] = name[i];
                i++;
            }
            g_device_info.device_name[i] = '\0';

            console_printf("[GPU Backend] Vulkan compute initialized: %s\n",
                           g_device_info.device_name);
            g_probe_result = 1;
            return 0;
        }
        /* vk_device_init already logged the precise reason (no device, no
         * Venus feature, no KMD). This is an expected condition under QEMU
         * TCG, not an error. */
        (void)vk_result;
    }

    /* GPU initialization failed - automatic CPU fallback */
    console_printf("[GPU Backend] No GPU available, CPU fallback active\n");
    g_backend_type = GPU_BACKEND_NONE;
    g_backend_available = 0;
    g_device_info.type = GPU_BACKEND_NONE;
    g_device_info.available = 0;
    g_probe_result = 0;

    /* Return error to signal GPU unavailable (caller should handle fallback) */
    return -1;
}

/* Shutdown GPU backend and free resources */
void gpu_backend_shutdown(void) {
    if (g_backend_available) {
        console_printf("[GPU Backend] Shutting down\n");
        vk_device_shutdown();
        g_backend_available = 0;
        g_backend_type = GPU_BACKEND_NONE;
    }
}

/* Check if GPU backend is available and initialized */
int gpu_backend_is_available(void) {
    return g_backend_available;
}

/* Get current GPU backend type */
gpu_backend_type_t gpu_backend_get_type(void) {
    return g_backend_type;
}

/* Get GPU device information
 * Returns: 0 on success, negative if no GPU available
 */
int gpu_backend_get_device_info(gpu_device_info_t* info) {
    if (!info) {
        return -1;
    }

    if (!g_backend_available) {
        return -1;
    }

    /* Copy device info */
    *info = g_device_info;
    return 0;
}

/* ============================================================================
 * Backend Selection & Configuration
 * ============================================================================ */

/* Select specific GPU device by index
 * Returns: 0 on success, negative on error
 */
int gpu_backend_select_device(int device_index) {
    if (!g_backend_available) {
        console_printf("[GPU Backend] No GPU backend available\n");
        return -1;
    }

    /* TODO: Implement device selection when Vulkan backend supports it */
    console_printf("[GPU Backend] Device selection not yet implemented\n");
    return -1;
}

/* Enumerate available GPU devices
 * Returns: Number of devices found, or negative on error
 */
int gpu_backend_enumerate_devices(gpu_device_info_t* devices, int max_devices) {
    if (!devices || max_devices < 1) {
        return -1;
    }

#ifdef GGML_USE_VULKAN
    if (g_backend_available) {
        int device_count = ggml_backend_vk_get_device_count();
        int count = device_count < max_devices ? device_count : max_devices;

        for (int i = 0; i < count; i++) {
            devices[i].type = GPU_BACKEND_VULKAN;
            devices[i].available = 1;
            ggml_backend_vk_get_device_description(i, devices[i].device_name,
                                                   sizeof(devices[i].device_name));
        }

        return count;
    }
#endif

    /* No GPU available */
    return 0;
}

/* ============================================================================
 * Runtime Probe & Compute Dispatch (WS-GPU, v0.5.0 "Tesla")
 * ============================================================================ */

/* Probe for a usable Vulkan compute GPU. Cached: only the first call scans
 * the PCI bus and logs. Returns 1 only when a device AND a kernel Vulkan
 * driver are both usable (never under QEMU TCG — no Venus). */
int gpu_backend_probe(void) {
    if (g_probe_result >= 0) {
        return g_probe_result;
    }

    int candidates = vk_device_probe(NULL, 0);
    if (candidates <= 0 || vk_device_init() != VK_DEV_OK ||
        !vk_device_available()) {
        g_probe_result = 0;
        return 0;
    }

    g_probe_result = 1;
    return 1;
}

/* GPU matmul with in-shader Q8_0 dequantization.
 * Returns 0 on success, negative when the work was NOT done (caller falls
 * back to the SIMD CPU path). Never blocks indefinitely: any transport
 * error surfaces as a negative result. */
int gpu_matmul_q8_0(const void *A, const void *B, float *C, int m, int k, int n) {
    if (g_probe_result < 0) {
        gpu_backend_probe();
    }
    if (g_probe_result <= 0) {
        return -1;
    }
    return vk_matmul_q8_0(A, (const float*)B, C,
                          (uint32_t)m, (uint32_t)k, (uint32_t)n);
}

/* GPU matmul with in-shader Q4_K x Q8_0 dequantization (v0.6.0 "Volta").
 * Same contract as gpu_matmul_q8_0: negative = not computed, caller falls
 * back to the CPU path. */
int gpu_matmul_q4_k_q8_0(const void *A, const void *B, float *C,
                         int m, int k, int n) {
    if (g_probe_result < 0) {
        gpu_backend_probe();
    }
    if (g_probe_result <= 0) {
        return -1;
    }
    return vk_matmul_q4_k_q8_0(A, B, C,
                               (uint32_t)m, (uint32_t)k, (uint32_t)n);
}

/* GPU matmul with in-shader Q6_K x Q8_0 dequantization (v0.6.0 "Volta"). */
int gpu_matmul_q6_k_q8_0(const void *A, const void *B, float *C,
                         int m, int k, int n) {
    if (g_probe_result < 0) {
        gpu_backend_probe();
    }
    if (g_probe_result <= 0) {
        return -1;
    }
    return vk_matmul_q6_k_q8_0(A, B, C,
                               (uint32_t)m, (uint32_t)k, (uint32_t)n);
}

const char *gpu_backend_name(void) {
    if (g_probe_result < 0) {
        gpu_backend_probe();
    }
    if (g_probe_result <= 0) {
        return "none";
    }
    return vk_device_name();
}
