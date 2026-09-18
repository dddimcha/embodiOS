/* PCI Vendor and Device ID Definitions
 *
 * Common vendor IDs and device IDs for known devices.
 * Used for human-readable output and driver matching.
 *
 * Note: Data tables are defined in pci.c to avoid duplication
 * if this header is included in multiple translation units.
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
#define PCI_VENDOR_QUALCOMM     0x168C
#define PCI_VENDOR_VMWARE       0x15AD
#define PCI_VENDOR_VIA          0x1106
#define PCI_VENDOR_SIS          0x1039
#define PCI_VENDOR_MARVELL      0x11AB
#define PCI_VENDOR_SAMSUNG      0x144D
#define PCI_VENDOR_SANDISK      0x15B7
#define PCI_VENDOR_TOSHIBA      0x1179
#define PCI_VENDOR_MICRON       0x1344
#define PCI_VENDOR_SEAGATE      0x1BB1

/* ============================================================================
 * Intel Device IDs
 * ============================================================================ */

/* Intel 440FX (QEMU default) */
#define PCI_DEVICE_INTEL_440FX_HOST     0x1237
#define PCI_DEVICE_INTEL_440FX_BRIDGE   0x7000
#define PCI_DEVICE_INTEL_440FX_ISA      0x7110

/* Intel ICH (I/O Controller Hub) */
#define PCI_DEVICE_INTEL_ICH_ISA        0x2918
#define PCI_DEVICE_INTEL_ICH_SATA       0x2922
#define PCI_DEVICE_INTEL_ICH_SMBUS      0x2930

/* Intel Q35 */
#define PCI_DEVICE_INTEL_Q35_HOST       0x29C0
#define PCI_DEVICE_INTEL_Q35_BRIDGE     0x29C1

/* Intel Network */
#define PCI_DEVICE_INTEL_E1000          0x100E
#define PCI_DEVICE_INTEL_E1000E         0x10D3
#define PCI_DEVICE_INTEL_I219           0x15B8
#define PCI_DEVICE_INTEL_I350           0x1521

/* Intel AHCI */
#define PCI_DEVICE_INTEL_AHCI           0x2829

/* ============================================================================
 * QEMU/VirtIO Device IDs
 * ============================================================================ */

/* QEMU Standard VGA */
#define PCI_DEVICE_QEMU_VGA             0x1111

/* VirtIO devices */
#define PCI_DEVICE_VIRTIO_NET           0x1000
#define PCI_DEVICE_VIRTIO_BLK           0x1001
#define PCI_DEVICE_VIRTIO_CONSOLE       0x1003
#define PCI_DEVICE_VIRTIO_RNG           0x1005
#define PCI_DEVICE_VIRTIO_BALLOON       0x1002
#define PCI_DEVICE_VIRTIO_GPU           0x1050
#define PCI_DEVICE_VIRTIO_INPUT         0x1052
#define PCI_DEVICE_VIRTIO_SOCKET        0x1053

/* Red Hat devices (QEMU) */
#define PCI_DEVICE_REDHAT_XHCI          0x000D

/* ============================================================================
 * Realtek Device IDs
 * ============================================================================ */

#define PCI_DEVICE_REALTEK_8139         0x8139
#define PCI_DEVICE_REALTEK_8169         0x8169

/* ============================================================================
 * AMD Device IDs
 * ============================================================================ */

#define PCI_DEVICE_AMD_IOMMU            0x1577
#define PCI_DEVICE_AMD_XHCI             0x149C

/* ============================================================================
 * NVIDIA Device IDs
 * ============================================================================ */

#define PCI_DEVICE_NVIDIA_GEFORCE       0x1C03

/* ============================================================================
 * Lookup Table Types
 * ============================================================================ */

/**
 * Vendor name lookup table entry
 */
typedef struct pci_vendor_entry {
    uint16_t    vendor_id;
    const char  *name;
} pci_vendor_entry_t;

/**
 * Class name lookup table entry
 */
typedef struct pci_class_entry {
    uint8_t     class_code;
    uint8_t     subclass;
    const char  *name;
} pci_class_entry_t;

#endif /* EMBODIOS_PCI_IDS_H */
