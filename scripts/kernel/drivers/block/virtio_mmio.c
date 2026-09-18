/* EMBODIOS VirtIO-MMIO Block Driver
 *
 * VirtIO-MMIO block device driver for ARM64 systems.
 * On QEMU's virt machine, VirtIO devices are memory-mapped starting at 0x0a000000.
 *
 * Usage in QEMU:
 *   qemu-system-aarch64 -M virt -cpu cortex-a57 -m 1G \
 *       -kernel embodios.elf -device virtio-blk-device,drive=model0 \
 *       -drive if=none,id=model0,format=raw,file=model.gguf -nographic
 *
 * Reference: https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html
 */

#include <embodios/virtio.h>
#include <embodios/virtio_blk.h>
#include <embodios/block.h>
#include <embodios/dma.h>
#include <embodios/console.h>
#include <embodios/types.h>
#include <embodios/mm.h>

/* Only compile for ARM64 */
#ifdef __aarch64__

/* ============================================================================
 * VirtIO-MMIO Register Definitions
 * ============================================================================ */

#define VIRTIO_MMIO_MAGIC           0x000   /* "virt" = 0x74726976 */
#define VIRTIO_MMIO_VERSION         0x004   /* 1 = legacy, 2 = v1.0+ */
#define VIRTIO_MMIO_DEVICE_ID       0x008   /* Device type */
#define VIRTIO_MMIO_VENDOR_ID       0x00C   /* Vendor ID */
#define VIRTIO_MMIO_HOST_FEATURES   0x010   /* Device features */
#define VIRTIO_MMIO_HOST_FEATURES_SEL 0x014 /* Feature select */
#define VIRTIO_MMIO_GUEST_FEATURES  0x020   /* Driver features */
#define VIRTIO_MMIO_GUEST_FEATURES_SEL 0x024 /* Feature select */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028   /* Page size (legacy) */
#define VIRTIO_MMIO_QUEUE_SEL       0x030   /* Queue select */
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034   /* Max queue size */
#define VIRTIO_MMIO_QUEUE_NUM       0x038   /* Current queue size */
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03C   /* Queue align (legacy) */
#define VIRTIO_MMIO_QUEUE_PFN       0x040   /* Queue PFN (legacy) */
#define VIRTIO_MMIO_QUEUE_READY     0x044   /* Queue ready (v1.0+) */
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050   /* Queue notify */
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060  /* Interrupt status */
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064   /* Interrupt ACK */
#define VIRTIO_MMIO_STATUS          0x070   /* Device status */
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080   /* Desc addr low (v1.0+) */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084   /* Desc addr high (v1.0+) */
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090  /* Avail addr low (v1.0+) */
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094 /* Avail addr high (v1.0+) */
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0A0  /* Used addr low (v1.0+) */
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0A4 /* Used addr high (v1.0+) */
#define VIRTIO_MMIO_CONFIG          0x100   /* Device config space */

/* VirtIO-MMIO magic number */
#define VIRTIO_MMIO_MAGIC_VALUE     0x74726976  /* "virt" */

/* Device types */
#define VIRTIO_DEV_NET              1
#define VIRTIO_DEV_BLK              2

/* QEMU virt machine VirtIO-MMIO addresses */
#define VIRTIO_MMIO_BASE            0x0a000000
#define VIRTIO_MMIO_SIZE            0x200
#define VIRTIO_MMIO_COUNT           32      /* QEMU provides up to 32 slots */

/* VirtIO Block config offsets */
#define VIRTIO_BLK_CFG_CAPACITY     0       /* 64-bit capacity in sectors */

/* ============================================================================
 * MMIO Access Functions
 * ============================================================================ */

static inline uint32_t mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}

static inline void mmio_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}

static inline uint64_t mmio_read64(uintptr_t addr) {
    uint32_t low = mmio_read32(addr);
    uint32_t high = mmio_read32(addr + 4);
    return ((uint64_t)high << 32) | low;
}

/* ============================================================================
 * Module State
 * ============================================================================ */

#define VIRTIO_MMIO_MAX_DEVICES     4

typedef struct virtio_mmio_blk_dev {
    uintptr_t base;                 /* MMIO base address */
    virtqueue_t vq;                 /* Request virtqueue */
    uint64_t capacity;              /* Capacity in sectors */
    uint32_t sector_size;           /* Sector size (512) */
    bool read_only;                 /* Read-only flag */
    bool initialized;               /* Device initialized */

    /* Request buffers */
    struct virtio_blk_req_hdr* req_hdr;
    struct virtio_blk_req_status* req_status;
    dma_addr_t req_hdr_dma;
    dma_addr_t req_status_dma;

    /* Block device interface */
    block_device_t block_dev;

    /* Statistics */
    uint64_t reads;
    uint64_t writes;
    uint64_t sectors_read;
    uint64_t sectors_written;
    uint64_t errors;
} virtio_mmio_blk_dev_t;

static virtio_mmio_blk_dev_t mmio_devices[VIRTIO_MMIO_MAX_DEVICES];
static int mmio_device_count = 0;
static bool mmio_initialized = false;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static int virtio_mmio_block_read(block_device_t* dev, uint64_t sector,
                                   uint32_t count, void* buffer);
static int virtio_mmio_block_write(block_device_t* dev, uint64_t sector,
                                    uint32_t count, const void* buffer);
static int virtio_mmio_block_flush(block_device_t* dev);
static int virtio_mmio_block_status(block_device_t* dev);

static const block_ops_t virtio_mmio_blk_ops = {
    .read = virtio_mmio_block_read,
    .write = virtio_mmio_block_write,
    .flush = virtio_mmio_block_flush,
    .status = virtio_mmio_block_status,
};

/* ============================================================================
 * Virtqueue for MMIO
 * ============================================================================ */

static int virtqueue_mmio_alloc(virtqueue_t* vq, uint16_t size, uintptr_t base, uint16_t index) {
    if (!vq || size == 0) {
        return VIRTIO_ERR_INVALID;
    }

    /* Size must be power of 2 */
    if ((size & (size - 1)) != 0) {
        return VIRTIO_ERR_INVALID;
    }

    size_t desc_size = VIRTQ_DESC_SIZE(size);
    size_t avail_size = VIRTQ_AVAIL_SIZE(size);
    size_t used_offset = (desc_size + avail_size + 4095) & ~4095;
    size_t used_size = VIRTQ_USED_SIZE(size);
    size_t total_size = used_offset + used_size;

    /* Allocate page-aligned memory */
    void* vq_mem = heap_alloc_aligned(total_size, 4096);
    if (!vq_mem) {
        return VIRTIO_ERR_NO_MEMORY;
    }

    vq->desc_dma = (dma_addr_t)(uintptr_t)vq_mem;
    memset(vq_mem, 0, total_size);

    vq->desc = (struct virtq_desc*)vq_mem;
    vq->avail = (struct virtq_avail*)((uint8_t*)vq_mem + desc_size);
    vq->used = (struct virtq_used*)((uint8_t*)vq_mem + used_offset);

    vq->avail_dma = vq->desc_dma + desc_size;
    vq->used_dma = vq->desc_dma + used_offset;

    vq->desc_state = (uint16_t*)heap_alloc(sizeof(uint16_t) * size);
    if (!vq->desc_state) {
        heap_free_aligned(vq_mem);
        return VIRTIO_ERR_NO_MEMORY;
    }

    /* Initialize free list */
    for (uint16_t i = 0; i < size - 1; i++) {
        vq->desc[i].next = i + 1;
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
    }
    vq->desc[size - 1].next = 0xFFFF;
    vq->desc[size - 1].flags = 0;

    vq->size = size;
    vq->free_head = 0;
    vq->free_count = size;
    vq->last_used_idx = 0;
    vq->index = index;
    vq->iobase = 0;  /* Not used for MMIO */

    vq->avail->flags = 0;
    vq->avail->idx = 0;
    vq->used->flags = 0;
    vq->used->idx = 0;

    return VIRTIO_OK;
}

static void virtqueue_mmio_kick(virtqueue_t* vq, uintptr_t base, uint16_t head) {
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = head;

    wmb();
    vq->avail->idx++;
    mb();

    /* Notify device via MMIO */
    mmio_write32(base + VIRTIO_MMIO_QUEUE_NOTIFY, vq->index);
}

static bool virtqueue_mmio_has_used(virtqueue_t* vq) {
    rmb();
    return vq->used->idx != vq->last_used_idx;
}

static uint16_t virtqueue_mmio_get_used(virtqueue_t* vq, uint32_t* len) {
    if (!virtqueue_mmio_has_used(vq)) {
        return 0xFFFF;
    }

    rmb();
    uint16_t used_idx = vq->last_used_idx % vq->size;
    uint32_t head = vq->used->ring[used_idx].id;

    if (len) {
        *len = vq->used->ring[used_idx].len;
    }

    vq->last_used_idx++;
    return (uint16_t)head;
}

/* ============================================================================
 * VirtIO-MMIO Device Probe
 * ============================================================================ */

static int virtio_mmio_blk_probe(uintptr_t base) {
    if (mmio_device_count >= VIRTIO_MMIO_MAX_DEVICES) {
        return VIRTIO_ERR_FULL;
    }

    /* Check magic */
    uint32_t magic = mmio_read32(base + VIRTIO_MMIO_MAGIC);
    if (magic != VIRTIO_MMIO_MAGIC_VALUE) {
        return VIRTIO_ERR_NOT_FOUND;
    }

    /* Check device type */
    uint32_t device_id = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID);
    if (device_id != VIRTIO_DEV_BLK) {
        return VIRTIO_ERR_NOT_FOUND;
    }

    uint32_t version = mmio_read32(base + VIRTIO_MMIO_VERSION);
    console_printf("[VirtIO-MMIO] Block device found at 0x%lx (version %d)\n",
                   (unsigned long)base, version);

    virtio_mmio_blk_dev_t* dev = &mmio_devices[mmio_device_count];
    dev->base = base;

    /* Reset device */
    mmio_write32(base + VIRTIO_MMIO_STATUS, 0);

    /* Acknowledge */
    mmio_write32(base + VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* Driver knows how to operate */
    mmio_write32(base + VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation */
    mmio_write32(base + VIRTIO_MMIO_HOST_FEATURES_SEL, 0);
    uint32_t host_features = mmio_read32(base + VIRTIO_MMIO_HOST_FEATURES);

    uint32_t guest_features = 0;
    if (host_features & VIRTIO_BLK_F_RO) {
        guest_features |= VIRTIO_BLK_F_RO;
        dev->read_only = true;
    }

    mmio_write32(base + VIRTIO_MMIO_GUEST_FEATURES_SEL, 0);
    mmio_write32(base + VIRTIO_MMIO_GUEST_FEATURES, guest_features);

    /* Set FEATURES_OK */
    mmio_write32(base + VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    /* Verify FEATURES_OK was accepted */
    uint32_t status = mmio_read32(base + VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        console_printf("[VirtIO-MMIO] Feature negotiation failed\n");
        mmio_write32(base + VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return VIRTIO_ERR_INVALID;
    }

    /* Set up virtqueue */
    mmio_write32(base + VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t queue_size = mmio_read32(base + VIRTIO_MMIO_QUEUE_NUM_MAX);

    if (queue_size == 0) {
        console_printf("[VirtIO-MMIO] Queue size is 0\n");
        mmio_write32(base + VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return VIRTIO_ERR_INVALID;
    }

    /* Use min of max_size and 256 */
    if (queue_size > 256) queue_size = 256;

    console_printf("[VirtIO-MMIO] Queue size: %d descriptors\n", queue_size);

    int ret = virtqueue_mmio_alloc(&dev->vq, queue_size, base, 0);
    if (ret != VIRTIO_OK) {
        console_printf("[VirtIO-MMIO] Failed to allocate virtqueue\n");
        mmio_write32(base + VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return ret;
    }

    /* Configure queue */
    mmio_write32(base + VIRTIO_MMIO_QUEUE_NUM, queue_size);

    if (version == 1) {
        /* Legacy: set page size and PFN */
        mmio_write32(base + VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        mmio_write32(base + VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dev->vq.desc_dma >> 12));
    } else {
        /* Modern: set individual addresses */
        mmio_write32(base + VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)dev->vq.desc_dma);
        mmio_write32(base + VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(dev->vq.desc_dma >> 32));
        mmio_write32(base + VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)dev->vq.avail_dma);
        mmio_write32(base + VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32_t)(dev->vq.avail_dma >> 32));
        mmio_write32(base + VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)dev->vq.used_dma);
        mmio_write32(base + VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)(dev->vq.used_dma >> 32));
        mmio_write32(base + VIRTIO_MMIO_QUEUE_READY, 1);
    }

    /* Allocate request buffers */
    dev->req_hdr = (struct virtio_blk_req_hdr*)dma_alloc_coherent(
        sizeof(struct virtio_blk_req_hdr), &dev->req_hdr_dma);
    dev->req_status = (struct virtio_blk_req_status*)dma_alloc_coherent(
        sizeof(struct virtio_blk_req_status), &dev->req_status_dma);

    if (!dev->req_hdr || !dev->req_status) {
        console_printf("[VirtIO-MMIO] Failed to allocate request buffers\n");
        mmio_write32(base + VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return VIRTIO_ERR_NO_MEMORY;
    }

    /* Mark driver ready */
    mmio_write32(base + VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /* Read capacity */
    dev->capacity = mmio_read64(base + VIRTIO_MMIO_CONFIG + VIRTIO_BLK_CFG_CAPACITY);
    dev->sector_size = 512;

    console_printf("[VirtIO-MMIO] Device capacity: %llu sectors (%llu MB)\n",
                   (unsigned long long)dev->capacity,
                   (unsigned long long)(dev->capacity * 512 / (1024 * 1024)));

    /* Register block device */
    block_device_t* blkdev = &dev->block_dev;
    blkdev->name[0] = 'v';
    blkdev->name[1] = 'd';
    blkdev->name[2] = 'a' + mmio_device_count;
    blkdev->name[3] = '\0';
    blkdev->total_sectors = dev->capacity;
    blkdev->sector_size = dev->sector_size;
    blkdev->flags = dev->read_only ? BLOCK_FLAG_READONLY : 0;
    blkdev->flags |= BLOCK_FLAG_VIRTUAL;
    blkdev->ops = &virtio_mmio_blk_ops;
    blkdev->private_data = dev;

    block_register_device(blkdev);

    dev->initialized = true;
    mmio_device_count++;

    console_printf("[VirtIO-MMIO] Block device %s initialized\n", blkdev->name);

    return VIRTIO_OK;
}

/* ============================================================================
 * Block I/O Operations
 * ============================================================================ */

static int virtio_mmio_do_io(virtio_mmio_blk_dev_t* dev, uint32_t type,
                              uint64_t sector, uint32_t count,
                              void* buffer, dma_addr_t buffer_dma) {
    virtqueue_t* vq = &dev->vq;
    (void)buffer;

    dev->req_hdr->type = type;
    dev->req_hdr->reserved = 0;
    dev->req_hdr->sector = sector;
    dev->req_status->status = 0xFF;

    /* Allocate descriptors */
    uint16_t head = virtqueue_alloc_desc(vq);
    uint16_t data_idx = virtqueue_alloc_desc(vq);
    uint16_t status_idx = virtqueue_alloc_desc(vq);

    if (head == 0xFFFF || data_idx == 0xFFFF || status_idx == 0xFFFF) {
        if (head != 0xFFFF) virtqueue_free_desc(vq, head);
        if (data_idx != 0xFFFF) virtqueue_free_desc(vq, data_idx);
        if (status_idx != 0xFFFF) virtqueue_free_desc(vq, status_idx);
        return VIRTIO_ERR_FULL;
    }

    /* Setup descriptors */
    vq->desc[head].addr = dev->req_hdr_dma;
    vq->desc[head].len = sizeof(struct virtio_blk_req_hdr);
    vq->desc[head].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[head].next = data_idx;

    vq->desc[data_idx].addr = buffer_dma;
    vq->desc[data_idx].len = count * dev->sector_size;
    vq->desc[data_idx].flags = VIRTQ_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN) {
        vq->desc[data_idx].flags |= VIRTQ_DESC_F_WRITE;
    }
    vq->desc[data_idx].next = status_idx;

    vq->desc[status_idx].addr = dev->req_status_dma;
    vq->desc[status_idx].len = sizeof(struct virtio_blk_req_status);
    vq->desc[status_idx].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[status_idx].next = 0;

    /* Submit */
    virtqueue_mmio_kick(vq, dev->base, head);

    /* Poll for completion */
    int timeout = 1000000;
    while (!virtqueue_mmio_has_used(vq) && timeout-- > 0) {
        for (volatile int i = 0; i < 100; i++);
    }

    if (timeout <= 0) {
        console_printf("[VirtIO-MMIO] I/O timeout\n");
        virtqueue_free_desc(vq, head);
        virtqueue_free_desc(vq, data_idx);
        virtqueue_free_desc(vq, status_idx);
        return VIRTIO_ERR_TIMEOUT;
    }

    uint32_t len;
    virtqueue_mmio_get_used(vq, &len);

    virtqueue_free_desc(vq, head);
    virtqueue_free_desc(vq, data_idx);
    virtqueue_free_desc(vq, status_idx);

    uint8_t status = dev->req_status->status;
    if (status != VIRTIO_BLK_S_OK) {
        dev->errors++;
        return VIRTIO_ERR_IO;
    }

    return VIRTIO_OK;
}

static int virtio_mmio_read(virtio_mmio_blk_dev_t* dev, uint64_t sector,
                             uint32_t count, void* buffer) {
    if (!dev || !dev->initialized || !buffer) {
        return VIRTIO_ERR_INVALID;
    }

    if (sector + count > dev->capacity) {
        return VIRTIO_ERR_INVALID;
    }

    dma_addr_t buffer_dma = dma_map_single(buffer, count * dev->sector_size, DMA_FROM_DEVICE);
    if (buffer_dma == DMA_ADDR_INVALID) {
        return VIRTIO_ERR_NO_MEMORY;
    }

    int ret = virtio_mmio_do_io(dev, VIRTIO_BLK_T_IN, sector, count, buffer, buffer_dma);

    dma_unmap_single(buffer_dma, count * dev->sector_size, DMA_FROM_DEVICE);

    if (ret == VIRTIO_OK) {
        dev->reads++;
        dev->sectors_read += count;
    }

    return ret;
}

static int virtio_mmio_write(virtio_mmio_blk_dev_t* dev, uint64_t sector,
                              uint32_t count, const void* buffer) {
    if (!dev || !dev->initialized || !buffer) {
        return VIRTIO_ERR_INVALID;
    }

    if (dev->read_only) {
        return VIRTIO_ERR_IO;
    }

    if (sector + count > dev->capacity) {
        return VIRTIO_ERR_INVALID;
    }

    dma_addr_t buffer_dma = dma_map_single((void*)buffer, count * dev->sector_size, DMA_TO_DEVICE);
    if (buffer_dma == DMA_ADDR_INVALID) {
        return VIRTIO_ERR_NO_MEMORY;
    }

    int ret = virtio_mmio_do_io(dev, VIRTIO_BLK_T_OUT, sector, count, (void*)buffer, buffer_dma);

    dma_unmap_single(buffer_dma, count * dev->sector_size, DMA_TO_DEVICE);

    if (ret == VIRTIO_OK) {
        dev->writes++;
        dev->sectors_written += count;
    }

    return ret;
}

/* ============================================================================
 * Block Device Interface
 * ============================================================================ */

static int virtio_mmio_block_read(block_device_t* dev, uint64_t sector,
                                   uint32_t count, void* buffer) {
    virtio_mmio_blk_dev_t* vdev = (virtio_mmio_blk_dev_t*)dev->private_data;
    int ret = virtio_mmio_read(vdev, sector, count, buffer);
    if (ret == VIRTIO_OK) return BLOCK_OK;
    if (ret == VIRTIO_ERR_TIMEOUT) return BLOCK_ERR_TIMEOUT;
    if (ret == VIRTIO_ERR_IO) return BLOCK_ERR_IO;
    return BLOCK_ERR_INVALID;
}

static int virtio_mmio_block_write(block_device_t* dev, uint64_t sector,
                                    uint32_t count, const void* buffer) {
    virtio_mmio_blk_dev_t* vdev = (virtio_mmio_blk_dev_t*)dev->private_data;
    int ret = virtio_mmio_write(vdev, sector, count, buffer);
    if (ret == VIRTIO_OK) return BLOCK_OK;
    if (ret == VIRTIO_ERR_TIMEOUT) return BLOCK_ERR_TIMEOUT;
    if (ret == VIRTIO_ERR_IO) return BLOCK_ERR_IO;
    return BLOCK_ERR_INVALID;
}

static int virtio_mmio_block_flush(block_device_t* dev) {
    (void)dev;
    return BLOCK_OK;  /* No-op for now */
}

static int virtio_mmio_block_status(block_device_t* dev) {
    virtio_mmio_blk_dev_t* vdev = (virtio_mmio_blk_dev_t*)dev->private_data;
    return vdev->initialized ? BLOCK_OK : BLOCK_ERR_IO;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int virtio_mmio_init(void) {
    if (mmio_initialized) {
        return VIRTIO_OK;
    }

    console_printf("[VirtIO-MMIO] Scanning for devices...\n");

    /* Initialize block subsystem */
    block_init();

    /* Initialize device array */
    for (int i = 0; i < VIRTIO_MMIO_MAX_DEVICES; i++) {
        mmio_devices[i].initialized = false;
    }
    mmio_device_count = 0;

    /* Scan QEMU virt machine VirtIO-MMIO addresses */
    for (int i = 0; i < VIRTIO_MMIO_COUNT && mmio_device_count < VIRTIO_MMIO_MAX_DEVICES; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (i * VIRTIO_MMIO_SIZE);

        /* Try to probe block device */
        int ret = virtio_mmio_blk_probe(base);
        if (ret == VIRTIO_OK) {
            /* Found one */
        } else if (ret != VIRTIO_ERR_NOT_FOUND) {
            /* Error during probe */
            console_printf("[VirtIO-MMIO] Probe error at 0x%lx: %d\n",
                          (unsigned long)base, ret);
        }
    }

    mmio_initialized = true;

    if (mmio_device_count > 0) {
        console_printf("[VirtIO-MMIO] Found %d block device(s)\n", mmio_device_count);
    } else {
        console_printf("[VirtIO-MMIO] No block devices found\n");
    }

    return VIRTIO_OK;
}

int virtio_mmio_device_count(void) {
    return mmio_device_count;
}

void virtio_mmio_print_stats(void) {
    console_printf("\n=== VirtIO-MMIO Block Statistics ===\n");

    for (int i = 0; i < mmio_device_count; i++) {
        virtio_mmio_blk_dev_t* dev = &mmio_devices[i];
        console_printf("Device %s:\n", dev->block_dev.name);
        console_printf("  Reads:    %llu (%llu sectors)\n",
                       (unsigned long long)dev->reads,
                       (unsigned long long)dev->sectors_read);
        console_printf("  Writes:   %llu (%llu sectors)\n",
                       (unsigned long long)dev->writes,
                       (unsigned long long)dev->sectors_written);
        console_printf("  Errors:   %llu\n", (unsigned long long)dev->errors);
    }
}

#else /* !__aarch64__ */

/* Stubs for non-ARM64 builds */
int virtio_mmio_init(void) { return 0; }
int virtio_mmio_device_count(void) { return 0; }
void virtio_mmio_print_stats(void) {}

#endif /* __aarch64__ */
