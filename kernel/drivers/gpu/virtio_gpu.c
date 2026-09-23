/* EMBODIOS VirtIO GPU Driver
 *
 * virtio-gpu control-plane driver for the Volta v0.6.0 Vulkan transport
 * workstream (WS-C, branch volta-vktransport).
 *
 * Validated under QEMU 7.2 with `-device virtio-gpu-pci`. QEMU exposes
 * virtio-gpu as a MODERN-ONLY device (1af4:1050, no I/O BAR), so this
 * driver implements the modern virtio PCI transport (vendor capabilities
 * -> MMIO common/notify/device-config regions, per virtio spec 1.2 4.1.4),
 * with the legacy I/O-port transport (transitional 1af4:1010) kept as a
 * fallback for hypervisors that still attach transitional GPUs.
 *
 * Probe sequence per virtio spec 1.2, 5.7.6:
 *   reset -> ACKNOWLEDGE -> DRIVER -> feature negotiation -> FEATURES_OK
 *   -> controlq/cursorq setup -> DRIVER_OK -> GET_DISPLAY_INFO
 *   -> GET_CAPSET_INFO enumeration -> Venus hand-off (venus.c)
 *
 * MMIO regions are accessed through identity-mapped physical addresses,
 * the same approach as the NVMe driver in this tree.
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

/* Debug output (uncomment to enable) */
/* #define VIRTIO_GPU_DEBUG 1 */

#include <embodios/virtio_gpu.h>
#include <embodios/venus.h>
#include <embodios/pci.h>
#include <arch/x86_64/paging.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/dma.h>
#include <embodios/kernel.h>

/* ============================================================================
 * Module State
 * ============================================================================ */

static virtio_gpu_dev_t g_gpu;
static bool g_driver_registered = false;

/* Static coherent buffers for the synchronous control path.
 * Requests/responses are small (largest: display info, 408 bytes) and all
 * control traffic is synchronous, so two page-aligned bounce buffers and a
 * lightweight lock-free single-user discipline are sufficient. */
static uint8_t g_ctrl_req[512]  __attribute__((aligned(4096)));
static uint8_t g_ctrl_resp[1024] __attribute__((aligned(4096)));

/* Coherent staging buffer for GET_CAPSET payloads and SUBMIT_3D streams */
#define VIRTIO_GPU_STAGE_SIZE   4096
static uint8_t g_stage[VIRTIO_GPU_STAGE_SIZE] __attribute__((aligned(4096)));

/* ============================================================================
 * MMIO access helpers (modern transport)
 * ============================================================================ */

static inline uint8_t vg_read8(volatile void *base, uint32_t off)
{
    return *(volatile uint8_t *)((uintptr_t)base + off);
}

static inline uint16_t vg_read16(volatile void *base, uint32_t off)
{
    return *(volatile uint16_t *)((uintptr_t)base + off);
}

static inline uint32_t vg_read32(volatile void *base, uint32_t off)
{
    return *(volatile uint32_t *)((uintptr_t)base + off);
}

static inline void vg_write8(volatile void *base, uint32_t off, uint8_t v)
{
    *(volatile uint8_t *)((uintptr_t)base + off) = v;
}

static inline void vg_write16(volatile void *base, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)((uintptr_t)base + off) = v;
}

static inline void vg_write32(volatile void *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)((uintptr_t)base + off) = v;
}

/* ============================================================================
 * Transport abstraction: status / features / notify
 * ============================================================================ */

static void vg_reset(virtio_gpu_dev_t *dev)
{
    if (dev->modern) {
        vg_write8((void *)dev->common_cfg, 0x14 /* device_status */, 0);
        /* Poll until the device reports the reset completed */
        int timeout = 100000;
        while (vg_read8((void *)dev->common_cfg, 0x14) != 0 && timeout-- > 0);
    } else {
        virtio_reset(&dev->base);
    }
}

static void vg_set_status(virtio_gpu_dev_t *dev, uint8_t status)
{
    if (dev->modern) {
        dev->base.status |= status;
        vg_write8((void *)dev->common_cfg, 0x14, dev->base.status);
    } else {
        virtio_set_status(&dev->base, status);
    }
}

static uint8_t vg_get_status(virtio_gpu_dev_t *dev)
{
    if (dev->modern) {
        return vg_read8((void *)dev->common_cfg, 0x14);
    }
    return virtio_get_status(&dev->base);
}

static uint32_t vg_get_features32(virtio_gpu_dev_t *dev, uint32_t select)
{
    if (dev->modern) {
        vg_write32((void *)dev->common_cfg, 0x00, select);
        return vg_read32((void *)dev->common_cfg, 0x04);
    }
    /* Legacy transport exposes only the low 32 feature bits */
    return select == 0 ? virtio_get_features(&dev->base) : 0;
}

static void vg_set_features32(virtio_gpu_dev_t *dev, uint32_t select,
                              uint32_t value)
{
    if (dev->modern) {
        vg_write32((void *)dev->common_cfg, 0x08, select);
        vg_write32((void *)dev->common_cfg, 0x0c, value);
    } else if (select == 0) {
        virtio_set_features(&dev->base, value);
    }
}

/**
 * Push a descriptor chain head onto the avail ring and notify the device.
 * Local copy of virtqueue_kick() extended for the modern notify MMIO
 * register (the shared helper only knows the legacy I/O-port notifier).
 */
static void vg_kick(virtio_gpu_dev_t *dev, virtqueue_t *vq, uint16_t head)
{
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = head;

    wmb();
    vq->avail->idx++;
    mb();

    if (dev->modern) {
        uint16_t off = dev->notify_off[vq->index];
        vg_write16((void *)dev->notify_base,
                   (uint32_t)off * dev->notify_off_multiplier,
                   vq->index);
    } else {
        outw(vq->iobase + VIRTIO_PCI_QUEUE_NOTIFY, vq->index);
    }
}

/* ============================================================================
 * Modern transport: capability discovery
 * ============================================================================ */

/**
 * Parse the PCI capability list for virtio vendor capabilities and map
 * the common/notify/device-config regions (identity-mapped MMIO).
 */
static int virtio_gpu_parse_modern_caps(virtio_gpu_dev_t *dev,
                                        pci_device_t *pci_dev)
{
    uint8_t cap_off = pci_config_read8(pci_dev->addr, PCI_CAPABILITIES);
    bool have_common = false, have_notify = false, have_devcfg = false;

    while (cap_off && cap_off != 0xFF) {
        uint8_t cap_id = pci_config_read8(pci_dev->addr, cap_off);
        uint8_t cap_next = pci_config_read8(pci_dev->addr, cap_off + 1);
        uint8_t cap_len = pci_config_read8(pci_dev->addr, cap_off + 2);

        if (cap_id == 0x09 && cap_len >= 16) {  /* vendor-specific */
            uint8_t cfg_type = pci_config_read8(pci_dev->addr, cap_off + 3);
            uint8_t bar = pci_config_read8(pci_dev->addr, cap_off + 4);

            /* Shared-memory window (virtio_pci_cap64): 64-bit length/offset.
             * This is the blob host-memory region — it is only mapped on
             * demand by RESOURCE_MAP_BLOB users (can be hundreds of MB), so
             * just record it here. */
            if (cfg_type == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG && cap_len >= 24) {
                uint64_t length = pci_config_read32(pci_dev->addr, cap_off + 8);
                uint64_t offset = pci_config_read32(pci_dev->addr, cap_off + 12);
                length |= (uint64_t)pci_config_read32(pci_dev->addr,
                                                      cap_off + 16) << 32;
                offset |= (uint64_t)pci_config_read32(pci_dev->addr,
                                                      cap_off + 20) << 32;
                uint64_t base = pci_bar_address(pci_dev, bar);
                if (base && !(pci_dev->bar[bar] & PCI_BAR_IO) && length) {
                    dev->hostmem_base = base + offset;
                    dev->hostmem_size = length;
                }
                cap_off = cap_next;
                continue;
            }

            uint32_t offset = pci_config_read32(pci_dev->addr, cap_off + 8);
            uint32_t length = pci_config_read32(pci_dev->addr, cap_off + 12);
            uint64_t base = pci_bar_address(pci_dev, bar);

            if (base == 0 || (pci_dev->bar[bar] & PCI_BAR_IO)) {
                cap_off = cap_next;
                continue;
            }

            /* The PCI MMIO window (e.g. 0xFE000000) is not covered by the
             * kernel's identity map — map the capability region before use
             * (same arch_identity_map_mmio() path as hpet/lapic/e1000e). */
            uint64_t phys = base + offset;
            if (!arch_identity_map_mmio(phys, length)) {
                console_printf("[VirtIO-GPU] failed to map MMIO region "
                               "0x%llx+0x%x\n",
                               (unsigned long long)phys, length);
                cap_off = cap_next;
                continue;
            }

            volatile uint8_t *region =
                (volatile uint8_t *)(uintptr_t)(base + offset);

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                if (length >= sizeof(struct virtio_pci_common_cfg)) {
                    dev->common_cfg =
                        (volatile struct virtio_pci_common_cfg *)region;
                    have_common = true;
                }
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                if (cap_len >= 20) {
                    dev->notify_off_multiplier =
                        pci_config_read32(pci_dev->addr, cap_off + 16);
                } else {
                    dev->notify_off_multiplier = 2;  /* spec default */
                }
                dev->notify_base = region;
                have_notify = true;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                dev->device_cfg = region;
                have_devcfg = true;
                break;
            default:
                break;  /* ISR / PCI_CFG capabilities unused (polling) */
            }
        }

        cap_off = cap_next;
    }

    if (!have_common || !have_notify || !have_devcfg) {
        console_printf("[VirtIO-GPU] missing virtio caps: common=%d "
                       "notify=%d devcfg=%d\n",
                       have_common, have_notify, have_devcfg);
        return VIRTIO_GPU_ERR_UNSUPPORTED;
    }

    return VIRTIO_GPU_OK;
}

/**
 * Set up one virtqueue on the modern transport (split ring addresses are
 * programmed as separate desc/driver/device 64-bit fields).
 */
static int virtio_gpu_setup_queue_modern(virtio_gpu_dev_t *dev,
                                         uint16_t index, virtqueue_t *vq,
                                         const char *name)
{
    volatile struct virtio_pci_common_cfg *cfg = dev->common_cfg;

    vg_write16((void *)cfg, 0x16, index);               /* queue_select */
    uint16_t queue_size = vg_read16((void *)cfg, 0x18); /* queue_size */
    if (queue_size == 0) {
        console_printf("[VirtIO-GPU] %s queue size is 0\n", name);
        return VIRTIO_GPU_ERR_INVALID;
    }

    int ret = virtqueue_alloc(vq, queue_size, 0 /* no iobase */, index);
    if (ret != VIRTIO_OK) {
        console_printf("[VirtIO-GPU] %s queue alloc failed: %d\n", name, ret);
        return VIRTIO_GPU_ERR_NO_MEMORY;
    }

    /* Program ring addresses */
    vg_write32((void *)cfg, 0x20, (uint32_t)(vq->desc_dma & 0xFFFFFFFF));
    vg_write32((void *)cfg, 0x24, (uint32_t)(vq->desc_dma >> 32));
    vg_write32((void *)cfg, 0x28, (uint32_t)(vq->avail_dma & 0xFFFFFFFF));
    vg_write32((void *)cfg, 0x2c, (uint32_t)(vq->avail_dma >> 32));
    vg_write32((void *)cfg, 0x30, (uint32_t)(vq->used_dma & 0xFFFFFFFF));
    vg_write32((void *)cfg, 0x34, (uint32_t)(vq->used_dma >> 32));

    dev->notify_off[index] = vg_read16((void *)cfg, 0x1e); /* notify_off */

    vg_write16((void *)cfg, 0x1c, 1);                   /* queue_enable */

    console_printf("[VirtIO-GPU] %s: %u descriptors (modern)\n",
                   name, queue_size);
    return VIRTIO_GPU_OK;
}

/**
 * Set up one virtqueue on the legacy transport.
 */
static int virtio_gpu_setup_queue_legacy(virtio_gpu_dev_t *dev,
                                         uint16_t index, virtqueue_t *vq,
                                         const char *name)
{
    virtio_select_queue(&dev->base, index);
    uint16_t queue_size = virtio_get_queue_size(&dev->base);
    if (queue_size == 0) {
        console_printf("[VirtIO-GPU] %s queue size is 0\n", name);
        return VIRTIO_GPU_ERR_INVALID;
    }

    int ret = virtqueue_alloc(vq, queue_size, dev->base.iobase, index);
    if (ret != VIRTIO_OK) {
        console_printf("[VirtIO-GPU] %s queue alloc failed: %d\n", name, ret);
        return VIRTIO_GPU_ERR_NO_MEMORY;
    }

    virtio_set_queue_pfn(&dev->base, (uint32_t)(vq->desc_dma >> 12));
    console_printf("[VirtIO-GPU] %s: %u descriptors (legacy)\n",
                   name, queue_size);
    return VIRTIO_GPU_OK;
}

/* ============================================================================
 * Device configuration
 * ============================================================================ */

static void virtio_gpu_read_config(virtio_gpu_dev_t *dev)
{
    if (dev->modern) {
        dev->num_scanouts = vg_read32((void *)dev->device_cfg,
                                      VIRTIO_GPU_CFG_NUM_SCANOUTS);
        dev->num_capsets = vg_read32((void *)dev->device_cfg,
                                     VIRTIO_GPU_CFG_NUM_CAPSETS);
    } else {
        uint16_t iobase = dev->base.iobase;
        dev->num_scanouts =
            inl(iobase + VIRTIO_PCI_CONFIG + VIRTIO_GPU_CFG_NUM_SCANOUTS);
        dev->num_capsets =
            inl(iobase + VIRTIO_PCI_CONFIG + VIRTIO_GPU_CFG_NUM_CAPSETS);
    }
    if (dev->num_scanouts > VIRTIO_GPU_MAX_SCANOUTS) {
        dev->num_scanouts = VIRTIO_GPU_MAX_SCANOUTS;
    }

#ifdef VIRTIO_GPU_DEBUG
    if (dev->modern) {
        console_printf("[VirtIO-GPU] devcfg raw:");
        for (int i = 0; i < 16; i += 4) {
            console_printf(" %08x", vg_read32((void *)dev->device_cfg, i));
        }
        console_printf("\n");
    }
#endif
}

/* ============================================================================
 * Control queue round trips
 * ============================================================================ */

/**
 * Submit a request/response pair on the control queue and poll for
 * completion. All requests flow through the coherent bounce buffers so
 * the device always DMAs from/to known-safe memory.
 *
 * Descriptor chain: [req: OUT] [resp: WRITE] (+ optional [data]).
 */
static int virtio_gpu_ctrl_xfer(virtio_gpu_dev_t *dev,
                                const void *req, uint32_t req_len,
                                void *resp, uint32_t resp_len,
                                const void *extra, uint32_t extra_len,
                                bool extra_write)
{
    virtqueue_t *vq = &dev->controlq;
    uint32_t need = extra_len ? 3u : 2u;

    if (req_len > sizeof(g_ctrl_req) || resp_len > sizeof(g_ctrl_resp) ||
        extra_len > sizeof(g_stage)) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    if (vq->free_count < need) {
        return VIRTIO_GPU_ERR_FULL;
    }

    /* Stage request/response into coherent buffers */
    memset(g_ctrl_req, 0, sizeof(g_ctrl_req));
    memcpy(g_ctrl_req, req, req_len);
    memset(g_ctrl_resp, 0, resp_len ? resp_len : 1);
    if (extra && !extra_write) {
        memcpy(g_stage, extra, extra_len);
    }

    uint16_t d_req = virtqueue_alloc_desc(vq);
    uint16_t d_resp = virtqueue_alloc_desc(vq);
    uint16_t d_extra = 0xFFFF;
    if (extra_len) {
        d_extra = virtqueue_alloc_desc(vq);
    }
    if (d_req == 0xFFFF || d_resp == 0xFFFF || (extra_len && d_extra == 0xFFFF)) {
        if (d_req != 0xFFFF) virtqueue_free_desc(vq, d_req);
        if (d_resp != 0xFFFF) virtqueue_free_desc(vq, d_resp);
        if (d_extra != 0xFFFF) virtqueue_free_desc(vq, d_extra);
        return VIRTIO_GPU_ERR_FULL;
    }

    /* Request descriptor (device reads) */
    vq->desc[d_req].addr = (uint64_t)(uintptr_t)g_ctrl_req;
    vq->desc[d_req].len = req_len;
    vq->desc[d_req].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[d_req].next = d_resp;

    /* Response descriptor (device writes); zero-length responses are not
     * valid in virtio-gpu — OK_NODATA still carries a ctrl header. */
    uint32_t rlen = resp_len ? resp_len : sizeof(struct virtio_gpu_ctrl_hdr);
    vq->desc[d_resp].addr = (uint64_t)(uintptr_t)g_ctrl_resp;
    vq->desc[d_resp].len = rlen;
    vq->desc[d_resp].flags = VIRTQ_DESC_F_WRITE;
    if (extra_len) {
        vq->desc[d_resp].flags |= VIRTQ_DESC_F_NEXT;
        vq->desc[d_resp].next = d_extra;

        vq->desc[d_extra].addr = (uint64_t)(uintptr_t)g_stage;
        vq->desc[d_extra].len = extra_len;
        vq->desc[d_extra].flags = extra_write ? VIRTQ_DESC_F_WRITE : 0;
        vq->desc[d_extra].next = 0;
    } else {
        vq->desc[d_resp].next = 0;
    }

    vg_kick(dev, vq, d_req);

    /* Poll for completion with a generous timeout (~seconds scale) */
    int timeout = 4000000;
    while (!virtqueue_has_used(vq) && timeout-- > 0) {
        for (volatile int i = 0; i < 100; i++);
    }

    if (timeout <= 0) {
        console_printf("[VirtIO-GPU] control queue timeout\n");
        virtqueue_free_desc(vq, d_req);
        virtqueue_free_desc(vq, d_resp);
        if (d_extra != 0xFFFF) virtqueue_free_desc(vq, d_extra);
        return VIRTIO_GPU_ERR_TIMEOUT;
    }

    uint32_t used_len = 0;
    (void)virtqueue_get_used(vq, &used_len);

    virtqueue_free_desc(vq, d_req);
    virtqueue_free_desc(vq, d_resp);
    if (d_extra != 0xFFFF) virtqueue_free_desc(vq, d_extra);

    /* Copy response back to the caller's buffer */
    if (resp && resp_len) {
        memcpy(resp, g_ctrl_resp, resp_len);
    }
    if (extra && extra_write) {
        memcpy((void *)extra, g_stage, extra_len);
    }

    /* Interpret the response type */
    const struct virtio_gpu_ctrl_hdr *rhdr =
        (const struct virtio_gpu_ctrl_hdr *)g_ctrl_resp;
    if (rhdr->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        return VIRTIO_GPU_ERR_DEVICE;
    }

    return VIRTIO_GPU_OK;
}

/**
 * Issue VIRTIO_GPU_CMD_GET_DISPLAY_INFO and store the result.
 */
static int virtio_gpu_query_display_info(virtio_gpu_dev_t *dev)
{
    struct virtio_gpu_ctrl_hdr cmd = {0};
    struct virtio_gpu_resp_display_info info;

    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    cmd.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd.fence_id = ++dev->fence_seq;

    int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                   &info, sizeof(info), NULL, 0, false);
    if (ret != VIRTIO_GPU_OK) {
        return ret;
    }
    if (info.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        return VIRTIO_GPU_ERR_DEVICE;
    }

    dev->displays_enabled = 0;
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        dev->pmodes[i] = info.pmodes[i];
        if (info.pmodes[i].enabled) {
            dev->displays_enabled++;
        }
    }
    return VIRTIO_GPU_OK;
}

/**
 * Enumerate capsets via VIRTIO_GPU_CMD_GET_CAPSET_INFO.
 */
static int virtio_gpu_enumerate_capsets(virtio_gpu_dev_t *dev)
{
    uint32_t n = dev->num_capsets;
    if (n > VIRTIO_GPU_MAX_CAPSETS) {
        n = VIRTIO_GPU_MAX_CAPSETS;
    }

    for (uint32_t i = 0; i < n; i++) {
        struct virtio_gpu_get_capset_info cmd = {0};
        struct virtio_gpu_resp_capset_info info;

        cmd.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        cmd.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        cmd.hdr.fence_id = ++dev->fence_seq;
        cmd.capset_index = i;

        int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                       &info, sizeof(info), NULL, 0, false);
        if (ret != VIRTIO_GPU_OK ||
            info.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO ||
            info.capset_id == 0) {
            console_printf("[VirtIO-GPU] capset %u: unavailable (ret=%d)\n",
                           i, ret);
            continue;
        }

        dev->capsets[dev->capsets_valid].id = info.capset_id;
        dev->capsets[dev->capsets_valid].max_version = info.capset_max_version;
        dev->capsets[dev->capsets_valid].max_size = info.capset_max_size;
        dev->capsets_valid++;
    }

    return VIRTIO_GPU_OK;
}

/* ============================================================================
 * PCI Probe
 * ============================================================================ */

static int virtio_gpu_probe(pci_device_t *pci_dev)
{
    virtio_gpu_dev_t *dev = &g_gpu;

    console_printf("[VirtIO-GPU] Probing device %04x:%04x at %02x:%02x.%u\n",
                   pci_dev->vendor_id, pci_dev->device_id,
                   pci_dev->addr.bus, pci_dev->addr.device,
                   pci_dev->addr.function);

    if (dev->initialized) {
        console_printf("[VirtIO-GPU] Single device supported; ignoring extra\n");
        return VIRTIO_GPU_ERR_FULL;
    }

    memset(dev, 0, sizeof(*dev));
    dev->base.pci_dev = pci_dev;

    /* Transport selection: QEMU's virtio-gpu-pci is modern-only (1af4:1050,
     * MMIO + vendor capabilities). A transitional device (1af4:1010) has an
     * I/O BAR and uses the legacy port transport like virtio_net/blk. */
    uint32_t bar0 = pci_dev->bar[0];
    bool have_io_bar = (bar0 & PCI_BAR_IO) != 0;

    if (have_io_bar) {
        dev->modern = false;
        dev->base.iobase = (uint16_t)(bar0 & PCI_BAR_IO_MASK);
        pci_enable_bus_master(pci_dev);
        pci_enable_io(pci_dev);
        console_printf("[VirtIO-GPU] legacy transport, I/O base: 0x%x\n",
                       dev->base.iobase);
    } else {
        dev->modern = true;
        pci_enable_bus_master(pci_dev);
        pci_enable_memory(pci_dev);
        if (virtio_gpu_parse_modern_caps(dev, pci_dev) != VIRTIO_GPU_OK) {
            console_printf("[VirtIO-GPU] modern capability discovery failed\n");
            return VIRTIO_GPU_ERR_UNSUPPORTED;
        }
        console_printf("[VirtIO-GPU] modern transport, common_cfg=%p "
                       "notify=%p devcfg=%p\n",
                       (void *)dev->common_cfg, (void *)dev->notify_base,
                       (void *)dev->device_cfg);
    }

    /* VirtIO initialization sequence (virtio spec 3.1.1) */
    vg_reset(dev);
    vg_set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);
    vg_set_status(dev, VIRTIO_STATUS_DRIVER);

    /* Feature negotiation. VIRGL is intentionally not requested (no GL
     * pipeline in the guest); we accept the Vulkan-relevant bits when the
     * host offers them. Modern devices additionally require
     * VIRTIO_F_VERSION_1 (bit 32). */
    uint32_t host_lo = vg_get_features32(dev, 0);
    uint32_t host_hi = vg_get_features32(dev, 1);
    uint32_t wanted_lo = VIRTIO_GPU_F_EDID_MASK |
                         VIRTIO_GPU_F_RESOURCE_BLOB_MASK |
                         VIRTIO_GPU_F_CONTEXT_INIT_MASK |
                         VIRTIO_GPU_F_VENUS_MASK;
    uint32_t nego_lo = host_lo & wanted_lo;
    uint32_t nego_hi = 0;
    if (dev->modern) {
        nego_hi = host_hi & 0x1;  /* VIRTIO_F_VERSION_1 */
        if (!(host_hi & 0x1)) {
            console_printf("[VirtIO-GPU] modern device without VERSION_1?\n");
            vg_set_status(dev, VIRTIO_STATUS_FAILED);
            return VIRTIO_GPU_ERR_UNSUPPORTED;
        }
    }

    vg_set_features32(dev, 0, nego_lo);
    vg_set_features32(dev, 1, nego_hi);
    dev->features64 = ((uint64_t)nego_hi << 32) | nego_lo;
    dev->base.features = nego_lo;

    /* FEATURES_OK handshake (mandatory on the modern transport) */
    if (dev->modern) {
        vg_set_status(dev, VIRTIO_STATUS_FEATURES_OK);
        if (!(vg_get_status(dev) & VIRTIO_STATUS_FEATURES_OK)) {
            console_printf("[VirtIO-GPU] host rejected features\n");
            vg_set_status(dev, VIRTIO_STATUS_FAILED);
            return VIRTIO_GPU_ERR_UNSUPPORTED;
        }
    }

    console_printf("[VirtIO-GPU] host features: 0x%08x%08x, "
                   "negotiated: 0x%08x%08x\n",
                   host_hi, host_lo, nego_hi, nego_lo);

    /* Queues: 0 = controlq, 1 = cursorq (virtio spec 5.7.5) */
    int qret;
    if (dev->modern) {
        qret = virtio_gpu_setup_queue_modern(dev, 0, &dev->controlq, "controlq");
        if (qret == VIRTIO_GPU_OK) {
            qret = virtio_gpu_setup_queue_modern(dev, 1, &dev->cursorq, "cursorq");
        }
    } else {
        qret = virtio_gpu_setup_queue_legacy(dev, 0, &dev->controlq, "controlq");
        if (qret == VIRTIO_GPU_OK) {
            qret = virtio_gpu_setup_queue_legacy(dev, 1, &dev->cursorq, "cursorq");
        }
    }
    if (qret != VIRTIO_GPU_OK) {
        vg_set_status(dev, VIRTIO_STATUS_FAILED);
        return qret;
    }

    vg_set_status(dev, VIRTIO_STATUS_DRIVER_OK);

    /* Device configuration */
    virtio_gpu_read_config(dev);
    console_printf("[VirtIO-GPU] scanouts: %u, capsets: %u\n",
                   dev->num_scanouts, dev->num_capsets);

    /* GET_DISPLAY_INFO */
    if (virtio_gpu_query_display_info(dev) == VIRTIO_GPU_OK) {
        console_printf("[VirtIO-GPU] display info: %u scanout(s) enabled\n",
                       dev->displays_enabled);
        for (uint32_t i = 0; i < dev->num_scanouts; i++) {
            if (dev->pmodes[i].enabled) {
                console_printf("[VirtIO-GPU]   scanout %u: %ux%u+%u+%u\n",
                               i, dev->pmodes[i].r.width,
                               dev->pmodes[i].r.height,
                               dev->pmodes[i].r.x, dev->pmodes[i].r.y);
            }
        }
    } else {
        console_printf("[VirtIO-GPU] GET_DISPLAY_INFO failed\n");
    }

    /* Capset enumeration */
    virtio_gpu_enumerate_capsets(dev);
    for (uint32_t i = 0; i < dev->capsets_valid; i++) {
        console_printf("[VirtIO-GPU] capset %u: id=%u (%s) max_version=%u max_size=%u\n",
                       i, dev->capsets[i].id,
                       dev->capsets[i].id == VIRTIO_GPU_CAPSET_VIRGL  ? "virgl" :
                       dev->capsets[i].id == VIRTIO_GPU_CAPSET_VIRGL2 ? "virgl2" :
                       dev->capsets[i].id == VIRTIO_GPU_CAPSET_VENUS  ? "venus" :
                       dev->capsets[i].id == VIRTIO_GPU_CAPSET_CROSS_DOMAIN ? "cross-domain" :
                       "unknown",
                       dev->capsets[i].max_version,
                       dev->capsets[i].max_size);
    }

    dev->initialized = true;
    console_printf("[VirtIO-GPU] Device initialized (features: %s%s%s)\n",
                   (nego_lo & VIRTIO_GPU_F_CONTEXT_INIT_MASK) ? "context-init " : "",
                   (nego_lo & VIRTIO_GPU_F_RESOURCE_BLOB_MASK) ? "blob " : "",
                   (nego_lo & VIRTIO_GPU_F_VENUS_MASK) ? "venus" : "no-venus");

    /* Hand off to the Venus protocol layer (Part 2). No-op with an honest
     * log line when the device has no Venus capset. */
    venus_transport_probe(dev);

    return VIRTIO_GPU_OK;
}

static void virtio_gpu_remove(pci_device_t *pci_dev)
{
    (void)pci_dev;
    if (g_gpu.initialized) {
        venus_transport_shutdown();
        vg_reset(&g_gpu);
        virtqueue_free(&g_gpu.controlq);
        virtqueue_free(&g_gpu.cursorq);
        g_gpu.initialized = false;
    }
}

/* PCI driver registrations: transitional and modern-only IDs both match;
 * probe() picks the transport based on the BAR type. */
static pci_driver_t virtio_gpu_driver_trans = {
    .name = "virtio-gpu",
    .vendor_id = VIRTIO_PCI_VENDOR,
    .device_id = VIRTIO_PCI_DEVICE_GPU_TRANS,
    .class_code = PCI_ANY_CLASS,
    .subclass = PCI_ANY_CLASS,
    .probe = virtio_gpu_probe,
    .remove = virtio_gpu_remove,
    .next = NULL,
};

static pci_driver_t virtio_gpu_driver_modern = {
    .name = "virtio-gpu-modern",
    .vendor_id = VIRTIO_PCI_VENDOR,
    .device_id = VIRTIO_PCI_DEVICE_GPU_MODERN,
    .class_code = PCI_ANY_CLASS,
    .subclass = PCI_ANY_CLASS,
    .probe = virtio_gpu_probe,
    .remove = virtio_gpu_remove,
    .next = NULL,
};

/* ============================================================================
 * Public API
 * ============================================================================ */

int virtio_gpu_init(void)
{
    if (g_driver_registered) {
        return VIRTIO_GPU_OK;
    }
    if (!pci_is_initialized()) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    int ret = pci_register_driver(&virtio_gpu_driver_trans);
    if (ret == PCI_OK) {
        ret = pci_register_driver(&virtio_gpu_driver_modern);
    }
    if (ret != PCI_OK) {
        console_printf("[VirtIO-GPU] PCI driver registration failed: %d\n", ret);
        return VIRTIO_GPU_ERR_IO;
    }

    g_driver_registered = true;

    if (!g_gpu.initialized) {
        console_printf("[VirtIO-GPU] No virtio-gpu device found "
                       "(use -device virtio-gpu-pci to attach one)\n");
    }

    return VIRTIO_GPU_OK;
}

bool virtio_gpu_is_ready(void)
{
    return g_gpu.initialized;
}

uint32_t virtio_gpu_features(void)
{
    return g_gpu.initialized ? g_gpu.base.features : 0;
}

const virtio_gpu_capset_t *virtio_gpu_find_capset(uint32_t capset_id,
                                                  uint32_t *out_index)
{
    if (!g_gpu.initialized) {
        return NULL;
    }
    for (uint32_t i = 0; i < g_gpu.capsets_valid; i++) {
        if (g_gpu.capsets[i].id == capset_id) {
            if (out_index) {
                *out_index = i;
            }
            return &g_gpu.capsets[i];
        }
    }
    return NULL;
}

int virtio_gpu_get_capset(uint32_t capset_index, uint32_t version,
                          void *buf, uint32_t size)
{
    virtio_gpu_dev_t *dev = &g_gpu;

    if (!dev->initialized || !buf) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    if (capset_index >= dev->capsets_valid) {
        return VIRTIO_GPU_ERR_INVALID;
    }
    if (size > dev->capsets[capset_index].max_size ||
        size > sizeof(g_stage) - sizeof(struct virtio_gpu_ctrl_hdr)) {
        size = dev->capsets[capset_index].max_size;
        if (size > sizeof(g_stage) - sizeof(struct virtio_gpu_ctrl_hdr)) {
            size = sizeof(g_stage) - sizeof(struct virtio_gpu_ctrl_hdr);
        }
    }
    if (size == 0) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    struct virtio_gpu_get_capset cmd = {0};
    struct virtio_gpu_ctrl_hdr resp_hdr;

    cmd.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd.hdr.fence_id = ++dev->fence_seq;
    cmd.capset_index = capset_index;
    cmd.capset_version = version;

    /* Response: ctrl header (bounce buffer) + capset payload (g_stage) */
    int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                   &resp_hdr, sizeof(resp_hdr),
                                   buf, size, true);
    if (ret != VIRTIO_GPU_OK) {
        return ret;
    }
    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_CAPSET) {
        return VIRTIO_GPU_ERR_DEVICE;
    }
    return VIRTIO_GPU_OK;
}

int virtio_gpu_ctrl_send(const void *req, uint32_t req_len,
                         void *resp, uint32_t resp_len,
                         uint64_t *fence_id)
{
    virtio_gpu_dev_t *dev = &g_gpu;

    if (!dev->initialized || !req ||
        req_len < sizeof(struct virtio_gpu_ctrl_hdr)) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    /* Stamp a fence on the request (callers track completion by fence id) */
    uint64_t fence = ++dev->fence_seq;

    /* Copy into a mutable header so the fence fields can be set */
    struct virtio_gpu_ctrl_hdr hdr_copy = *(const struct virtio_gpu_ctrl_hdr *)req;
    hdr_copy.flags |= VIRTIO_GPU_FLAG_FENCE;
    hdr_copy.fence_id = fence;

    if (req_len > sizeof(g_ctrl_req)) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    /* ctrl_xfer stages through g_ctrl_req anyway; pass a request whose
     * header already carries the fence. */
    uint8_t *tmp = heap_alloc(req_len);
    if (!tmp) {
        return VIRTIO_GPU_ERR_NO_MEMORY;
    }
    memcpy(tmp, req, req_len);
    memcpy(tmp, &hdr_copy, sizeof(hdr_copy));

    int ret = virtio_gpu_ctrl_xfer(dev, tmp, req_len, resp, resp_len,
                                   NULL, 0, false);
    heap_free(tmp);

    if (ret == VIRTIO_GPU_OK && fence_id) {
        *fence_id = fence;
    }
    return ret;
}

int virtio_gpu_submit_3d(uint32_t ctx_id, const void *cmds,
                         uint32_t cmds_len, uint64_t *fence_id)
{
    virtio_gpu_dev_t *dev = &g_gpu;

    if (!dev->initialized || !cmds || cmds_len == 0 ||
        cmds_len > sizeof(g_stage)) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    struct virtio_gpu_cmd_submit cmd = {0};
    struct virtio_gpu_ctrl_hdr resp_hdr;

    cmd.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd.hdr.fence_id = ++dev->fence_seq;
    cmd.hdr.ctx_id = ctx_id;
    cmd.size = cmds_len;

    /* Command stream is a device-READ extra descriptor after the response */
    int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                   &resp_hdr, sizeof(resp_hdr),
                                   cmds, cmds_len, false);
    if (ret != VIRTIO_GPU_OK) {
        return ret;
    }
    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return VIRTIO_GPU_ERR_DEVICE;
    }
    if (fence_id) {
        *fence_id = cmd.hdr.fence_id;
    }
    return VIRTIO_GPU_OK;
}

/* ============================================================================
 * Blob resources (VIRTIO_GPU_F_RESOURCE_BLOB)
 * ============================================================================ */

bool virtio_gpu_has_blob(void)
{
    const virtio_gpu_dev_t *dev = &g_gpu;
    if (!dev->initialized || !dev->hostmem_base) {
        return false;
    }
    if (dev->modern) {
        return (dev->features64 & (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB)) != 0;
    }
    return (dev->base.features & VIRTIO_GPU_F_RESOURCE_BLOB_MASK) != 0;
}

bool virtio_gpu_hostmem_window(uint64_t *base, uint64_t *size)
{
    const virtio_gpu_dev_t *dev = &g_gpu;
    if (!dev->initialized || !dev->hostmem_base) {
        return false;
    }
    if (base) *base = dev->hostmem_base;
    if (size) *size = dev->hostmem_size;
    return true;
}

int virtio_gpu_resource_create_blob(uint32_t resource_id, uint32_t blob_mem,
                                    uint32_t blob_flags, uint64_t blob_id,
                                    uint64_t size)
{
    virtio_gpu_dev_t *dev = &g_gpu;
    if (!dev->initialized) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    struct virtio_gpu_resource_create_blob cmd = {0};
    struct virtio_gpu_ctrl_hdr resp_hdr;

    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    cmd.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd.hdr.fence_id = ++dev->fence_seq;
    cmd.resource_id = resource_id;
    cmd.blob_mem = blob_mem;
    cmd.blob_flags = blob_flags;
    cmd.nr_entries = 0;     /* host-allocated blobs take no guest entries */
    cmd.blob_id = blob_id;
    cmd.size = size;

    int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                   &resp_hdr, sizeof(resp_hdr),
                                   NULL, 0, false);
    if (ret != VIRTIO_GPU_OK) {
        return ret;
    }
    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return VIRTIO_GPU_ERR_DEVICE;
    }
    return VIRTIO_GPU_OK;
}

int virtio_gpu_resource_attach_backing(uint32_t resource_id,
                                       const void *entries,
                                       uint32_t nr_entries)
{
    virtio_gpu_dev_t *dev = &g_gpu;
    if (!dev->initialized || (!entries && nr_entries)) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    struct virtio_gpu_resource_attach_backing cmd = {0};
    struct virtio_gpu_ctrl_hdr resp_hdr;

    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd.hdr.fence_id = ++dev->fence_seq;
    cmd.resource_id = resource_id;
    cmd.nr_entries = nr_entries;

    int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                   &resp_hdr, sizeof(resp_hdr),
                                   entries,
                                   nr_entries * sizeof(struct virtio_gpu_mem_entry),
                                   false);
    if (ret != VIRTIO_GPU_OK) {
        return ret;
    }
    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return VIRTIO_GPU_ERR_DEVICE;
    }
    return VIRTIO_GPU_OK;
}

int virtio_gpu_resource_map_blob(uint32_t resource_id, uint64_t offset,
                                 uint64_t *map_info)
{
    virtio_gpu_dev_t *dev = &g_gpu;
    if (!dev->initialized || !map_info) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    struct virtio_gpu_resource_map_blob cmd = {0};
    struct virtio_gpu_resp_resource_map_blob resp;

    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    /* MAP/UNMAP_BLOB must carry the ring index so the device fences them
     * on ring 0 (virtio spec 1.2, 5.7.6.10). */
    cmd.hdr.flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    cmd.hdr.fence_id = ++dev->fence_seq;
    cmd.hdr.ring_idx = 0;
    cmd.resource_id = resource_id;
    cmd.offset = offset;

    int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                   &resp, sizeof(resp), NULL, 0, false);
    if (ret != VIRTIO_GPU_OK) {
        return ret;
    }
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_MAP_BLOB) {
        return VIRTIO_GPU_ERR_DEVICE;
    }
    *map_info = resp.map_info;
    return VIRTIO_GPU_OK;
}

int virtio_gpu_resource_unmap_blob(uint32_t resource_id)
{
    virtio_gpu_dev_t *dev = &g_gpu;
    if (!dev->initialized) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    struct virtio_gpu_resource_unmap_blob cmd = {0};
    struct virtio_gpu_ctrl_hdr resp_hdr;

    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    cmd.hdr.flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    cmd.hdr.fence_id = ++dev->fence_seq;
    cmd.hdr.ring_idx = 0;
    cmd.resource_id = resource_id;

    int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                   &resp_hdr, sizeof(resp_hdr),
                                   NULL, 0, false);
    if (ret != VIRTIO_GPU_OK) {
        return ret;
    }
    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return VIRTIO_GPU_ERR_DEVICE;
    }
    return VIRTIO_GPU_OK;
}

int virtio_gpu_resource_unref(uint32_t resource_id)
{
    virtio_gpu_dev_t *dev = &g_gpu;
    if (!dev->initialized) {
        return VIRTIO_GPU_ERR_INVALID;
    }

    struct virtio_gpu_resource_unref cmd = {0};
    struct virtio_gpu_ctrl_hdr resp_hdr;

    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd.hdr.fence_id = ++dev->fence_seq;
    cmd.resource_id = resource_id;

    int ret = virtio_gpu_ctrl_xfer(dev, &cmd, sizeof(cmd),
                                   &resp_hdr, sizeof(resp_hdr),
                                   NULL, 0, false);
    if (ret != VIRTIO_GPU_OK) {
        return ret;
    }
    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return VIRTIO_GPU_ERR_DEVICE;
    }
    return VIRTIO_GPU_OK;
}

void virtio_gpu_print_info(void)
{
    const virtio_gpu_dev_t *dev = &g_gpu;

    console_printf("\n=== VirtIO GPU ===\n");
    if (!dev->initialized) {
        console_printf("  no virtio-gpu device (attach with -device virtio-gpu-pci)\n");
        return;
    }

    console_printf("  PCI: %04x:%04x, transport: %s\n",
                   dev->base.pci_dev->vendor_id,
                   dev->base.pci_dev->device_id,
                   dev->modern ? "modern MMIO" : "legacy I/O");
    console_printf("  features: 0x%08x (virgl not requested)\n",
                   dev->base.features);
    console_printf("  scanouts: %u (%u enabled), capsets enumerated: %u\n",
                   dev->num_scanouts, dev->displays_enabled,
                   dev->capsets_valid);
    for (uint32_t i = 0; i < dev->num_scanouts; i++) {
        if (dev->pmodes[i].enabled) {
            console_printf("    scanout %u: %ux%u+%u+%u\n",
                           i, dev->pmodes[i].r.width, dev->pmodes[i].r.height,
                           dev->pmodes[i].r.x, dev->pmodes[i].r.y);
        }
    }
    for (uint32_t i = 0; i < dev->capsets_valid; i++) {
        console_printf("    capset[%u]: id=%u max_version=%u max_size=%u\n",
                       i, dev->capsets[i].id, dev->capsets[i].max_version,
                       dev->capsets[i].max_size);
    }
    venus_print_info();
    console_printf("\n");
}

/* ============================================================================
 * Boot-time init
 * ============================================================================ */

/* Runs from __init_array after pci_init()/virtio_blk_init() (same pattern
 * as cmd_storage_boot_init). Registers the PCI drivers; probe only fires
 * when a virtio-gpu device is actually attached. */
__attribute__((constructor))
static void virtio_gpu_boot_init(void)
{
    virtio_gpu_init();
}
