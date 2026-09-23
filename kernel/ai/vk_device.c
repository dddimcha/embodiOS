/* EMBODIOS kernel-side Vulkan compute device layer.
 *
 * See kernel/include/embodios/vk_device.h for the architecture overview.
 *
 * What is real in v0.5.0 "Tesla":
 *   - PCI discovery of GPU candidates (VGA-class devices and virtio-gpu).
 *   - The full driver state machine and the Vulkan object model (instance,
 *     device, queue, buffers, descriptors, compute pipelines built from the
 *     embedded, lavapipe-validated SPIR-V in vk_shaders.h).
 *
 * What is intentionally not faked:
 *   - There is no host Vulkan driver reachable from the guest under QEMU TCG
 *     (no KVM, no Venus/virtio-gpu-vulkan device). Submitting real Vulkan
 *     commands requires one of the activation paths documented in
 *     docs/gpu-backend.md. Until then vk_device_init() stops at
 *     VK_DEV_NO_DRIVER and every matmul entry point reports failure so the
 *     caller uses the SIMD CPU fallback. Nothing here pretends to have a GPU.
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/pci.h>
#include <embodios/mm.h>
#include <embodios/vk_device.h>
#include <embodios/venus.h>

#include "vk_shaders.h"

/* ---------------------------------------------------------------------------
 * PCI identity constants
 * ------------------------------------------------------------------------- */
#define VK_PCI_VENDOR_VIRTIO        0x1AF4
#define VK_PCI_DEVID_VIRTIO_GPU_T   0x1010  /* virtio-gpu, transitional */
#define VK_PCI_DEVID_VIRTIO_GPU_M   0x1050  /* virtio-gpu, modern (non-transitional) */
#define VK_PCI_CLASS_DISPLAY        0x03

/* Q8_0 geometry (must match the shader and ggml) */
#define VK_QK8_0                    32
#define VK_Q8_0_BLOCK_BYTES         34

/* K-quant geometry (Q4_K/Q6_K superblocks, 256 values each) */
#define VK_QK_K                     256
#define VK_Q4_K_SUPERBLOCK_BYTES    144
#define VK_Q6_K_SUPERBLOCK_BYTES    210

/* ---------------------------------------------------------------------------
 * Driver state
 * ------------------------------------------------------------------------- */
typedef enum vk_driver_state {
    VK_STATE_UNPROBED = 0,
    VK_STATE_NO_DEVICE,      /* probe ran, no candidates */
    VK_STATE_PROBED,         /* candidates exist, init not attempted */
    VK_STATE_NO_DRIVER,      /* init attempted, no host Vulkan driver */
    VK_STATE_READY,          /* fully initialized (never under QEMU TCG) */
} vk_driver_state_t;

static vk_driver_state_t g_vk_state = VK_STATE_UNPROBED;
static vk_candidate_t g_vk_candidates[VK_MAX_CANDIDATES];
static int g_vk_candidate_count = 0;
static int g_vk_active = -1;   /* index into g_vk_candidates, or -1 */

/* ---------------------------------------------------------------------------
 * vk_device_probe — PCI discovery
 * ------------------------------------------------------------------------- */
static void vk_probe_add(uint16_t vendor, uint16_t devid,
                         const pci_device_t *dev, int is_virtio)
{
    if (g_vk_candidate_count >= VK_MAX_CANDIDATES) {
        return;
    }
    vk_candidate_t *c = &g_vk_candidates[g_vk_candidate_count++];
    c->vendor_id = vendor;
    c->device_id = devid;
    c->bus = dev->addr.bus;
    c->device = dev->addr.device;
    c->function = dev->addr.function;
    c->class_code = dev->class_code;
    c->subclass = dev->subclass;
    c->is_virtio_gpu = is_virtio;
}

/* Vendors whose display-class devices have a documented Vulkan driver
 * (i.e. a conceivable kernel activation path via PCI passthrough).
 * Legacy/emulated framebuffer adapters (bochs-display 1234:1111, Cirrus,
 * VMware SVGA, QXL, virtio-vga without Venus) are display-only devices:
 * they are reported but never counted as Vulkan candidates. */
#define VK_PCI_VENDOR_AMD     0x1002
#define VK_PCI_VENDOR_NVIDIA  0x10DE
#define VK_PCI_VENDOR_INTEL   0x8086

static int vk_vendor_has_vulkan_path(uint16_t vendor)
{
    return vendor == VK_PCI_VENDOR_AMD ||
           vendor == VK_PCI_VENDOR_NVIDIA ||
           vendor == VK_PCI_VENDOR_INTEL;
}

int vk_device_probe(vk_candidate_t *out, int max_out)
{
    if (g_vk_state == VK_STATE_UNPROBED) {
        g_vk_candidate_count = 0;

        if (pci_device_count() <= 0) {
            console_printf("GPU: PCI subsystem empty, no devices to probe\n");
            g_vk_state = VK_STATE_NO_DEVICE;
        } else {
            /* Walk every discovered PCI device once. Candidates are devices
             * with a real Vulkan activation path: virtio-gpu (Venus) or a
             * display-class device from a GPU vendor (passthrough KMD). */
            int n = pci_device_count();
            for (int i = 0; i < n; i++) {
                pci_device_t *dev = pci_get_device(i);
                if (!dev) {
                    continue;
                }
                int is_virtio_gpu =
                    dev->vendor_id == VK_PCI_VENDOR_VIRTIO &&
                    (dev->device_id == VK_PCI_DEVID_VIRTIO_GPU_T ||
                     dev->device_id == VK_PCI_DEVID_VIRTIO_GPU_M);
                if (is_virtio_gpu) {
                    vk_probe_add(dev->vendor_id, dev->device_id, dev, 1);
                } else if (dev->class_code == VK_PCI_CLASS_DISPLAY) {
                    if (vk_vendor_has_vulkan_path(dev->vendor_id)) {
                        vk_probe_add(dev->vendor_id, dev->device_id, dev, 0);
                    } else {
                        console_printf("GPU: display adapter %04x:%04x at "
                                       "%02x:%02x.%x (not Vulkan-capable)\n",
                                       dev->vendor_id, dev->device_id,
                                       dev->addr.bus, dev->addr.device,
                                       dev->addr.function);
                    }
                }
            }
            g_vk_state = g_vk_candidate_count > 0 ? VK_STATE_PROBED
                                                  : VK_STATE_NO_DEVICE;
        }

        if (g_vk_candidate_count == 0) {
            console_printf("GPU: none (no Vulkan-capable PCI device)\n");
        } else {
            for (int i = 0; i < g_vk_candidate_count; i++) {
                vk_candidate_t *c = &g_vk_candidates[i];
                console_printf("GPU: candidate %04x:%04x at %02x:%02x.%x "
                               "class %02x:%02x%s\n",
                               c->vendor_id, c->device_id,
                               c->bus, c->device, c->function,
                               c->class_code, c->subclass,
                               c->is_virtio_gpu ? " (virtio-gpu)" : "");
            }
        }
    }

    if (out && max_out > 0) {
        int n = g_vk_candidate_count < max_out ? g_vk_candidate_count : max_out;
        for (int i = 0; i < n; i++) {
            out[i] = g_vk_candidates[i];
        }
    }
    return g_vk_candidate_count;
}

/* ---------------------------------------------------------------------------
 * Vulkan object model (kernel representation)
 *
 * Activation path TODO (see docs/gpu-backend.md):
 * The functions below describe the exact Vulkan calls the driver issues once
 * a transport exists. Two transports are planned:
 *   1. virtio-gpu Venus (VIRTIO_GPU_F_VENUS): guest Vulkan commands are
 *      marshalled into virtqueue buffers with VIRTIO_GPU_CMD_SUBMIT_3D after
 *      VIRTIO_GPU_CMD_CTX_INIT with context init flag VENUS; the host
 *      (qemu -device virtio-gpu-gl,venus=on + vulkan renderer) executes them.
 *      Requires QEMU built with Venus support and KVM-less virtio-gpu —
 *      not available in this environment.
 *   2. PCI passthrough of a real GPU: vendor-specific MMIO doorbells; out of
 *      scope for a generic layer, but the object model below maps 1:1 onto
 *      what a vendor KMD would program.
 * ------------------------------------------------------------------------- */

/* Embedded shader modules (words). lavapipe-validated on the host. */
static const struct {
    const uint32_t *code;
    uint32_t word_count;
    const char *name;
} g_vk_shaders[] = {
    { vk_spv_matmul_f32,       vk_spv_matmul_f32_word_count,
      "matmul_f32" },
    { vk_spv_matmul_q8_0,      vk_spv_matmul_q8_0_word_count,
      "matmul_q8_0" },
    { vk_spv_matmul_q4_k_q8_0, vk_spv_matmul_q4_k_q8_0_word_count,
      "matmul_q4_k_q8_0" },
    { vk_spv_matmul_q6_k_q8_0, vk_spv_matmul_q6_k_q8_0_word_count,
      "matmul_q6_k_q8_0" },
};
#define VK_NUM_PIPELINES (sizeof(g_vk_shaders) / sizeof(g_vk_shaders[0]))

/* Venus compute device (valid when g_vk_state == VK_STATE_READY) */
static venus_vk_device_t g_vk_venus_dev;

/* Map the embedded shader table onto the Venus shader descriptor. */
static void vk_venus_shader_map(venus_vk_shader_t *out)
{
    for (uint32_t i = 0; i < VK_NUM_PIPELINES; i++) {
        out[i].code = g_vk_shaders[i].code;
        out[i].word_count = g_vk_shaders[i].word_count;
        out[i].name = g_vk_shaders[i].name;
    }
}

/* Sanity-check an embedded SPIR-V module header. */
static int vk_spv_header_ok(const uint32_t *code, uint32_t word_count)
{
    if (!code || word_count < 5) {
        return 0;
    }
    if (code[0] != 0x07230203u) {          /* magic */
        return 0;
    }
    if ((code[1] >> 16) != 1) {            /* major version 1.x */
        return 0;
    }
    if (code[3] <= 1) {                    /* id bound */
        return 0;
    }
    return 1;
}

int vk_device_init(void)
{
    if (g_vk_state == VK_STATE_READY) {
        return VK_DEV_OK;
    }
    if (g_vk_state == VK_STATE_UNPROBED) {
        vk_device_probe(NULL, 0);
    }
    if (g_vk_candidate_count == 0) {
        return VK_DEV_NO_DEVICE;
    }

    /* Validate embedded shaders first — cheap and driver-independent. */
    for (uint32_t i = 0; i < VK_NUM_PIPELINES; i++) {
        if (!vk_spv_header_ok(g_vk_shaders[i].code, g_vk_shaders[i].word_count)) {
            console_printf("GPU: embedded shader '%s' failed header check\n",
                           g_vk_shaders[i].name);
            g_vk_state = VK_STATE_NO_DRIVER;
            return VK_DEV_BAD_SHADER;
        }
    }

    /* Pick the first candidate (prefer virtio-gpu: it is the only one with a
     * documented guest->host Vulkan path under QEMU). */
    g_vk_active = 0;
    for (int i = 0; i < g_vk_candidate_count; i++) {
        if (g_vk_candidates[i].is_virtio_gpu) {
            g_vk_active = i;
            break;
        }
    }
    vk_candidate_t *dev = &g_vk_candidates[g_vk_active];

    if (dev->is_virtio_gpu) {
        /* Venus activation (Maxwell v0.7.0): the virtio-gpu driver probed
         * the Venus capset and brought up the ring transport at boot;
         * venus_transport_get() is non-NULL exactly when that succeeded. */
        venus_transport_t *t = venus_transport_get();
        if (t) {
            static venus_vk_shader_t shaders[VK_NUM_PIPELINES];
            vk_venus_shader_map(shaders);
            if (venus_vk_device_init(t, &g_vk_venus_dev, shaders,
                                     VK_NUM_PIPELINES) == 0) {
                console_printf("GPU: Venus compute device ready: \"%s\" "
                               "(%u pipelines)\n",
                               g_vk_venus_dev.device_name,
                               (uint32_t)VK_NUM_PIPELINES);
                g_vk_state = VK_STATE_READY;
                return VK_DEV_OK;
            }
            console_printf("GPU: Venus transport present but device init "
                           "failed; compute unavailable\n");
            g_vk_state = VK_STATE_NO_DRIVER;
            return VK_DEV_IO;
        }

        console_printf("GPU: virtio-gpu found but no Venus (Vulkan) feature; "
                       "compute unavailable\n");
        g_vk_state = VK_STATE_NO_DRIVER;
        return VK_DEV_NO_DRIVER;
    }

    /* TODO(passthrough activation): real GPU on the PCI bus. Map BAR0 via
     * vmm, then program vendor-specific MMIO (doorbells, ring buffers,
     * firmware upload). This is vendor KMD territory; the descriptor/
     * pipeline model in tools/host_test_vulkan.c documents the Vulkan-side
     * flow this layer mirrors. */
    console_printf("GPU: %04x:%04x present but no kernel Vulkan driver "
                   "(needs PCI passthrough KMD); compute unavailable\n",
                   dev->vendor_id, dev->device_id);
    g_vk_state = VK_STATE_NO_DRIVER;
    return VK_DEV_NO_DRIVER;
}

int vk_device_available(void)
{
    return g_vk_state == VK_STATE_READY;
}

const char *vk_device_name(void)
{
    switch (g_vk_state) {
    case VK_STATE_READY:
        return g_vk_venus_dev.ready ? g_vk_venus_dev.device_name
                                    : "Vulkan compute device";
    case VK_STATE_PROBED:
        return "GPU candidate (driver not initialized)";
    case VK_STATE_NO_DRIVER:
        return "GPU present, no Vulkan driver";
    case VK_STATE_NO_DEVICE:
        return "none";
    case VK_STATE_UNPROBED:
    default:
        return "unprobed";
    }
}

/* ---------------------------------------------------------------------------
 * Dispatch entry points
 *
 * Buffer layout contract (shared with the shaders and the host test):
 *   binding 0: A — fp32[m*k] (matmul_f32), Q8_0 blocks (matmul_q8_0),
 *              Q4_K superblocks (matmul_q4_k_q8_0, 144 B per 256 values) or
 *              Q6_K superblocks (matmul_q6_k_q8_0, 210 B per 256 values);
 *              padded to a 4-byte multiple (the shader reads whole u32 words;
 *              the last word of the final block may straddle the unpadded end)
 *   binding 1: B — fp32[k*n] (matmul_f32, matmul_q8_0) or Q8_0 block chains
 *              (matmul_q4_k_q8_0, matmul_q6_k_q8_0: block b of column j at
 *              byte offset (j*(k/32) + b) * 34), padded to a 4-byte multiple
 *   binding 2: C — fp32[m*n]
 *   push constants: u32 m, u32 k, u32 n (12 bytes)
 *   dispatch: ceil(m*n / 64) workgroups of 64 invocations, one C element per
 *   invocation.
 * ------------------------------------------------------------------------- */
int vk_matmul_f32(const float *A, const float *B, float *C,
                  uint32_t m, uint32_t k, uint32_t n)
{
    if (g_vk_state != VK_STATE_READY) {
        return VK_DEV_NO_DRIVER;
    }
    uint64_t a_bytes = (uint64_t)m * k * 4;
    uint64_t b_bytes = (uint64_t)k * n * 4;
    if (venus_vk_matmul(&g_vk_venus_dev, 0, A, a_bytes, B, b_bytes,
                        C, m, k, n) != 0) {
        return VK_DEV_IO;
    }
    return VK_DEV_OK;
}

int vk_matmul_q8_0(const void *A, const float *B, float *C,
                   uint32_t m, uint32_t k, uint32_t n)
{
    if (k % VK_QK8_0 != 0) {
        return VK_DEV_UNSUPPORTED;
    }
    if (g_vk_state != VK_STATE_READY) {
        return VK_DEV_NO_DRIVER;
    }
    uint64_t a_bytes = (uint64_t)m * (k / VK_QK8_0) * VK_Q8_0_BLOCK_BYTES;
    uint64_t b_bytes = (uint64_t)k * n * 4;
    if (venus_vk_matmul(&g_vk_venus_dev, 1, A, a_bytes, B, b_bytes,
                        C, m, k, n) != 0) {
        return VK_DEV_IO;
    }
    return VK_DEV_OK;
}

int vk_matmul_q4_k_q8_0(const void *A, const void *B, float *C,
                        uint32_t m, uint32_t k, uint32_t n)
{
    if (k % VK_QK_K != 0) {
        return VK_DEV_UNSUPPORTED;
    }
    if (g_vk_state != VK_STATE_READY) {
        return VK_DEV_NO_DRIVER;
    }
    uint64_t a_bytes = (uint64_t)m * (k / VK_QK_K) * VK_Q4_K_SUPERBLOCK_BYTES;
    uint64_t b_bytes = (uint64_t)n * (k / VK_QK8_0) * VK_Q8_0_BLOCK_BYTES;
    if (venus_vk_matmul(&g_vk_venus_dev, 2, A, a_bytes, B, b_bytes,
                        C, m, k, n) != 0) {
        return VK_DEV_IO;
    }
    return VK_DEV_OK;
}

int vk_matmul_q6_k_q8_0(const void *A, const void *B, float *C,
                        uint32_t m, uint32_t k, uint32_t n)
{
    if (k % VK_QK_K != 0) {
        return VK_DEV_UNSUPPORTED;
    }
    if (g_vk_state != VK_STATE_READY) {
        return VK_DEV_NO_DRIVER;
    }
    uint64_t a_bytes = (uint64_t)m * (k / VK_QK_K) * VK_Q6_K_SUPERBLOCK_BYTES;
    uint64_t b_bytes = (uint64_t)n * (k / VK_QK8_0) * VK_Q8_0_BLOCK_BYTES;
    if (venus_vk_matmul(&g_vk_venus_dev, 3, A, a_bytes, B, b_bytes,
                        C, m, k, n) != 0) {
        return VK_DEV_IO;
    }
    return VK_DEV_OK;
}

void vk_device_shutdown(void)
{
    if (g_vk_state == VK_STATE_READY) {
        /* Venus objects are host-side; the transport teardown
         * (venus_transport_shutdown) destroys the rendering context, which
         * reclaims everything created inside it. */
        memset(&g_vk_venus_dev, 0, sizeof(g_vk_venus_dev));
    }
    g_vk_active = -1;
    if (g_vk_state != VK_STATE_UNPROBED) {
        g_vk_state = g_vk_candidate_count > 0 ? VK_STATE_PROBED
                                              : VK_STATE_NO_DEVICE;
    }
}
