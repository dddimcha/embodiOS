/* EMBODIOS VirtIO-MMIO Block Driver Header
 *
 * VirtIO-MMIO block device driver for ARM64 systems.
 * On QEMU's virt machine, VirtIO devices are memory-mapped starting at 0x0a000000.
 */

#ifndef EMBODIOS_VIRTIO_MMIO_H
#define EMBODIOS_VIRTIO_MMIO_H

#include <embodios/types.h>

/**
 * Initialize VirtIO-MMIO block subsystem
 *
 * Scans for VirtIO-MMIO block devices at the standard QEMU virt
 * machine addresses and initializes any found devices.
 *
 * @return 0 on success (even if no devices found), negative on error
 */
int virtio_mmio_init(void);

#endif /* EMBODIOS_VIRTIO_MMIO_H */
