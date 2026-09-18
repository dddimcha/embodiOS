/* EMBODIOS VirtIO Network Driver Interface
 *
 * VirtIO network device driver for virtual network connectivity.
 * Implements VirtIO v1.0 legacy mode for QEMU compatibility.
 *
 * Usage in QEMU:
 *   qemu-system-x86_64 -kernel embodios.elf -m 2G \
 *       -netdev user,id=net0 -device virtio-net-pci,netdev=net0 -serial stdio
 */

#ifndef EMBODIOS_VIRTIO_NET_H
#define EMBODIOS_VIRTIO_NET_H

#include <embodios/types.h>
#include <embodios/virtio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VirtIO Network Feature Bits */
#define VIRTIO_NET_F_CSUM           (1 << 0)   /* Host handles checksum */
#define VIRTIO_NET_F_GUEST_CSUM     (1 << 1)   /* Guest handles checksum */
#define VIRTIO_NET_F_MAC            (1 << 5)   /* Device has given MAC */
#define VIRTIO_NET_F_GSO            (1 << 6)   /* Generic segmentation offload */
#define VIRTIO_NET_F_GUEST_TSO4     (1 << 7)   /* Guest can receive TSOv4 */
#define VIRTIO_NET_F_GUEST_TSO6     (1 << 8)   /* Guest can receive TSOv6 */
#define VIRTIO_NET_F_GUEST_ECN      (1 << 9)   /* Guest can receive TSO with ECN */
#define VIRTIO_NET_F_GUEST_UFO      (1 << 10)  /* Guest can receive UFO */
#define VIRTIO_NET_F_HOST_TSO4      (1 << 11)  /* Host can receive TSOv4 */
#define VIRTIO_NET_F_HOST_TSO6      (1 << 12)  /* Host can receive TSOv6 */
#define VIRTIO_NET_F_HOST_ECN       (1 << 13)  /* Host can receive TSO with ECN */
#define VIRTIO_NET_F_HOST_UFO       (1 << 14)  /* Host can receive UFO */
#define VIRTIO_NET_F_MRG_RXBUF      (1 << 15)  /* Merge receive buffers */
#define VIRTIO_NET_F_STATUS         (1 << 16)  /* Configuration status field */
#define VIRTIO_NET_F_CTRL_VQ        (1 << 17)  /* Control VQ available */
#define VIRTIO_NET_F_CTRL_RX        (1 << 18)  /* Control RX mode */
#define VIRTIO_NET_F_CTRL_VLAN      (1 << 19)  /* Control VLAN filtering */
#define VIRTIO_NET_F_GUEST_ANNOUNCE (1 << 21)  /* Guest can send gratuitous packets */
#define VIRTIO_NET_F_MQ             (1 << 22)  /* Multiple queues */
#define VIRTIO_NET_F_CTRL_MAC_ADDR  (1 << 23)  /* Set MAC address through control */

/* Network status bits */
#define VIRTIO_NET_S_LINK_UP        1          /* Link is up */
#define VIRTIO_NET_S_ANNOUNCE       2          /* Announcement needed */

/* VirtIO Network Header (prepended to packets) */
typedef struct virtio_net_hdr {
    uint8_t  flags;         /* Flags */
    uint8_t  gso_type;      /* GSO type */
    uint16_t hdr_len;       /* Header length (for merge buffers) */
    uint16_t gso_size;      /* GSO segment size */
    uint16_t csum_start;    /* Checksum start offset */
    uint16_t csum_offset;   /* Checksum offset from csum_start */
} __packed virtio_net_hdr_t;

/* Header flags */
#define VIRTIO_NET_HDR_F_NEEDS_CSUM 1  /* Checksum is required */
#define VIRTIO_NET_HDR_F_DATA_VALID 2  /* Data is valid */

/* GSO types */
#define VIRTIO_NET_HDR_GSO_NONE     0  /* No GSO */
#define VIRTIO_NET_HDR_GSO_TCPV4    1  /* TCP segmentation */
#define VIRTIO_NET_HDR_GSO_UDP      3  /* UDP fragmentation */
#define VIRTIO_NET_HDR_GSO_TCPV6    4  /* TCPv6 segmentation */
#define VIRTIO_NET_HDR_GSO_ECN      0x80  /* ECN bit */

/* VirtIO Network Config (at VIRTIO_PCI_CONFIG offset) */
typedef struct virtio_net_config {
    uint8_t  mac[6];        /* MAC address */
    uint16_t status;        /* Link status */
    uint16_t max_virtqueue_pairs;  /* Number of TX/RX queue pairs */
} __packed virtio_net_config_t;

/* Maximum packet size (MTU + headers) */
#define VIRTIO_NET_MAX_PACKET   1514    /* Ethernet MTU + header */
#define VIRTIO_NET_RX_BUFFERS   64      /* Number of receive buffers */
#define VIRTIO_NET_TX_BUFFERS   64      /* Number of transmit buffers */

/* Queue indices */
#define VIRTIO_NET_RX_QUEUE     0       /* Receive queue */
#define VIRTIO_NET_TX_QUEUE     1       /* Transmit queue */
#define VIRTIO_NET_CTRL_QUEUE   2       /* Control queue (if supported) */

/* VirtIO Network Device */
typedef struct virtio_net_dev {
    virtio_device_t base;       /* Base VirtIO device */

    virtqueue_t rx_vq;          /* Receive virtqueue */
    virtqueue_t tx_vq;          /* Transmit virtqueue */

    uint8_t mac[6];             /* MAC address */
    uint16_t status;            /* Link status */
    bool link_up;               /* Link up flag */

    /* Statistics */
    uint64_t rx_packets;        /* Packets received */
    uint64_t tx_packets;        /* Packets transmitted */
    uint64_t rx_bytes;          /* Bytes received */
    uint64_t tx_bytes;          /* Bytes transmitted */
    uint64_t rx_errors;         /* Receive errors */
    uint64_t tx_errors;         /* Transmit errors */
    uint64_t rx_dropped;        /* Dropped received packets */
} virtio_net_dev_t;

/* Error codes */
#define VIRTIO_NET_OK           0
#define VIRTIO_NET_ERR_INIT     -1
#define VIRTIO_NET_ERR_NOMEM    -2
#define VIRTIO_NET_ERR_IO       -3
#define VIRTIO_NET_ERR_FULL     -4
#define VIRTIO_NET_ERR_DOWN     -5

/* Public API */

/**
 * Initialize VirtIO network subsystem
 * Scans for VirtIO network devices and initializes the first one found
 * @return VIRTIO_NET_OK on success, error code on failure
 */
int virtio_net_init(void);

/**
 * Check if VirtIO network is initialized and ready
 * @return true if ready, false otherwise
 */
bool virtio_net_is_ready(void);

/**
 * Check if network link is up
 * @return true if link is up, false otherwise
 */
bool virtio_net_link_up(void);

/**
 * Get MAC address
 * @param mac   Buffer to store MAC address (6 bytes)
 */
void virtio_net_get_mac(uint8_t *mac);

/**
 * Send a packet
 * @param data      Packet data (Ethernet frame)
 * @param length    Length of packet in bytes
 * @return VIRTIO_NET_OK on success, error code on failure
 */
int virtio_net_send(const void *data, size_t length);

/**
 * Receive a packet (non-blocking)
 * @param buffer    Buffer to store packet
 * @param max_len   Maximum length of buffer
 * @return Number of bytes received, 0 if no packet, negative on error
 */
int virtio_net_receive(void *buffer, size_t max_len);

/**
 * Poll for received packets
 * Call this periodically to process incoming packets
 * @return Number of packets processed
 */
int virtio_net_poll(void);

/**
 * Get network statistics
 * @param rx_packets    Packets received (output, can be NULL)
 * @param tx_packets    Packets transmitted (output, can be NULL)
 * @param rx_bytes      Bytes received (output, can be NULL)
 * @param tx_bytes      Bytes transmitted (output, can be NULL)
 */
void virtio_net_get_stats(uint64_t *rx_packets, uint64_t *tx_packets,
                          uint64_t *rx_bytes, uint64_t *tx_bytes);

/**
 * Print network status and statistics
 */
void virtio_net_print_info(void);

/**
 * Run network self-tests
 * @return 0 on success, -1 on failure
 */
int virtio_net_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_VIRTIO_NET_H */
