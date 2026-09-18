/* GPU Backend Implementation for EMBODIOS
 *
 * Provides GPU acceleration for inference operations using Vulkan.
 * Implements automatic CPU fallback when GPU is unavailable.
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/gpu_backend.h>

/* ============================================================================
 * Global Backend State
 * ============================================================================ */

static gpu_backend_type_t g_backend_type = GPU_BACKEND_NONE;
static int g_backend_available = 0;
static gpu_device_info_t g_device_info = {0};

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

    /* Auto-detect: try Vulkan first */
    if (type == GPU_BACKEND_AUTO || type == GPU_BACKEND_VULKAN) {
#ifdef GGML_USE_VULKAN
        console_printf("[GPU Backend] Attempting Vulkan initialization...\n");

        /* Try to initialize Vulkan backend */
        int vk_result = ggml_backend_vk_init();
        if (vk_result == 0) {
            /* Check if any devices are available */
            int device_count = ggml_backend_vk_get_device_count();
            if (device_count > 0) {
                g_backend_type = GPU_BACKEND_VULKAN;
                g_backend_available = 1;

                /* Get first device info */
                g_device_info.type = GPU_BACKEND_VULKAN;
                g_device_info.available = 1;
                ggml_backend_vk_get_device_description(0, g_device_info.device_name,
                                                       sizeof(g_device_info.device_name));

                console_printf("[GPU Backend] Vulkan initialized: %d device(s) found\n", device_count);
                return 0;
            }

            console_printf("[GPU Backend] Vulkan initialized but no devices found\n");
        } else {
            console_printf("[GPU Backend] Vulkan initialization failed (code %d)\n", vk_result);
        }
#else
        console_printf("[GPU Backend] Vulkan support not compiled in (GGML_USE_VULKAN not defined)\n");
#endif
    }

    /* GPU initialization failed - automatic CPU fallback */
    console_printf("[GPU Backend] No GPU available, CPU fallback active\n");
    g_backend_type = GPU_BACKEND_NONE;
    g_backend_available = 0;
    g_device_info.type = GPU_BACKEND_NONE;
    g_device_info.available = 0;

    /* Return error to signal GPU unavailable (caller should handle fallback) */
    return -1;
}

/* Shutdown GPU backend and free resources */
void gpu_backend_shutdown(void) {
    if (g_backend_available) {
        console_printf("[GPU Backend] Shutting down\n");
        /* TODO: Call Vulkan cleanup when implemented */
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
