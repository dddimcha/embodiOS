/* EMBODIOS Venus (virtio-gpu Vulkan) Protocol Layer
 *
 * Maxwell v0.7.0, WS-A — Venus GPU inference transport.
 *
 * Two halves:
 *   1. Transport: capset handshake, Venus context, Mesa ring protocol
 *      (vkCreateRingMESA/vkNotifyRingMESA/vkSetReplyCommandStreamMESA) over
 *      blob-backed shared memory, synchronous command roundtrips.
 *   2. Compute device: venus_vk_device_init() brings up a full Vulkan
 *      instance->device->queue->pipelines stack by emitting
 *      VK_EXT_command_stream records (see venus_cs.h) into the ring, and
 *      venus_vk_matmul() runs one of the embedded compute shaders on
 *      blob-backed storage buffers.
 *
 * Wire format is byte-modeled on the Mesa venus-protocol generated encoders
 * (vk.xml VkCommandTypeEXT ids). In this sandbox no Venus device exists, so
 * the host-executed path is exercised only by tools/host_test_venus.c
 * (structural decode + lavapipe semantic replay); the kernel keeps the
 * honest no-Venus path: venus_transport_probe() returns NULL and gpuinfo
 * prints "unavailable".
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#ifndef EMBODIOS_VENUS_H
#define EMBODIOS_VENUS_H

#include <embodios/types.h>
#include <embodios/virtio_gpu.h>

/* ============================================================================
 * Venus capset (VIRTIO_GPU_CAPSET_VENUS = 4) payload
 * virglrenderer: struct virgl_renderer_capset_venus
 * ============================================================================ */

struct venus_capset {
    uint32_t wire_format_version;           /* venus wire format */
    uint32_t vk_xml_version;                /* Vulkan XML version */
    uint32_t vk_ext_command_stream_version; /* VK_EXT_command_stream */
} __packed;

/* ============================================================================
 * Mesa ring layout (vn_ring_get_layout, VK_RING_FLAGS_NONE)
 *
 * Cacheline-separated counters inside one blob resource:
 *   head @ 0, tail @ 64, status @ 128, buffer @ 192,
 *   extra region directly after the buffer.
 * head/tail are u32 monotonic byte counters (wrap-safe via signed diff).
 * ============================================================================ */

#define VN_RING_HEAD_OFF        0
#define VN_RING_TAIL_OFF        64
#define VN_RING_STATUS_OFF      128
#define VN_RING_DATA_OFF        192

#define VN_RING_STATUS_ALIVE    0   /* ring operating normally */
#define VN_RING_STATUS_ABORT    1   /* renderer asked guest to abort */

#define VN_RING_BUFFER_SIZE     (128 * 1024)    /* circular command data */
#define VN_RING_EXTRA_SIZE      (64 * 1024)     /* renderer scratch */
#define VN_RING_SHMEM_SIZE      (VN_RING_DATA_OFF + VN_RING_BUFFER_SIZE + \
                                 VN_RING_EXTRA_SIZE)

/* ============================================================================
 * Venus transport state
 * ============================================================================ */

typedef struct venus_transport {
    virtio_gpu_dev_t *gpu;          /* parent virtio-gpu device */

    struct venus_capset capset;     /* negotiated capset contents */
    uint32_t capset_index;          /* index used for GET_CAPSET */

    uint32_t ctx_id;                /* Venus rendering context (0 = none) */

    /* Command ring: HOST3D|MAPPABLE blob, mapped into the guest through the
     * virtio-gpu shared-memory window (identity-mapped MMIO). */
    volatile uint8_t *ring;         /* mapped ring base (NULL until mapped) */
    uint64_t ring_phys;             /* hostmem_base + map_info */
    uint32_t ring_size;             /* blob size (>= VN_RING_SHMEM_SIZE) */
    uint32_t ring_resource_id;      /* blob resource backing the ring */
    uint64_t ring_id;               /* VkRingMESA id chosen by the guest */
    bool ring_registered;           /* vkCreateRingMESA acknowledged */

    /* Reply shmem: small HOST3D|MAPPABLE blob, target of
     * vkSetReplyCommandStreamMESA for synchronous roundtrips. */
    volatile uint8_t *reply;
    uint32_t reply_resource_id;
    uint32_t reply_size;

    bool active;                    /* transport usable for vk commands */
} venus_transport_t;

/* ============================================================================
 * Venus Vulkan compute device
 * ============================================================================ */

#define VENUS_VK_MAX_SHADERS    4

/* Embedded SPIR-V module descriptor (vk_device.c maps g_vk_shaders onto this) */
typedef struct venus_vk_shader {
    const uint32_t *code;
    uint32_t word_count;
    const char *name;
} venus_vk_shader_t;

/* All handles are guest-assigned u64 object ids (Venus convention). */
typedef struct venus_vk_device {
    venus_transport_t *t;

    uint64_t instance;
    uint64_t physical;              /* first physical device */
    uint64_t device;
    uint64_t queue;
    uint32_t queue_family_index;    /* compute-capable family */

    uint64_t shader_modules[VENUS_VK_MAX_SHADERS];
    uint64_t set_layout;            /* 3 storage buffers, compute */
    uint64_t pipeline_layout;       /* + 12-byte push constants */
    uint64_t pipelines[VENUS_VK_MAX_SHADERS];
    uint32_t shader_count;

    uint64_t desc_pool;
    uint64_t desc_set;
    uint64_t cmd_pool;
    uint64_t cmd_buf;
    uint64_t fence;

    /* memory types that are HOST_VISIBLE|HOST_COHERENT (bitmask) */
    uint32_t host_visible_coherent_types;

    char device_name[256];          /* VkPhysicalDeviceProperties */
    uint32_t vendor_id;
    uint32_t device_type;

    bool ready;
} venus_vk_device_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Probe the virtio-gpu device for Venus support and, if present, bring up
 * the Venus transport (capset fetch + Venus context creation + ring and
 * reply shmem registration).
 *
 * Called by the virtio-gpu driver at the end of a successful probe.
 *
 * @param gpu  probed virtio-gpu device
 * @return transport instance, or NULL when the Venus capset is absent
 *         (with the log line "GPU: virtio-gpu without Venus capset
 *         (transport unavailable)")
 */
venus_transport_t *venus_transport_probe(virtio_gpu_dev_t *gpu);

/**
 * Tear down the Venus transport (destroy context, release ring).
 */
void venus_transport_shutdown(void);

/**
 * @return active transport, or NULL when Venus is unavailable
 */
venus_transport_t *venus_transport_get(void);

/**
 * Execute a marshalled Venus/Vulkan command stream on the Venus context.
 *
 * The caller provides vn_protocol-encoded command bytes (venus_cs.h wire
 * format). The stream is written to the command ring and the renderer is
 * woken with vkNotifyRingMESA; without a registered ring the stream is
 * submitted directly with VIRTIO_GPU_CMD_SUBMIT_3D.
 *
 * @param cmds      vn_protocol-encoded command stream
 * @param cmds_len  stream size in bytes
 * @param fence_id  optional out: virtio-gpu fence for completion tracking
 * @return 0 on success, negative error code otherwise
 *         (VIRTIO_GPU_ERR_INVALID when no Venus transport exists)
 */
int venus_vk_execute(const void *cmds, uint32_t cmds_len, uint64_t *fence_id);

/**
 * Bring up the Vulkan compute stack on an active Venus transport:
 * instance, physical device, compute queue, shader modules, descriptor
 * set layout (3 storage buffers), pipeline layout (12B push constants),
 * compute pipelines, descriptor pool/set, command pool/buffer, fence.
 *
 * Every step is a synchronous ring roundtrip with the renderer; on any
 * VkResult != VK_SUCCESS the function fails and `dev->ready` stays false.
 *
 * @return 0 on success, negative error otherwise
 */
int venus_vk_device_init(venus_transport_t *t, venus_vk_device_t *dev,
                         const venus_vk_shader_t *shaders, uint32_t count);

/**
 * Run one matmul on the compute device.
 *
 *   binding 0: A — `a_bytes` bytes (shader-specific layout)
 *   binding 1: B — `b_bytes` bytes
 *   binding 2: C — m*n fp32 results
 *   push constants: u32 m, u32 k, u32 n
 *   dispatch: ceil(m*n / 64) workgroups
 *
 * Buffers are per-call HOST3D/GUEST_VRAM blobs imported via
 * VkImportMemoryResourceInfoMESA, destroyed before return.
 *
 * @return 0 on success, negative error otherwise
 */
int venus_vk_matmul(venus_vk_device_t *dev, uint32_t pipeline_idx,
                    const void *A, uint64_t a_bytes,
                    const void *B, uint64_t b_bytes,
                    float *C, uint32_t m, uint32_t k, uint32_t n);

/**
 * Print Venus transport state to the console (used by the gpuinfo command).
 */
void venus_print_info(void);

#endif /* EMBODIOS_VENUS_H */
