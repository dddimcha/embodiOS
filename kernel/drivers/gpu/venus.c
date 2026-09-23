/* EMBODIOS Venus (virtio-gpu Vulkan) Protocol Layer
 *
 * Maxwell v0.7.0, WS-A — Venus GPU inference transport + compute device.
 *
 * Transport: Venus capset handshake, Venus context, Mesa ring protocol over
 * a blob-backed shared-memory ring (vkCreateRingMESA registered via
 * VIRTIO_GPU_CMD_SUBMIT_3D, then all Vulkan traffic written into the ring;
 * renderer woken with vkNotifyRingMESA; synchronous roundtrips via
 * vkSetReplyCommandStreamMESA + a GENERATE_REPLY-flagged command, completed
 * by polling the ring head counter past the submission tail).
 *
 * Compute device: venus_vk_device_init() / venus_vk_matmul() — see venus.h.
 * All command bytes are produced by the freestanding encoder in venus_cs.h,
 * byte-modeled on the Mesa venus-protocol generated encoders.
 *
 * No Venus device exists in this sandbox; the host-executed semantics are
 * validated by tools/host_test_venus.c (structural decode + lavapipe
 * replay). On plain QEMU the layer is inert (probe returns NULL).
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#include <embodios/venus.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <arch/x86_64/paging.h>

#include "venus_cs.h"

/* ============================================================================
 * Module state
 * ============================================================================ */

static venus_transport_t g_venus;
static bool g_venus_probed = false;

/* Command staging buffer (largest single record is vkCreateShaderModule,
 * ~7.1 KB for the q6_k shader + overhead; 16 KB leaves ample headroom). */
static uint8_t g_stage[16 * 1024] __attribute__((aligned(4096)));
static vncs_encoder_t g_enc;

/* Guest-assigned Venus object ids (any unique u64; Mesa uses pointers) */
static uint64_t g_next_handle = 0x1000;
static uint64_t venus_new_handle(void) { return g_next_handle++; }

/* virtio-gpu resource ids for blobs */
#define VENUS_RES_RING          100
#define VENUS_RES_REPLY         101
#define VENUS_RES_FIRST_BUFFER  1000
static uint32_t g_next_resource = VENUS_RES_FIRST_BUFFER;

/* VkRingMESA id (u64, guest-chosen) */
#define VENUS_RING_ID           0x454D424F44495352ULL  /* "EMBODISR" */

/* ============================================================================
 * Small helpers
 * ============================================================================ */

static uint64_t venus_round_page(uint64_t x)
{
    return (x + 4095) & ~4095ULL;
}

/* Map `size` bytes of a mappable blob into the guest address space via the
 * virtio-gpu shared-memory window. Returns the mapped address or NULL. */
static volatile uint8_t *venus_map_blob(uint32_t resource_id, uint64_t size)
{
    uint64_t map_info = 0;
    if (virtio_gpu_resource_map_blob(resource_id, 0, &map_info) != 0) {
        console_printf("[Venus] MAP_BLOB res=%u failed\n", resource_id);
        return NULL;
    }
    uint64_t hostmem_base = 0, hostmem_size = 0;
    if (!virtio_gpu_hostmem_window(&hostmem_base, &hostmem_size) ||
        map_info + size > hostmem_size) {
        console_printf("[Venus] blob map outside hostmem window\n");
        return NULL;
    }
    uint64_t phys = hostmem_base + map_info;
    if (!arch_identity_map_mmio(phys, size)) {
        console_printf("[Venus] failed to map blob MMIO 0x%llx+0x%llx\n",
                       (unsigned long long)phys, (unsigned long long)size);
        return NULL;
    }
    return (volatile uint8_t *)(uintptr_t)phys;
}

/* ============================================================================
 * Mesa ring protocol
 * ============================================================================ */

static volatile uint32_t *venus_ring_word(venus_transport_t *t, uint32_t off)
{
    return (volatile uint32_t *)(t->ring + off);
}

static uint32_t venus_ring_head(venus_transport_t *t)
{
    return __atomic_load_n(venus_ring_word(t, VN_RING_HEAD_OFF),
                           __ATOMIC_ACQUIRE);
}

static uint32_t venus_ring_tail(venus_transport_t *t)
{
    return __atomic_load_n(venus_ring_word(t, VN_RING_TAIL_OFF),
                           __ATOMIC_RELAXED);
}

static uint32_t venus_ring_status(venus_transport_t *t)
{
    return __atomic_load_n(venus_ring_word(t, VN_RING_STATUS_OFF),
                           __ATOMIC_RELAXED);
}

/* Busy-wait until head passes seqno (wrap-safe signed comparison, per
 * vn_ring_ge_seqno). Budget is generous: the renderer is host code that may
 * be scheduling real GPU work. */
static int venus_ring_wait(venus_transport_t *t, uint32_t seqno)
{
    uint64_t budget = 40000000;  /* ~seconds-scale, same style as ctrl_xfer */
    while (budget-- > 0) {
        if ((int32_t)(venus_ring_head(t) - seqno) >= 0) {
            return 0;
        }
        if (venus_ring_status(t) != VN_RING_STATUS_ALIVE) {
            console_printf("[Venus] ring aborted by renderer\n");
            return VIRTIO_GPU_ERR_DEVICE;
        }
        for (volatile int i = 0; i < 50; i++);
    }
    console_printf("[Venus] ring wait timeout (head=%u seqno=%u)\n",
                   venus_ring_head(t), seqno);
    return VIRTIO_GPU_ERR_TIMEOUT;
}

/* Append `len` bytes to the circular command buffer; returns the seqno
 * (tail value after the write). Waits for space when the ring is full. */
static int venus_ring_write(venus_transport_t *t, const void *data,
                            uint32_t len, uint32_t *out_seqno)
{
    uint32_t buf_size = VN_RING_BUFFER_SIZE;
    uint8_t *base = (uint8_t *)t->ring + VN_RING_DATA_OFF;

    uint32_t tail = venus_ring_tail(t);
    /* free space = buf_size - (tail - head); wait until len fits */
    uint64_t budget = 40000000;
    while (buf_size - (tail - venus_ring_head(t)) < len) {
        if (budget-- == 0) {
            return VIRTIO_GPU_ERR_TIMEOUT;
        }
        if (venus_ring_status(t) != VN_RING_STATUS_ALIVE) {
            return VIRTIO_GPU_ERR_DEVICE;
        }
        for (volatile int i = 0; i < 50; i++);
    }

    uint32_t off = tail % buf_size;
    uint32_t first = buf_size - off;
    if (first > len) {
        first = len;
    }
    memcpy(base + off, data, first);
    if (len > first) {
        memcpy(base, (const uint8_t *)data + first, len - first);
    }

    uint32_t seqno = tail + len;
    __atomic_store_n(venus_ring_word(t, VN_RING_TAIL_OFF), seqno,
                     __ATOMIC_RELEASE);
    *out_seqno = seqno;
    return 0;
}

/* Wake an idle renderer: vkNotifyRingMESA submitted directly via
 * VIRTIO_GPU_CMD_SUBMIT_3D (it is a transport command, not a ring record). */
static int venus_ring_notify(venus_transport_t *t, uint32_t seqno)
{
    uint8_t buf[32];
    vncs_encoder_t e;
    vncs_encoder_init(&e, buf, sizeof(buf));
    vncs_vkNotifyRingMESA(&e, 0, t->ring_id, seqno, 0 /* flags: no priorities */);
    if (e.fatal) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    return virtio_gpu_submit_3d(t->ctx_id, buf, (uint32_t)vncs_len(&e, buf),
                                NULL);
}

/**
 * Submit a command batch that starts with vkSetReplyCommandStreamMESA and
 * contains REPLY-flagged commands, then wait for the renderer to consume
 * it. The reply record(s) are at offset 0 of the reply shmem afterwards.
 */
static int venus_ring_submit(venus_transport_t *t, const void *cmds,
                             uint32_t cmds_len)
{
    if (!t->ring_registered) {
        return VIRTIO_GPU_ERR_UNSUPPORTED;
    }
    uint32_t seqno = 0;
    int rc = venus_ring_write(t, cmds, cmds_len, &seqno);
    if (rc) {
        return rc;
    }
    rc = venus_ring_notify(t, seqno);
    if (rc) {
        return rc;
    }
    return venus_ring_wait(t, seqno);
}

/* Begin a staged batch that requests a reply: SetReplyCommandStreamMESA
 * pointing at the reply shmem, then the caller encodes the command(s). */
static vncs_encoder_t *venus_batch_begin(venus_transport_t *t, bool with_reply)
{
    vncs_encoder_init(&g_enc, g_stage, sizeof(g_stage));
    if (with_reply) {
        vncs_vkSetReplyCommandStreamMESA(&g_enc, 0, t->reply_resource_id,
                                         0, t->reply_size);
    }
    return &g_enc;
}

/* Submit the staged batch and wait for consumption. */
static int venus_batch_end(venus_transport_t *t)
{
    if (g_enc.fatal) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    return venus_ring_submit(t, g_stage, (uint32_t)vncs_len(&g_enc, g_stage));
}

/* Reply decoder cursor over the reply shmem. */
static void venus_reply_begin(venus_transport_t *t, vncs_decoder_t *d)
{
    /* cast away volatile: contents are stable once the ring head passed */
    vncs_decoder_init(d, (const uint8_t *)t->reply, t->reply_size);
}

/* ============================================================================
 * Transport bring-up
 * ============================================================================ */

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
 * Allocate the blob-backed command ring and reply shmem and register the
 * ring with the renderer (vkCreateRingMESA via SUBMIT_3D).
 *
 * Both blobs are BLOB_MEM_HOST3D | USE_MAPPABLE: host-allocated, shared
 * guest<->host, mapped into the guest through the shared-memory window.
 */
static int venus_ring_init(venus_transport_t *t)
{
    if (!virtio_gpu_has_blob()) {
        console_printf("[Venus] no blob resources/hostmem window; "
                       "ring unavailable\n");
        return VIRTIO_GPU_ERR_UNSUPPORTED;
    }

    uint64_t ring_size = venus_round_page(VN_RING_SHMEM_SIZE);
    if (virtio_gpu_resource_create_blob(VENUS_RES_RING,
                                        VIRTIO_GPU_BLOB_MEM_HOST3D,
                                        VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                                        t->ring_id, ring_size) != 0) {
        console_printf("[Venus] ring blob creation failed\n");
        return VIRTIO_GPU_ERR_DEVICE;
    }
    t->ring = venus_map_blob(VENUS_RES_RING, ring_size);
    if (!t->ring) {
        virtio_gpu_resource_unref(VENUS_RES_RING);
        return VIRTIO_GPU_ERR_DEVICE;
    }
    /* zero the header/counters (blob contents are undefined at creation) */
    memset((void *)t->ring, 0, VN_RING_DATA_OFF);
    t->ring_size = (uint32_t)ring_size;
    t->ring_resource_id = VENUS_RES_RING;

    /* reply shmem */
    uint64_t reply_size = 4096;
    if (virtio_gpu_resource_create_blob(VENUS_RES_REPLY,
                                        VIRTIO_GPU_BLOB_MEM_HOST3D,
                                        VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                                        0, reply_size) != 0) {
        console_printf("[Venus] reply blob creation failed\n");
        virtio_gpu_resource_unmap_blob(VENUS_RES_RING);
        virtio_gpu_resource_unref(VENUS_RES_RING);
        t->ring = NULL;
        return VIRTIO_GPU_ERR_DEVICE;
    }
    t->reply = venus_map_blob(VENUS_RES_REPLY, reply_size);
    if (!t->reply) {
        virtio_gpu_resource_unmap_blob(VENUS_RES_REPLY);
        virtio_gpu_resource_unref(VENUS_RES_REPLY);
        virtio_gpu_resource_unmap_blob(VENUS_RES_RING);
        virtio_gpu_resource_unref(VENUS_RES_RING);
        t->ring = NULL;
        return VIRTIO_GPU_ERR_DEVICE;
    }
    memset((void *)t->reply, 0, reply_size);
    t->reply_resource_id = VENUS_RES_REPLY;
    t->reply_size = (uint32_t)reply_size;

    /* vkCreateRingMESA on the Venus context (direct SUBMIT_3D) */
    vncs_encoder_init(&g_enc, g_stage, sizeof(g_stage));
    vncs_vkCreateRingMESA(&g_enc, 0, t->ring_id, VENUS_RES_RING,
                          0, ring_size,
                          1000000000ULL,        /* idle timeout: 1s (ns) */
                          VN_RING_HEAD_OFF, VN_RING_TAIL_OFF,
                          VN_RING_STATUS_OFF, VN_RING_DATA_OFF,
                          VN_RING_BUFFER_SIZE,
                          VN_RING_DATA_OFF + VN_RING_BUFFER_SIZE,
                          VN_RING_EXTRA_SIZE);
    if (g_enc.fatal) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    uint64_t fence = 0;
    int ret = virtio_gpu_submit_3d(t->ctx_id, g_stage,
                                   (uint32_t)vncs_len(&g_enc, g_stage),
                                   &fence);
    if (ret != VIRTIO_GPU_OK) {
        console_printf("[Venus] vkCreateRingMESA submit failed: %d\n", ret);
        return ret;
    }

    t->ring_registered = true;
    console_printf("[Venus] command ring registered (%u KB buffer, fence %llu)\n",
                   VN_RING_BUFFER_SIZE / 1024, (unsigned long long)fence);
    return 0;
}

/* ============================================================================
 * Public transport API
 * ============================================================================ */

venus_transport_t *venus_transport_probe(virtio_gpu_dev_t *gpu)
{
    if (g_venus_probed) {
        return g_venus.active ? &g_venus : NULL;
    }
    g_venus_probed = true;

    memset(&g_venus, 0, sizeof(g_venus));
    g_venus.gpu = gpu;
    g_venus.ring_id = VENUS_RING_ID;

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

    /* Step 5: command ring + reply shmem (blob-backed). On failure the
     * context still exists and one-shot commands can go through
     * venus_vk_execute(), but the compute device needs the ring. */
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

    if (g_venus.ring_registered) {
        uint8_t buf[16];
        vncs_encoder_t e;
        vncs_encoder_init(&e, buf, sizeof(buf));
        vncs_vkDestroyRingMESA(&e, 0, g_venus.ring_id);
        if (!e.fatal) {
            virtio_gpu_submit_3d(g_venus.ctx_id, buf,
                                 (uint32_t)vncs_len(&e, buf), NULL);
        }
    }
    if (g_venus.ring) {
        virtio_gpu_resource_unmap_blob(g_venus.ring_resource_id);
        virtio_gpu_resource_unref(g_venus.ring_resource_id);
        g_venus.ring = NULL;
    }
    if (g_venus.reply) {
        virtio_gpu_resource_unmap_blob(g_venus.reply_resource_id);
        virtio_gpu_resource_unref(g_venus.reply_resource_id);
        g_venus.reply = NULL;
    }

    if (g_venus.ctx_id) {
        struct virtio_gpu_ctx_destroy cmd = {0};
        cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
        cmd.hdr.ctx_id = g_venus.ctx_id;
        virtio_gpu_ctrl_send(&cmd, sizeof(cmd), NULL, 0, NULL);
        g_venus.ctx_id = 0;
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
        /* Ring path: append the records, wake the renderer, wait for the
         * batch to be consumed (callers that need replies must have encoded
         * vkSetReplyCommandStreamMESA + GENERATE_REPLY themselves). */
        uint32_t seqno = 0;
        int rc = venus_ring_write(t, cmds, cmds_len, &seqno);
        if (rc) {
            return rc;
        }
        rc = venus_ring_notify(t, seqno);
        if (rc) {
            return rc;
        }
        return venus_ring_wait(t, seqno);
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

/* ============================================================================
 * Compute device bring-up
 *
 * Every step is a synchronous ring roundtrip: the batch carries
 * vkSetReplyCommandStreamMESA + one REPLY-flagged command; after the ring
 * head passes the submission tail the reply record is decoded from the
 * reply shmem and its VkResult checked.
 * ============================================================================ */

#define VENUS_VK_APP_NAME    "embodios"
#define VENUS_VK_ENGINE_NAME "embodios-venus"
#define VENUS_VK_API_VERSION ((1u << 22))  /* VK_API_VERSION_1_1 */

#define VENUS_FAIL(step, ret)                                             \
    do {                                                                  \
        console_printf("[Venus] vk_device_init: %s failed (%d)\n",        \
                       step, (int)(ret));                                 \
        return VIRTIO_GPU_ERR_DEVICE;                                     \
    } while (0)

/* Submit one REPLY-flagged command from an encoder that was primed with
 * venus_batch_begin(t, true), and check a ret-only VkResult reply. */
static int venus_check_ret(venus_transport_t *t, uint32_t cmd_type)
{
    if (venus_batch_end(t) != 0) {
        return -1;
    }
    vncs_decoder_t d;
    venus_reply_begin(t, &d);
    int32_t ret = vncr_ret_only_reply(&d, cmd_type);
    if (d.fatal || ret != VNCS_VK_SUCCESS) {
        return ret;
    }
    return 0;
}

int venus_vk_device_init(venus_transport_t *t, venus_vk_device_t *dev,
                         const venus_vk_shader_t *shaders, uint32_t count)
{
    if (!t || !t->active || !t->ring_registered || !dev || !shaders ||
        count == 0 || count > VENUS_VK_MAX_SHADERS) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    memset(dev, 0, sizeof(*dev));
    dev->t = t;
    dev->shader_count = count;

    vncs_encoder_t *e;
    vncs_decoder_t d;

    /* ---- vkCreateInstance ---- */
    dev->instance = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkCreateInstance(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                          VENUS_VK_APP_NAME, 1, VENUS_VK_ENGINE_NAME, 1,
                          VENUS_VK_API_VERSION, dev->instance);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkCreateInstance(submit)", -1);
    }
    venus_reply_begin(t, &d);
    uint64_t got = 0;
    int32_t ret = vncr_vkCreateInstance_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != dev->instance) {
        VENUS_FAIL("vkCreateInstance", ret);
    }

    /* ---- vkEnumeratePhysicalDevices (two-call pattern) ---- */
    e = venus_batch_begin(t, true);
    vncs_vkEnumeratePhysicalDevices(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                    dev->instance, 0, NULL);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkEnumeratePhysicalDevices(submit)", -1);
    }
    venus_reply_begin(t, &d);
    uint32_t phys_count = 0;
    ret = vncr_vkEnumeratePhysicalDevices_reply(&d, &phys_count, NULL, 0);
    if (d.fatal || ret != VNCS_VK_SUCCESS || phys_count == 0) {
        VENUS_FAIL("vkEnumeratePhysicalDevices(count)", ret);
    }

    uint64_t phys_ids[8];
    if (phys_count > 8) {
        phys_count = 8;
    }
    for (uint32_t i = 0; i < phys_count; i++) {
        phys_ids[i] = venus_new_handle();
    }
    e = venus_batch_begin(t, true);
    vncs_vkEnumeratePhysicalDevices(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                    dev->instance, phys_count, phys_ids);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkEnumeratePhysicalDevices(submit2)", -1);
    }
    venus_reply_begin(t, &d);
    uint32_t got_count = 0;
    uint64_t got_ids[8];
    ret = vncr_vkEnumeratePhysicalDevices_reply(&d, &got_count, got_ids, 8);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got_count == 0) {
        VENUS_FAIL("vkEnumeratePhysicalDevices", ret);
    }
    dev->physical = got_ids[0];

    /* ---- vkGetPhysicalDeviceProperties (device name for gpuinfo) ---- */
    e = venus_batch_begin(t, true);
    vncs_vkGetPhysicalDeviceProperties(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                       dev->physical);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkGetPhysicalDeviceProperties(submit)", -1);
    }
    venus_reply_begin(t, &d);
    vncr_vkGetPhysicalDeviceProperties_reply(&d, &dev->vendor_id,
                                             &dev->device_type,
                                             dev->device_name);
    if (d.fatal) {
        VENUS_FAIL("vkGetPhysicalDeviceProperties", -1);
    }

    /* ---- vkGetPhysicalDeviceQueueFamilyProperties (two-call) ---- */
    e = venus_batch_begin(t, true);
    vncs_vkGetPhysicalDeviceQueueFamilyProperties(
        e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->physical, 0);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkGetPhysicalDeviceQueueFamilyProperties(submit)", -1);
    }
    venus_reply_begin(t, &d);
    uint32_t qf_count = 0;
    vncr_vkGetPhysicalDeviceQueueFamilyProperties_reply(&d, &qf_count, NULL, 0);
    if (d.fatal || qf_count == 0) {
        VENUS_FAIL("vkGetPhysicalDeviceQueueFamilyProperties(count)", -1);
    }

    e = venus_batch_begin(t, true);
    vncs_vkGetPhysicalDeviceQueueFamilyProperties(
        e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->physical, qf_count);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkGetPhysicalDeviceQueueFamilyProperties(submit2)", -1);
    }
    venus_reply_begin(t, &d);
    static vncs_queue_family_props_t qf_props[16];
    uint32_t qf_got = 0;
    vncr_vkGetPhysicalDeviceQueueFamilyProperties_reply(&d, &qf_got,
                                                        qf_props, 16);
    if (d.fatal || qf_got == 0) {
        VENUS_FAIL("vkGetPhysicalDeviceQueueFamilyProperties", -1);
    }

    uint32_t compute_family = UINT32_MAX;
    for (uint32_t i = 0; i < qf_got; i++) {
        if ((qf_props[i].queue_flags & VNCS_QUEUE_COMPUTE_BIT) &&
            qf_props[i].queue_count > 0) {
            compute_family = i;
            break;
        }
    }
    if (compute_family == UINT32_MAX) {
        VENUS_FAIL("no compute queue family", -1);
    }
    dev->queue_family_index = compute_family;

    /* ---- vkGetPhysicalDeviceMemoryProperties ---- */
    e = venus_batch_begin(t, true);
    vncs_vkGetPhysicalDeviceMemoryProperties(
        e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->physical);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkGetPhysicalDeviceMemoryProperties(submit)", -1);
    }
    venus_reply_begin(t, &d);
    static vncs_memory_props_t mem_props;
    vncr_vkGetPhysicalDeviceMemoryProperties_reply(&d, &mem_props);
    if (d.fatal || mem_props.memory_type_count == 0) {
        VENUS_FAIL("vkGetPhysicalDeviceMemoryProperties", -1);
    }

    dev->host_visible_coherent_types = 0;
    for (uint32_t i = 0; i < mem_props.memory_type_count && i < 32; i++) {
        uint32_t f = mem_props.memory_types[i].property_flags;
        if ((f & VNCS_MEMORY_PROPERTY_HOST_VISIBLE) &&
            (f & VNCS_MEMORY_PROPERTY_HOST_COHERENT)) {
            dev->host_visible_coherent_types |= (1u << i);
        }
    }
    if (!dev->host_visible_coherent_types) {
        VENUS_FAIL("no host-visible coherent memory type", -1);
    }

    /* ---- vkCreateDevice (single queue on the compute family) ---- */
    dev->device = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkCreateDevice(e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->physical,
                        dev->queue_family_index, 1.0f, dev->device);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkCreateDevice(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkCreateDevice_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != dev->device) {
        VENUS_FAIL("vkCreateDevice", ret);
    }

    /* ---- vkGetDeviceQueue (no reply; ring order suffices) ---- */
    dev->queue = venus_new_handle();
    e = venus_batch_begin(t, false);
    vncs_vkGetDeviceQueue(e, 0, dev->device, dev->queue_family_index, 0,
                          dev->queue);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkGetDeviceQueue", -1);
    }

    /* ---- shader modules ---- */
    for (uint32_t i = 0; i < count; i++) {
        dev->shader_modules[i] = venus_new_handle();
        e = venus_batch_begin(t, true);
        vncs_vkCreateShaderModule(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                  dev->device, shaders[i].code,
                                  shaders[i].word_count,
                                  dev->shader_modules[i]);
        if (venus_batch_end(t) != 0) {
            VENUS_FAIL("vkCreateShaderModule(submit)", -1);
        }
        venus_reply_begin(t, &d);
        ret = vncr_vkCreateShaderModule_reply(&d, &got);
        if (d.fatal || ret != VNCS_VK_SUCCESS || got != dev->shader_modules[i]) {
            VENUS_FAIL("vkCreateShaderModule", ret);
        }
    }

    /* ---- descriptor set layout: 3 storage buffers ---- */
    static const vncs_dset_layout_binding_t bindings[3] = {
        { 0, VNCS_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VNCS_SHADER_STAGE_COMPUTE_BIT },
        { 1, VNCS_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VNCS_SHADER_STAGE_COMPUTE_BIT },
        { 2, VNCS_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VNCS_SHADER_STAGE_COMPUTE_BIT },
    };
    dev->set_layout = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkCreateDescriptorSetLayout(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                     dev->device, bindings, 3,
                                     dev->set_layout);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkCreateDescriptorSetLayout(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkCreateDescriptorSetLayout_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != dev->set_layout) {
        VENUS_FAIL("vkCreateDescriptorSetLayout", ret);
    }

    /* ---- pipeline layout: 1 set, 12-byte push constants (m, k, n) ---- */
    dev->pipeline_layout = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkCreatePipelineLayout(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                dev->device, &dev->set_layout, 1,
                                VNCS_SHADER_STAGE_COMPUTE_BIT, 0, 12,
                                dev->pipeline_layout);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkCreatePipelineLayout(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkCreatePipelineLayout_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != dev->pipeline_layout) {
        VENUS_FAIL("vkCreatePipelineLayout", ret);
    }

    /* ---- compute pipelines (one call, one per shader) ---- */
    for (uint32_t i = 0; i < count; i++) {
        dev->pipelines[i] = venus_new_handle();
    }
    e = venus_batch_begin(t, true);
    vncs_vkCreateComputePipelines(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                  dev->device, dev->shader_modules,
                                  dev->pipeline_layout, dev->pipelines,
                                  count);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkCreateComputePipelines(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkCreateComputePipelines_reply(&d, got_ids, count);
    if (d.fatal || ret != VNCS_VK_SUCCESS) {
        VENUS_FAIL("vkCreateComputePipelines", ret);
    }
    for (uint32_t i = 0; i < count; i++) {
        if (got_ids[i] != dev->pipelines[i]) {
            VENUS_FAIL("vkCreateComputePipelines(handle)", -1);
        }
    }

    /* ---- descriptor pool + set ---- */
    dev->desc_pool = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkCreateDescriptorPool(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                dev->device, 1,
                                VNCS_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3,
                                dev->desc_pool);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkCreateDescriptorPool(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkCreateDescriptorPool_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != dev->desc_pool) {
        VENUS_FAIL("vkCreateDescriptorPool", ret);
    }

    dev->desc_set = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkAllocateDescriptorSets(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                  dev->device, dev->desc_pool,
                                  &dev->set_layout, &dev->desc_set, 1);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkAllocateDescriptorSets(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_handle_array_reply(&d, VNCS_CMD_vkAllocateDescriptorSets_EXT,
                                  got_ids, 1);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got_ids[0] != dev->desc_set) {
        VENUS_FAIL("vkAllocateDescriptorSets", ret);
    }

    /* ---- command pool + buffer ---- */
    dev->cmd_pool = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkCreateCommandPool(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                             dev->device, dev->queue_family_index,
                             dev->cmd_pool);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkCreateCommandPool(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkCreateCommandPool_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != dev->cmd_pool) {
        VENUS_FAIL("vkCreateCommandPool", ret);
    }

    dev->cmd_buf = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkAllocateCommandBuffers(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                  dev->device, dev->cmd_pool,
                                  &dev->cmd_buf, 1);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkAllocateCommandBuffers(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_handle_array_reply(&d, VNCS_CMD_vkAllocateCommandBuffers_EXT,
                                  got_ids, 1);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got_ids[0] != dev->cmd_buf) {
        VENUS_FAIL("vkAllocateCommandBuffers", ret);
    }

    /* ---- fence (unsignaled; reset before every submit) ---- */
    dev->fence = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkCreateFence(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                       dev->device, false, dev->fence);
    if (venus_batch_end(t) != 0) {
        VENUS_FAIL("vkCreateFence(submit)", -1);
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkCreateFence_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != dev->fence) {
        VENUS_FAIL("vkCreateFence", ret);
    }

    dev->ready = true;
    console_printf("[Venus] Vulkan device up: \"%s\" (vendor 0x%x), %u "
                   "pipelines, queue family %u\n",
                   dev->device_name, dev->vendor_id, count,
                   dev->queue_family_index);
    return 0;
}

/* ============================================================================
 * Matmul dispatch
 *
 * Mirrors vk_run_matmul() in tools/host_test_vulkan.c: three storage buffers
 * (A, B, C), descriptor update, begin/bind/push/dispatch/end, submit with
 * fence, wait, copy C back.
 *
 * Buffers are per-call blob resources imported as VkDeviceMemory via
 * VkImportMemoryResourceInfoMESA and CPU-accessed through the shared-memory
 * window mapping.
 * ============================================================================ */

typedef struct venus_buf {
    uint64_t buffer;            /* VkBuffer id */
    uint64_t memory;            /* VkDeviceMemory id */
    uint32_t resource_id;       /* blob resource */
    volatile uint8_t *map;      /* guest mapping (hostmem window) */
    uint64_t map_size;          /* blob size */
    uint64_t size;              /* logical (padded) size */
} venus_buf_t;

/* Create a storage buffer of `size` bytes backed by an imported blob.
 * Follows Mesa's vn_buffer creation order: CreateBuffer ->
 * GetBufferMemoryRequirements -> blob sized to fit -> GetMemoryResource
 * PropertiesMESA -> AllocateMemory(import) -> BindBufferMemory. */
static int venus_create_storage_buffer(venus_vk_device_t *dev, uint64_t size,
                                       venus_buf_t *out)
{
    venus_transport_t *t = dev->t;
    memset(out, 0, sizeof(*out));
    out->size = size;

    vncs_encoder_t *e;
    vncs_decoder_t d;
    int32_t ret = 0;
    uint64_t got = 0;

    /* vkCreateBuffer */
    out->buffer = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkCreateBuffer(e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->device,
                        size, VNCS_BUFFER_USAGE_STORAGE_BUFFER, out->buffer);
    if (venus_batch_end(t) != 0) {
        return VIRTIO_GPU_ERR_IO;
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkCreateBuffer_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != out->buffer) {
        return VIRTIO_GPU_ERR_DEVICE;
    }

    /* vkGetBufferMemoryRequirements */
    e = venus_batch_begin(t, true);
    vncs_vkGetBufferMemoryRequirements(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                       dev->device, out->buffer);
    if (venus_batch_end(t) != 0) {
        return VIRTIO_GPU_ERR_IO;
    }
    uint64_t mem_size = 0, mem_align = 0;
    uint32_t mem_type_bits = 0;
    venus_reply_begin(t, &d);
    vncr_vkGetBufferMemoryRequirements_reply(&d, &mem_size, &mem_align,
                                             &mem_type_bits);
    if (d.fatal) {
        return VIRTIO_GPU_ERR_DEVICE;
    }

    /* blob resource sized to fit both the logical size and the driver
     * requirement (allocation size == blob size for the import) */
    uint64_t blob_size = venus_round_page(size > mem_size ? size : mem_size);
    out->resource_id = g_next_resource++;
    if (virtio_gpu_resource_create_blob(out->resource_id,
                                        VIRTIO_GPU_BLOB_MEM_GUEST_VRAM,
                                        VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                                        out->buffer, blob_size) != 0) {
        /* renderer without guest VRAM: fall back to host-3D memory */
        if (virtio_gpu_resource_create_blob(out->resource_id,
                                            VIRTIO_GPU_BLOB_MEM_HOST3D,
                                            VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                                            out->buffer, blob_size) != 0) {
            return VIRTIO_GPU_ERR_DEVICE;
        }
    }
    out->map_size = blob_size;

    /* vkGetMemoryResourcePropertiesMESA: which memory types can import it */
    e = venus_batch_begin(t, true);
    vncs_vkGetMemoryResourcePropertiesMESA(e, VNCS_COMMAND_GENERATE_REPLY_BIT,
                                           dev->device, out->resource_id);
    if (venus_batch_end(t) != 0) {
        return VIRTIO_GPU_ERR_IO;
    }
    uint32_t res_type_bits = 0;
    venus_reply_begin(t, &d);
    ret = vncr_vkGetMemoryResourcePropertiesMESA_reply(&d, &res_type_bits);
    if (d.fatal || ret != VNCS_VK_SUCCESS) {
        return VIRTIO_GPU_ERR_DEVICE;
    }

    /* memory type: host-visible+coherent, allowed by buffer and resource */
    uint32_t ok = dev->host_visible_coherent_types & mem_type_bits &
                  res_type_bits;
    if (!ok) {
        return VIRTIO_GPU_ERR_UNSUPPORTED;
    }
    uint32_t type_index = 0;
    while (!((ok >> type_index) & 1)) {
        type_index++;
    }

    /* vkAllocateMemory with VkImportMemoryResourceInfoMESA on pNext */
    out->memory = venus_new_handle();
    e = venus_batch_begin(t, true);
    vncs_vkAllocateMemory(e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->device,
                          blob_size, type_index, out->resource_id,
                          out->memory);
    if (venus_batch_end(t) != 0) {
        return VIRTIO_GPU_ERR_IO;
    }
    venus_reply_begin(t, &d);
    ret = vncr_vkAllocateMemory_reply(&d, &got);
    if (d.fatal || ret != VNCS_VK_SUCCESS || got != out->memory) {
        return VIRTIO_GPU_ERR_DEVICE;
    }

    /* vkBindBufferMemory */
    e = venus_batch_begin(t, true);
    vncs_vkBindBufferMemory(e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->device,
                            out->buffer, out->memory, 0);
    ret = venus_check_ret(t, VNCS_CMD_vkBindBufferMemory_EXT);
    if (ret != 0) {
        return VIRTIO_GPU_ERR_DEVICE;
    }

    /* map for guest CPU access */
    out->map = venus_map_blob(out->resource_id, blob_size);
    if (!out->map) {
        return VIRTIO_GPU_ERR_DEVICE;
    }
    return 0;
}

static void venus_destroy_storage_buffer(venus_vk_device_t *dev,
                                         venus_buf_t *buf)
{
    if (!buf->buffer) {
        return;
    }
    venus_transport_t *t = dev->t;

    /* host objects (fire-and-forget; renderer executes in ring order) */
    vncs_encoder_t *e = venus_batch_begin(t, false);
    vncs_vkDestroyBuffer(e, 0, dev->device, buf->buffer);
    vncs_vkFreeMemory(e, 0, dev->device, buf->memory);
    venus_batch_end(t);

    if (buf->map) {
        virtio_gpu_resource_unmap_blob(buf->resource_id);
    }
    virtio_gpu_resource_unref(buf->resource_id);
    buf->buffer = 0;
}

int venus_vk_matmul(venus_vk_device_t *dev, uint32_t pipeline_idx,
                    const void *A, uint64_t a_bytes,
                    const void *B, uint64_t b_bytes,
                    float *C, uint32_t m, uint32_t k, uint32_t n)
{
    if (!dev || !dev->ready || pipeline_idx >= dev->shader_count ||
        !A || !B || !C || !m || !k || !n) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    venus_transport_t *t = dev->t;

    /* shaders read whole u32 words: pad payload sizes to 4-byte multiples */
    uint64_t asz = (a_bytes + 3) & ~3ULL;
    uint64_t bsz = (b_bytes + 3) & ~3ULL;
    uint64_t csz = (uint64_t)m * n * 4;
    if (asz == 0 || bsz == 0 || csz == 0 ||
        csz > (64ULL << 20) || asz > (64ULL << 20) || bsz > (64ULL << 20)) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    venus_buf_t bufs[3];
    memset(bufs, 0, sizeof(bufs));

    uint64_t sizes[3] = { asz, bsz, csz };
    for (int i = 0; i < 3; i++) {
        int rc = venus_create_storage_buffer(dev, sizes[i], &bufs[i]);
        if (rc != 0) {
            console_printf("[Venus] buffer %d creation failed: %d\n", i, rc);
            for (int j = 0; j < i; j++) {
                venus_destroy_storage_buffer(dev, &bufs[j]);
            }
            return rc;
        }
    }

    /* upload A and B (coherent host-visible memory: no flush needed) */
    memcpy((void *)bufs[0].map, A, a_bytes);
    if (asz > a_bytes) {
        memset((void *)(bufs[0].map + a_bytes), 0, asz - a_bytes);
    }
    memcpy((void *)bufs[1].map, B, b_bytes);
    if (bsz > b_bytes) {
        memset((void *)(bufs[1].map + b_bytes), 0, bsz - b_bytes);
    }

    /* ---- descriptor update (no reply; ring order guarantees execution
     * before the submit that follows) ---- */
    vncs_desc_buffer_write_t writes[3] = {
        { dev->desc_set, 0, bufs[0].buffer, 0, VNCS_WHOLE_SIZE },
        { dev->desc_set, 1, bufs[1].buffer, 0, VNCS_WHOLE_SIZE },
        { dev->desc_set, 2, bufs[2].buffer, 0, VNCS_WHOLE_SIZE },
    };
    vncs_encoder_t *e = venus_batch_begin(t, false);
    vncs_vkUpdateDescriptorSets(e, 0, dev->device, writes, 3);
    if (venus_batch_end(t) != 0) {
        goto fail;
    }

    /* ---- fence reset ---- */
    e = venus_batch_begin(t, true);
    vncs_vkResetFences(e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->device,
                       &dev->fence, 1);
    if (venus_check_ret(t, VNCS_CMD_vkResetFences_EXT) != 0) {
        goto fail;
    }

    /* ---- record the command buffer ---- */
    uint32_t groups = ((uint32_t)((uint64_t)m * n) + 63) / 64;
    uint32_t pc[3] = { m, k, n };
    e = venus_batch_begin(t, false);
    vncs_vkBeginCommandBuffer(e, 0, dev->cmd_buf);
    vncs_vkCmdBindPipeline(e, 0, dev->cmd_buf, VNCS_PIPELINE_BIND_POINT_COMPUTE,
                           dev->pipelines[pipeline_idx]);
    vncs_vkCmdBindDescriptorSets(e, 0, dev->cmd_buf,
                                 VNCS_PIPELINE_BIND_POINT_COMPUTE,
                                 dev->pipeline_layout, 0, &dev->desc_set, 1);
    vncs_vkCmdPushConstants(e, 0, dev->cmd_buf, dev->pipeline_layout,
                            VNCS_SHADER_STAGE_COMPUTE_BIT, 0, pc, 12);
    vncs_vkCmdDispatch(e, 0, dev->cmd_buf, groups, 1, 1);
    vncs_vkEndCommandBuffer(e, 0, dev->cmd_buf);
    if (venus_batch_end(t) != 0) {
        goto fail;
    }

    /* ---- submit with fence ---- */
    e = venus_batch_begin(t, true);
    vncs_vkQueueSubmit(e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->queue,
                       &dev->cmd_buf, 1, dev->fence);
    if (venus_check_ret(t, VNCS_CMD_vkQueueSubmit_EXT) != 0) {
        goto fail;
    }

    /* ---- wait for completion (renderer answers once the fence signals) ---- */
    e = venus_batch_begin(t, true);
    vncs_vkWaitForFences(e, VNCS_COMMAND_GENERATE_REPLY_BIT, dev->device,
                         &dev->fence, 1, true, 0xFFFFFFFFFFFFFFFFULL);
    if (venus_check_ret(t, VNCS_CMD_vkWaitForFences_EXT) != 0) {
        console_printf("[Venus] vkWaitForFences failed\n");
        goto fail;
    }

    /* copy C back */
    memcpy(C, (const void *)bufs[2].map, csz);

    for (int i = 0; i < 3; i++) {
        venus_destroy_storage_buffer(dev, &bufs[i]);
    }
    return 0;

fail:
    for (int i = 0; i < 3; i++) {
        venus_destroy_storage_buffer(dev, &bufs[i]);
    }
    return VIRTIO_GPU_ERR_DEVICE;
}
