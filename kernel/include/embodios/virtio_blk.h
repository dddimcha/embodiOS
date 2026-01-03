/* EMBODIOS VirtIO Block Device Driver
 *
 * VirtIO block device driver for reading/writing virtual disks in QEMU.
 * Implements VirtIO v1.0 legacy mode for broad compatibility.
 *
 * Reference: https://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html#x1-2490002
 */

#ifndef EMBODIOS_VIRTIO_BLK_H
#define EMBODIOS_VIRTIO_BLK_H

#include <embodios/types.h>
#include <embodios/virtio.h>
#include <embodios/block.h>
#include <embodios/pci.h>

/* ============================================================================
 * VirtIO Block Feature Bits
 * ============================================================================ */

#define VIRTIO_BLK_F_SIZE_MAX       (1 << 1)    /* Max segment size */
#define VIRTIO_BLK_F_SEG_MAX        (1 << 2)    /* Max segments per request */
#define VIRTIO_BLK_F_GEOMETRY       (1 << 4)    /* Disk geometry available */
#define VIRTIO_BLK_F_RO             (1 << 5)    /* Device is read-only */
#define VIRTIO_BLK_F_BLK_SIZE       (1 << 6)    /* Block size available */
#define VIRTIO_BLK_F_FLUSH          (1 << 9)    /* Flush command supported */
#define VIRTIO_BLK_F_TOPOLOGY       (1 << 10)   /* Topology info available */
#define VIRTIO_BLK_F_CONFIG_WCE     (1 << 11)   /* Writeback mode control */
#define VIRTIO_BLK_F_MQ             (1 << 12)   /* Multi-queue support */
#define VIRTIO_BLK_F_DISCARD        (1 << 13)   /* Discard command supported */
#define VIRTIO_BLK_F_WRITE_ZEROES   (1 << 14)   /* Write zeroes supported */

/* ============================================================================
 * VirtIO Block Request Types
 * ============================================================================ */

#define VIRTIO_BLK_T_IN             0   /* Read request */
#define VIRTIO_BLK_T_OUT            1   /* Write request */
#define VIRTIO_BLK_T_FLUSH          4   /* Flush request */
#define VIRTIO_BLK_T_GET_ID         8   /* Get device ID */
#define VIRTIO_BLK_T_DISCARD        11  /* Discard sectors */
#define VIRTIO_BLK_T_WRITE_ZEROES   13  /* Write zeroes */

/* ============================================================================
 * VirtIO Block Status Codes
 * ============================================================================ */

#define VIRTIO_BLK_S_OK             0   /* Success */
#define VIRTIO_BLK_S_IOERR          1   /* Device or driver error */
#define VIRTIO_BLK_S_UNSUPP         2   /* Request unsupported */

/* ============================================================================
 * VirtIO Block Configuration Space (offset from VIRTIO_PCI_CONFIG)
 * ============================================================================ */

#define VIRTIO_BLK_CFG_CAPACITY     0   /* 64-bit: Device capacity in sectors */
#define VIRTIO_BLK_CFG_SIZE_MAX     8   /* 32-bit: Max segment size */
#define VIRTIO_BLK_CFG_SEG_MAX      12  /* 32-bit: Max segments */
#define VIRTIO_BLK_CFG_GEOMETRY     16  /* Geometry (cylinders, heads, sectors) */
#define VIRTIO_BLK_CFG_BLK_SIZE     20  /* 32-bit: Logical block size */

/* ============================================================================
 * VirtIO Block Request Structure
 * ============================================================================ */

/**
 * VirtIO block request header
 * Sent as first descriptor in request chain
 */
struct virtio_blk_req_hdr {
    uint32_t type;          /* VIRTIO_BLK_T_* request type */
    uint32_t reserved;      /* Reserved (must be 0) */
    uint64_t sector;        /* Starting sector for I/O */
} __packed;

/**
 * VirtIO block request status
 * Returned as last descriptor in request chain
 */
struct virtio_blk_req_status {
    uint8_t status;         /* VIRTIO_BLK_S_* status code */
} __packed;

/* ============================================================================
 * VirtIO Block Device Structure
 * ============================================================================ */

/**
 * VirtIO block device state
 */
typedef struct virtio_blk_dev {
    /* Base VirtIO device */
    virtio_device_t vdev;

    /* Virtqueue for requests */
    virtqueue_t vq;

    /* Device configuration */
    uint64_t capacity;          /* Total sectors */
    uint32_t sector_size;       /* Bytes per sector (usually 512) */
    uint32_t max_segment_size;  /* Max bytes per segment */
    uint32_t max_segments;      /* Max segments per request */
    bool read_only;             /* Device is read-only */

    /* Request buffers (pre-allocated for synchronous I/O) */
    struct virtio_blk_req_hdr* req_hdr;     /* Request header */
    struct virtio_blk_req_status* req_status; /* Request status */
    dma_addr_t req_hdr_dma;                 /* DMA address of header */
    dma_addr_t req_status_dma;              /* DMA address of status */

    /* Block device interface */
    block_device_t block_dev;

    /* Statistics */
    uint64_t reads;             /* Total read requests */
    uint64_t writes;            /* Total write requests */
    uint64_t sectors_read;      /* Total sectors read */
    uint64_t sectors_written;   /* Total sectors written */
    uint64_t errors;            /* Total errors */
} virtio_blk_dev_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize VirtIO block subsystem
 *
 * Registers the VirtIO block PCI driver and initializes
 * any detected block devices.
 *
 * @return VIRTIO_OK on success, error code on failure
 */
int virtio_blk_init(void);

/**
 * Probe a VirtIO block device
 *
 * Called by PCI subsystem when a VirtIO block device is found.
 *
 * @param pci_dev   PCI device (vendor 0x1AF4, device 0x1001)
 *
 * @return 0 on success, negative error on failure
 */
int virtio_blk_probe(pci_device_t* pci_dev);

/**
 * Remove a VirtIO block device
 *
 * Called on driver unbind or device removal.
 *
 * @param pci_dev   PCI device
 */
void virtio_blk_remove(pci_device_t* pci_dev);

/**
 * Read sectors from VirtIO block device
 *
 * @param dev       VirtIO block device
 * @param sector    Starting sector number
 * @param count     Number of sectors to read
 * @param buffer    Output buffer (must be count * 512 bytes)
 *
 * @return VIRTIO_OK on success, negative error on failure
 */
int virtio_blk_read(virtio_blk_dev_t* dev, uint64_t sector,
                    uint32_t count, void* buffer);

/**
 * Write sectors to VirtIO block device
 *
 * @param dev       VirtIO block device
 * @param sector    Starting sector number
 * @param count     Number of sectors to write
 * @param buffer    Input buffer (must be count * 512 bytes)
 *
 * @return VIRTIO_OK on success, negative error on failure
 */
int virtio_blk_write(virtio_blk_dev_t* dev, uint64_t sector,
                     uint32_t count, const void* buffer);

/**
 * Flush pending writes
 *
 * @param dev   VirtIO block device
 *
 * @return VIRTIO_OK on success, negative error on failure
 */
int virtio_blk_flush(virtio_blk_dev_t* dev);

/**
 * Get VirtIO block device by index
 *
 * @param index     Device index (0, 1, 2, ...)
 *
 * @return VirtIO block device, or NULL if not found
 */
virtio_blk_dev_t* virtio_blk_get_device(int index);

/**
 * Get number of VirtIO block devices
 *
 * @return Device count
 */
int virtio_blk_device_count(void);

/* ============================================================================
 * Shell Commands
 * ============================================================================ */

/**
 * Run VirtIO block tests (blktest command)
 */
void virtio_blk_test(void);

/**
 * Print VirtIO block device info (blkinfo command)
 */
void virtio_blk_info(void);

/**
 * Read and dump sectors (blkread command)
 *
 * @param sector    Starting sector
 * @param count     Number of sectors (default 1)
 */
void virtio_blk_read_cmd(uint64_t sector, uint32_t count);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * VirtIO block statistics
 */
typedef struct virtio_blk_stats {
    uint64_t reads;             /* Total read requests */
    uint64_t writes;            /* Total write requests */
    uint64_t sectors_read;      /* Total sectors read */
    uint64_t sectors_written;   /* Total sectors written */
    uint64_t errors;            /* Total errors */
} virtio_blk_stats_t;

/**
 * Get VirtIO block statistics
 *
 * @param dev       VirtIO block device
 * @param stats     Output statistics structure
 */
void virtio_blk_get_stats(virtio_blk_dev_t* dev, virtio_blk_stats_t* stats);

/**
 * Print VirtIO block statistics
 */
void virtio_blk_print_stats(void);

#endif /* EMBODIOS_VIRTIO_BLK_H */
