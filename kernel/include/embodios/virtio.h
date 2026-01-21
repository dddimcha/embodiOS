/* EMBODIOS VirtIO Common Definitions
 *
 * VirtIO specification v1.0 (Legacy mode) common structures and constants.
 * Used by VirtIO block, network, and other device drivers.
 *
 * Reference: https://docs.oasis-open.org/virtio/virtio/v1.0/virtio-v1.0.html
 */

#ifndef EMBODIOS_VIRTIO_H
#define EMBODIOS_VIRTIO_H

#include <embodios/types.h>
#include <embodios/dma.h>
#include <embodios/pci.h>

/* ============================================================================
 * VirtIO PCI Vendor/Device IDs
 * ============================================================================ */

#define VIRTIO_PCI_VENDOR           0x1AF4  /* Red Hat / VirtIO */

/* Transitional device IDs (legacy) */
#define VIRTIO_PCI_DEVICE_NET       0x1000  /* Network card */
#define VIRTIO_PCI_DEVICE_BLK       0x1001  /* Block device */
#define VIRTIO_PCI_DEVICE_CONSOLE   0x1003  /* Console */
#define VIRTIO_PCI_DEVICE_ENTROPY   0x1005  /* Entropy source */
#define VIRTIO_PCI_DEVICE_BALLOON   0x1002  /* Memory balloon */
#define VIRTIO_PCI_DEVICE_SCSI      0x1004  /* SCSI host */
#define VIRTIO_PCI_DEVICE_GPU       0x1050  /* GPU device */

/* ============================================================================
 * VirtIO PCI Configuration Space (Legacy Mode - BAR0)
 * ============================================================================ */

/* I/O space registers */
#define VIRTIO_PCI_HOST_FEATURES    0x00    /* 32-bit: Features supported by host */
#define VIRTIO_PCI_GUEST_FEATURES   0x04    /* 32-bit: Features activated by guest */
#define VIRTIO_PCI_QUEUE_PFN        0x08    /* 32-bit: Physical page number of queue */
#define VIRTIO_PCI_QUEUE_SIZE       0x0C    /* 16-bit: Number of entries in queue */
#define VIRTIO_PCI_QUEUE_SEL        0x0E    /* 16-bit: Queue selector */
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10    /* 16-bit: Queue notifier */
#define VIRTIO_PCI_STATUS           0x12    /* 8-bit: Device status */
#define VIRTIO_PCI_ISR              0x13    /* 8-bit: Interrupt status */

/* Device-specific configuration starts at offset 0x14 (legacy) */
#define VIRTIO_PCI_CONFIG           0x14

/* With MSI-X, device config moves to offset 0x18 */
#define VIRTIO_PCI_CONFIG_MSIX      0x18

/* ============================================================================
 * VirtIO Device Status Bits
 * ============================================================================ */

#define VIRTIO_STATUS_RESET         0x00    /* Device reset */
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01    /* Guest has found the device */
#define VIRTIO_STATUS_DRIVER        0x02    /* Guest knows how to drive it */
#define VIRTIO_STATUS_DRIVER_OK     0x04    /* Driver is set up and ready */
#define VIRTIO_STATUS_FEATURES_OK   0x08    /* Feature negotiation complete */
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40  /* Device encountered an error */
#define VIRTIO_STATUS_FAILED        0x80    /* Something went wrong */

/* ============================================================================
 * VirtIO Common Feature Bits
 * ============================================================================ */

#define VIRTIO_F_NOTIFY_ON_EMPTY    (1 << 24)  /* Notify when queue empty */
#define VIRTIO_F_ANY_LAYOUT         (1 << 27)  /* Arbitrary buffer layout */
#define VIRTIO_F_RING_INDIRECT_DESC (1 << 28)  /* Indirect descriptors */
#define VIRTIO_F_RING_EVENT_IDX     (1 << 29)  /* Event index feature */
#define VIRTIO_F_VERSION_1          (1 << 32)  /* v1.0 compliant device */

/* ============================================================================
 * Virtqueue Descriptor Flags
 * ============================================================================ */

#define VIRTQ_DESC_F_NEXT           0x01    /* Buffer continues via next field */
#define VIRTQ_DESC_F_WRITE          0x02    /* Buffer is device-writable (read by guest) */
#define VIRTQ_DESC_F_INDIRECT       0x04    /* Buffer contains list of descriptors */

/* ============================================================================
 * Virtqueue Used Ring Flags
 * ============================================================================ */

#define VIRTQ_USED_F_NO_NOTIFY      0x01    /* Don't notify guest on used buffer */

/* ============================================================================
 * Virtqueue Available Ring Flags
 * ============================================================================ */

#define VIRTQ_AVAIL_F_NO_INTERRUPT  0x01    /* Don't interrupt on available buffer */

/* ============================================================================
 * Virtqueue Structures (Section 2.4 of VirtIO spec)
 * ============================================================================ */

/**
 * Virtqueue descriptor table entry
 * Points to a buffer in guest memory
 */
struct virtq_desc {
    uint64_t addr;      /* Physical address of buffer */
    uint32_t len;       /* Length of buffer in bytes */
    uint16_t flags;     /* VIRTQ_DESC_F_* flags */
    uint16_t next;      /* Next descriptor index (if VIRTQ_DESC_F_NEXT set) */
} __packed;

/**
 * Virtqueue available ring
 * Written by driver, read by device
 */
struct virtq_avail {
    uint16_t flags;     /* VIRTQ_AVAIL_F_* flags */
    uint16_t idx;       /* Next index driver will write to */
    uint16_t ring[];    /* Descriptor chain heads */
    /* uint16_t used_event; follows ring (if VIRTIO_F_RING_EVENT_IDX) */
} __packed;

/**
 * Virtqueue used ring element
 * One for each completed descriptor chain
 */
struct virtq_used_elem {
    uint32_t id;        /* Index of start of used descriptor chain */
    uint32_t len;       /* Total bytes written by device */
} __packed;

/**
 * Virtqueue used ring
 * Written by device, read by driver
 */
struct virtq_used {
    uint16_t flags;     /* VIRTQ_USED_F_* flags */
    uint16_t idx;       /* Next index device will write to */
    struct virtq_used_elem ring[];
    /* uint16_t avail_event; follows ring (if VIRTIO_F_RING_EVENT_IDX) */
} __packed;

/* ============================================================================
 * Virtqueue Helper Macros
 * ============================================================================ */

/* Alignment requirements */
#define VIRTQ_DESC_ALIGN            16
#define VIRTQ_AVAIL_ALIGN           2
#define VIRTQ_USED_ALIGN            4

/* Calculate sizes for queue allocation */
#define VIRTQ_DESC_SIZE(n)          (sizeof(struct virtq_desc) * (n))
#define VIRTQ_AVAIL_SIZE(n)         (sizeof(struct virtq_avail) + sizeof(uint16_t) * ((n) + 1))
#define VIRTQ_USED_SIZE(n)          (sizeof(struct virtq_used) + sizeof(struct virtq_used_elem) * (n) + sizeof(uint16_t))

/* Total size needed for virtqueue (page aligned) */
#define VIRTQ_TOTAL_SIZE(n) \
    (((VIRTQ_DESC_SIZE(n) + VIRTQ_AVAIL_SIZE(n) + 4095) & ~4095) + \
     ((VIRTQ_USED_SIZE(n) + 4095) & ~4095))

/* ============================================================================
 * Virtqueue Management Structure
 * ============================================================================ */

/**
 * Virtqueue runtime state
 */
typedef struct virtqueue {
    /* Descriptor table */
    struct virtq_desc* desc;    /* Descriptor array */
    dma_addr_t desc_dma;        /* DMA address of descriptors */

    /* Available ring */
    struct virtq_avail* avail;  /* Available ring */
    dma_addr_t avail_dma;       /* DMA address of available ring */

    /* Used ring */
    struct virtq_used* used;    /* Used ring */
    dma_addr_t used_dma;        /* DMA address of used ring */

    /* Queue state */
    uint16_t size;              /* Number of descriptors (power of 2) */
    uint16_t free_head;         /* Head of free descriptor list */
    uint16_t free_count;        /* Number of free descriptors */
    uint16_t last_used_idx;     /* Last processed used index */

    /* Queue index */
    uint16_t index;             /* Queue index (0, 1, 2...) */

    /* Free descriptor chain tracking */
    uint16_t* desc_state;       /* Track descriptor chain heads */

    /* Parent device I/O base for notifications */
    uint16_t iobase;            /* I/O port base address */
} virtqueue_t;

/* ============================================================================
 * VirtIO Device Base Structure
 * ============================================================================ */

/**
 * Base structure for all VirtIO devices
 */
typedef struct virtio_device {
    pci_device_t* pci_dev;      /* PCI device */
    uint16_t iobase;            /* I/O port base address */
    uint32_t features;          /* Negotiated features */
    uint8_t status;             /* Current device status */
    bool initialized;           /* Initialization complete */
} virtio_device_t;

/* ============================================================================
 * I/O Port Access (for legacy VirtIO) - x86_64 only
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)
/* x86 I/O port operations */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "dN"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "dN"(port));
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "dN"(port));
}
#endif /* __x86_64__ || __i386__ */

#ifdef __aarch64__
/* ARM64 doesn't use I/O ports - use MMIO instead */
/* These stubs prevent compilation errors when headers are included */
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0; }
static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline void outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline void outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
#endif /* __aarch64__ */

/* ============================================================================
 * Memory Barriers
 * ============================================================================ */

/* Compiler barrier - architecture independent */
#define barrier()           __asm__ volatile("" : : : "memory")

#if defined(__x86_64__) || defined(__i386__)
/* x86 memory barriers */
#define rmb()               __asm__ volatile("lfence" : : : "memory")
#define wmb()               __asm__ volatile("sfence" : : : "memory")
#define mb()                __asm__ volatile("mfence" : : : "memory")
#elif defined(__aarch64__)
/* ARM64 memory barriers */
#define rmb()               __asm__ volatile("dmb ld" : : : "memory")
#define wmb()               __asm__ volatile("dmb st" : : : "memory")
#define mb()                __asm__ volatile("dmb sy" : : : "memory")
#else
/* Generic fallback */
#define rmb()               barrier()
#define wmb()               barrier()
#define mb()                barrier()
#endif

/* ============================================================================
 * VirtIO Common Functions
 * ============================================================================ */

/**
 * Reset a VirtIO device
 *
 * @param dev   VirtIO device
 */
static inline void virtio_reset(virtio_device_t* dev) {
    outb(dev->iobase + VIRTIO_PCI_STATUS, VIRTIO_STATUS_RESET);
    /* Read back to ensure reset completes */
    (void)inb(dev->iobase + VIRTIO_PCI_STATUS);
}

/**
 * Set device status bits
 *
 * @param dev       VirtIO device
 * @param status    Status bits to set
 */
static inline void virtio_set_status(virtio_device_t* dev, uint8_t status) {
    dev->status |= status;
    outb(dev->iobase + VIRTIO_PCI_STATUS, dev->status);
}

/**
 * Get device status
 *
 * @param dev   VirtIO device
 * @return Current status
 */
static inline uint8_t virtio_get_status(virtio_device_t* dev) {
    return inb(dev->iobase + VIRTIO_PCI_STATUS);
}

/**
 * Read host (device) features
 *
 * @param dev   VirtIO device
 * @return Feature bits supported by device
 */
static inline uint32_t virtio_get_features(virtio_device_t* dev) {
    return inl(dev->iobase + VIRTIO_PCI_HOST_FEATURES);
}

/**
 * Write guest (driver) features
 *
 * @param dev       VirtIO device
 * @param features  Feature bits to activate
 */
static inline void virtio_set_features(virtio_device_t* dev, uint32_t features) {
    outl(dev->iobase + VIRTIO_PCI_GUEST_FEATURES, features);
    dev->features = features;
}

/**
 * Select a virtqueue for configuration
 *
 * @param dev   VirtIO device
 * @param idx   Queue index
 */
static inline void virtio_select_queue(virtio_device_t* dev, uint16_t idx) {
    outw(dev->iobase + VIRTIO_PCI_QUEUE_SEL, idx);
}

/**
 * Get size of currently selected queue
 *
 * @param dev   VirtIO device
 * @return Queue size (number of descriptors)
 */
static inline uint16_t virtio_get_queue_size(virtio_device_t* dev) {
    return inw(dev->iobase + VIRTIO_PCI_QUEUE_SIZE);
}

/**
 * Set the page frame number for a virtqueue
 *
 * @param dev   VirtIO device
 * @param pfn   Page frame number (physical address >> 12)
 */
static inline void virtio_set_queue_pfn(virtio_device_t* dev, uint32_t pfn) {
    outl(dev->iobase + VIRTIO_PCI_QUEUE_PFN, pfn);
}

/**
 * Notify device that virtqueue has new buffers
 *
 * @param dev   VirtIO device
 * @param idx   Queue index to notify
 */
static inline void virtio_notify(virtio_device_t* dev, uint16_t idx) {
    outw(dev->iobase + VIRTIO_PCI_QUEUE_NOTIFY, idx);
}

/**
 * Read and clear interrupt status
 *
 * @param dev   VirtIO device
 * @return ISR status bits
 */
static inline uint8_t virtio_read_isr(virtio_device_t* dev) {
    return inb(dev->iobase + VIRTIO_PCI_ISR);
}

/* ============================================================================
 * Virtqueue Allocation Functions
 * ============================================================================ */

/**
 * Allocate and initialize a virtqueue
 *
 * @param vq        Output: virtqueue structure to initialize
 * @param size      Queue size (number of descriptors)
 * @param iobase    I/O port base for notifications
 * @param index     Queue index
 *
 * @return 0 on success, negative error code on failure
 */
int virtqueue_alloc(virtqueue_t* vq, uint16_t size, uint16_t iobase, uint16_t index);

/**
 * Free a virtqueue
 *
 * @param vq    Virtqueue to free
 */
void virtqueue_free(virtqueue_t* vq);

/**
 * Allocate a descriptor from the virtqueue
 *
 * @param vq    Virtqueue
 * @return Descriptor index, or 0xFFFF if no free descriptors
 */
uint16_t virtqueue_alloc_desc(virtqueue_t* vq);

/**
 * Free a descriptor back to the virtqueue
 *
 * @param vq    Virtqueue
 * @param idx   Descriptor index to free
 */
void virtqueue_free_desc(virtqueue_t* vq, uint16_t idx);

/**
 * Add a buffer chain to the available ring
 *
 * @param vq        Virtqueue
 * @param head      Head descriptor index
 */
void virtqueue_kick(virtqueue_t* vq, uint16_t head);

/**
 * Check if there are used buffers to process
 *
 * @param vq    Virtqueue
 * @return true if used buffers available
 */
bool virtqueue_has_used(virtqueue_t* vq);

/**
 * Get next used buffer
 *
 * @param vq    Virtqueue
 * @param len   Output: length written by device
 * @return Head descriptor index, or 0xFFFF if none available
 */
uint16_t virtqueue_get_used(virtqueue_t* vq, uint32_t* len);

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define VIRTIO_OK               0
#define VIRTIO_ERR_NOT_FOUND   -1       /* Device not found */
#define VIRTIO_ERR_NO_MEMORY   -2       /* Out of memory */
#define VIRTIO_ERR_INVALID     -3       /* Invalid parameter */
#define VIRTIO_ERR_TIMEOUT     -4       /* Operation timed out */
#define VIRTIO_ERR_IO          -5       /* I/O error */
#define VIRTIO_ERR_FULL        -6       /* Queue full */
#define VIRTIO_ERR_BUSY        -7       /* Device busy */

#endif /* EMBODIOS_VIRTIO_H */
