/* EMBODIOS PCI Subsystem
 *
 * Provides PCI device enumeration and driver registration framework
 * for discovering and configuring network cards, storage controllers,
 * and other PCI devices.
 *
 * Features:
 * - Legacy I/O port configuration access (0xCF8/0xCFC)
 * - Bus/Device/Function enumeration
 * - Device class identification
 * - Driver registration and matching
 * - Debug utilities
 */

#ifndef EMBODIOS_PCI_H
#define EMBODIOS_PCI_H

#include <embodios/types.h>

/* ============================================================================
 * PCI I/O Ports (Legacy Configuration Mechanism)
 * ============================================================================ */

#define PCI_CONFIG_ADDRESS      0xCF8   /* Configuration address port */
#define PCI_CONFIG_DATA         0xCFC   /* Configuration data port */

/* ============================================================================
 * PCI Limits
 * ============================================================================ */

#define PCI_MAX_BUSES           256     /* Maximum PCI buses */
#define PCI_MAX_DEVICES         32      /* Devices per bus */
#define PCI_MAX_FUNCTIONS       8       /* Functions per device */
#define PCI_MAX_STORED          128     /* Max devices to store */
#define PCI_MAX_DRIVERS         32      /* Max registered drivers */

/* ============================================================================
 * Configuration Space Offsets (Common Header)
 * ============================================================================ */

#define PCI_VENDOR_ID           0x00    /* Vendor ID (16-bit) */
#define PCI_DEVICE_ID           0x02    /* Device ID (16-bit) */
#define PCI_COMMAND             0x04    /* Command register (16-bit) */
#define PCI_STATUS              0x06    /* Status register (16-bit) */
#define PCI_REVISION            0x08    /* Revision ID (8-bit) */
#define PCI_PROG_IF             0x09    /* Programming interface (8-bit) */
#define PCI_SUBCLASS            0x0A    /* Subclass code (8-bit) */
#define PCI_CLASS               0x0B    /* Class code (8-bit) */
#define PCI_CACHE_LINE_SIZE     0x0C    /* Cache line size (8-bit) */
#define PCI_LATENCY_TIMER       0x0D    /* Latency timer (8-bit) */
#define PCI_HEADER_TYPE         0x0E    /* Header type (8-bit) */
#define PCI_BIST                0x0F    /* Built-in self test (8-bit) */

/* Type 0 Header (Endpoints) */
#define PCI_BAR0                0x10    /* Base Address Register 0 */
#define PCI_BAR1                0x14    /* Base Address Register 1 */
#define PCI_BAR2                0x18    /* Base Address Register 2 */
#define PCI_BAR3                0x1C    /* Base Address Register 3 */
#define PCI_BAR4                0x20    /* Base Address Register 4 */
#define PCI_BAR5                0x24    /* Base Address Register 5 */
#define PCI_CARDBUS_CIS         0x28    /* CardBus CIS pointer */
#define PCI_SUBSYSTEM_VENDOR    0x2C    /* Subsystem vendor ID */
#define PCI_SUBSYSTEM_ID        0x2E    /* Subsystem ID */
#define PCI_ROM_ADDRESS         0x30    /* Expansion ROM base */
#define PCI_CAPABILITIES        0x34    /* Capabilities pointer */
#define PCI_INTERRUPT_LINE      0x3C    /* Interrupt line (IRQ) */
#define PCI_INTERRUPT_PIN       0x3D    /* Interrupt pin (A-D) */
#define PCI_MIN_GRANT           0x3E    /* Minimum grant */
#define PCI_MAX_LATENCY         0x3F    /* Maximum latency */

/* ============================================================================
 * Header Type Values
 * ============================================================================ */

#define PCI_HEADER_TYPE_MASK    0x7F    /* Mask for header type */
#define PCI_HEADER_ENDPOINT     0x00    /* Type 0: Endpoint device */
#define PCI_HEADER_BRIDGE       0x01    /* Type 1: PCI-to-PCI bridge */
#define PCI_HEADER_CARDBUS      0x02    /* Type 2: CardBus bridge */
#define PCI_HEADER_MULTIFUNCTION 0x80   /* Multifunction device flag */

/* ============================================================================
 * Command Register Bits
 * ============================================================================ */

#define PCI_COMMAND_IO          0x0001  /* Enable I/O space access */
#define PCI_COMMAND_MEMORY      0x0002  /* Enable memory space access */
#define PCI_COMMAND_MASTER      0x0004  /* Enable bus mastering */
#define PCI_COMMAND_SPECIAL     0x0008  /* Enable special cycles */
#define PCI_COMMAND_INVALIDATE  0x0010  /* Enable memory write and invalidate */
#define PCI_COMMAND_VGA_PALETTE 0x0020  /* Enable VGA palette snooping */
#define PCI_COMMAND_PARITY      0x0040  /* Enable parity error response */
#define PCI_COMMAND_SERR        0x0100  /* Enable SERR# driver */
#define PCI_COMMAND_FAST_BACK   0x0200  /* Enable fast back-to-back */
#define PCI_COMMAND_INTX_DISABLE 0x0400 /* Disable INTx emulation */

/* ============================================================================
 * Device Classes
 * ============================================================================ */

#define PCI_CLASS_UNCLASSIFIED  0x00    /* Unclassified device */
#define PCI_CLASS_STORAGE       0x01    /* Mass storage controller */
#define PCI_CLASS_NETWORK       0x02    /* Network controller */
#define PCI_CLASS_DISPLAY       0x03    /* Display controller */
#define PCI_CLASS_MULTIMEDIA    0x04    /* Multimedia device */
#define PCI_CLASS_MEMORY        0x05    /* Memory controller */
#define PCI_CLASS_BRIDGE        0x06    /* Bridge device */
#define PCI_CLASS_COMMUNICATION 0x07    /* Simple communication controller */
#define PCI_CLASS_SYSTEM        0x08    /* Base system peripheral */
#define PCI_CLASS_INPUT         0x09    /* Input device */
#define PCI_CLASS_DOCKING       0x0A    /* Docking station */
#define PCI_CLASS_PROCESSOR     0x0B    /* Processor */
#define PCI_CLASS_SERIAL        0x0C    /* Serial bus controller */
#define PCI_CLASS_WIRELESS      0x0D    /* Wireless controller */
#define PCI_CLASS_INTELLIGENT   0x0E    /* Intelligent I/O controller */
#define PCI_CLASS_SATELLITE     0x0F    /* Satellite controller */
#define PCI_CLASS_ENCRYPTION    0x10    /* Encryption/Decryption controller */
#define PCI_CLASS_SIGNAL        0x11    /* Data acquisition controller */

/* ============================================================================
 * BAR (Base Address Register) Bits
 * ============================================================================ */

#define PCI_BAR_IO              0x01    /* I/O space indicator */
#define PCI_BAR_TYPE_MASK       0x06    /* Memory type mask */
#define PCI_BAR_TYPE_32         0x00    /* 32-bit memory */
#define PCI_BAR_TYPE_1M         0x02    /* Below 1MB (legacy) */
#define PCI_BAR_TYPE_64         0x04    /* 64-bit memory */
#define PCI_BAR_PREFETCH        0x08    /* Prefetchable memory */
#define PCI_BAR_MEM_MASK        0xFFFFFFF0  /* Memory address mask */
#define PCI_BAR_IO_MASK         0xFFFFFFFC  /* I/O address mask */

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define PCI_OK                  0       /* Success */
#define PCI_ERR_NOT_INIT       -1       /* Not initialized */
#define PCI_ERR_NOT_FOUND      -2       /* Device not found */
#define PCI_ERR_INVALID        -3       /* Invalid parameter */
#define PCI_ERR_FULL           -4       /* Device/driver table full */
#define PCI_ERR_ALREADY_INIT   -5       /* Already initialized */

/* ============================================================================
 * Special Values
 * ============================================================================ */

#define PCI_VENDOR_INVALID      0xFFFF  /* Invalid/no device vendor ID */
#define PCI_ANY_ID              0xFFFF  /* Match any vendor/device ID */
#define PCI_ANY_CLASS           0xFF    /* Match any class/subclass */

/* ============================================================================
 * Data Types
 * ============================================================================ */

/* Forward declarations */
struct pci_device;
struct pci_driver;

/**
 * PCI device address (Bus/Device/Function)
 */
typedef struct pci_addr {
    uint8_t bus;        /* Bus number (0-255) */
    uint8_t device;     /* Device number (0-31) */
    uint8_t function;   /* Function number (0-7) */
} pci_addr_t;

/**
 * Discovered PCI device
 */
typedef struct pci_device {
    pci_addr_t addr;            /* Bus/Device/Function address */
    uint16_t vendor_id;         /* Vendor identifier */
    uint16_t device_id;         /* Device identifier */
    uint16_t subsystem_vendor;  /* Subsystem vendor ID */
    uint16_t subsystem_id;      /* Subsystem device ID */
    uint8_t class_code;         /* Base class code */
    uint8_t subclass;           /* Subclass code */
    uint8_t prog_if;            /* Programming interface */
    uint8_t revision;           /* Revision ID */
    uint8_t header_type;        /* Header type */
    uint8_t interrupt_line;     /* Interrupt line (IRQ) */
    uint8_t interrupt_pin;      /* Interrupt pin (1=A, 2=B, etc.) */
    uint32_t bar[6];            /* Base Address Registers */
    bool multifunction;         /* Is multifunction device */
    struct pci_driver *driver;  /* Bound driver (NULL if none) */
} pci_device_t;

/**
 * PCI driver registration structure
 */
typedef struct pci_driver {
    const char *name;           /* Driver name */
    uint16_t vendor_id;         /* Match vendor (PCI_ANY_ID = any) */
    uint16_t device_id;         /* Match device (PCI_ANY_ID = any) */
    uint8_t class_code;         /* Match class (PCI_ANY_CLASS = any) */
    uint8_t subclass;           /* Match subclass (PCI_ANY_CLASS = any) */
    int (*probe)(pci_device_t *dev);    /* Called when device matches */
    void (*remove)(pci_device_t *dev);  /* Called on driver unbind */
    struct pci_driver *next;    /* Next driver in list */
} pci_driver_t;

/**
 * PCI subsystem statistics
 */
typedef struct pci_stats {
    int devices_found;          /* Total devices discovered */
    int buses_scanned;          /* Buses scanned during enumeration */
    int bridges_found;          /* PCI bridges found */
    int drivers_registered;     /* Registered drivers */
    int devices_bound;          /* Devices with bound drivers */
} pci_stats_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize PCI subsystem
 * @return PCI_OK on success, error code on failure
 */
int pci_init(void);

/**
 * Check if PCI subsystem is initialized
 * @return true if initialized
 */
bool pci_is_initialized(void);

/* ============================================================================
 * Configuration Space Access
 * ============================================================================ */

uint8_t pci_config_read8(pci_addr_t addr, uint8_t offset);
uint16_t pci_config_read16(pci_addr_t addr, uint8_t offset);
uint32_t pci_config_read32(pci_addr_t addr, uint8_t offset);
void pci_config_write8(pci_addr_t addr, uint8_t offset, uint8_t value);
void pci_config_write16(pci_addr_t addr, uint8_t offset, uint16_t value);
void pci_config_write32(pci_addr_t addr, uint8_t offset, uint32_t value);

/* ============================================================================
 * Device Enumeration
 * ============================================================================ */

/**
 * Enumerate all PCI devices
 * @return Number of devices found, or negative on error
 */
int pci_enumerate(void);

/**
 * Get device by index
 * @return Pointer to device, or NULL if invalid index
 */
pci_device_t *pci_get_device(int index);

/**
 * Find device by vendor and device ID
 * @return Pointer to first matching device, or NULL
 */
pci_device_t *pci_find_device(uint16_t vendor, uint16_t device);

/**
 * Find device by class code
 * @return Pointer to first matching device, or NULL
 */
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);

/**
 * Get number of discovered devices
 */
int pci_device_count(void);

/* ============================================================================
 * Driver Framework
 * ============================================================================ */

int pci_register_driver(pci_driver_t *driver);
void pci_unregister_driver(pci_driver_t *driver);

/* ============================================================================
 * Device Control
 * ============================================================================ */

void pci_enable_bus_master(pci_device_t *dev);
void pci_enable_memory(pci_device_t *dev);
void pci_enable_io(pci_device_t *dev);

/* ============================================================================
 * BAR Access
 * ============================================================================ */

uint64_t pci_bar_address(pci_device_t *dev, int bar_index);
size_t pci_bar_size(pci_device_t *dev, int bar_index);
bool pci_bar_is_io(pci_device_t *dev, int bar_index);

/* ============================================================================
 * Debugging and Diagnostics
 * ============================================================================ */

void pci_print_devices(void);
void pci_print_stats(void);
void pci_get_stats(pci_stats_t *stats);
int pci_run_tests(void);

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

const char *pci_class_name(uint8_t class_code);
const char *pci_vendor_name(uint16_t vendor_id);

#endif /* EMBODIOS_PCI_H */
