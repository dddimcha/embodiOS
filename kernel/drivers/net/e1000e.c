/* EMBODIOS Intel e1000e Gigabit Ethernet Driver
 *
 * Driver for Intel GbE controllers (82574L, 82579, I217, I218, I219)
 * commonly found in Intel NUCs and laptops.
 *
 * Implementation Notes:
 * - Uses legacy descriptors for simplicity
 * - MMIO register access
 * - Polling mode (no interrupt handler)
 */

#include <embodios/e1000e.h>
#include <embodios/pci.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* Helper for aligned allocation - uses page-aligned heap allocator */
static inline void *kmalloc_aligned(size_t size, size_t alignment)
{
    return heap_alloc_aligned(size, alignment);
}

static inline void kfree_aligned(void *ptr)
{
    heap_free_aligned(ptr);
}

/* Static device instance */
static e1000e_device_t e1000e_dev;
static bool e1000e_initialized = false;

/* Supported device IDs */
static const uint16_t e1000e_device_ids[] = {
    E1000E_DEV_82574L,
    E1000E_DEV_82579LM,
    E1000E_DEV_82579V,
    E1000E_DEV_I217LM,
    E1000E_DEV_I217V,
    E1000E_DEV_I218LM,
    E1000E_DEV_I218V,
    E1000E_DEV_I219LM,
    E1000E_DEV_I219V,
    E1000E_DEV_I219LM2,
    E1000E_DEV_I219V2,
    E1000E_DEV_I219LM3,
    E1000E_DEV_I219V3,
    0  /* Terminator */
};

/* ============================================================================
 * MMIO Register Access
 * ============================================================================ */

static inline uint32_t e1000e_read(uint32_t reg)
{
    return *(volatile uint32_t *)((uint8_t *)e1000e_dev.mmio_base + reg);
}

static inline void e1000e_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)((uint8_t *)e1000e_dev.mmio_base + reg) = value;
}

/* Memory barrier for MMIO */
static inline void e1000e_flush(void)
{
    (void)e1000e_read(E1000E_STATUS);
}

/* ============================================================================
 * EEPROM Access
 * ============================================================================ */

static uint16_t e1000e_eeprom_read(uint8_t addr)
{
    uint32_t val;

    /* Write address and start read */
    e1000e_write(E1000E_EERD, ((uint32_t)addr << 8) | 1);

    /* Wait for completion */
    for (int i = 0; i < 1000; i++) {
        val = e1000e_read(E1000E_EERD);
        if (val & (1 << 4)) {  /* Done bit */
            return (uint16_t)(val >> 16);
        }
        /* Small delay */
        for (volatile int j = 0; j < 100; j++);
    }

    console_printf("e1000e: EEPROM read timeout\n");
    return 0xFFFF;
}

/* ============================================================================
 * MAC Address
 * ============================================================================ */

static void e1000e_read_mac_address(void)
{
    uint32_t ral, rah;

    /* Try reading from RAL/RAH registers first */
    ral = e1000e_read(E1000E_RAL);
    rah = e1000e_read(E1000E_RAH);

    if (ral != 0 && ral != 0xFFFFFFFF) {
        /* Use registers */
        e1000e_dev.mac_addr[0] = ral & 0xFF;
        e1000e_dev.mac_addr[1] = (ral >> 8) & 0xFF;
        e1000e_dev.mac_addr[2] = (ral >> 16) & 0xFF;
        e1000e_dev.mac_addr[3] = (ral >> 24) & 0xFF;
        e1000e_dev.mac_addr[4] = rah & 0xFF;
        e1000e_dev.mac_addr[5] = (rah >> 8) & 0xFF;
    } else {
        /* Read from EEPROM */
        uint16_t word;

        word = e1000e_eeprom_read(0);
        e1000e_dev.mac_addr[0] = word & 0xFF;
        e1000e_dev.mac_addr[1] = word >> 8;

        word = e1000e_eeprom_read(1);
        e1000e_dev.mac_addr[2] = word & 0xFF;
        e1000e_dev.mac_addr[3] = word >> 8;

        word = e1000e_eeprom_read(2);
        e1000e_dev.mac_addr[4] = word & 0xFF;
        e1000e_dev.mac_addr[5] = word >> 8;
    }

    /* Write MAC address to receive filter */
    ral = e1000e_dev.mac_addr[0] |
          (e1000e_dev.mac_addr[1] << 8) |
          (e1000e_dev.mac_addr[2] << 16) |
          (e1000e_dev.mac_addr[3] << 24);
    rah = e1000e_dev.mac_addr[4] |
          (e1000e_dev.mac_addr[5] << 8) |
          (1 << 31);  /* Address Valid bit */

    e1000e_write(E1000E_RAL, ral);
    e1000e_write(E1000E_RAH, rah);
}

/* ============================================================================
 * Link Management
 * ============================================================================ */

static void e1000e_update_link_status(void)
{
    uint32_t status = e1000e_read(E1000E_STATUS);

    e1000e_dev.link_up = (status & E1000E_STATUS_LU) != 0;
    e1000e_dev.full_duplex = (status & E1000E_STATUS_FD) != 0;

    switch (status & E1000E_STATUS_SPEED_MASK) {
    case E1000E_STATUS_SPEED_10:
        e1000e_dev.speed = 10;
        break;
    case E1000E_STATUS_SPEED_100:
        e1000e_dev.speed = 100;
        break;
    case E1000E_STATUS_SPEED_1000:
        e1000e_dev.speed = 1000;
        break;
    default:
        e1000e_dev.speed = 0;
        break;
    }
}

/* ============================================================================
 * Hardware Reset
 * ============================================================================ */

static void e1000e_reset(void)
{
    uint32_t ctrl;

    /* Disable interrupts */
    e1000e_write(E1000E_IMC, 0xFFFFFFFF);

    /* Disable RX and TX */
    e1000e_write(E1000E_RCTL, 0);
    e1000e_write(E1000E_TCTL, 0);

    /* Reset device */
    ctrl = e1000e_read(E1000E_CTRL);
    e1000e_write(E1000E_CTRL, ctrl | E1000E_CTRL_RST);

    /* Wait for reset to complete */
    for (int i = 0; i < 1000; i++) {
        for (volatile int j = 0; j < 1000; j++);
        ctrl = e1000e_read(E1000E_CTRL);
        if (!(ctrl & E1000E_CTRL_RST)) {
            break;
        }
    }

    /* Disable interrupts again after reset */
    e1000e_write(E1000E_IMC, 0xFFFFFFFF);

    console_printf("e1000e: Device reset complete\n");
}

/* ============================================================================
 * Descriptor Ring Setup
 * ============================================================================ */

static int e1000e_setup_rx(void)
{
    size_t desc_size = E1000E_NUM_RX_DESC * sizeof(e1000e_rx_desc_t);
    size_t buf_size = E1000E_NUM_RX_DESC * E1000E_RX_BUFFER_SIZE;

    /* Allocate descriptor ring (must be 128-byte aligned) */
    e1000e_dev.rx_desc = (e1000e_rx_desc_t *)kmalloc_aligned(desc_size, 128);
    if (!e1000e_dev.rx_desc) {
        console_printf("e1000e: Failed to allocate RX descriptors\n");
        return E1000E_ERR_NOMEM;
    }
    memset(e1000e_dev.rx_desc, 0, desc_size);
    e1000e_dev.rx_desc_phys = (uint64_t)(uintptr_t)e1000e_dev.rx_desc;

    /* Allocate RX buffers */
    e1000e_dev.rx_buffers = kmalloc_aligned(buf_size, 16);
    if (!e1000e_dev.rx_buffers) {
        console_printf("e1000e: Failed to allocate RX buffers\n");
        kfree(e1000e_dev.rx_desc);
        return E1000E_ERR_NOMEM;
    }
    memset(e1000e_dev.rx_buffers, 0, buf_size);
    e1000e_dev.rx_buffers_phys = (uint64_t)(uintptr_t)e1000e_dev.rx_buffers;

    /* Initialize descriptors with buffer addresses */
    for (int i = 0; i < E1000E_NUM_RX_DESC; i++) {
        e1000e_dev.rx_desc[i].buffer_addr = e1000e_dev.rx_buffers_phys +
                                            i * E1000E_RX_BUFFER_SIZE;
        e1000e_dev.rx_desc[i].status = 0;
    }

    /* Configure RX descriptor ring */
    e1000e_write(E1000E_RDBAL, (uint32_t)(e1000e_dev.rx_desc_phys & 0xFFFFFFFF));
    e1000e_write(E1000E_RDBAH, (uint32_t)(e1000e_dev.rx_desc_phys >> 32));
    e1000e_write(E1000E_RDLEN, desc_size);
    e1000e_write(E1000E_RDH, 0);
    e1000e_write(E1000E_RDT, E1000E_NUM_RX_DESC - 1);

    e1000e_dev.rx_cur = 0;

    console_printf("e1000e: RX ring setup at 0x%lx (%d descriptors)\n",
            e1000e_dev.rx_desc_phys, E1000E_NUM_RX_DESC);

    return E1000E_OK;
}

static int e1000e_setup_tx(void)
{
    size_t desc_size = E1000E_NUM_TX_DESC * sizeof(e1000e_tx_desc_t);
    size_t buf_size = E1000E_NUM_TX_DESC * E1000E_TX_BUFFER_SIZE;

    /* Allocate descriptor ring (must be 128-byte aligned) */
    e1000e_dev.tx_desc = (e1000e_tx_desc_t *)kmalloc_aligned(desc_size, 128);
    if (!e1000e_dev.tx_desc) {
        console_printf("e1000e: Failed to allocate TX descriptors\n");
        return E1000E_ERR_NOMEM;
    }
    memset(e1000e_dev.tx_desc, 0, desc_size);
    e1000e_dev.tx_desc_phys = (uint64_t)(uintptr_t)e1000e_dev.tx_desc;

    /* Allocate TX buffers */
    e1000e_dev.tx_buffers = kmalloc_aligned(buf_size, 16);
    if (!e1000e_dev.tx_buffers) {
        console_printf("e1000e: Failed to allocate TX buffers\n");
        kfree(e1000e_dev.tx_desc);
        return E1000E_ERR_NOMEM;
    }
    memset(e1000e_dev.tx_buffers, 0, buf_size);
    e1000e_dev.tx_buffers_phys = (uint64_t)(uintptr_t)e1000e_dev.tx_buffers;

    /* Initialize descriptors with buffer addresses */
    for (int i = 0; i < E1000E_NUM_TX_DESC; i++) {
        e1000e_dev.tx_desc[i].buffer_addr = e1000e_dev.tx_buffers_phys +
                                            i * E1000E_TX_BUFFER_SIZE;
        e1000e_dev.tx_desc[i].status = E1000E_TXD_STAT_DD;  /* Mark as done */
    }

    /* Configure TX descriptor ring */
    e1000e_write(E1000E_TDBAL, (uint32_t)(e1000e_dev.tx_desc_phys & 0xFFFFFFFF));
    e1000e_write(E1000E_TDBAH, (uint32_t)(e1000e_dev.tx_desc_phys >> 32));
    e1000e_write(E1000E_TDLEN, desc_size);
    e1000e_write(E1000E_TDH, 0);
    e1000e_write(E1000E_TDT, 0);

    e1000e_dev.tx_cur = 0;
    e1000e_dev.tx_tail = 0;

    /* Set TX Inter-Packet Gap (standard values for IEEE 802.3) */
    e1000e_write(E1000E_TIPG, (10 << 0) | (8 << 10) | (6 << 20));

    console_printf("e1000e: TX ring setup at 0x%lx (%d descriptors)\n",
            e1000e_dev.tx_desc_phys, E1000E_NUM_TX_DESC);

    return E1000E_OK;
}

/* ============================================================================
 * RX/TX Enable
 * ============================================================================ */

static void e1000e_enable_rx(void)
{
    uint32_t rctl;

    /* Clear multicast table */
    for (int i = 0; i < 128; i++) {
        e1000e_write(E1000E_MTA + i * 4, 0);
    }

    /* Configure receive control */
    rctl = E1000E_RCTL_EN |         /* Enable receiver */
           E1000E_RCTL_BAM |         /* Accept broadcast */
           E1000E_RCTL_BSIZE_2048 |  /* 2KB buffers */
           E1000E_RCTL_SECRC;        /* Strip CRC */

    e1000e_write(E1000E_RCTL, rctl);
    e1000e_flush();

    console_printf("e1000e: Receiver enabled\n");
}

static void e1000e_enable_tx(void)
{
    uint32_t tctl;

    /* Configure transmit control */
    tctl = E1000E_TCTL_EN |                     /* Enable transmitter */
           E1000E_TCTL_PSP |                    /* Pad short packets */
           (15 << E1000E_TCTL_CT_SHIFT) |       /* Collision threshold */
           (64 << E1000E_TCTL_COLD_SHIFT);      /* Collision distance */

    e1000e_write(E1000E_TCTL, tctl);
    e1000e_flush();

    console_printf("e1000e: Transmitter enabled\n");
}

/* ============================================================================
 * Device Detection
 * ============================================================================ */

static pci_device_t *e1000e_find_device(void)
{
    pci_device_t *pci_dev;
    int count = pci_device_count();

    for (int i = 0; i < count; i++) {
        pci_dev = pci_get_device(i);
        if (!pci_dev) continue;

        if (pci_dev->vendor_id != E1000E_VENDOR_INTEL) continue;

        /* Check if device ID matches any supported device */
        for (int j = 0; e1000e_device_ids[j] != 0; j++) {
            if (pci_dev->device_id == e1000e_device_ids[j]) {
                return pci_dev;
            }
        }
    }

    return NULL;
}

static const char *e1000e_device_name(uint16_t device_id)
{
    switch (device_id) {
    case E1000E_DEV_82574L:  return "82574L";
    case E1000E_DEV_82579LM: return "82579LM";
    case E1000E_DEV_82579V:  return "82579V";
    case E1000E_DEV_I217LM:  return "I217-LM";
    case E1000E_DEV_I217V:   return "I217-V";
    case E1000E_DEV_I218LM:  return "I218-LM";
    case E1000E_DEV_I218V:   return "I218-V";
    case E1000E_DEV_I219LM:  return "I219-LM";
    case E1000E_DEV_I219V:   return "I219-V";
    case E1000E_DEV_I219LM2: return "I219-LM (2)";
    case E1000E_DEV_I219V2:  return "I219-V (2)";
    case E1000E_DEV_I219LM3: return "I219-LM (3)";
    case E1000E_DEV_I219V3:  return "I219-V (3)";
    default:                 return "Unknown";
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int e1000e_init(void)
{
    pci_device_t *pci_dev;
    int ret;

    if (e1000e_initialized) {
        return E1000E_OK;
    }

    memset(&e1000e_dev, 0, sizeof(e1000e_dev));

    console_printf("e1000e: Scanning for Intel GbE controllers...\n");

    /* Find device */
    pci_dev = e1000e_find_device();
    if (!pci_dev) {
        console_printf("e1000e: No supported device found\n");
        return E1000E_ERR_NOT_FOUND;
    }

    e1000e_dev.pci_dev = pci_dev;

    console_printf("e1000e: Found Intel %s at %02x:%02x.%d\n",
            e1000e_device_name(pci_dev->device_id),
            pci_dev->addr.bus, pci_dev->addr.device, pci_dev->addr.function);

    /* Get MMIO base address from BAR0 */
    if (pci_dev->bar[0] == 0) {
        console_printf("e1000e: BAR0 not configured\n");
        return E1000E_ERR_INIT;
    }

    e1000e_dev.mmio_phys = pci_bar_address(pci_dev, 0);
    e1000e_dev.mmio_size = pci_bar_size(pci_dev, 0);

    if (e1000e_dev.mmio_size == 0) {
        e1000e_dev.mmio_size = 128 * 1024;  /* Default 128KB */
    }

    /* Map MMIO region (identity mapping for kernel) */
    e1000e_dev.mmio_base = (void *)(uintptr_t)e1000e_dev.mmio_phys;

    console_printf("e1000e: MMIO at 0x%lx, size %lu KB\n",
            e1000e_dev.mmio_phys, e1000e_dev.mmio_size / 1024);

    /* Enable PCI bus mastering and memory space */
    pci_enable_bus_master(pci_dev);
    pci_enable_memory(pci_dev);

    /* Reset device */
    e1000e_reset();

    /* Read MAC address */
    e1000e_read_mac_address();

    console_printf("e1000e: MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
            e1000e_dev.mac_addr[0], e1000e_dev.mac_addr[1],
            e1000e_dev.mac_addr[2], e1000e_dev.mac_addr[3],
            e1000e_dev.mac_addr[4], e1000e_dev.mac_addr[5]);

    /* Setup descriptor rings */
    ret = e1000e_setup_rx();
    if (ret != E1000E_OK) {
        return ret;
    }

    ret = e1000e_setup_tx();
    if (ret != E1000E_OK) {
        kfree(e1000e_dev.rx_desc);
        kfree(e1000e_dev.rx_buffers);
        return ret;
    }

    /* Enable RX and TX */
    e1000e_enable_rx();
    e1000e_enable_tx();

    /* Set link up */
    uint32_t ctrl = e1000e_read(E1000E_CTRL);
    ctrl |= E1000E_CTRL_SLU;  /* Set Link Up */
    ctrl &= ~E1000E_CTRL_LRST;  /* Clear link reset */
    e1000e_write(E1000E_CTRL, ctrl);

    /* Wait for link */
    for (int i = 0; i < 100; i++) {
        for (volatile int j = 0; j < 10000; j++);
        e1000e_update_link_status();
        if (e1000e_dev.link_up) {
            break;
        }
    }

    e1000e_dev.initialized = true;
    e1000e_initialized = true;

    if (e1000e_dev.link_up) {
        console_printf("e1000e: Link up at %u Mbps %s duplex\n",
                e1000e_dev.speed,
                e1000e_dev.full_duplex ? "full" : "half");
    } else {
        console_printf("e1000e: Link down (cable not connected?)\n");
    }

    console_printf("e1000e: Initialization complete\n");
    return E1000E_OK;
}

bool e1000e_is_ready(void)
{
    return e1000e_initialized && e1000e_dev.initialized;
}

bool e1000e_link_up(void)
{
    if (!e1000e_initialized) return false;
    e1000e_update_link_status();
    return e1000e_dev.link_up;
}

uint32_t e1000e_get_speed(void)
{
    if (!e1000e_initialized) return 0;
    e1000e_update_link_status();
    return e1000e_dev.link_up ? e1000e_dev.speed : 0;
}

void e1000e_get_mac(uint8_t *mac)
{
    if (!mac) return;

    if (e1000e_initialized) {
        memcpy(mac, e1000e_dev.mac_addr, 6);
    } else {
        memset(mac, 0, 6);
    }
}

int e1000e_send(const void *data, size_t length)
{
    if (!e1000e_initialized) return E1000E_ERR_INIT;
    if (!data || length == 0) return E1000E_ERR_IO;
    if (length > E1000E_MAX_PACKET) return E1000E_ERR_IO;

    /* Check link status */
    if (!e1000e_dev.link_up) {
        e1000e_update_link_status();
        if (!e1000e_dev.link_up) {
            return E1000E_ERR_LINK_DOWN;
        }
    }

    uint16_t tx_idx = e1000e_dev.tx_tail;
    e1000e_tx_desc_t *desc = &e1000e_dev.tx_desc[tx_idx];

    /* Wait for descriptor to be available */
    int timeout = 1000;
    while (!(desc->status & E1000E_TXD_STAT_DD) && timeout > 0) {
        for (volatile int i = 0; i < 100; i++);
        timeout--;
    }
    if (timeout == 0) {
        e1000e_dev.tx_errors++;
        return E1000E_ERR_TIMEOUT;
    }

    /* Copy data to TX buffer */
    void *buf = (void *)((uintptr_t)e1000e_dev.tx_buffers +
                         tx_idx * E1000E_TX_BUFFER_SIZE);
    memcpy(buf, data, length);

    /* Setup descriptor */
    desc->length = (uint16_t)length;
    desc->cmd = E1000E_TXD_CMD_EOP | E1000E_TXD_CMD_IFCS | E1000E_TXD_CMD_RS;
    desc->status = 0;

    /* Update tail pointer */
    e1000e_dev.tx_tail = (tx_idx + 1) % E1000E_NUM_TX_DESC;
    e1000e_write(E1000E_TDT, e1000e_dev.tx_tail);

    /* Update statistics */
    e1000e_dev.tx_packets++;
    e1000e_dev.tx_bytes += length;

    return E1000E_OK;
}

int e1000e_receive(void *buffer, size_t max_len)
{
    if (!e1000e_initialized) return E1000E_ERR_INIT;
    if (!buffer || max_len == 0) return E1000E_ERR_IO;

    uint16_t rx_idx = e1000e_dev.rx_cur;
    e1000e_rx_desc_t *desc = &e1000e_dev.rx_desc[rx_idx];

    /* Check if descriptor has data */
    if (!(desc->status & E1000E_RXD_STAT_DD)) {
        return 0;  /* No packet available */
    }

    /* Check for errors */
    if (desc->errors) {
        e1000e_dev.rx_errors++;
        /* Reset descriptor for reuse */
        desc->status = 0;
        e1000e_dev.rx_cur = (rx_idx + 1) % E1000E_NUM_RX_DESC;
        e1000e_write(E1000E_RDT, rx_idx);
        return E1000E_ERR_IO;
    }

    /* Get packet length */
    uint16_t length = desc->length;
    if (length > max_len) {
        length = max_len;
    }

    /* Copy data from RX buffer */
    void *buf = (void *)((uintptr_t)e1000e_dev.rx_buffers +
                         rx_idx * E1000E_RX_BUFFER_SIZE);
    memcpy(buffer, buf, length);

    /* Reset descriptor for reuse */
    desc->status = 0;

    /* Update RX index and tail pointer */
    e1000e_dev.rx_cur = (rx_idx + 1) % E1000E_NUM_RX_DESC;
    e1000e_write(E1000E_RDT, rx_idx);

    /* Update statistics */
    e1000e_dev.rx_packets++;
    e1000e_dev.rx_bytes += length;

    return length;
}

int e1000e_poll(void)
{
    if (!e1000e_initialized) return 0;

    int packets = 0;
    uint8_t dummy_buffer[E1000E_MAX_PACKET];

    /* Process all available packets */
    while (true) {
        uint16_t rx_idx = e1000e_dev.rx_cur;
        e1000e_rx_desc_t *desc = &e1000e_dev.rx_desc[rx_idx];

        if (!(desc->status & E1000E_RXD_STAT_DD)) {
            break;
        }

        /* Process packet (just count it for now) */
        e1000e_receive(dummy_buffer, sizeof(dummy_buffer));
        packets++;
    }

    return packets;
}

void e1000e_get_stats(uint64_t *rx_packets, uint64_t *tx_packets,
                      uint64_t *rx_bytes, uint64_t *tx_bytes)
{
    if (rx_packets) *rx_packets = e1000e_dev.rx_packets;
    if (tx_packets) *tx_packets = e1000e_dev.tx_packets;
    if (rx_bytes) *rx_bytes = e1000e_dev.rx_bytes;
    if (tx_bytes) *tx_bytes = e1000e_dev.tx_bytes;
}

void e1000e_print_info(void)
{
    if (!e1000e_initialized) {
        console_printf("e1000e: Not initialized\n");
        return;
    }

    console_printf("\n=== Intel e1000e Network Controller ===\n");
    console_printf("Device: Intel %s\n", e1000e_device_name(e1000e_dev.pci_dev->device_id));
    console_printf("PCI Address: %02x:%02x.%d\n",
            e1000e_dev.pci_dev->addr.bus,
            e1000e_dev.pci_dev->addr.device,
            e1000e_dev.pci_dev->addr.function);
    console_printf("MMIO Base: 0x%lx\n", e1000e_dev.mmio_phys);
    console_printf("MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
            e1000e_dev.mac_addr[0], e1000e_dev.mac_addr[1],
            e1000e_dev.mac_addr[2], e1000e_dev.mac_addr[3],
            e1000e_dev.mac_addr[4], e1000e_dev.mac_addr[5]);

    e1000e_update_link_status();
    if (e1000e_dev.link_up) {
        console_printf("Link: UP at %u Mbps %s duplex\n",
                e1000e_dev.speed,
                e1000e_dev.full_duplex ? "full" : "half");
    } else {
        console_printf("Link: DOWN\n");
    }

    console_printf("\nStatistics:\n");
    console_printf("  RX Packets: %lu\n", e1000e_dev.rx_packets);
    console_printf("  TX Packets: %lu\n", e1000e_dev.tx_packets);
    console_printf("  RX Bytes: %lu\n", e1000e_dev.rx_bytes);
    console_printf("  TX Bytes: %lu\n", e1000e_dev.tx_bytes);
    console_printf("  RX Errors: %lu\n", e1000e_dev.rx_errors);
    console_printf("  TX Errors: %lu\n", e1000e_dev.tx_errors);

    /* Read hardware statistics */
    console_printf("\nHardware Statistics:\n");
    console_printf("  CRC Errors: %u\n", e1000e_read(E1000E_CRCERRS));
    console_printf("  Missed Packets: %u\n", e1000e_read(E1000E_MPC));
    console_printf("  Good RX Packets: %u\n", e1000e_read(E1000E_GPRC));
    console_printf("  Good TX Packets: %u\n", e1000e_read(E1000E_GPTC));
    console_printf("  Collisions: %u\n", e1000e_read(E1000E_COLC));
}

int e1000e_run_tests(void)
{
    console_printf("\n=== e1000e Driver Self-Tests ===\n");

    if (!e1000e_initialized) {
        console_printf("TEST: Initialization... ");
        int ret = e1000e_init();
        if (ret != E1000E_OK && ret != E1000E_ERR_NOT_FOUND) {
            console_printf("FAILED (error %d)\n", ret);
            return -1;
        }
        if (ret == E1000E_ERR_NOT_FOUND) {
            console_printf("SKIPPED (no device)\n");
            return 0;
        }
        console_printf("PASSED\n");
    }

    /* Test register access */
    console_printf("TEST: Register access... ");
    uint32_t status = e1000e_read(E1000E_STATUS);
    if (status == 0xFFFFFFFF) {
        console_printf("FAILED (invalid status)\n");
        return -1;
    }
    console_printf("PASSED (status=0x%08x)\n", status);

    /* Test MAC address */
    console_printf("TEST: MAC address... ");
    uint8_t mac[6];
    e1000e_get_mac(mac);
    bool mac_valid = false;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0 && mac[i] != 0xFF) {
            mac_valid = true;
            break;
        }
    }
    if (!mac_valid) {
        console_printf("FAILED (invalid MAC)\n");
        return -1;
    }
    console_printf("PASSED (%02x:%02x:%02x:%02x:%02x:%02x)\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Test link status */
    console_printf("TEST: Link status... ");
    e1000e_update_link_status();
    console_printf("PASSED (link %s)\n", e1000e_dev.link_up ? "UP" : "DOWN");

    /* Test RX ring */
    console_printf("TEST: RX ring... ");
    uint32_t rdlen = e1000e_read(E1000E_RDLEN);
    if (rdlen != E1000E_NUM_RX_DESC * sizeof(e1000e_rx_desc_t)) {
        console_printf("FAILED (rdlen mismatch)\n");
        return -1;
    }
    console_printf("PASSED\n");

    /* Test TX ring */
    console_printf("TEST: TX ring... ");
    uint32_t tdlen = e1000e_read(E1000E_TDLEN);
    if (tdlen != E1000E_NUM_TX_DESC * sizeof(e1000e_tx_desc_t)) {
        console_printf("FAILED (tdlen mismatch)\n");
        return -1;
    }
    console_printf("PASSED\n");

    console_printf("=== All e1000e tests passed ===\n");
    return 0;
}
