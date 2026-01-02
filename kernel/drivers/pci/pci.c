/* EMBODIOS PCI Subsystem Implementation
 *
 * Provides PCI device enumeration and driver registration for
 * discovering and configuring PCI devices at boot.
 *
 * Implementation notes:
 * - Uses legacy I/O port method (0xCF8/0xCFC) for config access
 * - Scans all 256 buses, 32 devices per bus, 8 functions per device
 * - Stores up to PCI_MAX_STORED devices
 * - Supports driver registration with vendor/device/class matching
 */

#include <embodios/pci.h>
#include <embodios/console.h>
#include <embodios/kernel.h>
#include <embodios/mm.h>
#include "pci_ids.h"

/* ============================================================================
 * Port I/O Functions (x86_64 specific)
 * ============================================================================ */

#ifdef __x86_64__

/**
 * Write 32-bit value to I/O port
 */
static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

/**
 * Read 32-bit value from I/O port
 */
static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/**
 * Write 16-bit value to I/O port
 * Note: Currently unused but provided for future driver use
 */
static inline __attribute__((unused))
void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

/**
 * Read 16-bit value from I/O port
 * Note: Currently unused but provided for future driver use
 */
static inline __attribute__((unused))
uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/**
 * Write 8-bit value to I/O port
 * Note: Currently unused but provided for future driver use
 */
static inline __attribute__((unused))
void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/**
 * Read 8-bit value from I/O port
 * Note: Currently unused but provided for future driver use
 */
static inline __attribute__((unused))
uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#else /* Non-x86_64 architectures */

/* Stubs for non-x86_64 architectures - PCI not supported */
static inline void outl(uint16_t port, uint32_t value)
{
    (void)port;
    (void)value;
}

static inline uint32_t inl(uint16_t port)
{
    (void)port;
    return 0xFFFFFFFF;
}

static inline __attribute__((unused))
void outw(uint16_t port, uint16_t value)
{
    (void)port;
    (void)value;
}

static inline __attribute__((unused))
uint16_t inw(uint16_t port)
{
    (void)port;
    return 0xFFFF;
}

static inline __attribute__((unused))
void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

static inline __attribute__((unused))
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0xFF;
}

#endif /* __x86_64__ */

/* ============================================================================
 * Lookup Tables (PCI ID Database)
 * ============================================================================ */

/**
 * Vendor name lookup table
 */
static const pci_vendor_entry_t pci_vendor_table[] = {
    { PCI_VENDOR_INTEL,     "Intel" },
    { PCI_VENDOR_AMD,       "AMD" },
    { PCI_VENDOR_NVIDIA,    "NVIDIA" },
    { PCI_VENDOR_QEMU,      "QEMU/Bochs" },
    { PCI_VENDOR_VIRTIO,    "VirtIO" },
    { PCI_VENDOR_REDHAT,    "Red Hat" },
    { PCI_VENDOR_REALTEK,   "Realtek" },
    { PCI_VENDOR_BROADCOM,  "Broadcom" },
    { PCI_VENDOR_QUALCOMM,  "Qualcomm" },
    { PCI_VENDOR_VMWARE,    "VMware" },
    { PCI_VENDOR_VIA,       "VIA" },
    { PCI_VENDOR_SIS,       "SiS" },
    { PCI_VENDOR_MARVELL,   "Marvell" },
    { PCI_VENDOR_SAMSUNG,   "Samsung" },
    { PCI_VENDOR_SANDISK,   "SanDisk" },
    { PCI_VENDOR_TOSHIBA,   "Toshiba" },
    { PCI_VENDOR_MICRON,    "Micron" },
    { PCI_VENDOR_SEAGATE,   "Seagate" },
    { 0, NULL }  /* Sentinel */
};

/**
 * Class name lookup table
 */
static const char *pci_class_names[] = {
    /* 0x00 */ "Unclassified",
    /* 0x01 */ "Storage",
    /* 0x02 */ "Network",
    /* 0x03 */ "Display",
    /* 0x04 */ "Multimedia",
    /* 0x05 */ "Memory",
    /* 0x06 */ "Bridge",
    /* 0x07 */ "Communication",
    /* 0x08 */ "System",
    /* 0x09 */ "Input",
    /* 0x0A */ "Docking",
    /* 0x0B */ "Processor",
    /* 0x0C */ "Serial Bus",
    /* 0x0D */ "Wireless",
    /* 0x0E */ "Intelligent I/O",
    /* 0x0F */ "Satellite",
    /* 0x10 */ "Encryption",
    /* 0x11 */ "Signal Processing"
};

#define PCI_CLASS_NAME_COUNT \
    (sizeof(pci_class_names) / sizeof(pci_class_names[0]))

/**
 * Storage subclass names
 */
static const char *pci_storage_subclass_names[] = {
    /* 0x00 */ "SCSI",
    /* 0x01 */ "IDE",
    /* 0x02 */ "Floppy",
    /* 0x03 */ "IPI",
    /* 0x04 */ "RAID",
    /* 0x05 */ "ATA",
    /* 0x06 */ "SATA",
    /* 0x07 */ "SAS",
    /* 0x08 */ "NVMe"
};

/**
 * Bridge subclass names
 */
static const char *pci_bridge_subclass_names[] = {
    /* 0x00 */ "Host",
    /* 0x01 */ "ISA",
    /* 0x02 */ "EISA",
    /* 0x03 */ "MCA",
    /* 0x04 */ "PCI-PCI",
    /* 0x05 */ "PCMCIA",
    /* 0x06 */ "NuBus",
    /* 0x07 */ "CardBus"
};

/**
 * Network subclass names
 */
static const char *pci_network_subclass_names[] = {
    /* 0x00 */ "Ethernet",
    /* 0x01 */ "Token Ring",
    /* 0x02 */ "FDDI",
    /* 0x03 */ "ATM",
    /* 0x04 */ "ISDN",
    /* 0x05 */ "WorldFip",
    /* 0x06 */ "PICMG"
};

/**
 * Serial bus subclass names
 */
static const char *pci_serial_subclass_names[] = {
    /* 0x00 */ "FireWire",
    /* 0x01 */ "ACCESS",
    /* 0x02 */ "SSA",
    /* 0x03 */ "USB",
    /* 0x04 */ "Fibre Channel",
    /* 0x05 */ "SMBus"
};

/* ============================================================================
 * Internal State
 * ============================================================================ */

/**
 * PCI subsystem internal state
 */
typedef struct pci_state {
    bool            initialized;
    pci_device_t    devices[PCI_MAX_STORED];
    int             device_count;
    pci_driver_t    *drivers;
    int             driver_count;
    pci_stats_t     stats;
} pci_state_t;

static pci_state_t g_pci = {0};

/* ============================================================================
 * Configuration Space Access
 * ============================================================================ */

/**
 * Build PCI configuration address from BDF and register offset
 *
 * @param addr      Device address (bus/device/function)
 * @param offset    Register offset (0-255)
 * @return          Configuration address for CONFIG_ADDRESS port
 */
static uint32_t pci_config_address(pci_addr_t addr, uint8_t offset)
{
    return 0x80000000 |
           ((uint32_t)addr.bus << 16) |
           ((uint32_t)addr.device << 11) |
           ((uint32_t)addr.function << 8) |
           (offset & 0xFC);
}

uint32_t pci_config_read32(pci_addr_t addr, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(pci_addr_t addr, uint8_t offset)
{
    uint32_t value;

    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    value = inl(PCI_CONFIG_DATA);

    /* Extract correct 16-bit portion based on offset */
    return (uint16_t)(value >> ((offset & 2) * 8));
}

uint8_t pci_config_read8(pci_addr_t addr, uint8_t offset)
{
    uint32_t value;

    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    value = inl(PCI_CONFIG_DATA);

    /* Extract correct 8-bit portion based on offset */
    return (uint8_t)(value >> ((offset & 3) * 8));
}

void pci_config_write32(pci_addr_t addr, uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(pci_addr_t addr, uint8_t offset, uint16_t value)
{
    uint32_t old;
    uint32_t new_val;
    uint32_t mask;
    int shift;

    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));

    /* Read-modify-write to preserve other 16-bit portion */
    old = inl(PCI_CONFIG_DATA);
    shift = (offset & 2) * 8;
    mask = 0xFFFF << shift;
    new_val = (old & ~mask) | ((uint32_t)value << shift);

    outl(PCI_CONFIG_DATA, new_val);
}

void pci_config_write8(pci_addr_t addr, uint8_t offset, uint8_t value)
{
    uint32_t old;
    uint32_t new_val;
    uint32_t mask;
    int shift;

    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));

    /* Read-modify-write to preserve other bytes */
    old = inl(PCI_CONFIG_DATA);
    shift = (offset & 3) * 8;
    mask = 0xFF << shift;
    new_val = (old & ~mask) | ((uint32_t)value << shift);

    outl(PCI_CONFIG_DATA, new_val);
}

/* ============================================================================
 * Device Storage
 * ============================================================================ */

/**
 * Store a discovered device in the device table
 *
 * @param addr  Device address to store
 * @return      PCI_OK on success, error code on failure
 */
static int pci_store_device(pci_addr_t addr)
{
    pci_device_t *dev;
    uint32_t id;
    uint32_t class_info;
    uint32_t subsys;
    int i;

    if (g_pci.device_count >= PCI_MAX_STORED) {
        return PCI_ERR_FULL;
    }

    dev = &g_pci.devices[g_pci.device_count];

    /* Read device identification */
    id = pci_config_read32(addr, PCI_VENDOR_ID);
    dev->vendor_id = id & 0xFFFF;
    dev->device_id = (id >> 16) & 0xFFFF;

    /* Verify device exists */
    if (dev->vendor_id == PCI_VENDOR_INVALID) {
        return PCI_ERR_NOT_FOUND;
    }

    dev->addr = addr;

    /* Read class information */
    class_info = pci_config_read32(addr, PCI_REVISION);
    dev->revision = class_info & 0xFF;
    dev->prog_if = (class_info >> 8) & 0xFF;
    dev->subclass = (class_info >> 16) & 0xFF;
    dev->class_code = (class_info >> 24) & 0xFF;

    /* Read header type */
    dev->header_type = pci_config_read8(addr, PCI_HEADER_TYPE) &
                       PCI_HEADER_TYPE_MASK;
    dev->multifunction = (pci_config_read8(addr, PCI_HEADER_TYPE) &
                          PCI_HEADER_MULTIFUNCTION) != 0;

    /* Read interrupt info */
    dev->interrupt_line = pci_config_read8(addr, PCI_INTERRUPT_LINE);
    dev->interrupt_pin = pci_config_read8(addr, PCI_INTERRUPT_PIN);

    /* Read subsystem info and BARs based on header type */
    if (dev->header_type == PCI_HEADER_ENDPOINT) {
        subsys = pci_config_read32(addr, PCI_SUBSYSTEM_VENDOR);
        dev->subsystem_vendor = subsys & 0xFFFF;
        dev->subsystem_id = (subsys >> 16) & 0xFFFF;

        /* Read all 6 BARs for endpoint devices */
        for (i = 0; i < 6; i++) {
            dev->bar[i] = pci_config_read32(addr, PCI_BAR0 + (i * 4));
        }
    } else {
        /* Bridges have limited config space */
        dev->subsystem_vendor = 0;
        dev->subsystem_id = 0;

        /* Bridges only have 2 BARs */
        dev->bar[0] = pci_config_read32(addr, PCI_BAR0);
        dev->bar[1] = pci_config_read32(addr, PCI_BAR1);
        for (i = 2; i < 6; i++) {
            dev->bar[i] = 0;
        }
    }

    dev->driver = NULL;

    g_pci.device_count++;
    g_pci.stats.devices_found++;

    /* Track bridge devices */
    if (dev->class_code == PCI_CLASS_BRIDGE) {
        g_pci.stats.bridges_found++;
    }

    return PCI_OK;
}

/* ============================================================================
 * Initialization and Enumeration
 * ============================================================================ */

int pci_init(void)
{
    int count;

    if (g_pci.initialized) {
        return PCI_ERR_ALREADY_INIT;
    }

    /* Clear all state */
    memset(&g_pci, 0, sizeof(g_pci));

    console_printf("[PCI] Initializing PCI subsystem...\n");

    g_pci.initialized = true;

    /* Enumerate all PCI devices */
    count = pci_enumerate();

    console_printf("[PCI] Subsystem initialized: %d devices found\n", count);

    return PCI_OK;
}

bool pci_is_initialized(void)
{
    return g_pci.initialized;
}

int pci_enumerate(void)
{
    int bus;
    int dev;
    int func;
    int max_func;
    int buses_with_devices = 0;
    bool found_on_bus;
    pci_addr_t addr;
    uint16_t vendor;
    uint8_t header;

    if (!g_pci.initialized) {
        return PCI_ERR_NOT_INIT;
    }

    console_printf("[PCI] Scanning PCI buses...\n");

    for (bus = 0; bus < PCI_MAX_BUSES; bus++) {
        found_on_bus = false;

        for (dev = 0; dev < PCI_MAX_DEVICES; dev++) {
            addr.bus = (uint8_t)bus;
            addr.device = (uint8_t)dev;
            addr.function = 0;

            /* Check if device exists */
            vendor = pci_config_read16(addr, PCI_VENDOR_ID);
            if (vendor == PCI_VENDOR_INVALID) {
                continue;
            }

            found_on_bus = true;

            /* Check for multifunction device */
            header = pci_config_read8(addr, PCI_HEADER_TYPE);
            max_func = (header & PCI_HEADER_MULTIFUNCTION) ?
                       PCI_MAX_FUNCTIONS : 1;

            for (func = 0; func < max_func; func++) {
                addr.function = (uint8_t)func;

                /* Verify function exists (for func > 0) */
                if (func > 0) {
                    vendor = pci_config_read16(addr, PCI_VENDOR_ID);
                    if (vendor == PCI_VENDOR_INVALID) {
                        continue;
                    }
                }

                /* Store device info */
                if (pci_store_device(addr) != PCI_OK) {
                    console_printf("[PCI] Warning: Device table full\n");
                    goto done;
                }
            }
        }

        if (found_on_bus) {
            buses_with_devices++;
        }

        /* Optimization: skip remaining buses if bus 0 is empty */
        if (bus == 0 && !found_on_bus) {
            console_printf("[PCI] No devices on bus 0, stopping scan\n");
            break;
        }
    }

done:
    g_pci.stats.buses_scanned = buses_with_devices;

    console_printf("[PCI] Enumeration complete: %d devices on %d bus(es)\n",
                   g_pci.device_count, buses_with_devices);

    return g_pci.device_count;
}

/* ============================================================================
 * Device Lookup
 * ============================================================================ */

pci_device_t *pci_get_device(int index)
{
    if (index < 0 || index >= g_pci.device_count) {
        return NULL;
    }
    return &g_pci.devices[index];
}

pci_device_t *pci_find_device(uint16_t vendor, uint16_t device)
{
    int i;
    pci_device_t *dev;

    for (i = 0; i < g_pci.device_count; i++) {
        dev = &g_pci.devices[i];
        if (dev->vendor_id == vendor && dev->device_id == device) {
            return dev;
        }
    }
    return NULL;
}

pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    int i;
    pci_device_t *dev;

    for (i = 0; i < g_pci.device_count; i++) {
        dev = &g_pci.devices[i];
        if (dev->class_code == class_code) {
            if (subclass == PCI_ANY_CLASS || dev->subclass == subclass) {
                return dev;
            }
        }
    }
    return NULL;
}

int pci_device_count(void)
{
    return g_pci.device_count;
}

/* ============================================================================
 * Driver Framework
 * ============================================================================ */

/**
 * Check if a driver matches a device
 *
 * @param drv   Driver to check
 * @param dev   Device to match against
 * @return      true if driver matches device
 */
static bool pci_driver_matches(pci_driver_t *drv, pci_device_t *dev)
{
    if (drv->vendor_id != PCI_ANY_ID && drv->vendor_id != dev->vendor_id) {
        return false;
    }
    if (drv->device_id != PCI_ANY_ID && drv->device_id != dev->device_id) {
        return false;
    }
    if (drv->class_code != PCI_ANY_CLASS && drv->class_code != dev->class_code) {
        return false;
    }
    if (drv->subclass != PCI_ANY_CLASS && drv->subclass != dev->subclass) {
        return false;
    }
    return true;
}

int pci_register_driver(pci_driver_t *driver)
{
    int i;
    pci_device_t *dev;

    if (!g_pci.initialized) {
        return PCI_ERR_NOT_INIT;
    }

    if (!driver || !driver->name || !driver->probe) {
        return PCI_ERR_INVALID;
    }

    if (g_pci.driver_count >= PCI_MAX_DRIVERS) {
        return PCI_ERR_FULL;
    }

    /* Add to driver list (head insertion) */
    driver->next = g_pci.drivers;
    g_pci.drivers = driver;
    g_pci.driver_count++;
    g_pci.stats.drivers_registered++;

    console_printf("[PCI] Registered driver: %s\n", driver->name);

    /* Probe all matching unbound devices */
    for (i = 0; i < g_pci.device_count; i++) {
        dev = &g_pci.devices[i];

        if (dev->driver != NULL) {
            continue;  /* Already has a driver */
        }

        if (pci_driver_matches(driver, dev)) {
            console_printf("[PCI] Probing %02x:%02x.%x with %s\n",
                           dev->addr.bus, dev->addr.device,
                           dev->addr.function, driver->name);

            if (driver->probe(dev) == 0) {
                dev->driver = driver;
                g_pci.stats.devices_bound++;
                console_printf("[PCI] Device bound to %s\n", driver->name);
            }
        }
    }

    return PCI_OK;
}

void pci_unregister_driver(pci_driver_t *driver)
{
    int i;
    pci_device_t *dev;
    pci_driver_t **pp;

    if (!driver) {
        return;
    }

    /* Unbind from all devices */
    for (i = 0; i < g_pci.device_count; i++) {
        dev = &g_pci.devices[i];

        if (dev->driver == driver) {
            if (driver->remove) {
                driver->remove(dev);
            }
            dev->driver = NULL;
            g_pci.stats.devices_bound--;
        }
    }

    /* Remove from driver list */
    pp = &g_pci.drivers;
    while (*pp) {
        if (*pp == driver) {
            *pp = driver->next;
            g_pci.driver_count--;
            break;
        }
        pp = &(*pp)->next;
    }

    console_printf("[PCI] Unregistered driver: %s\n", driver->name);
}

/* ============================================================================
 * Device Control
 * ============================================================================ */

void pci_enable_bus_master(pci_device_t *dev)
{
    uint16_t cmd;

    if (!dev) {
        return;
    }

    cmd = pci_config_read16(dev->addr, PCI_COMMAND);
    cmd |= PCI_COMMAND_MASTER;
    pci_config_write16(dev->addr, PCI_COMMAND, cmd);
}

void pci_enable_memory(pci_device_t *dev)
{
    uint16_t cmd;

    if (!dev) {
        return;
    }

    cmd = pci_config_read16(dev->addr, PCI_COMMAND);
    cmd |= PCI_COMMAND_MEMORY;
    pci_config_write16(dev->addr, PCI_COMMAND, cmd);
}

void pci_enable_io(pci_device_t *dev)
{
    uint16_t cmd;

    if (!dev) {
        return;
    }

    cmd = pci_config_read16(dev->addr, PCI_COMMAND);
    cmd |= PCI_COMMAND_IO;
    pci_config_write16(dev->addr, PCI_COMMAND, cmd);
}

/* ============================================================================
 * BAR Access
 * ============================================================================ */

uint64_t pci_bar_address(pci_device_t *dev, int bar_index)
{
    uint32_t bar;
    uint64_t addr;

    if (!dev || bar_index < 0 || bar_index > 5) {
        return 0;
    }

    bar = dev->bar[bar_index];
    if (bar == 0) {
        return 0;
    }

    if (bar & PCI_BAR_IO) {
        /* I/O space BAR */
        return bar & PCI_BAR_IO_MASK;
    }

    /* Memory space BAR */
    addr = bar & PCI_BAR_MEM_MASK;

    /* Check for 64-bit BAR */
    if ((bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64 && bar_index < 5) {
        addr |= ((uint64_t)dev->bar[bar_index + 1]) << 32;
    }

    return addr;
}

size_t pci_bar_size(pci_device_t *dev, int bar_index)
{
    uint32_t original;
    uint32_t sized;
    uint32_t mask;

    if (!dev || bar_index < 0 || bar_index > 5) {
        return 0;
    }

    /* Save original BAR value */
    original = pci_config_read32(dev->addr, PCI_BAR0 + (bar_index * 4));
    if (original == 0) {
        return 0;
    }

    /* Write all 1s to determine size */
    pci_config_write32(dev->addr, PCI_BAR0 + (bar_index * 4), 0xFFFFFFFF);

    /* Read back size mask */
    sized = pci_config_read32(dev->addr, PCI_BAR0 + (bar_index * 4));

    /* Restore original value */
    pci_config_write32(dev->addr, PCI_BAR0 + (bar_index * 4), original);

    if (sized == 0) {
        return 0;
    }

    /* Calculate size from mask */
    mask = (original & PCI_BAR_IO) ? PCI_BAR_IO_MASK : PCI_BAR_MEM_MASK;
    sized &= mask;
    sized = ~sized + 1;  /* Two's complement gives size */

    return sized;
}

bool pci_bar_is_io(pci_device_t *dev, int bar_index)
{
    if (!dev || bar_index < 0 || bar_index > 5) {
        return false;
    }

    return (dev->bar[bar_index] & PCI_BAR_IO) != 0;
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

const char *pci_class_name(uint8_t class_code)
{
    if (class_code < PCI_CLASS_NAME_COUNT) {
        return pci_class_names[class_code];
    }
    return "Unknown";
}

const char *pci_vendor_name(uint16_t vendor_id)
{
    int i;

    for (i = 0; pci_vendor_table[i].name != NULL; i++) {
        if (pci_vendor_table[i].vendor_id == vendor_id) {
            return pci_vendor_table[i].name;
        }
    }
    return "Unknown";
}

/**
 * Get human-readable subclass name
 *
 * @param class_code    Base class code
 * @param subclass      Subclass code
 * @return              Subclass name or NULL if not found
 */
static const char *pci_subclass_name(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case PCI_CLASS_STORAGE:
        if (subclass < 9) {
            return pci_storage_subclass_names[subclass];
        }
        break;
    case PCI_CLASS_BRIDGE:
        if (subclass < 8) {
            return pci_bridge_subclass_names[subclass];
        }
        break;
    case PCI_CLASS_NETWORK:
        if (subclass < 7) {
            return pci_network_subclass_names[subclass];
        }
        break;
    case PCI_CLASS_SERIAL:
        if (subclass < 6) {
            return pci_serial_subclass_names[subclass];
        }
        break;
    }
    return NULL;
}

/* ============================================================================
 * Debugging and Diagnostics
 * ============================================================================ */

void pci_print_devices(void)
{
    int i;
    pci_device_t *dev;
    const char *class_str;
    const char *subclass_str;
    const char *vendor_str;

    console_printf("\n[PCI] Discovered Devices:\n");
    console_printf("Bus  Dev  Fn   Vendor:Device  Class       ");
    console_printf("Description\n");
    console_printf("---  ---  --   -------------  ----------  ");
    console_printf("---------------------------\n");

    for (i = 0; i < g_pci.device_count; i++) {
        dev = &g_pci.devices[i];

        class_str = pci_class_name(dev->class_code);
        subclass_str = pci_subclass_name(dev->class_code, dev->subclass);
        vendor_str = pci_vendor_name(dev->vendor_id);

        console_printf("%02x   %02x   %x    %04x:%04x      %-10s  %s",
                       dev->addr.bus,
                       dev->addr.device,
                       dev->addr.function,
                       dev->vendor_id,
                       dev->device_id,
                       class_str,
                       vendor_str);

        if (subclass_str) {
            console_printf(" %s", subclass_str);
        }

        if (dev->driver) {
            console_printf(" [%s]", dev->driver->name);
        }

        console_printf("\n");
    }

    console_printf("\nTotal: %d device(s)\n\n", g_pci.device_count);
}

void pci_print_stats(void)
{
    console_printf("\n[PCI] Statistics:\n");
    console_printf("  Initialized:    %s\n",
                   g_pci.initialized ? "yes" : "no");
    console_printf("  Devices found:  %d\n", g_pci.stats.devices_found);
    console_printf("  Buses scanned:  %d\n", g_pci.stats.buses_scanned);
    console_printf("  Bridges found:  %d\n", g_pci.stats.bridges_found);
    console_printf("  Drivers:        %d\n", g_pci.stats.drivers_registered);
    console_printf("  Devices bound:  %d\n", g_pci.stats.devices_bound);
    console_printf("\n");
}

void pci_get_stats(pci_stats_t *stats)
{
    if (stats) {
        *stats = g_pci.stats;
    }
}

/* ============================================================================
 * Self-Tests
 * ============================================================================ */

int pci_run_tests(void)
{
    int passed = 0;
    int failed = 0;
    pci_device_t *dev;
    uint16_t vendor;

    console_printf("\n[PCI] Running self-tests...\n");

    /* Test 1: Initialization check */
    console_printf("  Test 1: Initialization... ");
    if (g_pci.initialized) {
        console_printf("PASS\n");
        passed++;
    } else {
        console_printf("FAIL\n");
        failed++;
    }

    /* Test 2: Device enumeration */
    console_printf("  Test 2: Device enumeration... ");
    if (g_pci.device_count > 0) {
        console_printf("PASS (%d devices)\n", g_pci.device_count);
        passed++;
    } else {
        console_printf("FAIL (no devices found)\n");
        failed++;
    }

    /* Test 3: Config space read consistency */
    console_printf("  Test 3: Config space read... ");
    if (g_pci.device_count > 0) {
        dev = &g_pci.devices[0];
        vendor = pci_config_read16(dev->addr, PCI_VENDOR_ID);
        if (vendor == dev->vendor_id && vendor != PCI_VENDOR_INVALID) {
            console_printf("PASS (vendor=%04x)\n", vendor);
            passed++;
        } else {
            console_printf("FAIL (mismatch)\n");
            failed++;
        }
    } else {
        console_printf("SKIP (no devices)\n");
    }

    /* Test 4: Find device by class */
    console_printf("  Test 4: Find by class... ");
    dev = pci_find_class(PCI_CLASS_BRIDGE, PCI_ANY_CLASS);
    if (dev != NULL) {
        console_printf("PASS (found bridge at %02x:%02x.%x)\n",
                       dev->addr.bus, dev->addr.device, dev->addr.function);
        passed++;
    } else {
        console_printf("FAIL (no bridge found)\n");
        failed++;
    }

    /* Test 5: Device lookup by index */
    console_printf("  Test 5: Device lookup... ");
    dev = pci_get_device(0);
    if (dev != NULL && dev->vendor_id != PCI_VENDOR_INVALID) {
        console_printf("PASS\n");
        passed++;
    } else if (g_pci.device_count == 0) {
        console_printf("SKIP (no devices)\n");
    } else {
        console_printf("FAIL\n");
        failed++;
    }

    console_printf("[PCI] Tests complete: %d passed, %d failed\n\n",
                   passed, failed);

    return (failed == 0) ? 0 : -1;
}
