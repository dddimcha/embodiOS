/* EMBODIOS VirtIO Block Driver
 *
 * VirtIO block device driver for reading GGUF models from virtual disks.
 * Implements VirtIO v1.0 legacy mode for QEMU compatibility.
 *
 * Usage in QEMU:
 *   qemu-system-x86_64 -kernel embodios.elf -m 2G \
 *       -drive file=model.img,format=raw,if=virtio -serial stdio
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

/* Debug output for VirtIO driver (uncomment to enable) */
/* #define VIRTIO_DEBUG 1 */

#include <embodios/virtio.h>
#include <embodios/virtio_blk.h>
#include <embodios/block.h>
#include <embodios/pci.h>
#include <embodios/dma.h>
#include <embodios/console.h>
#include <embodios/types.h>
#include <embodios/mm.h>
#include <embodios/benchmark.h>

/* ============================================================================
 * Module State
 * ============================================================================ */

#define VIRTIO_BLK_MAX_DEVICES  4

static virtio_blk_dev_t virtio_blk_devices[VIRTIO_BLK_MAX_DEVICES];
static int virtio_blk_count = 0;
static bool virtio_blk_initialized = false;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static int virtio_blk_block_read(block_device_t* dev, uint64_t sector,
                                  uint32_t count, void* buffer);
static int virtio_blk_block_write(block_device_t* dev, uint64_t sector,
                                   uint32_t count, const void* buffer);
static int virtio_blk_block_flush(block_device_t* dev);
static int virtio_blk_block_status(block_device_t* dev);

/* Block device operations */
static const block_ops_t virtio_blk_ops = {
    .read = virtio_blk_block_read,
    .write = virtio_blk_block_write,
    .flush = virtio_blk_block_flush,
    .status = virtio_blk_block_status,
};

/* ============================================================================
 * Virtqueue Implementation
 * ============================================================================ */

int virtqueue_alloc(virtqueue_t* vq, uint16_t size, uint16_t iobase, uint16_t index) {
    if (!vq || size == 0) {
        return VIRTIO_ERR_INVALID;
    }

    /* Size must be power of 2 */
    if ((size & (size - 1)) != 0) {
        return VIRTIO_ERR_INVALID;
    }

    /*
     * Legacy VirtIO requires virtqueue to be contiguous memory with:
     * - Descriptor table at start (PAGE ALIGNED!)
     * - Available ring immediately after descriptors
     * - Used ring at page-aligned offset
     *
     * Layout (all in one allocation, page-aligned):
     *   [Descriptors][Available ring + padding to page boundary][Used ring]
     */
    size_t desc_size = VIRTQ_DESC_SIZE(size);
    size_t avail_size = VIRTQ_AVAIL_SIZE(size);

    /* Offset of used ring must be page-aligned (as per legacy VirtIO spec) */
    size_t used_offset = (desc_size + avail_size + 4095) & ~4095;
    size_t used_size = VIRTQ_USED_SIZE(size);
    size_t total_size = used_offset + used_size;

    /* Allocate entire virtqueue as PAGE-ALIGNED memory (required for legacy VirtIO PFN) */
    void* vq_mem = heap_alloc_aligned(total_size, 4096);  /* Page alignment */
    if (!vq_mem) {
        return VIRTIO_ERR_NO_MEMORY;
    }

    /* Get DMA address (identity mapped) */
    vq->desc_dma = (dma_addr_t)(uintptr_t)vq_mem;

    /* Zero the entire region */
    memset(vq_mem, 0, total_size);

    /* Set up pointers within the contiguous block */
    vq->desc = (struct virtq_desc*)vq_mem;
    vq->avail = (struct virtq_avail*)((uint8_t*)vq_mem + desc_size);
    vq->used = (struct virtq_used*)((uint8_t*)vq_mem + used_offset);

    /* DMA addresses are at known offsets from base */
    vq->avail_dma = vq->desc_dma + desc_size;
    vq->used_dma = vq->desc_dma + used_offset;

    /* Allocate descriptor state tracking (CPU-only, no DMA needed) */
    vq->desc_state = (uint16_t*)heap_alloc(sizeof(uint16_t) * size);
    if (!vq->desc_state) {
        dma_free_coherent(vq_mem, total_size, vq->desc_dma);
        return VIRTIO_ERR_NO_MEMORY;
    }

    /* Initialize descriptor free list */
    for (uint16_t i = 0; i < size - 1; i++) {
        vq->desc[i].next = i + 1;
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
    }
    vq->desc[size - 1].next = 0xFFFF;
    vq->desc[size - 1].flags = 0;

    /* Initialize state */
    vq->size = size;
    vq->free_head = 0;
    vq->free_count = size;
    vq->last_used_idx = 0;
    vq->index = index;
    vq->iobase = iobase;

    /* Initialize rings - already zeroed by memset */
    vq->avail->flags = 0;
    vq->avail->idx = 0;
    vq->used->flags = 0;
    vq->used->idx = 0;

    return VIRTIO_OK;
}

void virtqueue_free(virtqueue_t* vq) {
    if (!vq) return;

    /* Free the page-aligned virtqueue memory (desc points to start) */
    if (vq->desc) {
        heap_free_aligned(vq->desc);
    }

    if (vq->desc_state) {
        heap_free(vq->desc_state);
    }

    vq->desc = NULL;
    vq->avail = NULL;
    vq->used = NULL;
    vq->desc_state = NULL;
}

uint16_t virtqueue_alloc_desc(virtqueue_t* vq) {
    if (vq->free_count == 0) {
        return 0xFFFF;
    }

    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->free_count--;

    return idx;
}

void virtqueue_free_desc(virtqueue_t* vq, uint16_t idx) {
    vq->desc[idx].next = vq->free_head;
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->free_head = idx;
    vq->free_count++;
}

void virtqueue_kick(virtqueue_t* vq, uint16_t head) {
    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = head;

    /* Memory barrier before updating index */
    wmb();

    /* Update available index */
    vq->avail->idx++;

    /* Memory barrier before notify */
    mb();

#ifdef VIRTIO_DEBUG
    console_printf("[VirtIO] kick: head=%u avail_idx=%u iobase=0x%x\n",
                   head, vq->avail->idx - 1, vq->iobase);
    console_printf("[VirtIO] desc[0]: addr=0x%llx len=%u flags=0x%x next=%u\n",
                   vq->desc[head].addr, vq->desc[head].len,
                   vq->desc[head].flags, vq->desc[head].next);
#endif

    /* Notify device */
    outw(vq->iobase + VIRTIO_PCI_QUEUE_NOTIFY, vq->index);
}

bool virtqueue_has_used(virtqueue_t* vq) {
    /* Memory barrier to ensure we see latest used index */
    rmb();
    return vq->used->idx != vq->last_used_idx;
}

uint16_t virtqueue_get_used(virtqueue_t* vq, uint32_t* len) {
    if (!virtqueue_has_used(vq)) {
        return 0xFFFF;
    }

    /* Memory barrier before reading used element */
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
 * Block Subsystem Implementation
 * ============================================================================ */

static block_device_t* block_devices[BLOCK_MAX_DEVICES];
static int block_count = 0;
static bool block_initialized = false;

int block_init(void) {
    if (block_initialized) {
        return BLOCK_OK;
    }

    for (int i = 0; i < BLOCK_MAX_DEVICES; i++) {
        block_devices[i] = NULL;
    }
    block_count = 0;
    block_initialized = true;

    return BLOCK_OK;
}

int block_register_device(block_device_t* dev) {
    if (!block_initialized) {
        block_init();
    }

    if (!dev) {
        return BLOCK_ERR_INVALID;
    }

    if (block_count >= BLOCK_MAX_DEVICES) {
        return BLOCK_ERR_FULL;
    }

    dev->index = block_count;
    block_devices[block_count++] = dev;

    console_printf("[BLOCK] Registered device %s: %llu sectors (%llu MB)\n",
                   dev->name, dev->total_sectors,
                   (dev->total_sectors * dev->sector_size) / (1024 * 1024));

    return BLOCK_OK;
}

void block_unregister_device(block_device_t* dev) {
    if (!dev) return;

    for (int i = 0; i < block_count; i++) {
        if (block_devices[i] == dev) {
            /* Shift remaining devices */
            for (int j = i; j < block_count - 1; j++) {
                block_devices[j] = block_devices[j + 1];
                block_devices[j]->index = j;
            }
            block_devices[--block_count] = NULL;
            break;
        }
    }
}

block_device_t* block_get_device(const char* name) {
    if (!name) return NULL;

    for (int i = 0; i < block_count; i++) {
        if (block_devices[i]) {
            /* Simple string comparison */
            const char* a = name;
            const char* b = block_devices[i]->name;
            while (*a && *b && *a == *b) {
                a++;
                b++;
            }
            if (*a == '\0' && *b == '\0') {
                return block_devices[i];
            }
        }
    }
    return NULL;
}

block_device_t* block_get_device_by_index(int index) {
    if (index < 0 || index >= block_count) {
        return NULL;
    }
    return block_devices[index];
}

int block_device_count(void) {
    return block_count;
}

int block_read(block_device_t* dev, uint64_t sector,
               uint32_t count, void* buffer) {
    if (!dev || !dev->ops || !dev->ops->read || !buffer) {
        return BLOCK_ERR_INVALID;
    }

    if (sector + count > dev->total_sectors) {
        return BLOCK_ERR_INVALID;
    }

    return dev->ops->read(dev, sector, count, buffer);
}

int block_write(block_device_t* dev, uint64_t sector,
                uint32_t count, const void* buffer) {
    if (!dev || !dev->ops || !dev->ops->write || !buffer) {
        return BLOCK_ERR_INVALID;
    }

    if (dev->flags & BLOCK_FLAG_READONLY) {
        return BLOCK_ERR_READONLY;
    }

    if (sector + count > dev->total_sectors) {
        return BLOCK_ERR_INVALID;
    }

    return dev->ops->write(dev, sector, count, buffer);
}

int block_read_bytes(block_device_t* dev, uint64_t offset,
                     size_t size, void* buffer) {
    if (!dev || !buffer) {
        return BLOCK_ERR_INVALID;
    }

    if (size == 0) {
        return BLOCK_OK;
    }

    uint32_t sector_size = dev->sector_size;

    /* Check if read is within device bounds */
    if (offset + size > dev->total_sectors * sector_size) {
        return BLOCK_ERR_INVALID;
    }

    /* Calculate sector-aligned range */
    uint64_t start_sector = offset / sector_size;
    uint64_t end_offset = offset + size;
    uint64_t end_sector = (end_offset + sector_size - 1) / sector_size;
    uint32_t sector_count = (uint32_t)(end_sector - start_sector);

    /* Calculate byte offset within first sector */
    uint32_t sector_offset = (uint32_t)(offset % sector_size);

    /* Fast path: aligned read of whole sectors */
    if (sector_offset == 0 && (size % sector_size) == 0) {
        return block_read(dev, start_sector, sector_count, buffer);
    }

    /* Slow path: unaligned read - need temporary buffer */
    size_t temp_size = sector_count * sector_size;
    uint8_t* temp_buffer = (uint8_t*)heap_alloc(temp_size);
    if (!temp_buffer) {
        return BLOCK_ERR_NOMEM;
    }

    /* Read sectors into temporary buffer */
    int ret = block_read(dev, start_sector, sector_count, temp_buffer);
    if (ret != BLOCK_OK) {
        heap_free(temp_buffer);
        return ret;
    }

    /* Copy requested bytes from temporary buffer */
    memcpy(buffer, temp_buffer + sector_offset, size);

    heap_free(temp_buffer);
    return BLOCK_OK;
}

int block_write_bytes(block_device_t* dev, uint64_t offset,
                      size_t size, const void* buffer) {
    if (!dev || !buffer) {
        return BLOCK_ERR_INVALID;
    }

    if (dev->flags & BLOCK_FLAG_READONLY) {
        return BLOCK_ERR_READONLY;
    }

    if (size == 0) {
        return BLOCK_OK;
    }

    uint32_t sector_size = dev->sector_size;

    /* Check if write is within device bounds */
    if (offset + size > dev->total_sectors * sector_size) {
        return BLOCK_ERR_INVALID;
    }

    /* Calculate sector-aligned range */
    uint64_t start_sector = offset / sector_size;
    uint64_t end_offset = offset + size;
    uint64_t end_sector = (end_offset + sector_size - 1) / sector_size;
    uint32_t sector_count = (uint32_t)(end_sector - start_sector);

    /* Calculate byte offset within first sector */
    uint32_t sector_offset = (uint32_t)(offset % sector_size);

    /* Fast path: aligned write of whole sectors */
    if (sector_offset == 0 && (size % sector_size) == 0) {
        return block_write(dev, start_sector, sector_count, buffer);
    }

    /* Slow path: unaligned write - need read-modify-write */
    size_t temp_size = sector_count * sector_size;
    uint8_t* temp_buffer = (uint8_t*)heap_alloc(temp_size);
    if (!temp_buffer) {
        return BLOCK_ERR_NOMEM;
    }

    /* Read sectors into temporary buffer */
    int ret = block_read(dev, start_sector, sector_count, temp_buffer);
    if (ret != BLOCK_OK) {
        heap_free(temp_buffer);
        return ret;
    }

    /* Modify the requested bytes in temporary buffer */
    memcpy(temp_buffer + sector_offset, buffer, size);

    /* Write modified sectors back */
    ret = block_write(dev, start_sector, sector_count, temp_buffer);

    heap_free(temp_buffer);
    return ret;
}

void block_print_devices(void) {
    console_printf("\n=== Block Devices ===\n");

    if (block_count == 0) {
        console_printf("  No block devices registered\n");
        return;
    }

    for (int i = 0; i < block_count; i++) {
        block_device_t* dev = block_devices[i];
        if (dev) {
            console_printf("  %s: %llu sectors (%llu MB)%s\n",
                          dev->name, dev->total_sectors,
                          (dev->total_sectors * dev->sector_size) / (1024 * 1024),
                          (dev->flags & BLOCK_FLAG_READONLY) ? " [RO]" : "");
        }
    }
}

/* ============================================================================
 * VirtIO Block Driver Implementation
 * ============================================================================ */

/**
 * Read device capacity from configuration space
 */
static uint64_t virtio_blk_read_capacity(virtio_blk_dev_t* dev) {
    uint16_t iobase = dev->vdev.iobase;
    uint32_t low = inl(iobase + VIRTIO_PCI_CONFIG + VIRTIO_BLK_CFG_CAPACITY);
    uint32_t high = inl(iobase + VIRTIO_PCI_CONFIG + VIRTIO_BLK_CFG_CAPACITY + 4);
    return ((uint64_t)high << 32) | low;
}

int virtio_blk_probe(pci_device_t* pci_dev) {
    if (virtio_blk_count >= VIRTIO_BLK_MAX_DEVICES) {
        console_printf("[VirtIO] Too many block devices\n");
        return VIRTIO_ERR_FULL;
    }

    virtio_blk_dev_t* dev = &virtio_blk_devices[virtio_blk_count];

    /* Get I/O base address from BAR0 */
    uint32_t bar0 = pci_dev->bar[0];
    if (!(bar0 & PCI_BAR_IO)) {
        console_printf("[VirtIO] BAR0 is not I/O space\n");
        return VIRTIO_ERR_INVALID;
    }
    dev->vdev.iobase = bar0 & PCI_BAR_IO_MASK;
    dev->vdev.pci_dev = pci_dev;

    console_printf("[VirtIO] Block device at I/O port 0x%x\n", dev->vdev.iobase);

    /* Enable PCI bus mastering and I/O space */
    pci_enable_bus_master(pci_dev);
    pci_enable_io(pci_dev);

    /* Reset device */
    virtio_reset(&dev->vdev);

    /* Acknowledge device */
    virtio_set_status(&dev->vdev, VIRTIO_STATUS_ACKNOWLEDGE);

    /* We know how to drive this device */
    virtio_set_status(&dev->vdev, VIRTIO_STATUS_DRIVER);

    /* Feature negotiation */
    uint32_t host_features = virtio_get_features(&dev->vdev);
    uint32_t guest_features = 0;

    /* Accept read-only flag if set */
    if (host_features & VIRTIO_BLK_F_RO) {
        guest_features |= VIRTIO_BLK_F_RO;
        dev->read_only = true;
    } else {
        dev->read_only = false;
    }

    /* Accept block size if available */
    if (host_features & VIRTIO_BLK_F_BLK_SIZE) {
        guest_features |= VIRTIO_BLK_F_BLK_SIZE;
    }

    /* Accept flush if available */
    if (host_features & VIRTIO_BLK_F_FLUSH) {
        guest_features |= VIRTIO_BLK_F_FLUSH;
    }

    virtio_set_features(&dev->vdev, guest_features);

    /* Set up virtqueue 0 (request queue) */
    virtio_select_queue(&dev->vdev, 0);
    uint16_t queue_size = virtio_get_queue_size(&dev->vdev);

    if (queue_size == 0) {
        console_printf("[VirtIO] Queue size is 0\n");
        virtio_set_status(&dev->vdev, VIRTIO_STATUS_FAILED);
        return VIRTIO_ERR_INVALID;
    }

    console_printf("[VirtIO] Queue size: %d descriptors\n", queue_size);

    /* Allocate virtqueue */
    int ret = virtqueue_alloc(&dev->vq, queue_size, dev->vdev.iobase, 0);
    if (ret != VIRTIO_OK) {
        console_printf("[VirtIO] Failed to allocate virtqueue: %d\n", ret);
        virtio_set_status(&dev->vdev, VIRTIO_STATUS_FAILED);
        return ret;
    }

    /* Tell device where the queue is (legacy mode uses page frame number) */
    uint32_t pfn = (uint32_t)(dev->vq.desc_dma >> 12);
    virtio_set_queue_pfn(&dev->vdev, pfn);

#ifdef VIRTIO_DEBUG
    console_printf("[VirtIO] Queue PFN: 0x%x (phys addr 0x%llx)\n", pfn, dev->vq.desc_dma);
    console_printf("[VirtIO] desc=%p avail=%p used=%p\n",
                   dev->vq.desc, dev->vq.avail, dev->vq.used);
#endif

    /* Allocate request buffers */
    dev->req_hdr = (struct virtio_blk_req_hdr*)dma_alloc_coherent(
        sizeof(struct virtio_blk_req_hdr), &dev->req_hdr_dma);
    dev->req_status = (struct virtio_blk_req_status*)dma_alloc_coherent(
        sizeof(struct virtio_blk_req_status), &dev->req_status_dma);

    if (!dev->req_hdr || !dev->req_status) {
        console_printf("[VirtIO] Failed to allocate request buffers\n");
        virtqueue_free(&dev->vq);
        virtio_set_status(&dev->vdev, VIRTIO_STATUS_FAILED);
        return VIRTIO_ERR_NO_MEMORY;
    }

    /* Mark driver ready */
    virtio_set_status(&dev->vdev, VIRTIO_STATUS_DRIVER_OK);

    /* Read device configuration */
    dev->capacity = virtio_blk_read_capacity(dev);
    dev->sector_size = BLOCK_SECTOR_SIZE;  /* Standard 512-byte sectors */

    console_printf("[VirtIO] Device capacity: %llu sectors (%llu MB)\n",
                   dev->capacity, (dev->capacity * 512) / (1024 * 1024));

    /* Register as block device */
    block_device_t* blkdev = &dev->block_dev;
    blkdev->name[0] = 'v';
    blkdev->name[1] = 'd';
    blkdev->name[2] = 'a' + virtio_blk_count;
    blkdev->name[3] = '\0';
    blkdev->total_sectors = dev->capacity;
    blkdev->sector_size = dev->sector_size;
    blkdev->flags = dev->read_only ? BLOCK_FLAG_READONLY : 0;
    blkdev->flags |= BLOCK_FLAG_VIRTUAL;
    blkdev->ops = &virtio_blk_ops;
    blkdev->private_data = dev;

    block_register_device(blkdev);

    dev->vdev.initialized = true;
    virtio_blk_count++;

    console_printf("[VirtIO] Block device %s initialized successfully\n", blkdev->name);

    return VIRTIO_OK;
}

void virtio_blk_remove(pci_device_t* pci_dev) {
    for (int i = 0; i < virtio_blk_count; i++) {
        virtio_blk_dev_t* dev = &virtio_blk_devices[i];
        if (dev->vdev.pci_dev == pci_dev) {
            /* Unregister block device */
            block_unregister_device(&dev->block_dev);

            /* Free request buffers */
            if (dev->req_hdr) {
                dma_free_coherent(dev->req_hdr, sizeof(struct virtio_blk_req_hdr),
                                  dev->req_hdr_dma);
            }
            if (dev->req_status) {
                dma_free_coherent(dev->req_status, sizeof(struct virtio_blk_req_status),
                                  dev->req_status_dma);
            }

            /* Free virtqueue */
            virtqueue_free(&dev->vq);

            /* Reset device */
            virtio_reset(&dev->vdev);

            dev->vdev.initialized = false;
            break;
        }
    }
}

/**
 * Perform synchronous block I/O
 */
static int virtio_blk_do_io(virtio_blk_dev_t* dev, uint32_t type,
                             uint64_t sector, uint32_t count,
                             void* buffer, dma_addr_t buffer_dma) {
    virtqueue_t* vq = &dev->vq;

    /* Setup request header */
    dev->req_hdr->type = type;
    dev->req_hdr->reserved = 0;
    dev->req_hdr->sector = sector;

    /* Clear status */
    dev->req_status->status = 0xFF;

    /* Allocate descriptors for: header, data, status */
    uint16_t head = virtqueue_alloc_desc(vq);
    uint16_t data_idx = virtqueue_alloc_desc(vq);
    uint16_t status_idx = virtqueue_alloc_desc(vq);

    if (head == 0xFFFF || data_idx == 0xFFFF || status_idx == 0xFFFF) {
        /* Free any allocated descriptors */
        if (head != 0xFFFF) virtqueue_free_desc(vq, head);
        if (data_idx != 0xFFFF) virtqueue_free_desc(vq, data_idx);
        if (status_idx != 0xFFFF) virtqueue_free_desc(vq, status_idx);
        return VIRTIO_ERR_FULL;
    }

    /* Setup header descriptor */
    vq->desc[head].addr = dev->req_hdr_dma;
    vq->desc[head].len = sizeof(struct virtio_blk_req_hdr);
    vq->desc[head].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[head].next = data_idx;

    /* Setup data descriptor */
    vq->desc[data_idx].addr = buffer_dma;
    vq->desc[data_idx].len = count * dev->sector_size;
    vq->desc[data_idx].flags = VIRTQ_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN) {
        vq->desc[data_idx].flags |= VIRTQ_DESC_F_WRITE;  /* Device writes to buffer */
    }
    vq->desc[data_idx].next = status_idx;

    /* Setup status descriptor */
    vq->desc[status_idx].addr = dev->req_status_dma;
    vq->desc[status_idx].len = sizeof(struct virtio_blk_req_status);
    vq->desc[status_idx].flags = VIRTQ_DESC_F_WRITE;  /* Device writes status */
    vq->desc[status_idx].next = 0;

    /* Submit request */
#ifdef VIRTIO_DEBUG
    console_printf("[VirtIO] IO: type=%u sector=%llu count=%u\n", type, sector, count);
    console_printf("[VirtIO] hdr_dma=0x%llx buf_dma=0x%llx status_dma=0x%llx\n",
                   dev->req_hdr_dma, buffer_dma, dev->req_status_dma);
    console_printf("[VirtIO] desc chain: %u->%u->%u\n", head, data_idx, status_idx);
#endif
    virtqueue_kick(vq, head);

    /* Poll for completion with timeout */
    int timeout = 1000000;  /* ~1 second timeout */
    while (!virtqueue_has_used(vq) && timeout-- > 0) {
        /* Small delay */
        for (volatile int i = 0; i < 100; i++);
    }

    if (timeout <= 0) {
        console_printf("[VirtIO] I/O timeout\n");
        virtqueue_free_desc(vq, head);
        virtqueue_free_desc(vq, data_idx);
        virtqueue_free_desc(vq, status_idx);
        return VIRTIO_ERR_TIMEOUT;
    }

    /* Get completion */
    uint32_t len;
    uint16_t used_head = virtqueue_get_used(vq, &len);
    (void)used_head;  /* Should match head */

    /* Free descriptors */
    virtqueue_free_desc(vq, head);
    virtqueue_free_desc(vq, data_idx);
    virtqueue_free_desc(vq, status_idx);

    /* Check status */
    uint8_t status = dev->req_status->status;
    if (status != VIRTIO_BLK_S_OK) {
        dev->errors++;
        if (status == VIRTIO_BLK_S_IOERR) {
            return VIRTIO_ERR_IO;
        } else if (status == VIRTIO_BLK_S_UNSUPP) {
            return VIRTIO_ERR_INVALID;
        }
        return VIRTIO_ERR_IO;
    }

    return VIRTIO_OK;
}

int virtio_blk_read(virtio_blk_dev_t* dev, uint64_t sector,
                    uint32_t count, void* buffer) {
    if (!dev || !dev->vdev.initialized || !buffer) {
        return VIRTIO_ERR_INVALID;
    }

    if (sector + count > dev->capacity) {
        return VIRTIO_ERR_INVALID;
    }

    if (count == 0) {
        return VIRTIO_OK;
    }

    /* Map buffer for DMA */
    dma_addr_t buffer_dma = dma_map_single(buffer, count * dev->sector_size,
                                            DMA_FROM_DEVICE);
    if (buffer_dma == DMA_ADDR_INVALID) {
        return VIRTIO_ERR_NO_MEMORY;
    }

    int ret = virtio_blk_do_io(dev, VIRTIO_BLK_T_IN, sector, count,
                                buffer, buffer_dma);

    dma_unmap_single(buffer_dma, count * dev->sector_size, DMA_FROM_DEVICE);

    if (ret == VIRTIO_OK) {
        dev->reads++;
        dev->sectors_read += count;
    }

    return ret;
}

int virtio_blk_write(virtio_blk_dev_t* dev, uint64_t sector,
                     uint32_t count, const void* buffer) {
    if (!dev || !dev->vdev.initialized || !buffer) {
        return VIRTIO_ERR_INVALID;
    }

    if (dev->read_only) {
        return VIRTIO_ERR_IO;
    }

    if (sector + count > dev->capacity) {
        return VIRTIO_ERR_INVALID;
    }

    if (count == 0) {
        return VIRTIO_OK;
    }

    /* Map buffer for DMA */
    dma_addr_t buffer_dma = dma_map_single((void*)buffer, count * dev->sector_size,
                                            DMA_TO_DEVICE);
    if (buffer_dma == DMA_ADDR_INVALID) {
        return VIRTIO_ERR_NO_MEMORY;
    }

    int ret = virtio_blk_do_io(dev, VIRTIO_BLK_T_OUT, sector, count,
                                (void*)buffer, buffer_dma);

    dma_unmap_single(buffer_dma, count * dev->sector_size, DMA_TO_DEVICE);

    if (ret == VIRTIO_OK) {
        dev->writes++;
        dev->sectors_written += count;
    }

    return ret;
}

int virtio_blk_flush(virtio_blk_dev_t* dev) {
    if (!dev || !dev->vdev.initialized) {
        return VIRTIO_ERR_INVALID;
    }

    /* Check if flush is supported */
    if (!(dev->vdev.features & VIRTIO_BLK_F_FLUSH)) {
        return VIRTIO_OK;  /* No-op if not supported */
    }

    /* Setup flush request (no data buffer needed) */
    dev->req_hdr->type = VIRTIO_BLK_T_FLUSH;
    dev->req_hdr->reserved = 0;
    dev->req_hdr->sector = 0;
    dev->req_status->status = 0xFF;

    virtqueue_t* vq = &dev->vq;

    uint16_t head = virtqueue_alloc_desc(vq);
    uint16_t status_idx = virtqueue_alloc_desc(vq);

    if (head == 0xFFFF || status_idx == 0xFFFF) {
        if (head != 0xFFFF) virtqueue_free_desc(vq, head);
        if (status_idx != 0xFFFF) virtqueue_free_desc(vq, status_idx);
        return VIRTIO_ERR_FULL;
    }

    vq->desc[head].addr = dev->req_hdr_dma;
    vq->desc[head].len = sizeof(struct virtio_blk_req_hdr);
    vq->desc[head].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[head].next = status_idx;

    vq->desc[status_idx].addr = dev->req_status_dma;
    vq->desc[status_idx].len = sizeof(struct virtio_blk_req_status);
    vq->desc[status_idx].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[status_idx].next = 0;

    virtqueue_kick(vq, head);

    /* Poll for completion */
    int timeout = 1000000;
    while (!virtqueue_has_used(vq) && timeout-- > 0) {
        for (volatile int i = 0; i < 100; i++);
    }

    uint32_t len;
    virtqueue_get_used(vq, &len);
    virtqueue_free_desc(vq, head);
    virtqueue_free_desc(vq, status_idx);

    return (dev->req_status->status == VIRTIO_BLK_S_OK) ? VIRTIO_OK : VIRTIO_ERR_IO;
}

/* ============================================================================
 * Block Device Interface Wrappers
 * ============================================================================ */

static int virtio_blk_block_read(block_device_t* dev, uint64_t sector,
                                  uint32_t count, void* buffer) {
    virtio_blk_dev_t* vdev = (virtio_blk_dev_t*)dev->private_data;
    int ret = virtio_blk_read(vdev, sector, count, buffer);
    if (ret == VIRTIO_OK) return BLOCK_OK;
    if (ret == VIRTIO_ERR_TIMEOUT) return BLOCK_ERR_TIMEOUT;
    if (ret == VIRTIO_ERR_IO) return BLOCK_ERR_IO;
    return BLOCK_ERR_INVALID;
}

static int virtio_blk_block_write(block_device_t* dev, uint64_t sector,
                                   uint32_t count, const void* buffer) {
    virtio_blk_dev_t* vdev = (virtio_blk_dev_t*)dev->private_data;
    int ret = virtio_blk_write(vdev, sector, count, buffer);
    if (ret == VIRTIO_OK) return BLOCK_OK;
    if (ret == VIRTIO_ERR_TIMEOUT) return BLOCK_ERR_TIMEOUT;
    if (ret == VIRTIO_ERR_IO) return BLOCK_ERR_IO;
    return BLOCK_ERR_INVALID;
}

static int virtio_blk_block_flush(block_device_t* dev) {
    virtio_blk_dev_t* vdev = (virtio_blk_dev_t*)dev->private_data;
    int ret = virtio_blk_flush(vdev);
    return (ret == VIRTIO_OK) ? BLOCK_OK : BLOCK_ERR_IO;
}

static int virtio_blk_block_status(block_device_t* dev) {
    virtio_blk_dev_t* vdev = (virtio_blk_dev_t*)dev->private_data;
    return vdev->vdev.initialized ? BLOCK_OK : BLOCK_ERR_IO;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/* PCI driver for VirtIO block devices */
static pci_driver_t virtio_blk_pci_driver = {
    .name = "VirtIO Block",
    .vendor_id = VIRTIO_PCI_VENDOR,
    .device_id = VIRTIO_PCI_DEVICE_BLK,
    .class_code = PCI_ANY_CLASS,
    .subclass = PCI_ANY_CLASS,
    .probe = virtio_blk_probe,
    .remove = virtio_blk_remove,
    .next = NULL,
};

int virtio_blk_init(void) {
    if (virtio_blk_initialized) {
        return VIRTIO_OK;
    }

    console_printf("[VirtIO] Initializing VirtIO block driver...\n");

    /* Initialize block subsystem */
    block_init();

    /* Initialize device array */
    for (int i = 0; i < VIRTIO_BLK_MAX_DEVICES; i++) {
        virtio_blk_devices[i].vdev.initialized = false;
    }
    virtio_blk_count = 0;

    /* Register PCI driver */
    int ret = pci_register_driver(&virtio_blk_pci_driver);
    if (ret != PCI_OK) {
        console_printf("[VirtIO] Failed to register PCI driver: %d\n", ret);
        return VIRTIO_ERR_NOT_FOUND;
    }

    virtio_blk_initialized = true;

    if (virtio_blk_count > 0) {
        console_printf("[VirtIO] Found %d block device(s)\n", virtio_blk_count);
    } else {
        console_printf("[VirtIO] No VirtIO block devices found\n");
    }

    return VIRTIO_OK;
}

virtio_blk_dev_t* virtio_blk_get_device(int index) {
    if (index < 0 || index >= virtio_blk_count) {
        return NULL;
    }
    return &virtio_blk_devices[index];
}

int virtio_blk_device_count(void) {
    return virtio_blk_count;
}

void virtio_blk_get_stats(virtio_blk_dev_t* dev, virtio_blk_stats_t* stats) {
    if (!dev || !stats) return;

    stats->reads = dev->reads;
    stats->writes = dev->writes;
    stats->sectors_read = dev->sectors_read;
    stats->sectors_written = dev->sectors_written;
    stats->errors = dev->errors;
}

void virtio_blk_print_stats(void) {
    console_printf("\n=== VirtIO Block Statistics ===\n");

    for (int i = 0; i < virtio_blk_count; i++) {
        virtio_blk_dev_t* dev = &virtio_blk_devices[i];
        console_printf("Device %s:\n", dev->block_dev.name);
        console_printf("  Reads:    %llu (%llu sectors)\n",
                       dev->reads, dev->sectors_read);
        console_printf("  Writes:   %llu (%llu sectors)\n",
                       dev->writes, dev->sectors_written);
        console_printf("  Errors:   %llu\n", dev->errors);
    }
}

/* ============================================================================
 * Shell Commands
 * ============================================================================ */

void virtio_blk_info(void) {
    console_printf("\n=== VirtIO Block Devices ===\n");

    if (virtio_blk_count == 0) {
        console_printf("  No VirtIO block devices found\n");
        console_printf("  QEMU usage: -drive file=disk.img,format=raw,if=virtio\n");
        return;
    }

    for (int i = 0; i < virtio_blk_count; i++) {
        virtio_blk_dev_t* dev = &virtio_blk_devices[i];
        console_printf("\nDevice %s (virtio%d):\n", dev->block_dev.name, i);
        console_printf("  Capacity:  %llu sectors (%llu MB)\n",
                       dev->capacity, (dev->capacity * 512) / (1024 * 1024));
        console_printf("  I/O Port:  0x%x\n", dev->vdev.iobase);
        console_printf("  Queue:     %d descriptors\n", dev->vq.size);
        console_printf("  Read-only: %s\n", dev->read_only ? "Yes" : "No");
        console_printf("  Features:  0x%x\n", dev->vdev.features);
    }
}

void virtio_blk_test(void) {
    console_printf("\n=== VirtIO Block Tests ===\n");

    if (virtio_blk_count == 0) {
        console_printf("SKIP: No VirtIO block devices available\n");
        return;
    }

    virtio_blk_dev_t* dev = &virtio_blk_devices[0];

    /* Allocate test buffer */
    uint8_t* buffer = (uint8_t*)dma_alloc_coherent(512, NULL);
    if (!buffer) {
        console_printf("FAIL: Could not allocate test buffer\n");
        return;
    }

    /* Test 1: Read sector 0 */
    console_printf("Test 1: Read sector 0... ");
    int ret = virtio_blk_read(dev, 0, 1, buffer);
    if (ret == VIRTIO_OK) {
        console_printf("PASS\n");
        /* Show first 16 bytes */
        console_printf("  Data: ");
        for (int i = 0; i < 16; i++) {
            console_printf("%02x ", buffer[i]);
        }
        console_printf("...\n");
    } else {
        console_printf("FAIL (error %d)\n", ret);
    }

    /* Test 2: Read 8 sectors */
    uint8_t* big_buffer = (uint8_t*)dma_alloc_coherent(512 * 8, NULL);
    if (big_buffer) {
        console_printf("Test 2: Read 8 sectors... ");
        ret = virtio_blk_read(dev, 0, 8, big_buffer);
        if (ret == VIRTIO_OK) {
            console_printf("PASS (read %d bytes)\n", 512 * 8);
        } else {
            console_printf("FAIL (error %d)\n", ret);
        }
        dma_free_coherent(big_buffer, 512 * 8, 0);
    }

    /* Test 3: Read at offset (if device has enough sectors) */
    if (dev->capacity > 100) {
        console_printf("Test 3: Read sector 100... ");
        ret = virtio_blk_read(dev, 100, 1, buffer);
        if (ret == VIRTIO_OK) {
            console_printf("PASS\n");
        } else {
            console_printf("FAIL (error %d)\n", ret);
        }
    }

    dma_free_coherent(buffer, 512, 0);

    console_printf("\nAll tests completed!\n");
    virtio_blk_print_stats();
}

void virtio_blk_read_cmd(uint64_t sector, uint32_t count) {
    if (virtio_blk_count == 0) {
        console_printf("Error: No VirtIO block devices\n");
        return;
    }

    if (count == 0) count = 1;
    if (count > 8) count = 8;  /* Limit to 4KB */

    virtio_blk_dev_t* dev = &virtio_blk_devices[0];

    if (sector >= dev->capacity) {
        console_printf("Error: Sector %llu beyond device capacity (%llu)\n",
                       sector, dev->capacity);
        return;
    }

    size_t size = count * 512;
    uint8_t* buffer = (uint8_t*)dma_alloc_coherent(size, NULL);
    if (!buffer) {
        console_printf("Error: Could not allocate buffer\n");
        return;
    }

    int ret = virtio_blk_read(dev, sector, count, buffer);
    if (ret != VIRTIO_OK) {
        console_printf("Error: Read failed (%d)\n", ret);
        dma_free_coherent(buffer, size, 0);
        return;
    }

    console_printf("Sector %llu (%d sector(s)):\n", sector, count);

    /* Hex dump first 256 bytes */
    size_t dump_size = (size < 256) ? size : 256;
    for (size_t i = 0; i < dump_size; i += 16) {
        console_printf("%08llx: ", sector * 512 + i);

        /* Hex values */
        for (size_t j = 0; j < 16 && i + j < dump_size; j++) {
            console_printf("%02x ", buffer[i + j]);
        }

        /* ASCII */
        console_printf(" |");
        for (size_t j = 0; j < 16 && i + j < dump_size; j++) {
            uint8_t c = buffer[i + j];
            console_printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        console_printf("|\n");
    }

    if (size > 256) {
        console_printf("... (%zu more bytes)\n", size - 256);
    }

    dma_free_coherent(buffer, size, 0);
}

void virtio_blk_perf_test(void) {
    console_printf("\n=== VirtIO Block Performance Test ===\n");

    if (virtio_blk_count == 0) {
        console_printf("SKIP: No VirtIO block devices available\n");
        return;
    }

    virtio_blk_dev_t* dev = &virtio_blk_devices[0];

    /* Test parameters */
    const size_t TEST_SIZE_MB = 50;  /* Read 50MB for accurate measurement */
    const size_t TEST_SIZE_BYTES = TEST_SIZE_MB * 1024 * 1024;
    const uint32_t SECTORS_TO_READ = TEST_SIZE_BYTES / 512;

    /* Check if device is large enough */
    if (dev->capacity < SECTORS_TO_READ) {
        console_printf("SKIP: Device too small (%llu sectors, need %u)\n",
                       dev->capacity, SECTORS_TO_READ);
        console_printf("  Create larger disk: dd if=/dev/zero of=test.img bs=1M count=100\n");
        return;
    }

    console_printf("Test configuration:\n");
    console_printf("  Device:      %s\n", dev->block_dev.name);
    console_printf("  Capacity:    %llu MB\n", (dev->capacity * 512) / (1024 * 1024));
    console_printf("  Test size:   %zu MB (%u sectors)\n", TEST_SIZE_MB, SECTORS_TO_READ);
    console_printf("  Target:      100 MB/s\n");
    console_printf("\n");

    /* Allocate buffer for reads - use smaller chunks to avoid allocation failures */
    const size_t CHUNK_SIZE = 1024 * 1024;  /* 1MB chunks */
    const uint32_t SECTORS_PER_CHUNK = CHUNK_SIZE / 512;
    uint8_t* buffer = (uint8_t*)dma_alloc_coherent(CHUNK_SIZE, NULL);
    if (!buffer) {
        console_printf("FAIL: Could not allocate %zu byte buffer\n", CHUNK_SIZE);
        return;
    }

    /* Get CPU timestamp counter for timing */
    extern uint64_t rdtsc(void);

    console_printf("Starting sequential read test...\n");

    /* Start timing */
    uint64_t start_tsc = rdtsc();

    /* Read data in chunks */
    uint32_t sectors_read = 0;
    int errors = 0;

    while (sectors_read < SECTORS_TO_READ) {
        uint32_t sectors_to_read = SECTORS_PER_CHUNK;
        if (sectors_read + sectors_to_read > SECTORS_TO_READ) {
            sectors_to_read = SECTORS_TO_READ - sectors_read;
        }

        int ret = virtio_blk_read(dev, sectors_read, sectors_to_read, buffer);
        if (ret != VIRTIO_OK) {
            console_printf("ERROR: Read failed at sector %u (error %d)\n", sectors_read, ret);
            errors++;
            break;
        }

        sectors_read += sectors_to_read;

        /* Show progress every 10MB */
        if (sectors_read % (10 * 1024 * 1024 / 512) == 0) {
            console_printf("  Progress: %u MB / %zu MB\r",
                          (sectors_read * 512) / (1024 * 1024), TEST_SIZE_MB);
        }
    }

    /* End timing */
    uint64_t end_tsc = rdtsc();

    dma_free_coherent(buffer, CHUNK_SIZE, 0);

    if (errors > 0) {
        console_printf("\nFAIL: Test aborted due to read errors\n");
        return;
    }

    console_printf("\nRead complete: %u sectors (%zu MB)\n",
                   sectors_read, (size_t)(sectors_read * 512) / (1024 * 1024));

    /* Calculate throughput */
    uint64_t elapsed_cycles = end_tsc - start_tsc;

    /* Assume CPU frequency ~2.0 GHz for rough estimate */
    /* In real hardware, we'd query CPUID or calibrate the TSC */
    const uint64_t CPU_FREQ_MHZ = 2000;  /* 2.0 GHz */
    uint64_t elapsed_us = elapsed_cycles / CPU_FREQ_MHZ;

    if (elapsed_us == 0) {
        console_printf("ERROR: Timer resolution too low\n");
        return;
    }

    /* Calculate MB/s */
    uint64_t bytes_read = (uint64_t)sectors_read * 512;
    uint64_t throughput_mbps = (bytes_read * 1000000ULL) / (elapsed_us * 1024 * 1024);

    console_printf("\nPerformance results:\n");
    console_printf("  Elapsed time:  %llu us (%.2f ms)\n",
                   elapsed_us, (double)elapsed_us / 1000.0);
    console_printf("  Throughput:    %llu MB/s\n", throughput_mbps);
    console_printf("\n");

    /* Check against target */
    const uint64_t TARGET_MBPS = 100;
    if (throughput_mbps >= TARGET_MBPS) {
        console_printf("✓ PASS: Throughput meets target (%llu MB/s >= %llu MB/s)\n",
                       throughput_mbps, TARGET_MBPS);
    } else {
        console_printf("✗ FAIL: Throughput below target (%llu MB/s < %llu MB/s)\n",
                       throughput_mbps, TARGET_MBPS);
    }

    console_printf("\nNote: Actual throughput may vary based on:\n");
    console_printf("  - CPU frequency (assumed %llu MHz)\n", CPU_FREQ_MHZ);
    console_printf("  - QEMU I/O backend configuration\n");
    console_printf("  - Host disk performance\n");
    console_printf("  - Virtualization overhead\n");
}
