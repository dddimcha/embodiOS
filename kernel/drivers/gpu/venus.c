/* EMBODIOS Venus (virtio-gpu Vulkan) Protocol Layer
 *
 * Volta v0.6.0, WS-C Part 2 — SPEC-COMPLETE, NOT VALIDATED IN THIS
 * ENVIRONMENT (this QEMU 7.2 has no Venus-enabled virtio-gpu device).
 *
 * What is real and exercised by construction:
 *   - VIRTIO_GPU_CAPSET_VENUS detection from the virtio-gpu capset table
 *   - honest no-Venus path: transport stays NULL with a clear log line
 *
 * What is written to the spec but can only run on a Venus host
 * (`qemu -device virtio-gpu-gl-pci,venus=on` with virglrenderer >= 0.10
 * and a host Vulkan driver):
 *   - GET_CAPSET payload parse (virgl_renderer_capset_venus layout)
 *   - VIRTIO_GPU_CMD_CTX_CREATE with context_init = CAPSET_ID(VENUS)
 *     (requires VIRTIO_GPU_F_CONTEXT_INIT)
 *   - ring buffer registration via vkCreateRingMESA and subsequent
 *     vn_protocol command submission through VIRTIO_GPU_CMD_SUBMIT_3D
 *
 * TODO(venus-host) markers flag every boundary that needs validation
 * against a live host before the bytes can be trusted.
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#include <embodios/venus.h>
#include <embodios/console.h>
#include <embodios/mm.h>

/* ============================================================================
 * Module State
 * ============================================================================ */

static venus_transport_t g_venus;
static bool g_venus_probed = false;

/* ============================================================================
 * vn_protocol command stream encoding (VK_EXT_command_stream wire format)
 *
 * Venus commands are serialized by the generated vn_protocol_driver_*
 * encoders. Each command record starts with a header carrying the command
 * type (VkCommandTypeEXT) and the serialized size.
 * TODO(venus-host): vkCreateRingMESA command id and full record layout must
 * be taken from the venus-protocol release matching the host capset's
 * wire_format_version; the constants below are placeholders for that
 * generated header.
 * ============================================================================ */

#define VN_CMD_HEADER_SIZE      8   /* u32 command_type + u32 stream size */

/* TODO(venus-host): replace with VK_COMMAND_TYPE_vkCreateRingMESA_EXT from
 * the generated vn_protocol_driver_command.h */
#define VN_CMD_TYPE_VKCREATERING_MESA  0xFFFFFF01u

/**
 * Serialize a vkCreateRingMESA command record into `out`.
 *
 * Wire layout (per vn_cs_encoder conventions):
 *   [0..3]  command type
 *   [4..7]  total serialized size of the record
 *   [8..]   payload: VkRingCreateInfoMESA fields (vn_ring_create_info_t)
 *
 * TODO(venus-host): the real encoder wraps the payload in a VkCommand
 * structure with sType chain handling; confirm against generated code.
 */
static uint32_t venus_encode_create_ring(uint8_t *out, uint32_t out_cap,
                                         const vn_ring_create_info_t *info)
{
    uint32_t total = VN_CMD_HEADER_SIZE + sizeof(*info);
    if (out_cap < total) {
        return 0;
    }

    uint32_t *w = (uint32_t *)out;
    w[0] = VN_CMD_TYPE_VKCREATERING_MESA;
    w[1] = total;
    memcpy(out + VN_CMD_HEADER_SIZE, info, sizeof(*info));
    return total;
}

/* ============================================================================
 * Transport bring-up
 * ============================================================================ */

/**
 * Create the Venus rendering context.
 * VIRTIO_GPU_CMD_CTX_CREATE with context_init carrying the Venus capset id
 * (virtio spec 5.7.6.10, requires VIRTIO_GPU_F_CONTEXT_INIT).
 */
static int venus_create_context(venus_transport_t *t)
{
    static const char name[] = "embodios-venus";

    struct virtio_gpu_ctx_create cmd = {0};
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = 1;  /* our only rendering context */
    cmd.nlen = sizeof(name) - 1;
    cmd.context_init = VIRTIO_GPU_CAPSET_VENUS &
                       VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    memcpy(cmd.debug_name, name, sizeof(name) - 1);

    struct virtio_gpu_ctrl_hdr resp;
    int ret = virtio_gpu_ctrl_send(&cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    if (ret != VIRTIO_GPU_OK) {
        console_printf("[Venus] CTX_CREATE failed: %d\n", ret);
        return ret;
    }

    t->ctx_id = 1;
    console_printf("[Venus] context %u created (capset-init venus)\n",
                   t->ctx_id);
    return 0;
}

/**
 * Allocate and register the command ring.
 *
 * Spec path (Mesa vn_instance_ring_init + virglrenderer
 * vkr_dispatch_vkCreateRingMESA):
 *   1. guest allocates ring shmem (blob resource when
 *      VIRTIO_GPU_F_RESOURCE_BLOB is negotiated, linear resource +
 *      ATTACH_BACKING otherwise);
 *   2. guest submits vkCreateRingMESA on the Venus context via
 *      VIRTIO_GPU_CMD_SUBMIT_3D, naming the resource and the offsets of
 *      head/tail/status/data inside it;
 *   3. all further Vulkan traffic is vn_protocol records written into the
 *      ring; SUBMIT_3D pokes the renderer to consume.
 *
 * TODO(venus-host): steps 1-2 are compiled but never run here — no QEMU
 * device in this environment exposes the Venus capset. Validate the
 * encoded record against vn_protocol_driver_command.h on a live host.
 */
static int venus_ring_init(venus_transport_t *t)
{
    uint32_t total = VN_RING_DATA_OFFSET + VN_RING_DEFAULT_SIZE;

    t->ring = heap_alloc_aligned(total, 4096);
    if (!t->ring) {
        return VIRTIO_GPU_ERR_NO_MEMORY;
    }
    memset(t->ring, 0, total);
    t->ring_dma = (uint64_t)(uintptr_t)t->ring;  /* identity mapped */
    t->ring_size = total;
    t->ring->buffer_size = VN_RING_DEFAULT_SIZE;

    /* TODO(venus-host): create a host-visible blob resource for the ring
     * (VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, BLOB_MEM_GUEST +
     * BLOB_FLAG_MAPPABLE) so the renderer can map the same pages; with a
     * plain linear resource the ring cannot be shared. Store the assigned
     * resource id in t->ring_resource_id. */
    t->ring_resource_id = 1;  /* placeholder until blob creation lands */

    vn_ring_create_info_t info = {
        .resource_id   = t->ring_resource_id,
        .size          = total,
        .head_offset   = 0x00,
        .tail_offset   = 0x04,
        .status_offset = 0x08,
        .buffer_offset = VN_RING_DATA_OFFSET,
        .extra_size    = 0,
        .idle_timeout  = 1000,
    };

    uint8_t record[VN_CMD_HEADER_SIZE + sizeof(info)];
    uint32_t record_len = venus_encode_create_ring(record, sizeof(record), &info);
    if (record_len == 0) {
        heap_free_aligned(t->ring);
        t->ring = NULL;
        return VIRTIO_GPU_ERR_INVALID;
    }

    uint64_t fence;
    int ret = virtio_gpu_submit_3d(t->ctx_id, record, record_len, &fence);
    if (ret != VIRTIO_GPU_OK) {
        console_printf("[Venus] vkCreateRingMESA submit failed: %d\n", ret);
        heap_free_aligned(t->ring);
        t->ring = NULL;
        return ret;
    }

    t->ring_registered = true;
    console_printf("[Venus] command ring registered (%u KB, fence %llu)\n",
                   VN_RING_DEFAULT_SIZE / 1024,
                   (unsigned long long)fence);
    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

venus_transport_t *venus_transport_probe(virtio_gpu_dev_t *gpu)
{
    if (g_venus_probed) {
        return g_venus.active ? &g_venus : NULL;
    }
    g_venus_probed = true;

    memset(&g_venus, 0, sizeof(g_venus));
    g_venus.gpu = gpu;

    if (!gpu || !gpu->initialized) {
        return NULL;
    }

    /* Step 1: is there a Venus capset at all? */
    uint32_t capset_index = 0;
    const virtio_gpu_capset_t *cs =
        virtio_gpu_find_capset(VIRTIO_GPU_CAPSET_VENUS, &capset_index);
    if (!cs) {
        console_printf("GPU: virtio-gpu without Venus capset "
                       "(transport unavailable)\n");
        return NULL;
    }
    g_venus.capset_index = capset_index;

    console_printf("[Venus] capset found (index %u, max_version=%u, max_size=%u)\n",
                   capset_index, cs->max_version, cs->max_size);

    /* --- From here on: Venus-host-only path (untestable in this env) --- */

    /* Step 2: fetch the capset payload */
    if (cs->max_size < sizeof(struct venus_capset)) {
        console_printf("[Venus] capset too small (%u < %u); aborting\n",
                       cs->max_size, (uint32_t)sizeof(struct venus_capset));
        return NULL;
    }
    if (virtio_gpu_get_capset(capset_index, 1,
                              &g_venus.capset, sizeof(g_venus.capset)) != 0) {
        console_printf("[Venus] GET_CAPSET failed; transport unavailable\n");
        return NULL;
    }
    console_printf("[Venus] wire_format=%u vk_xml=%u vk_ext_command_stream=%u\n",
                   g_venus.capset.wire_format_version,
                   g_venus.capset.vk_xml_version,
                   g_venus.capset.vk_ext_command_stream_version);

    /* Step 3: Venus contexts require VIRTIO_GPU_F_CONTEXT_INIT */
    if (!(virtio_gpu_features() & VIRTIO_GPU_F_CONTEXT_INIT_MASK)) {
        console_printf("[Venus] host did not offer CONTEXT_INIT; "
                       "cannot create Venus context\n");
        return NULL;
    }

    /* Step 4: create the Venus rendering context */
    if (venus_create_context(&g_venus) != 0) {
        return NULL;
    }

    /* Step 5: command ring registration.
     * TODO(venus-host): depends on blob resource creation (see
     * venus_ring_init); on failure the context still exists and callers
     * may submit one-shot commands via venus_vk_execute(). */
    if (venus_ring_init(&g_venus) != 0) {
        console_printf("[Venus] ring registration incomplete; "
                       "context-only transport\n");
    }

    g_venus.active = true;
    console_printf("[Venus] transport active (ctx %u, ring %s)\n",
                   g_venus.ctx_id,
                   g_venus.ring_registered ? "registered" : "unregistered");
    return &g_venus;
}

void venus_transport_shutdown(void)
{
    if (!g_venus.active) {
        return;
    }

    if (g_venus.ctx_id) {
        struct virtio_gpu_ctx_destroy cmd = {0};
        cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
        cmd.hdr.ctx_id = g_venus.ctx_id;
        virtio_gpu_ctrl_send(&cmd, sizeof(cmd), NULL, 0, NULL);
        g_venus.ctx_id = 0;
    }

    if (g_venus.ring) {
        heap_free_aligned(g_venus.ring);
        g_venus.ring = NULL;
    }

    g_venus.active = false;
    g_venus.ring_registered = false;
}

venus_transport_t *venus_transport_get(void)
{
    return g_venus.active ? &g_venus : NULL;
}

int venus_vk_execute(const void *cmds, uint32_t cmds_len, uint64_t *fence_id)
{
    venus_transport_t *t = venus_transport_get();
    if (!t || !cmds || cmds_len == 0) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    if (t->ring_registered) {
        /* Ring path: append the record to the ring data region and poke the
         * renderer with an empty SUBMIT_3D (host consumes from the ring).
         * TODO(venus-host): implement proper ring wrap/wait-space semantics
         * per Mesa vn_ring_submit; direct submit is correct-but-slower and
         * is what we use until the ring protocol is host-validated. */
        return virtio_gpu_submit_3d(t->ctx_id, cmds, cmds_len, fence_id);
    }

    /* Context-only path: direct one-shot submission */
    return virtio_gpu_submit_3d(t->ctx_id, cmds, cmds_len, fence_id);
}

void venus_print_info(void)
{
    if (!g_venus_probed || !g_venus.active) {
        console_printf("  venus: unavailable (no VIRTIO_GPU_CAPSET_VENUS)\n");
        return;
    }

    console_printf("  venus: ctx=%u ring=%s wire_format=%u vk_xml=%u "
                   "cmd_stream=%u\n",
                   g_venus.ctx_id,
                   g_venus.ring_registered ? "registered" : "none",
                   g_venus.capset.wire_format_version,
                   g_venus.capset.vk_xml_version,
                   g_venus.capset.vk_ext_command_stream_version);
}
