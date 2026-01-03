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

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#else /* Non-x86_64 */

static inline void outl(uint16_t port, uint32_t value)
{
    (void)port; (void)value;
}

static inline uint32_t inl(uint16_t port)
{
    (void)port;
    return 0xFFFFFFFF;
}

#endif

/* ============================================================================
 * Lookup Tables
 * ============================================================================ */

static const pci_vendor_entry_t pci_vendor_table[] = {
    { PCI_VENDOR_INTEL,     "Intel" },
    { PCI_VENDOR_AMD,       "AMD" },
    { PCI_VENDOR_NVIDIA,    "NVIDIA" },
    { PCI_VENDOR_QEMU,      "QEMU" },
    { PCI_VENDOR_VIRTIO,    "VirtIO" },
    { PCI_VENDOR_REDHAT,    "Red Hat" },
    { PCI_VENDOR_REALTEK,   "Realtek" },
    { PCI_VENDOR_BROADCOM,  "Broadcom" },
    { PCI_VENDOR_VMWARE,    "VMware" },
    { 0, NULL }
};

static const char *pci_class_names[] = {
    "Unclassified", "Storage", "Network", "Display",
    "Multimedia", "Memory", "Bridge", "Communication",
    "System", "Input", "Docking", "Processor",
    "Serial Bus", "Wireless", "Intelligent I/O", "Satellite",
    "Encryption", "Signal Processing"
};

#define PCI_CLASS_NAME_COUNT (sizeof(pci_class_names) / sizeof(pci_class_names[0]))

/* ============================================================================
 * Internal State
 * ============================================================================ */

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
 * Build PCI configuration address for port I/O
 * @param addr      PCI device address (bus/device/function)
 * @param offset    Configuration space offset
 * @return          32-bit address for CONFIG_ADDRESS port
 */
static uint32_t pci_config_address(pci_addr_t addr, uint8_t offset)
{
    return 0x80000000 |
           ((uint32_t)addr.bus << 16) |
           ((uint32_t)addr.device << 11) |
           ((uint32_t)addr.function << 8) |
           (offset & 0xFC);
}

/**
 * Read 32-bit value from PCI configuration space
 * @param addr      Device address (bus/device/function)
 * @param offset    Register offset (must be 4-byte aligned)
 * @return          32-bit value from configuration space
 */
uint32_t pci_config_read32(pci_addr_t addr, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    return inl(PCI_CONFIG_DATA);
}

/**
 * Read 16-bit value from PCI configuration space
 * @param addr      Device address (bus/device/function)
 * @param offset    Register offset (must be 2-byte aligned)
 * @return          16-bit value from configuration space
 */
uint16_t pci_config_read16(pci_addr_t addr, uint8_t offset)
{
    uint32_t value;
    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    value = inl(PCI_CONFIG_DATA);
    return (uint16_t)(value >> ((offset & 2) * 8));
}

/**
 * Read 8-bit value from PCI configuration space
 * @param addr      Device address (bus/device/function)
 * @param offset    Register offset
 * @return          8-bit value from configuration space
 */
uint8_t pci_config_read8(pci_addr_t addr, uint8_t offset)
{
    uint32_t value;
    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    value = inl(PCI_CONFIG_DATA);
    return (uint8_t)(value >> ((offset & 3) * 8));
}

/**
 * Write 32-bit value to PCI configuration space
 * @param addr      Device address (bus/device/function)
 * @param offset    Register offset (must be 4-byte aligned)
 * @param value     Value to write
 */
void pci_config_write32(pci_addr_t addr, uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    outl(PCI_CONFIG_DATA, value);
}

/**
 * Write 16-bit value to PCI configuration space
 * @param addr      Device address (bus/device/function)
 * @param offset    Register offset (must be 2-byte aligned)
 * @param value     Value to write
 */
void pci_config_write16(pci_addr_t addr, uint8_t offset, uint16_t value)
{
    uint32_t old, mask, new_val;
    int shift;

    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
    old = inl(PCI_CONFIG_DATA);
    shift = (offset & 2) * 8;
    mask = 0xFFFF << shift;
    new_val = (old & ~mask) | ((uint32_t)value << shift);
    outl(PCI_CONFIG_DATA, new_val);
}

/**
 * Write 8-bit value to PCI configuration space
 * @param addr      Device address (bus/device/function)
 * @param offset    Register offset
 * @param value     Value to write
 */
void pci_config_write8(pci_addr_t addr, uint8_t offset, uint8_t value)
{
    uint32_t old, mask, new_val;
    int shift;

    outl(PCI_CONFIG_ADDRESS, pci_config_address(addr, offset));
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
 * Store discovered device in device table
 * @param addr      Device address (bus/device/function)
 * @return          PCI_OK on success, error code on failure
 */
static int pci_store_device(pci_addr_t addr)
{
    pci_device_t *dev;
    uint32_t id, class_info, subsys;
    int i;

    if (g_pci.device_count >= PCI_MAX_STORED) {
        return PCI_ERR_FULL;
    }

    dev = &g_pci.devices[g_pci.device_count];

    id = pci_config_read32(addr, PCI_VENDOR_ID);
    dev->vendor_id = id & 0xFFFF;
    dev->device_id = (id >> 16) & 0xFFFF;

    if (dev->vendor_id == PCI_VENDOR_INVALID) {
        return PCI_ERR_NOT_FOUND;
    }

    dev->addr = addr;

    class_info = pci_config_read32(addr, PCI_REVISION);
    dev->revision = class_info & 0xFF;
    dev->prog_if = (class_info >> 8) & 0xFF;
    dev->subclass = (class_info >> 16) & 0xFF;
    dev->class_code = (class_info >> 24) & 0xFF;

    dev->header_type = pci_config_read8(addr, PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;
    dev->multifunction = (pci_config_read8(addr, PCI_HEADER_TYPE) & PCI_HEADER_MULTIFUNCTION) != 0;

    dev->interrupt_line = pci_config_read8(addr, PCI_INTERRUPT_LINE);
    dev->interrupt_pin = pci_config_read8(addr, PCI_INTERRUPT_PIN);

    if (dev->header_type == PCI_HEADER_ENDPOINT) {
        subsys = pci_config_read32(addr, PCI_SUBSYSTEM_VENDOR);
        dev->subsystem_vendor = subsys & 0xFFFF;
        dev->subsystem_id = (subsys >> 16) & 0xFFFF;

        for (i = 0; i < 6; i++) {
            dev->bar[i] = pci_config_read32(addr, PCI_BAR0 + (i * 4));
        }
    } else {
        dev->subsystem_vendor = 0;
        dev->subsystem_id = 0;
        dev->bar[0] = pci_config_read32(addr, PCI_BAR0);
        dev->bar[1] = pci_config_read32(addr, PCI_BAR1);
        for (i = 2; i < 6; i++) {
            dev->bar[i] = 0;
        }
    }

    dev->driver = NULL;
    g_pci.device_count++;
    g_pci.stats.devices_found++;

    if (dev->class_code == PCI_CLASS_BRIDGE) {
        g_pci.stats.bridges_found++;
    }

    return PCI_OK;
}

/* ============================================================================
 * Initialization and Enumeration
 * ============================================================================ */

/**
 * Initialize PCI subsystem
 * Clears state and enumerates all PCI devices.
 * @return  PCI_OK on success, error code on failure
 */
int pci_init(void)
{
    int count;

    if (g_pci.initialized) {
        return PCI_ERR_ALREADY_INIT;
    }

    memset(&g_pci, 0, sizeof(g_pci));
    console_printf("[PCI] Initializing PCI subsystem...\n");
    g_pci.initialized = true;

    count = pci_enumerate();
    console_printf("[PCI] Subsystem initialized: %d devices found\n", count);

    return PCI_OK;
}

/**
 * Check if PCI subsystem is initialized
 * @return  true if initialized
 */
bool pci_is_initialized(void)
{
    return g_pci.initialized;
}

/**
 * Enumerate all PCI devices on all buses
 * Scans buses 0-255, devices 0-31, functions 0-7.
 * @return  Number of devices found, or negative error code
 */
int pci_enumerate(void)
{
    int bus, dev, func, max_func;
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

            vendor = pci_config_read16(addr, PCI_VENDOR_ID);
            if (vendor == PCI_VENDOR_INVALID) {
                continue;
            }

            found_on_bus = true;

            header = pci_config_read8(addr, PCI_HEADER_TYPE);
            max_func = (header & PCI_HEADER_MULTIFUNCTION) ? PCI_MAX_FUNCTIONS : 1;

            for (func = 0; func < max_func; func++) {
                addr.function = (uint8_t)func;

                if (func > 0) {
                    vendor = pci_config_read16(addr, PCI_VENDOR_ID);
                    if (vendor == PCI_VENDOR_INVALID) {
                        continue;
                    }
                }

                if (pci_store_device(addr) != PCI_OK) {
                    console_printf("[PCI] Warning: Device table full\n");
                    goto done;
                }
            }
        }

        if (found_on_bus) {
            buses_with_devices++;
        }

        if (bus == 0 && !found_on_bus) {
            console_printf("[PCI] No devices on bus 0\n");
            break;
        }
    }

done:
    g_pci.stats.buses_scanned = buses_with_devices;
    console_printf("[PCI] Found %d devices on %d bus(es)\n",
                   g_pci.device_count, buses_with_devices);

    return g_pci.device_count;
}

/* ============================================================================
 * Device Lookup
 * ============================================================================ */

/**
 * Get device by index
 * @param index     Device index (0 to device_count-1)
 * @return          Pointer to device, or NULL if invalid index
 */
pci_device_t *pci_get_device(int index)
{
    if (index < 0 || index >= g_pci.device_count) {
        return NULL;
    }
    return &g_pci.devices[index];
}

/**
 * Find device by vendor and device ID
 * @param vendor    Vendor ID to match
 * @param device    Device ID to match
 * @return          Pointer to first matching device, or NULL
 */
pci_device_t *pci_find_device(uint16_t vendor, uint16_t device)
{
    int i;

    for (i = 0; i < g_pci.device_count; i++) {
        if (g_pci.devices[i].vendor_id == vendor &&
            g_pci.devices[i].device_id == device) {
            return &g_pci.devices[i];
        }
    }
    return NULL;
}

/**
 * Find device by class code
 * @param class_code    Base class to match
 * @param subclass      Subclass to match (PCI_ANY_CLASS = any)
 * @return              Pointer to first matching device, or NULL
 */
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    int i;
    for (i = 0; i < g_pci.device_count; i++) {
        if (g_pci.devices[i].class_code == class_code) {
            if (subclass == PCI_ANY_CLASS || g_pci.devices[i].subclass == subclass) {
                return &g_pci.devices[i];
            }
        }
    }
    return NULL;
}

/**
 * Get number of discovered devices
 * @return  Device count
 */
int pci_device_count(void)
{
    return g_pci.device_count;
}

/* ============================================================================
 * Driver Framework
 * ============================================================================ */

/**
 * Check if driver matches device
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

/**
 * Register a PCI driver
 * @param driver    Driver structure to register
 * @return          PCI_OK on success, error code on failure
 */
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

    driver->next = g_pci.drivers;
    g_pci.drivers = driver;
    g_pci.driver_count++;
    g_pci.stats.drivers_registered++;

    console_printf("[PCI] Registered driver: %s\n", driver->name);

    for (i = 0; i < g_pci.device_count; i++) {
        dev = &g_pci.devices[i];
        if (dev->driver == NULL && pci_driver_matches(driver, dev)) {
            if (driver->probe(dev) == 0) {
                dev->driver = driver;
                g_pci.stats.devices_bound++;
            }
        }
    }

    return PCI_OK;
}

/**
 * Unregister a PCI driver
 * @param driver    Driver to unregister
 */
void pci_unregister_driver(pci_driver_t *driver)
{
    int i;
    pci_driver_t **pp;

    if (!driver) {
        return;
    }

    for (i = 0; i < g_pci.device_count; i++) {
        if (g_pci.devices[i].driver == driver) {
            if (driver->remove) {
                driver->remove(&g_pci.devices[i]);
            }
            g_pci.devices[i].driver = NULL;
            g_pci.stats.devices_bound--;
        }
    }

    pp = &g_pci.drivers;
    while (*pp) {
        if (*pp == driver) {
            *pp = driver->next;
            g_pci.driver_count--;
            break;
        }
        pp = &(*pp)->next;
    }
}

/* ============================================================================
 * Device Control
 * ============================================================================ */

void pci_enable_bus_master(pci_device_t *dev)
{
    uint16_t cmd;
    if (!dev) return;
    cmd = pci_config_read16(dev->addr, PCI_COMMAND);
    cmd |= PCI_COMMAND_MASTER;
    pci_config_write16(dev->addr, PCI_COMMAND, cmd);
}

void pci_enable_memory(pci_device_t *dev)
{
    uint16_t cmd;
    if (!dev) return;
    cmd = pci_config_read16(dev->addr, PCI_COMMAND);
    cmd |= PCI_COMMAND_MEMORY;
    pci_config_write16(dev->addr, PCI_COMMAND, cmd);
}

void pci_enable_io(pci_device_t *dev)
{
    uint16_t cmd;
    if (!dev) return;
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

    if (!dev || bar_index < 0 || bar_index > 5) return 0;

    bar = dev->bar[bar_index];
    if (bar == 0) return 0;

    if (bar & PCI_BAR_IO) {
        return bar & PCI_BAR_IO_MASK;
    }

    addr = bar & PCI_BAR_MEM_MASK;
    if ((bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64 && bar_index < 5) {
        addr |= ((uint64_t)dev->bar[bar_index + 1]) << 32;
    }
    return addr;
}

size_t pci_bar_size(pci_device_t *dev, int bar_index)
{
    uint32_t original, sized, mask;

    if (!dev || bar_index < 0 || bar_index > 5) return 0;

    original = pci_config_read32(dev->addr, PCI_BAR0 + (bar_index * 4));
    if (original == 0) return 0;

    pci_config_write32(dev->addr, PCI_BAR0 + (bar_index * 4), 0xFFFFFFFF);
    sized = pci_config_read32(dev->addr, PCI_BAR0 + (bar_index * 4));
    pci_config_write32(dev->addr, PCI_BAR0 + (bar_index * 4), original);

    if (sized == 0) return 0;

    mask = (original & PCI_BAR_IO) ? PCI_BAR_IO_MASK : PCI_BAR_MEM_MASK;
    sized &= mask;
    sized = ~sized + 1;

    return sized;
}

bool pci_bar_is_io(pci_device_t *dev, int bar_index)
{
    if (!dev || bar_index < 0 || bar_index > 5) return false;
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

/* ============================================================================
 * Debugging
 * ============================================================================ */

void pci_print_devices(void)
{
    int i;
    pci_device_t *dev;

    console_printf("\n[PCI] Discovered Devices:\n");
    console_printf("Bus Dev Fn  Vendor:Device Class      Vendor\n");
    console_printf("--- --- --  ------------- ---------- --------\n");

    for (i = 0; i < g_pci.device_count; i++) {
        dev = &g_pci.devices[i];
        console_printf("%02x  %02x  %x   %04x:%04x     %-10s %s\n",
                       dev->addr.bus,
                       dev->addr.device,
                       dev->addr.function,
                       dev->vendor_id,
                       dev->device_id,
                       pci_class_name(dev->class_code),
                       pci_vendor_name(dev->vendor_id));
    }
    console_printf("\nTotal: %d device(s)\n", g_pci.device_count);
}

void pci_print_stats(void)
{
    console_printf("\n[PCI] Statistics:\n");
    console_printf("  Initialized:    %s\n", g_pci.initialized ? "yes" : "no");
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

int pci_run_tests(void)
{
    int passed = 0, failed = 0;
    pci_device_t *dev;
    uint16_t vendor;

    console_printf("\n[PCI] Running self-tests...\n");

    console_printf("  Test 1: Initialization... ");
    if (g_pci.initialized) {
        console_printf("PASS\n");
        passed++;
    } else {
        console_printf("FAIL\n");
        failed++;
    }

    console_printf("  Test 2: Device enumeration... ");
    if (g_pci.device_count > 0) {
        console_printf("PASS (%d devices)\n", g_pci.device_count);
        passed++;
    } else {
        console_printf("FAIL\n");
        failed++;
    }

    console_printf("  Test 3: Config space read... ");
    if (g_pci.device_count > 0) {
        dev = &g_pci.devices[0];
        vendor = pci_config_read16(dev->addr, PCI_VENDOR_ID);
        if (vendor == dev->vendor_id) {
            console_printf("PASS\n");
            passed++;
        } else {
            console_printf("FAIL\n");
            failed++;
        }
    } else {
        console_printf("SKIP\n");
    }

    console_printf("  Test 4: Find by class... ");
    dev = pci_find_class(PCI_CLASS_BRIDGE, PCI_ANY_CLASS);
    if (dev != NULL) {
        console_printf("PASS\n");
        passed++;
    } else {
        console_printf("FAIL\n");
        failed++;
    }

    console_printf("  Test 5: Device lookup... ");
    dev = pci_get_device(0);
    if (dev != NULL) {
        console_printf("PASS\n");
        passed++;
    } else if (g_pci.device_count == 0) {
        console_printf("SKIP\n");
    } else {
        console_printf("FAIL\n");
        failed++;
    }

    console_printf("[PCI] Tests: %d passed, %d failed\n\n", passed, failed);
    return (failed == 0) ? 0 : -1;
}
