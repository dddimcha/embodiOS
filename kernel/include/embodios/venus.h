/* EMBODIOS Venus (virtio-gpu Vulkan) Protocol Layer
 *
 * Volta v0.6.0, WS-C Part 2 — SPEC-COMPLETE, NOT VALIDATED IN THIS
 * ENVIRONMENT. QEMU 7.2 here ships `virtio-gpu-pci`/`virtio-gpu-gl-pci`
 * but no Venus-enabled device, so the code below is written against:
 *
 *   - Virtio specification v1.2, 5.7 "GPU Device" (capsets, contexts)
 *   - virglrenderer: virgl_renderer_capset_venus (capset wire layout)
 *   - Mesa src/virtio/vulkan (vn_ring, VK_MESA ring protocol) and the
 *     venus-protocol repo (vn_protocol_driver_* generated encoders)
 *
 * Everything past the VIRTIO_GPU_CAPSET_VENUS capability handshake is
 * marked TODO(venus-host) and must be validated against a live Venus host
 * (`qemu -device virtio-gpu-gl-pci,venus=on` + host Vulkan driver) before
 * being trusted.
 *
 * When the GPU device has no Venus capset the layer is inert:
 * venus_transport_probe() logs the absence and returns NULL.
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
 * Venus ring layout (shared guest/host memory region)
 *
 * The Venus command transport is a single-producer/single-consumer ring in
 * guest-allocated shared memory. The guest registers the ring with the
 * host renderer via vkCreateRingMESA (VK_MESA ring protocol command),
 * which references a virtio-gpu resource (blob) backing the ring pages.
 *
 * TODO(venus-host): field offsets follow Mesa vn_ring_layout; validate
 * against a live host before relying on them.
 * ============================================================================ */

#define VN_RING_STATUS_ALIVE        0   /* ring operating normally */
#define VN_RING_STATUS_ABORT        1   /* renderer asked guest to abort */

struct vn_ring_layout {
    volatile uint32_t head;         /* 0x00: consumer position (host) */
    volatile uint32_t tail;         /* 0x04: producer position (guest) */
    volatile uint32_t status;       /* 0x08: VN_RING_STATUS_* */
    uint32_t pad0[5];               /* 0x0c..0x1f */
    volatile uint32_t buffer_size;  /* 0x20: data region size in bytes */
    uint32_t pad1[7];               /* 0x24..0x3f */
    uint8_t  data[];                /* 0x40: ring data (circular) */
} __packed;

#define VN_RING_DATA_OFFSET         0x40
#define VN_RING_DEFAULT_SIZE        (128 * 1024)

/* VK_MESA ring protocol: ring registration command (host-side handler
 * vkr_dispatch_vkCreateRingMESA in virglrenderer). Serialized by the
 * vn_protocol encoders; fields below are the logical payload.
 * TODO(venus-host): confirm exact serialized layout from the generated
 * vn_protocol_driver_command.h of the target venus-protocol release. */
typedef struct vn_ring_create_info {
    uint32_t resource_id;   /* virtio-gpu resource backing the ring */
    uint32_t size;          /* total shared region size */
    uint32_t head_offset;   /* offset of head counter */
    uint32_t tail_offset;   /* offset of tail counter */
    uint32_t status_offset; /* offset of status word */
    uint32_t buffer_offset; /* offset of ring data */
    uint32_t extra_size;    /* trailing scratch bytes */
    uint32_t idle_timeout;  /* renderer idle timeout (ms) */
} vn_ring_create_info_t;

/* ============================================================================
 * Venus transport state
 * ============================================================================ */

typedef struct venus_transport {
    virtio_gpu_dev_t *gpu;          /* parent virtio-gpu device */

    struct venus_capset capset;     /* negotiated capset contents */
    uint32_t capset_index;          /* index used for GET_CAPSET */

    uint32_t ctx_id;                /* Venus rendering context (0 = none) */

    /* Command ring (guest-allocated shared memory) */
    struct vn_ring_layout *ring;    /* NULL until ring is registered */
    uint64_t ring_dma;              /* DMA/physical address of ring */
    uint32_t ring_size;             /* total bytes of ring allocation */
    uint32_t ring_resource_id;      /* blob/resource backing the ring */
    bool ring_registered;           /* vkCreateRingMESA acknowledged */

    bool active;                    /* transport usable for vk commands */
} venus_transport_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Probe the virtio-gpu device for Venus support and, if present, bring up
 * the Venus transport (capset fetch + Venus context creation + ring
 * registration).
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
 * The caller provides vn_protocol-encoded command bytes (per the
 * venus-protocol vn_cs_encoder wire format). The stream is written to the
 * command ring when it is registered, or submitted directly with
 * VIRTIO_GPU_CMD_SUBMIT_3D otherwise.
 *
 * @param cmds      vn_protocol-encoded command stream
 * @param cmds_len  stream size in bytes
 * @param fence_id  optional out: virtio-gpu fence for completion tracking
 * @return 0 on success, negative error code otherwise
 *         (VIRTIO_GPU_ERR_INVALID when no Venus transport exists)
 */
int venus_vk_execute(const void *cmds, uint32_t cmds_len, uint64_t *fence_id);

/**
 * Print Venus transport state to the console (used by the gpuinfo command).
 */
void venus_print_info(void);

#endif /* EMBODIOS_VENUS_H */
