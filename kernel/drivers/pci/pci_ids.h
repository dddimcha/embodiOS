/* PCI Vendor and Device ID Definitions
 *
 * Common vendor IDs and device IDs for known devices.
 * Data tables are defined in pci.c to avoid duplication.
 */

#ifndef EMBODIOS_PCI_IDS_H
#define EMBODIOS_PCI_IDS_H

#include <embodios/types.h>

/* ============================================================================
 * Common Vendor IDs
 * ============================================================================ */

#define PCI_VENDOR_INTEL        0x8086
#define PCI_VENDOR_AMD          0x1022
#define PCI_VENDOR_NVIDIA       0x10DE
#define PCI_VENDOR_QEMU         0x1234
#define PCI_VENDOR_VIRTIO       0x1AF4
#define PCI_VENDOR_REDHAT       0x1B36
#define PCI_VENDOR_REALTEK      0x10EC
#define PCI_VENDOR_BROADCOM     0x14E4
#define PCI_VENDOR_VMWARE       0x15AD

/* ============================================================================
 * Intel Device IDs
 * ============================================================================ */

#define PCI_DEVICE_INTEL_440FX_HOST     0x1237
#define PCI_DEVICE_INTEL_440FX_ISA      0x7000
#define PCI_DEVICE_INTEL_PIIX3_IDE      0x7010
#define PCI_DEVICE_INTEL_E1000          0x100E
#define PCI_DEVICE_INTEL_AHCI           0x2829

/* ============================================================================
 * QEMU Device IDs
 * ============================================================================ */

#define PCI_DEVICE_QEMU_VGA             0x1111

/* ============================================================================
 * VirtIO Device IDs
 * ============================================================================ */

#define PCI_DEVICE_VIRTIO_NET           0x1000
#define PCI_DEVICE_VIRTIO_BLK           0x1001

/* ============================================================================
 * Lookup Table Types
 * ============================================================================ */

typedef struct pci_vendor_entry {
    uint16_t    vendor_id;
    const char  *name;
} pci_vendor_entry_t;

#endif /* EMBODIOS_PCI_IDS_H */
