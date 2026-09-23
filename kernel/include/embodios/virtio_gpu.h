/* EMBODIOS VirtIO GPU Driver
 *
 * virtio-gpu device driver (control virtqueue transport) for the Volta
 * v0.6.0 Vulkan transport workstream (WS-C).
 *
 * Part 1 (validated under QEMU 7.2 `-device virtio-gpu-pci`):
 *   - PCI probe for transitional (1af4:1010) and modern-only (1af4:1050)
 *     virtio-gpu devices (legacy I/O-port transport, same as virtio_net/
 *     virtio_blk in this tree)
 *   - virtio feature negotiation (VIRGL not required; CONTEXT_INIT/VENUS/
 *     RESOURCE_BLOB accepted when offered)
 *   - controlq + cursorq setup
 *   - VIRTIO_GPU_CMD_GET_DISPLAY_INFO
 *   - VIRTIO_GPU_CMD_GET_CAPSET_INFO / GET_CAPSET enumeration
 *     (capset id 4 = Venus, per virtio spec)
 *
 * Part 2 (Venus protocol layer) lives in venus.c / venus.h.
 *
 * References:
 *   - Virtio specification v1.2, section 5.7 "GPU Device"
 *   - include/uapi/linux/virtio_gpu.h (command/structure layout)
 */

#ifndef EMBODIOS_VIRTIO_GPU_H
#define EMBODIOS_VIRTIO_GPU_H

#include <embodios/types.h>
#include <embodios/virtio.h>
#include <embodios/pci.h>

/* ============================================================================
 * PCI IDs
 * ============================================================================ */

/* Transitional (legacy) virtio-gpu: 0x1000 + device id 16.
 * NOTE: 0x1041 is modern virtio-*net*, not a GPU id. */
#define VIRTIO_PCI_DEVICE_GPU_TRANS     0x1010  /* virtio-gpu, transitional */
#define VIRTIO_PCI_DEVICE_GPU_MODERN    0x1050  /* virtio-gpu, non-transitional */

/* ============================================================================
 * Feature bits (virtio spec 1.2, 5.7.4.1)
 * ============================================================================ */

#define VIRTIO_GPU_F_VIRGL              0   /* virgl 3D mode (not required) */
#define VIRTIO_GPU_F_EDID               1   /* EDID query support */
#define VIRTIO_GPU_F_RESOURCE_UUID      2   /* resource UUIDs */
#define VIRTIO_GPU_F_RESOURCE_BLOB      3   /* blob resources */
#define VIRTIO_GPU_F_CONTEXT_INIT       4   /* context_init in CTX_CREATE */
/* bit 5: reserved (VIRTIO_GPU_F_RUTABAGA_GFXSTREAM_VULKAN / BLOB_ALIGNMENT) */
#define VIRTIO_GPU_F_VENUS              6   /* Venus (Vulkan) contexts */

#define VIRTIO_GPU_F_VIRGL_MASK         (1u << VIRTIO_GPU_F_VIRGL)
#define VIRTIO_GPU_F_EDID_MASK          (1u << VIRTIO_GPU_F_EDID)
#define VIRTIO_GPU_F_RESOURCE_UUID_MASK (1u << VIRTIO_GPU_F_RESOURCE_UUID)
#define VIRTIO_GPU_F_RESOURCE_BLOB_MASK (1u << VIRTIO_GPU_F_RESOURCE_BLOB)
#define VIRTIO_GPU_F_CONTEXT_INIT_MASK  (1u << VIRTIO_GPU_F_CONTEXT_INIT)
#define VIRTIO_GPU_F_VENUS_MASK         (1u << VIRTIO_GPU_F_VENUS)

/* ============================================================================
 * Device configuration layout (virtio spec 1.2, 5.7.4)
 * Offsets are relative to the device-specific config region
 * (VIRTIO_PCI_CONFIG on the legacy I/O-port transport).
 * ============================================================================ */

/* struct virtio_gpu_config per the implemented ABI (Linux uapi
 * virtio_gpu.h / QEMU): events fields are 32-bit, not 16-bit as some
 * spec drafts show. Verified against QEMU 7.2 at runtime. */
#define VIRTIO_GPU_CFG_EVENTS_READ      0x00    /* u32, pending events */
#define VIRTIO_GPU_CFG_EVENTS_CLEAR     0x04    /* u32, clear events */
#define VIRTIO_GPU_CFG_NUM_SCANOUTS     0x08    /* u32 */
#define VIRTIO_GPU_CFG_NUM_CAPSETS      0x0c    /* u32 */

#define VIRTIO_GPU_MAX_SCANOUTS         16

/* ============================================================================
 * Control command / response types (enum virtio_gpu_ctrl_type)
 * ============================================================================ */

#define VIRTIO_GPU_UNDEFINED                    0

/* 2D commands */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109
#define VIRTIO_GPU_CMD_GET_EDID                 0x010a
#define VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID     0x010b
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB     0x010c
#define VIRTIO_GPU_CMD_SET_SCANOUT_BLOB         0x010d

/* 3D commands */
#define VIRTIO_GPU_CMD_CTX_CREATE               0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY              0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE      0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE      0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D       0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D      0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D    0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D                0x0207
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB        0x0208
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB      0x0209

/* Cursor commands */
#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

/* Success responses */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO          0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET               0x1103
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104
#define VIRTIO_GPU_RESP_OK_RESOURCE_UUID        0x1105
#define VIRTIO_GPU_RESP_OK_MAP_BLOB             0x1106

/* Error responses */
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200
#define VIRTIO_GPU_RESP_ERR_NOMEM               0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

/* Control header flags */
#define VIRTIO_GPU_FLAG_FENCE           (1u << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX   (1u << 1)

/* ============================================================================
 * Capset IDs (virtio spec 1.2, 5.7.4.2)
 * ============================================================================ */

#define VIRTIO_GPU_CAPSET_VIRGL         1
#define VIRTIO_GPU_CAPSET_VIRGL2        2
/* 3 is reserved for gfxstream */
#define VIRTIO_GPU_CAPSET_VENUS         4
#define VIRTIO_GPU_CAPSET_CROSS_DOMAIN  5

#define VIRTIO_GPU_MAX_CAPSETS          8

/* ============================================================================
 * Wire structures (all little-endian, packed)
 * ============================================================================ */

/**
 * Common control header — starts every command and every response.
 */
struct virtio_gpu_ctrl_hdr {
    uint32_t type;          /* VIRTIO_GPU_CMD_* / VIRTIO_GPU_RESP_* */
    uint32_t flags;         /* VIRTIO_GPU_FLAG_* */
    uint64_t fence_id;      /* fence id when VIRTIO_GPU_FLAG_FENCE set */
    uint32_t ctx_id;        /* rendering context (0 = default) */
    uint8_t  ring_idx;      /* controlq = 0 */
    uint8_t  padding[3];
} __packed;

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __packed;

/* VIRTIO_GPU_CMD_GET_DISPLAY_INFO -> VIRTIO_GPU_RESP_OK_DISPLAY_INFO */
struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __packed;

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __packed;

/* VIRTIO_GPU_CMD_GET_CAPSET_INFO -> VIRTIO_GPU_RESP_OK_CAPSET_INFO */
struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_index;
    uint32_t padding;
} __packed;

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} __packed;

/* VIRTIO_GPU_CMD_GET_CAPSET -> VIRTIO_GPU_RESP_OK_CAPSET */
struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_index;
    uint32_t capset_version;
} __packed;

struct virtio_gpu_resp_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint8_t  capset_data[];
} __packed;

/* VIRTIO_GPU_CMD_CTX_CREATE (3D; used by the Venus layer) */
struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;              /* length of debug_name */
    uint32_t context_init;      /* VIRTIO_GPU_F_CONTEXT_INIT: capset id bits */
    uint8_t  debug_name[64];
} __packed;

#define VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK  0x000000ff

/* VIRTIO_GPU_CMD_CTX_DESTROY */
struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
} __packed;

/* VIRTIO_GPU_CMD_SUBMIT_3D — command buffer follows in extra descriptors */
struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;              /* size of the command stream that follows */
    uint32_t padding;
} __packed;

/* ============================================================================
 * Blob resources (virtio spec 1.2, 5.7.6.10; requires VIRTIO_GPU_F_RESOURCE_BLOB)
 *
 * Venus uses blobs for everything that must be shared guest<->host:
 * the command ring / reply shmem (BLOB_MEM_HOST3D) and imported device
 * memory for storage buffers (BLOB_MEM_GUEST_VRAM). A blob is mapped into
 * the guest through RESOURCE_MAP_BLOB, which returns an offset into the
 * device's shared-memory PCI window (VIRTIO_PCI_CAP_SHARED_MEMORY_CFG).
 * ============================================================================ */

/* enum virtio_gpu_blob_mem */
#define VIRTIO_GPU_BLOB_MEM_GUEST       1   /* guest-allocated pages */
#define VIRTIO_GPU_BLOB_MEM_HOST3D      2   /* host 3D (virgl/venus shmem) */
#define VIRTIO_GPU_BLOB_MEM_GUEST_VRAM  3   /* guest pages, host VRAM view */

/* enum virtio_gpu_blob_flags */
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE     1
#define VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE    2
#define VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE 4

/* VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB */
struct virtio_gpu_resource_create_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t blob_mem;          /* VIRTIO_GPU_BLOB_MEM_* */
    uint32_t blob_flags;        /* VIRTIO_GPU_BLOB_FLAG_* */
    uint32_t nr_entries;        /* guest mem entries (0 for host-allocated) */
    uint64_t blob_id;           /* context-specific (Venus: vulkan handle) */
    uint64_t size;
} __packed;

/* VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING */
struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __packed;

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;        /* mem_entry array follows as extra data */
} __packed;

/* VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB -> VIRTIO_GPU_RESP_OK_MAP_BLOB */
struct virtio_gpu_resource_map_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
    uint64_t offset;            /* offset into the blob */
} __packed;

struct virtio_gpu_resp_resource_map_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint64_t map_info;          /* offset into the shared-memory window */
    uint32_t padding;
} __packed;

/* VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB */
struct virtio_gpu_resource_unmap_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __packed;

/* VIRTIO_GPU_CMD_RESOURCE_UNREF */
struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __packed;

/* ============================================================================
 * Driver state
 * ============================================================================ */

typedef struct virtio_gpu_capset {
    uint32_t id;                /* VIRTIO_GPU_CAPSET_* (0 = slot invalid) */
    uint32_t max_version;
    uint32_t max_size;
} virtio_gpu_capset_t;

/* ============================================================================
 * VirtIO modern PCI transport (virtio spec 1.2, 4.1.4)
 *
 * QEMU's virtio-gpu-pci is a MODERN-ONLY device (1af4:1050, no I/O BAR),
 * so this driver primarily uses the capability-based MMIO transport below.
 * The legacy I/O-port transport (transitional 1af4:1010) is kept for
 * non-QEMU hypervisors.
 * ============================================================================ */

/* PCI vendor capability (cap id 0x09) cfg_type values */
#define VIRTIO_PCI_CAP_COMMON_CFG       1
#define VIRTIO_PCI_CAP_NOTIFY_CFG       2
#define VIRTIO_PCI_CAP_ISR_CFG          3
#define VIRTIO_PCI_CAP_DEVICE_CFG       4
#define VIRTIO_PCI_CAP_PCI_CFG          5
#define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8  /* blob host-memory window */

/* Common configuration structure (virtio spec 4.1.4.3) */
struct virtio_pci_common_cfg {
    uint32_t device_feature_select;     /* 0x00 */
    uint32_t device_feature;            /* 0x04 */
    uint32_t driver_feature_select;     /* 0x08 */
    uint32_t driver_feature;            /* 0x0c */
    uint16_t msix_config;               /* 0x10 */
    uint16_t num_queues;                /* 0x12 */
    uint8_t  device_status;             /* 0x14 */
    uint8_t  config_generation;         /* 0x15 */
    uint16_t queue_select;              /* 0x16 */
    uint16_t queue_size;                /* 0x18 */
    uint16_t queue_msix_vector;         /* 0x1a */
    uint16_t queue_enable;              /* 0x1c */
    uint16_t queue_notify_off;          /* 0x1e */
    uint32_t queue_desc_lo;             /* 0x20 */
    uint32_t queue_desc_hi;             /* 0x24 */
    uint32_t queue_driver_lo;           /* 0x28 (avail ring) */
    uint32_t queue_driver_hi;           /* 0x2c */
    uint32_t queue_device_lo;           /* 0x30 (used ring) */
    uint32_t queue_device_hi;           /* 0x34 */
    uint16_t queue_notify_data;         /* 0x36 (virtio 1.1) */
    uint16_t queue_reset;               /* 0x38 (virtio 1.1) */
} __packed;

typedef struct virtio_gpu_dev {
    virtio_device_t base;       /* common virtio state (iobase, features) */
    virtqueue_t controlq;       /* queue 0: control commands */
    virtqueue_t cursorq;        /* queue 1: cursor commands (set up, unused) */

    /* Modern virtio PCI transport (capability-based MMIO; identity-mapped
     * physical addresses, same approach as the NVMe driver) */
    bool modern;                        /* true: MMIO caps, false: I/O ports */
    uint64_t features64;                /* negotiated features (modern) */
    volatile struct virtio_pci_common_cfg *common_cfg;
    volatile uint8_t *notify_base;      /* notify region base */
    uint32_t notify_off_multiplier;     /* from the notify capability */
    volatile uint8_t *device_cfg;       /* device-specific config region */
    uint16_t notify_off[2];             /* queue_notify_off per queue */

    /* Device config snapshot */
    uint32_t num_scanouts;
    uint32_t num_capsets;

    /* GET_DISPLAY_INFO result */
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
    uint32_t displays_enabled;

    /* Capset enumeration result */
    virtio_gpu_capset_t capsets[VIRTIO_GPU_MAX_CAPSETS];
    uint32_t capsets_valid;     /* number of valid entries in capsets[] */

    /* Shared-memory window (VIRTIO_PCI_CAP_SHARED_MEMORY_CFG), the PCI
     * range blob resources are mapped into via RESOURCE_MAP_BLOB.
     * hostmem_base == 0 when the device offers no blob host memory. */
    uint64_t hostmem_base;      /* physical base of the window */
    uint64_t hostmem_size;      /* window size in bytes */

    uint64_t fence_seq;         /* monotonically increasing fence ids */
    bool initialized;
} virtio_gpu_dev_t;

/* ============================================================================
 * Error codes
 * ============================================================================ */

#define VIRTIO_GPU_OK               0
#define VIRTIO_GPU_ERR_NOT_FOUND   -1
#define VIRTIO_GPU_ERR_NO_MEMORY   -2
#define VIRTIO_GPU_ERR_INVALID     -3
#define VIRTIO_GPU_ERR_TIMEOUT     -4
#define VIRTIO_GPU_ERR_IO          -5
#define VIRTIO_GPU_ERR_FULL        -6
#define VIRTIO_GPU_ERR_UNSUPPORTED -7
#define VIRTIO_GPU_ERR_DEVICE      -8   /* device answered with ERR_* */

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize the virtio-gpu driver (registers PCI drivers; probe runs for
 * every matching device). Called from a boot constructor after pci_init().
 *
 * @return VIRTIO_GPU_OK if registration succeeded (device may still be
 *         absent — check virtio_gpu_is_ready())
 */
int virtio_gpu_init(void);

/**
 * @return true if a virtio-gpu device was probed and its control queue is up
 */
bool virtio_gpu_is_ready(void);

/**
 * Print probe results (device, features, display info, capsets) to the
 * console. Safe to call when no device is present.
 */
void virtio_gpu_print_info(void);

/**
 * @return negotiated feature bits (low 32; legacy transport), 0 if no device
 */
uint32_t virtio_gpu_features(void);

/**
 * Look up an enumerated capset by id.
 *
 * @param capset_id  VIRTIO_GPU_CAPSET_*
 * @param out_index  optional output: capset index for GET_CAPSET
 * @return pointer to capset entry, or NULL if the device has no such capset
 */
const virtio_gpu_capset_t *virtio_gpu_find_capset(uint32_t capset_id,
                                                  uint32_t *out_index);

/**
 * Fetch capset contents (VIRTIO_GPU_CMD_GET_CAPSET).
 *
 * @param capset_index  index from virtio_gpu_find_capset()
 * @param version       capset version to request (0 = host default)
 * @param buf           output buffer (DMA-able memory not required; the
 *                      driver bounces through its own coherent buffer)
 * @param size          buffer size in bytes (clamped to capset_max_size)
 * @return VIRTIO_GPU_OK on success, negative error otherwise
 */
int virtio_gpu_get_capset(uint32_t capset_index, uint32_t version,
                          void *buf, uint32_t size);

/**
 * Synchronous controlq round trip: send `req` (starts with
 * virtio_gpu_ctrl_hdr), wait for the response written into `resp`.
 * Used by the Venus layer for CTX_CREATE/CTX_DESTROY.
 *
 * @param req       request buffer (must remain valid during the call)
 * @param req_len   request size
 * @param resp      response buffer (device-writable), may be NULL
 * @param resp_len  response buffer size (0 if resp == NULL)
 * @param fence_id  optional fence id output (request gets FLAG_FENCE)
 * @return VIRTIO_GPU_OK if the device answered with any RESP_OK_* type,
 *         VIRTIO_GPU_ERR_DEVICE on a RESP_ERR_* answer, negative error else
 */
int virtio_gpu_ctrl_send(const void *req, uint32_t req_len,
                         void *resp, uint32_t resp_len,
                         uint64_t *fence_id);

/**
 * Submit a raw command stream on a rendering context
 * (VIRTIO_GPU_CMD_SUBMIT_3D). The command bytes stay in driver-allocated
 * coherent memory for the duration of the call.
 *
 * @param ctx_id    target context (from a CTX_CREATE)
 * @param cmds      command stream bytes
 * @param cmds_len  stream size
 * @param fence_id  fence id assigned to this submission (out)
 * @return VIRTIO_GPU_OK on success
 */
int virtio_gpu_submit_3d(uint32_t ctx_id, const void *cmds,
                         uint32_t cmds_len, uint64_t *fence_id);

/**
 * @return true if blob resources are usable (VIRTIO_GPU_F_RESOURCE_BLOB
 *         negotiated AND a shared-memory PCI window is present)
 */
bool virtio_gpu_has_blob(void);

/**
 * Copy out the shared-memory window (base/size). Returns false when absent.
 */
bool virtio_gpu_hostmem_window(uint64_t *base, uint64_t *size);

/**
 * VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB. Host-allocated blobs
 * (BLOB_MEM_HOST3D / BLOB_MEM_GUEST_VRAM) take no guest mem entries.
 *
 * @return VIRTIO_GPU_OK on success
 */
int virtio_gpu_resource_create_blob(uint32_t resource_id, uint32_t blob_mem,
                                    uint32_t blob_flags, uint64_t blob_id,
                                    uint64_t size);

/**
 * VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING (for BLOB_MEM_GUEST blobs).
 *
 * @param entries     array of virtio_gpu_mem_entry (copied synchronously)
 * @param nr_entries  entry count
 */
int virtio_gpu_resource_attach_backing(uint32_t resource_id,
                                       const void *entries,
                                       uint32_t nr_entries);

/**
 * VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB.
 *
 * @param resource_id  blob resource
 * @param offset       offset into the blob
 * @param map_info     out: offset into the shared-memory window; the guest
 *                     address of the mapped blob is hostmem_base + map_info
 * @return VIRTIO_GPU_OK on success
 */
int virtio_gpu_resource_map_blob(uint32_t resource_id, uint64_t offset,
                                 uint64_t *map_info);

/**
 * VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB.
 */
int virtio_gpu_resource_unmap_blob(uint32_t resource_id);

/**
 * VIRTIO_GPU_CMD_RESOURCE_UNREF (host destroys the resource).
 */
int virtio_gpu_resource_unref(uint32_t resource_id);

#endif /* EMBODIOS_VIRTIO_GPU_H */
