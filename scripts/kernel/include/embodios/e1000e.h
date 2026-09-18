/* EMBODIOS Intel e1000e Gigabit Ethernet Driver
 *
 * Driver for Intel 82574L, 82579LM, I217, I218, I219 and similar
 * Gigabit Ethernet controllers commonly found in Intel NUCs and laptops.
 *
 * Features:
 * - PCI device detection and initialization
 * - MMIO register access
 * - TX/RX ring buffer management
 * - Link status detection
 * - MAC address handling
 * - Basic statistics
 */

#ifndef EMBODIOS_E1000E_H
#define EMBODIOS_E1000E_H

#include <embodios/types.h>
#include <embodios/pci.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Intel Vendor and Device IDs
 * ============================================================================ */

#define E1000E_VENDOR_INTEL     0x8086

/* Common e1000e Device IDs */
#define E1000E_DEV_82574L       0x10D3  /* Intel 82574L GbE */
#define E1000E_DEV_82579LM      0x1502  /* Intel 82579LM GbE */
#define E1000E_DEV_82579V       0x1503  /* Intel 82579V GbE */
#define E1000E_DEV_I217LM       0x153A  /* Intel I217-LM */
#define E1000E_DEV_I217V        0x153B  /* Intel I217-V */
#define E1000E_DEV_I218LM       0x155A  /* Intel I218-LM */
#define E1000E_DEV_I218V        0x1559  /* Intel I218-V */
#define E1000E_DEV_I219LM       0x156F  /* Intel I219-LM */
#define E1000E_DEV_I219V        0x1570  /* Intel I219-V */
#define E1000E_DEV_I219LM2      0x15B7  /* Intel I219-LM (2) */
#define E1000E_DEV_I219V2       0x15B8  /* Intel I219-V (2) */
#define E1000E_DEV_I219LM3      0x15BB  /* Intel I219-LM (3) */
#define E1000E_DEV_I219V3       0x15BC  /* Intel I219-V (3) */

/* ============================================================================
 * Register Offsets (MMIO)
 * ============================================================================ */

/* Device Control */
#define E1000E_CTRL             0x0000  /* Device Control */
#define E1000E_STATUS           0x0008  /* Device Status */
#define E1000E_CTRL_EXT         0x0018  /* Extended Device Control */

/* EEPROM/Flash */
#define E1000E_EERD             0x0014  /* EEPROM Read */
#define E1000E_EECD             0x0010  /* EEPROM/Flash Control */

/* Interrupt */
#define E1000E_ICR              0x00C0  /* Interrupt Cause Read */
#define E1000E_ICS              0x00C8  /* Interrupt Cause Set */
#define E1000E_IMS              0x00D0  /* Interrupt Mask Set */
#define E1000E_IMC              0x00D8  /* Interrupt Mask Clear */

/* Receive */
#define E1000E_RCTL             0x0100  /* Receive Control */
#define E1000E_RDBAL            0x2800  /* RX Descriptor Base Low */
#define E1000E_RDBAH            0x2804  /* RX Descriptor Base High */
#define E1000E_RDLEN            0x2808  /* RX Descriptor Length */
#define E1000E_RDH              0x2810  /* RX Descriptor Head */
#define E1000E_RDT              0x2818  /* RX Descriptor Tail */
#define E1000E_RDTR             0x2820  /* RX Delay Timer */

/* Transmit */
#define E1000E_TCTL             0x0400  /* Transmit Control */
#define E1000E_TIPG             0x0410  /* TX Inter-Packet Gap */
#define E1000E_TDBAL            0x3800  /* TX Descriptor Base Low */
#define E1000E_TDBAH            0x3804  /* TX Descriptor Base High */
#define E1000E_TDLEN            0x3808  /* TX Descriptor Length */
#define E1000E_TDH              0x3810  /* TX Descriptor Head */
#define E1000E_TDT              0x3818  /* TX Descriptor Tail */

/* Receive Address */
#define E1000E_RAL              0x5400  /* Receive Address Low */
#define E1000E_RAH              0x5404  /* Receive Address High */

/* Multicast Table Array */
#define E1000E_MTA              0x5200  /* Multicast Table Array (128 entries) */

/* Statistics */
#define E1000E_CRCERRS          0x4000  /* CRC Error Count */
#define E1000E_ALGNERRC         0x4004  /* Alignment Error Count */
#define E1000E_RXERRC           0x400C  /* RX Error Count */
#define E1000E_MPC              0x4010  /* Missed Packets Count */
#define E1000E_COLC             0x4028  /* Collision Count */
#define E1000E_GPRC             0x4074  /* Good Packets Received Count */
#define E1000E_GPTC             0x4080  /* Good Packets Transmitted Count */
#define E1000E_GORCL            0x4088  /* Good Octets Received Low */
#define E1000E_GORCH            0x408C  /* Good Octets Received High */
#define E1000E_GOTCL            0x4090  /* Good Octets Transmitted Low */
#define E1000E_GOTCH            0x4094  /* Good Octets Transmitted High */
#define E1000E_TPR              0x40D0  /* Total Packets Received */
#define E1000E_TPT              0x40D4  /* Total Packets Transmitted */

/* ============================================================================
 * Control Register Bits (CTRL)
 * ============================================================================ */

#define E1000E_CTRL_FD          (1 << 0)    /* Full Duplex */
#define E1000E_CTRL_LRST        (1 << 3)    /* Link Reset */
#define E1000E_CTRL_ASDE        (1 << 5)    /* Auto-Speed Detection Enable */
#define E1000E_CTRL_SLU         (1 << 6)    /* Set Link Up */
#define E1000E_CTRL_ILOS        (1 << 7)    /* Invert Loss of Signal */
#define E1000E_CTRL_SPEED_MASK  (3 << 8)    /* Speed Selection */
#define E1000E_CTRL_SPEED_10    (0 << 8)    /* 10 Mbps */
#define E1000E_CTRL_SPEED_100   (1 << 8)    /* 100 Mbps */
#define E1000E_CTRL_SPEED_1000  (2 << 8)    /* 1000 Mbps */
#define E1000E_CTRL_FRCSPD      (1 << 11)   /* Force Speed */
#define E1000E_CTRL_FRCDPLX     (1 << 12)   /* Force Duplex */
#define E1000E_CTRL_RST         (1 << 26)   /* Device Reset */
#define E1000E_CTRL_VME         (1 << 30)   /* VLAN Mode Enable */
#define E1000E_CTRL_PHY_RST     (1 << 31)   /* PHY Reset */

/* ============================================================================
 * Status Register Bits (STATUS)
 * ============================================================================ */

#define E1000E_STATUS_FD        (1 << 0)    /* Full Duplex */
#define E1000E_STATUS_LU        (1 << 1)    /* Link Up */
#define E1000E_STATUS_TXOFF     (1 << 4)    /* TX Paused */
#define E1000E_STATUS_SPEED_MASK (3 << 6)   /* Link Speed */
#define E1000E_STATUS_SPEED_10  (0 << 6)    /* 10 Mbps */
#define E1000E_STATUS_SPEED_100 (1 << 6)    /* 100 Mbps */
#define E1000E_STATUS_SPEED_1000 (2 << 6)   /* 1000 Mbps */

/* ============================================================================
 * Receive Control Bits (RCTL)
 * ============================================================================ */

#define E1000E_RCTL_EN          (1 << 1)    /* Receiver Enable */
#define E1000E_RCTL_SBP         (1 << 2)    /* Store Bad Packets */
#define E1000E_RCTL_UPE         (1 << 3)    /* Unicast Promiscuous Enable */
#define E1000E_RCTL_MPE         (1 << 4)    /* Multicast Promiscuous Enable */
#define E1000E_RCTL_LPE         (1 << 5)    /* Long Packet Enable */
#define E1000E_RCTL_LBM_MASK    (3 << 6)    /* Loopback Mode */
#define E1000E_RCTL_RDMTS_HALF  (0 << 8)    /* RX Desc Min Threshold 1/2 */
#define E1000E_RCTL_RDMTS_QUARTER (1 << 8)  /* RX Desc Min Threshold 1/4 */
#define E1000E_RCTL_RDMTS_EIGHTH (2 << 8)   /* RX Desc Min Threshold 1/8 */
#define E1000E_RCTL_MO_MASK     (3 << 12)   /* Multicast Offset */
#define E1000E_RCTL_BAM         (1 << 15)   /* Broadcast Accept Mode */
#define E1000E_RCTL_BSIZE_MASK  (3 << 16)   /* Buffer Size */
#define E1000E_RCTL_BSIZE_2048  (0 << 16)   /* 2048 bytes */
#define E1000E_RCTL_BSIZE_1024  (1 << 16)   /* 1024 bytes */
#define E1000E_RCTL_BSIZE_512   (2 << 16)   /* 512 bytes */
#define E1000E_RCTL_BSIZE_256   (3 << 16)   /* 256 bytes */
#define E1000E_RCTL_VFE         (1 << 18)   /* VLAN Filter Enable */
#define E1000E_RCTL_BSEX        (1 << 25)   /* Buffer Size Extension */
#define E1000E_RCTL_SECRC       (1 << 26)   /* Strip Ethernet CRC */

/* ============================================================================
 * Transmit Control Bits (TCTL)
 * ============================================================================ */

#define E1000E_TCTL_EN          (1 << 1)    /* Transmitter Enable */
#define E1000E_TCTL_PSP         (1 << 3)    /* Pad Short Packets */
#define E1000E_TCTL_CT_SHIFT    4           /* Collision Threshold */
#define E1000E_TCTL_COLD_SHIFT  12          /* Collision Distance */
#define E1000E_TCTL_SWXOFF      (1 << 22)   /* Software XOFF */
#define E1000E_TCTL_RTLC        (1 << 24)   /* Re-transmit on Late Collision */

/* ============================================================================
 * Interrupt Bits
 * ============================================================================ */

#define E1000E_INT_TXDW         (1 << 0)    /* TX Descriptor Written Back */
#define E1000E_INT_TXQE         (1 << 1)    /* TX Queue Empty */
#define E1000E_INT_LSC          (1 << 2)    /* Link Status Change */
#define E1000E_INT_RXSEQ        (1 << 3)    /* RX Sequence Error */
#define E1000E_INT_RXDMT0       (1 << 4)    /* RX Desc Min Threshold */
#define E1000E_INT_RXO          (1 << 6)    /* RX Overrun */
#define E1000E_INT_RXT0         (1 << 7)    /* RX Timer Interrupt */

/* ============================================================================
 * Descriptor Structures
 * ============================================================================ */

/* Receive Descriptor (Legacy) */
typedef struct e1000e_rx_desc {
    uint64_t buffer_addr;   /* Address of receive buffer */
    uint16_t length;        /* Length of received data */
    uint16_t checksum;      /* Packet checksum */
    uint8_t  status;        /* Descriptor status */
    uint8_t  errors;        /* Descriptor errors */
    uint16_t special;       /* Special field (VLAN tag) */
} __packed e1000e_rx_desc_t;

/* Transmit Descriptor (Legacy) */
typedef struct e1000e_tx_desc {
    uint64_t buffer_addr;   /* Address of data buffer */
    uint16_t length;        /* Data buffer length */
    uint8_t  cso;           /* Checksum offset */
    uint8_t  cmd;           /* Command field */
    uint8_t  status;        /* Descriptor status (upper nibble = RSV) */
    uint8_t  css;           /* Checksum start */
    uint16_t special;       /* Special field (VLAN tag) */
} __packed e1000e_tx_desc_t;

/* RX Descriptor Status Bits */
#define E1000E_RXD_STAT_DD      (1 << 0)    /* Descriptor Done */
#define E1000E_RXD_STAT_EOP     (1 << 1)    /* End of Packet */
#define E1000E_RXD_STAT_IXSM    (1 << 2)    /* Ignore Checksum */
#define E1000E_RXD_STAT_VP      (1 << 3)    /* VLAN Packet */

/* TX Descriptor Command Bits */
#define E1000E_TXD_CMD_EOP      (1 << 0)    /* End of Packet */
#define E1000E_TXD_CMD_IFCS     (1 << 1)    /* Insert FCS */
#define E1000E_TXD_CMD_RS       (1 << 3)    /* Report Status */
#define E1000E_TXD_CMD_DEXT     (1 << 5)    /* Descriptor Extension */
#define E1000E_TXD_CMD_VLE      (1 << 6)    /* VLAN Packet Enable */
#define E1000E_TXD_CMD_IDE      (1 << 7)    /* Interrupt Delay Enable */

/* TX Descriptor Status Bits */
#define E1000E_TXD_STAT_DD      (1 << 0)    /* Descriptor Done */

/* ============================================================================
 * Driver Configuration
 * ============================================================================ */

#define E1000E_NUM_RX_DESC      64          /* Number of RX descriptors */
#define E1000E_NUM_TX_DESC      64          /* Number of TX descriptors */
#define E1000E_RX_BUFFER_SIZE   2048        /* RX buffer size */
#define E1000E_TX_BUFFER_SIZE   2048        /* TX buffer size */
#define E1000E_MAX_PACKET       1514        /* Max packet size (MTU + header) */

/* ============================================================================
 * Device Structure
 * ============================================================================ */

typedef struct e1000e_device {
    pci_device_t *pci_dev;          /* PCI device reference */
    volatile void *mmio_base;       /* Memory-mapped I/O base */
    uint64_t mmio_phys;             /* Physical MMIO address */
    size_t mmio_size;               /* MMIO region size */

    /* Descriptor rings */
    e1000e_rx_desc_t *rx_desc;      /* RX descriptor ring */
    e1000e_tx_desc_t *tx_desc;      /* TX descriptor ring */
    uint64_t rx_desc_phys;          /* RX descriptor physical address */
    uint64_t tx_desc_phys;          /* TX descriptor physical address */

    /* RX/TX buffers */
    void *rx_buffers;               /* RX buffer pool */
    void *tx_buffers;               /* TX buffer pool */
    uint64_t rx_buffers_phys;       /* RX buffers physical address */
    uint64_t tx_buffers_phys;       /* TX buffers physical address */

    /* Ring indices */
    uint16_t rx_cur;                /* Current RX descriptor */
    uint16_t tx_cur;                /* Current TX descriptor */
    uint16_t tx_tail;               /* TX tail (next to send) */

    /* Device info */
    uint8_t mac_addr[6];            /* MAC address */
    bool link_up;                   /* Link status */
    uint32_t speed;                 /* Link speed (10/100/1000) */
    bool full_duplex;               /* Full duplex mode */

    /* Statistics */
    uint64_t rx_packets;            /* Packets received */
    uint64_t tx_packets;            /* Packets transmitted */
    uint64_t rx_bytes;              /* Bytes received */
    uint64_t tx_bytes;              /* Bytes transmitted */
    uint64_t rx_errors;             /* Receive errors */
    uint64_t tx_errors;             /* Transmit errors */
    uint64_t rx_dropped;            /* Dropped packets */

    bool initialized;               /* Initialization complete */
} e1000e_device_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define E1000E_OK               0
#define E1000E_ERR_NOT_FOUND    -1
#define E1000E_ERR_INIT         -2
#define E1000E_ERR_NOMEM        -3
#define E1000E_ERR_IO           -4
#define E1000E_ERR_TIMEOUT      -5
#define E1000E_ERR_LINK_DOWN    -6
#define E1000E_ERR_FULL         -7

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize e1000e driver
 * Scans for Intel e1000e devices and initializes the first one found
 * @return E1000E_OK on success, error code on failure
 */
int e1000e_init(void);

/**
 * Check if e1000e is initialized and ready
 * @return true if ready, false otherwise
 */
bool e1000e_is_ready(void);

/**
 * Check if link is up
 * @return true if link is up, false otherwise
 */
bool e1000e_link_up(void);

/**
 * Get link speed
 * @return Link speed in Mbps (10, 100, or 1000), 0 if link down
 */
uint32_t e1000e_get_speed(void);

/**
 * Get MAC address
 * @param mac Buffer to store MAC address (6 bytes)
 */
void e1000e_get_mac(uint8_t *mac);

/**
 * Send a packet
 * @param data    Packet data (Ethernet frame)
 * @param length  Length of packet in bytes
 * @return E1000E_OK on success, error code on failure
 */
int e1000e_send(const void *data, size_t length);

/**
 * Receive a packet (non-blocking)
 * @param buffer  Buffer to store received packet
 * @param max_len Maximum buffer length
 * @return Number of bytes received, 0 if no packet, negative on error
 */
int e1000e_receive(void *buffer, size_t max_len);

/**
 * Poll for received packets
 * @return Number of packets processed
 */
int e1000e_poll(void);

/**
 * Get driver statistics
 * @param rx_packets  Packets received (output, can be NULL)
 * @param tx_packets  Packets transmitted (output, can be NULL)
 * @param rx_bytes    Bytes received (output, can be NULL)
 * @param tx_bytes    Bytes transmitted (output, can be NULL)
 */
void e1000e_get_stats(uint64_t *rx_packets, uint64_t *tx_packets,
                      uint64_t *rx_bytes, uint64_t *tx_bytes);

/**
 * Print device information and statistics
 */
void e1000e_print_info(void);

/**
 * Run driver self-tests
 * @return 0 on success, -1 on failure
 */
int e1000e_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_E1000E_H */
