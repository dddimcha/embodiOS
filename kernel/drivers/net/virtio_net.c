/* EMBODIOS VirtIO Network Driver Implementation
 *
 * VirtIO network device driver for virtual network connectivity.
 * Implements VirtIO v1.0 legacy mode for QEMU compatibility.
 */

#include <embodios/virtio_net.h>
#include <embodios/pci.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* Debug output (uncomment to enable) */
/* #define VIRTIO_NET_DEBUG 1 */

/* Module State */
static virtio_net_dev_t g_net = {0};
static bool g_net_initialized = false;

/* Receive buffer pool - page-aligned for DMA */
#define RX_BUFFER_SIZE  (sizeof(virtio_net_hdr_t) + VIRTIO_NET_MAX_PACKET)
static uint8_t rx_buffers[VIRTIO_NET_RX_BUFFERS][RX_BUFFER_SIZE]
    __attribute__((aligned(4096)));

/* Transmit buffer - page-aligned for DMA */
static uint8_t tx_buffer[RX_BUFFER_SIZE] __attribute__((aligned(4096)));

/* Track which buffers are in use */
static uint16_t rx_buffer_desc[VIRTIO_NET_RX_BUFFERS];  /* Descriptor for each buffer */

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Read MAC address from device config
 */
static void virtio_net_read_mac(virtio_net_dev_t *dev)
{
    uint16_t iobase = dev->base.iobase;

    for (int i = 0; i < 6; i++) {
        dev->mac[i] = inb(iobase + VIRTIO_PCI_CONFIG + i);
    }
}

/**
 * Read link status from device config
 */
static void virtio_net_read_status(virtio_net_dev_t *dev)
{
    uint16_t iobase = dev->base.iobase;

    /* Status is at offset 6 in config (after MAC) */
    dev->status = inw(iobase + VIRTIO_PCI_CONFIG + 6);
    dev->link_up = (dev->status & VIRTIO_NET_S_LINK_UP) != 0;
}

/**
 * Add a receive buffer to the RX virtqueue
 */
static int virtio_net_add_rx_buffer(virtio_net_dev_t *dev, int buf_idx)
{
    virtqueue_t *vq = &dev->rx_vq;
    uint16_t desc_idx;

    /* Allocate descriptor */
    desc_idx = virtqueue_alloc_desc(vq);
    if (desc_idx == 0xFFFF) {
        return VIRTIO_NET_ERR_FULL;
    }

    /* Setup descriptor for receive buffer */
    vq->desc[desc_idx].addr = (uint64_t)(uintptr_t)rx_buffers[buf_idx];
    vq->desc[desc_idx].len = RX_BUFFER_SIZE;
    vq->desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;  /* Device writes to this buffer */
    vq->desc[desc_idx].next = 0;

    /* Track which descriptor is for which buffer */
    rx_buffer_desc[buf_idx] = desc_idx;

    /* Add to available ring */
    virtqueue_kick(vq, desc_idx);

    return VIRTIO_NET_OK;
}

/**
 * Initialize receive buffers
 */
static int virtio_net_init_rx_buffers(virtio_net_dev_t *dev)
{
    for (int i = 0; i < VIRTIO_NET_RX_BUFFERS; i++) {
        int ret = virtio_net_add_rx_buffer(dev, i);
        if (ret != VIRTIO_NET_OK) {
            console_printf("[VirtIO-Net] Failed to add RX buffer %d\n", i);
            return ret;
        }
    }

    console_printf("[VirtIO-Net] Initialized %d RX buffers\n", VIRTIO_NET_RX_BUFFERS);
    return VIRTIO_NET_OK;
}

/* ============================================================================
 * Device Initialization
 * ============================================================================ */

/**
 * Probe and initialize VirtIO network device
 */
static int virtio_net_probe(pci_device_t *pci_dev)
{
    virtio_net_dev_t *dev = &g_net;
    uint16_t iobase;
    uint32_t features;
    uint16_t queue_size;
    int ret;

    console_printf("[VirtIO-Net] Probing device %04x:%04x\n",
                   pci_dev->vendor_id, pci_dev->device_id);

    /* Get I/O base from BAR0 */
    iobase = (uint16_t)pci_bar_address(pci_dev, 0);
    if (iobase == 0) {
        console_printf("[VirtIO-Net] Invalid BAR0\n");
        return VIRTIO_NET_ERR_INIT;
    }

    /* Enable bus mastering and I/O */
    pci_enable_bus_master(pci_dev);
    pci_enable_io(pci_dev);

    /* Initialize device structure */
    memset(dev, 0, sizeof(virtio_net_dev_t));
    dev->base.pci_dev = pci_dev;
    dev->base.iobase = iobase;
    dev->base.status = 0;

    console_printf("[VirtIO-Net] I/O base: 0x%x\n", iobase);

    /* VirtIO initialization sequence */

    /* Step 1: Reset device */
    virtio_reset(&dev->base);

    /* Step 2: Acknowledge we found the device */
    virtio_set_status(&dev->base, VIRTIO_STATUS_ACKNOWLEDGE);

    /* Step 3: Acknowledge we know how to drive it */
    virtio_set_status(&dev->base, VIRTIO_STATUS_DRIVER);

    /* Step 4: Read and negotiate features */
    features = virtio_get_features(&dev->base);
    console_printf("[VirtIO-Net] Device features: 0x%08x\n", features);

    /* We want MAC address and status notification */
    uint32_t wanted = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    uint32_t negotiated = features & wanted;

    virtio_set_features(&dev->base, negotiated);
    console_printf("[VirtIO-Net] Negotiated features: 0x%08x\n", negotiated);

    /* Read MAC address */
    if (features & VIRTIO_NET_F_MAC) {
        virtio_net_read_mac(dev);
        console_printf("[VirtIO-Net] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                       dev->mac[0], dev->mac[1], dev->mac[2],
                       dev->mac[3], dev->mac[4], dev->mac[5]);
    } else {
        /* Generate random MAC (locally administered) */
        dev->mac[0] = 0x52;
        dev->mac[1] = 0x54;
        dev->mac[2] = 0x00;
        dev->mac[3] = 0x12;
        dev->mac[4] = 0x34;
        dev->mac[5] = 0x56;
        console_printf("[VirtIO-Net] Using default MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                       dev->mac[0], dev->mac[1], dev->mac[2],
                       dev->mac[3], dev->mac[4], dev->mac[5]);
    }

    /* Read link status if supported */
    if (features & VIRTIO_NET_F_STATUS) {
        virtio_net_read_status(dev);
        console_printf("[VirtIO-Net] Link: %s\n", dev->link_up ? "UP" : "DOWN");
    } else {
        dev->link_up = true;  /* Assume up if status not supported */
    }

    /* Step 5: Setup virtqueues */

    /* Setup RX queue (queue 0) */
    virtio_select_queue(&dev->base, VIRTIO_NET_RX_QUEUE);
    queue_size = virtio_get_queue_size(&dev->base);
    if (queue_size == 0) {
        console_printf("[VirtIO-Net] RX queue size is 0\n");
        return VIRTIO_NET_ERR_INIT;
    }

    console_printf("[VirtIO-Net] RX queue size: %d\n", queue_size);

    ret = virtqueue_alloc(&dev->rx_vq, queue_size, iobase, VIRTIO_NET_RX_QUEUE);
    if (ret != VIRTIO_OK) {
        console_printf("[VirtIO-Net] Failed to alloc RX queue\n");
        return VIRTIO_NET_ERR_NOMEM;
    }

    /* Tell device where the RX queue is */
    virtio_set_queue_pfn(&dev->base, (uint32_t)(dev->rx_vq.desc_dma >> 12));

    /* Setup TX queue (queue 1) */
    virtio_select_queue(&dev->base, VIRTIO_NET_TX_QUEUE);
    queue_size = virtio_get_queue_size(&dev->base);
    if (queue_size == 0) {
        console_printf("[VirtIO-Net] TX queue size is 0\n");
        virtqueue_free(&dev->rx_vq);
        return VIRTIO_NET_ERR_INIT;
    }

    console_printf("[VirtIO-Net] TX queue size: %d\n", queue_size);

    ret = virtqueue_alloc(&dev->tx_vq, queue_size, iobase, VIRTIO_NET_TX_QUEUE);
    if (ret != VIRTIO_OK) {
        console_printf("[VirtIO-Net] Failed to alloc TX queue\n");
        virtqueue_free(&dev->rx_vq);
        return VIRTIO_NET_ERR_NOMEM;
    }

    /* Tell device where the TX queue is */
    virtio_set_queue_pfn(&dev->base, (uint32_t)(dev->tx_vq.desc_dma >> 12));

    /* Step 6: Initialize receive buffers */
    ret = virtio_net_init_rx_buffers(dev);
    if (ret != VIRTIO_NET_OK) {
        virtqueue_free(&dev->rx_vq);
        virtqueue_free(&dev->tx_vq);
        return ret;
    }

    /* Step 7: Mark driver as ready */
    virtio_set_status(&dev->base, VIRTIO_STATUS_DRIVER_OK);

    dev->base.initialized = true;
    g_net_initialized = true;

    console_printf("[VirtIO-Net] Initialization complete\n");
    return VIRTIO_NET_OK;
}

/* PCI driver registration */
static pci_driver_t virtio_net_driver = {
    .name = "virtio-net",
    .vendor_id = VIRTIO_PCI_VENDOR,
    .device_id = VIRTIO_PCI_DEVICE_NET,
    .class_code = PCI_ANY_CLASS,
    .subclass = PCI_ANY_CLASS,
    .probe = virtio_net_probe,
    .remove = NULL,
    .next = NULL
};

/* ============================================================================
 * Public API
 * ============================================================================ */

int virtio_net_init(void)
{
    int ret;

    console_printf("[VirtIO-Net] Initializing VirtIO network subsystem...\n");

    /* Make sure PCI is initialized */
    if (!pci_is_initialized()) {
        console_printf("[VirtIO-Net] PCI not initialized\n");
        return VIRTIO_NET_ERR_INIT;
    }

    /* Register driver */
    ret = pci_register_driver(&virtio_net_driver);
    if (ret != PCI_OK) {
        console_printf("[VirtIO-Net] Driver registration failed\n");
        return VIRTIO_NET_ERR_INIT;
    }

    /* Check if we found a device */
    if (!g_net_initialized) {
        console_printf("[VirtIO-Net] No VirtIO network device found\n");
        return VIRTIO_NET_ERR_INIT;
    }

    return VIRTIO_NET_OK;
}

bool virtio_net_is_ready(void)
{
    return g_net_initialized;
}

bool virtio_net_link_up(void)
{
    if (!g_net_initialized) return false;

    /* Re-read status */
    if (g_net.base.features & VIRTIO_NET_F_STATUS) {
        virtio_net_read_status(&g_net);
    }

    return g_net.link_up;
}

void virtio_net_get_mac(uint8_t *mac)
{
    if (!mac) return;

    if (g_net_initialized) {
        memcpy(mac, g_net.mac, 6);
    } else {
        memset(mac, 0, 6);
    }
}

int virtio_net_send(const void *data, size_t length)
{
    virtqueue_t *vq;
    uint16_t desc_idx;
    virtio_net_hdr_t *hdr;
    uint16_t used_idx;

    if (!g_net_initialized) {
        return VIRTIO_NET_ERR_INIT;
    }

    if (!data || length == 0 || length > VIRTIO_NET_MAX_PACKET) {
        return VIRTIO_ERR_INVALID;
    }

    if (!g_net.link_up) {
        return VIRTIO_NET_ERR_DOWN;
    }

    vq = &g_net.tx_vq;

    /* Allocate descriptor */
    desc_idx = virtqueue_alloc_desc(vq);
    if (desc_idx == 0xFFFF) {
        g_net.tx_errors++;
        return VIRTIO_NET_ERR_FULL;
    }

    /* Prepare transmit buffer with VirtIO header */
    hdr = (virtio_net_hdr_t *)tx_buffer;
    memset(hdr, 0, sizeof(virtio_net_hdr_t));
    hdr->flags = 0;
    hdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;

    /* Copy packet data after header */
    memcpy(tx_buffer + sizeof(virtio_net_hdr_t), data, length);

    /* Setup descriptor */
    vq->desc[desc_idx].addr = (uint64_t)(uintptr_t)tx_buffer;
    vq->desc[desc_idx].len = sizeof(virtio_net_hdr_t) + length;
    vq->desc[desc_idx].flags = 0;  /* Device reads from this buffer */
    vq->desc[desc_idx].next = 0;

    /* Submit to device */
    virtqueue_kick(vq, desc_idx);

    /* Wait for completion (polling) */
    uint32_t timeout = 100000;
    while (timeout > 0) {
        rmb();
        used_idx = vq->used->idx;

        if (used_idx != vq->last_used_idx) {
            /* TX completed */
            vq->last_used_idx = used_idx;
            virtqueue_free_desc(vq, desc_idx);

            g_net.tx_packets++;
            g_net.tx_bytes += length;

#ifdef VIRTIO_NET_DEBUG
            console_printf("[VirtIO-Net] TX: %zu bytes\n", length);
#endif
            return VIRTIO_NET_OK;
        }

        timeout--;
    }

    /* Timeout - free descriptor anyway */
    virtqueue_free_desc(vq, desc_idx);
    g_net.tx_errors++;
    return VIRTIO_ERR_TIMEOUT;
}

int virtio_net_receive(void *buffer, size_t max_len)
{
    virtqueue_t *vq;
    uint16_t used_idx;
    struct virtq_used_elem *elem;
    uint16_t desc_idx;
    int buf_idx;
    size_t packet_len;

    if (!g_net_initialized) {
        return VIRTIO_NET_ERR_INIT;
    }

    if (!buffer || max_len == 0) {
        return VIRTIO_ERR_INVALID;
    }

    vq = &g_net.rx_vq;

    rmb();
    used_idx = vq->used->idx;

    /* Check if any packets received */
    if (used_idx == vq->last_used_idx) {
        return 0;  /* No packets */
    }

    /* Get used element */
    elem = &vq->used->ring[vq->last_used_idx % vq->size];
    desc_idx = (uint16_t)elem->id;

    /* Find which buffer this descriptor belongs to */
    buf_idx = -1;
    for (int i = 0; i < VIRTIO_NET_RX_BUFFERS; i++) {
        if (rx_buffer_desc[i] == desc_idx) {
            buf_idx = i;
            break;
        }
    }

    if (buf_idx < 0) {
        g_net.rx_errors++;
        vq->last_used_idx++;
        return VIRTIO_NET_ERR_IO;
    }

    /* Calculate packet length (skip VirtIO header) */
    packet_len = elem->len - sizeof(virtio_net_hdr_t);

    if (packet_len > max_len) {
        /* Buffer too small, truncate */
        packet_len = max_len;
    }

    /* Copy packet data (skip VirtIO header) */
    memcpy(buffer, rx_buffers[buf_idx] + sizeof(virtio_net_hdr_t), packet_len);

    g_net.rx_packets++;
    g_net.rx_bytes += packet_len;

#ifdef VIRTIO_NET_DEBUG
    console_printf("[VirtIO-Net] RX: %zu bytes\n", packet_len);
#endif

    /* Update used index */
    vq->last_used_idx++;

    /* Re-add buffer to available ring */
    virtqueue_free_desc(vq, desc_idx);
    virtio_net_add_rx_buffer(&g_net, buf_idx);

    return (int)packet_len;
}

int virtio_net_poll(void)
{
    static uint8_t poll_buffer[VIRTIO_NET_MAX_PACKET];
    int packets = 0;
    int ret;

    while ((ret = virtio_net_receive(poll_buffer, sizeof(poll_buffer))) > 0) {
        packets++;
        /* In a real implementation, we'd pass this to the network stack */
    }

    return packets;
}

void virtio_net_get_stats(uint64_t *rx_packets, uint64_t *tx_packets,
                          uint64_t *rx_bytes, uint64_t *tx_bytes)
{
    if (rx_packets) *rx_packets = g_net.rx_packets;
    if (tx_packets) *tx_packets = g_net.tx_packets;
    if (rx_bytes) *rx_bytes = g_net.rx_bytes;
    if (tx_bytes) *tx_bytes = g_net.tx_bytes;
}

void virtio_net_print_info(void)
{
    if (!g_net_initialized) {
        console_printf("[VirtIO-Net] Not initialized\n");
        return;
    }

    console_printf("\n[VirtIO-Net] Network Interface:\n");
    console_printf("  MAC Address:    %02x:%02x:%02x:%02x:%02x:%02x\n",
                   g_net.mac[0], g_net.mac[1], g_net.mac[2],
                   g_net.mac[3], g_net.mac[4], g_net.mac[5]);
    console_printf("  Link Status:    %s\n", g_net.link_up ? "UP" : "DOWN");
    console_printf("  Features:       0x%08x\n", g_net.base.features);

    console_printf("\n[VirtIO-Net] Statistics:\n");
    console_printf("  RX Packets:     %llu\n", (unsigned long long)g_net.rx_packets);
    console_printf("  TX Packets:     %llu\n", (unsigned long long)g_net.tx_packets);
    console_printf("  RX Bytes:       %llu\n", (unsigned long long)g_net.rx_bytes);
    console_printf("  TX Bytes:       %llu\n", (unsigned long long)g_net.tx_bytes);
    console_printf("  RX Errors:      %llu\n", (unsigned long long)g_net.rx_errors);
    console_printf("  TX Errors:      %llu\n", (unsigned long long)g_net.tx_errors);
    console_printf("  RX Dropped:     %llu\n", (unsigned long long)g_net.rx_dropped);
    console_printf("\n");
}

int virtio_net_run_tests(void)
{
    int passed = 0;
    int failed = 0;

    console_printf("\n[VirtIO-Net] Running self-tests...\n");

    /* Test 1: Initialization */
    console_printf("  Test 1: Initialization... ");
    if (g_net_initialized) {
        console_printf("PASS\n");
        passed++;
    } else {
        console_printf("FAIL\n");
        failed++;
        goto done;
    }

    /* Test 2: MAC address */
    console_printf("  Test 2: MAC address... ");
    if (g_net.mac[0] != 0 || g_net.mac[1] != 0 || g_net.mac[2] != 0) {
        console_printf("PASS (%02x:%02x:%02x:...)\n",
                       g_net.mac[0], g_net.mac[1], g_net.mac[2]);
        passed++;
    } else {
        console_printf("FAIL (all zeros)\n");
        failed++;
    }

    /* Test 3: Link status */
    console_printf("  Test 3: Link status... ");
    console_printf("%s\n", g_net.link_up ? "PASS (UP)" : "WARN (DOWN)");
    passed++;

    /* Test 4: RX queue setup */
    console_printf("  Test 4: RX queue... ");
    if (g_net.rx_vq.size > 0 && g_net.rx_vq.desc != NULL) {
        console_printf("PASS (size=%d)\n", g_net.rx_vq.size);
        passed++;
    } else {
        console_printf("FAIL\n");
        failed++;
    }

    /* Test 5: TX queue setup */
    console_printf("  Test 5: TX queue... ");
    if (g_net.tx_vq.size > 0 && g_net.tx_vq.desc != NULL) {
        console_printf("PASS (size=%d)\n", g_net.tx_vq.size);
        passed++;
    } else {
        console_printf("FAIL\n");
        failed++;
    }

done:
    console_printf("[VirtIO-Net] Tests complete: %d passed, %d failed\n\n",
                   passed, failed);

    return (failed == 0) ? 0 : -1;
}
