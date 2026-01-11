/* EMBODIOS Block Device Interface
 *
 * Generic block device abstraction layer for storage drivers.
 * Supports VirtIO block, NVMe, and future storage controllers.
 */

#ifndef EMBODIOS_BLOCK_H
#define EMBODIOS_BLOCK_H

#include <embodios/types.h>

/* ============================================================================
 * Block Device Constants
 * ============================================================================ */

#define BLOCK_SECTOR_SIZE       512     /* Standard sector size */
#define BLOCK_MAX_DEVICES       16      /* Maximum registered block devices */
#define BLOCK_MAX_NAME_LEN      32      /* Maximum device name length */

/* Maximum sectors per I/O request */
#define BLOCK_MAX_SECTORS       256     /* 128KB per request */

/* ============================================================================
 * Block Device Error Codes
 * ============================================================================ */

#define BLOCK_OK                0       /* Success */
#define BLOCK_ERR_IO           -1       /* I/O error */
#define BLOCK_ERR_NOMEM        -2       /* Out of memory */
#define BLOCK_ERR_INVALID      -3       /* Invalid parameter */
#define BLOCK_ERR_NOT_FOUND    -4       /* Device not found */
#define BLOCK_ERR_TIMEOUT      -5       /* Operation timed out */
#define BLOCK_ERR_READONLY     -6       /* Device is read-only */
#define BLOCK_ERR_FULL         -7       /* Device table full */
#define BLOCK_ERR_BUSY         -8       /* Device busy */

/* ============================================================================
 * Block Device Flags
 * ============================================================================ */

#define BLOCK_FLAG_READONLY     0x01    /* Device is read-only */
#define BLOCK_FLAG_REMOVABLE    0x02    /* Device is removable */
#define BLOCK_FLAG_VIRTUAL      0x04    /* Virtual/emulated device */

/* ============================================================================
 * Block Device Structure
 * ============================================================================ */

struct block_device;

/**
 * Block device operations structure
 */
typedef struct block_ops {
    /**
     * Read sectors from device
     *
     * @param dev       Block device
     * @param sector    Starting sector number
     * @param count     Number of sectors to read
     * @param buffer    Output buffer (must be count * sector_size bytes)
     *
     * @return BLOCK_OK on success, negative error on failure
     */
    int (*read)(struct block_device* dev, uint64_t sector,
                uint32_t count, void* buffer);

    /**
     * Write sectors to device
     *
     * @param dev       Block device
     * @param sector    Starting sector number
     * @param count     Number of sectors to write
     * @param buffer    Input buffer (must be count * sector_size bytes)
     *
     * @return BLOCK_OK on success, negative error on failure
     */
    int (*write)(struct block_device* dev, uint64_t sector,
                 uint32_t count, const void* buffer);

    /**
     * Flush cached writes to device
     *
     * @param dev   Block device
     *
     * @return BLOCK_OK on success, negative error on failure
     */
    int (*flush)(struct block_device* dev);

    /**
     * Get device status
     *
     * @param dev   Block device
     *
     * @return Device-specific status code
     */
    int (*status)(struct block_device* dev);
} block_ops_t;

/**
 * Block device structure
 */
typedef struct block_device {
    char name[BLOCK_MAX_NAME_LEN];      /* Device name (e.g., "vda", "nvme0") */
    uint64_t total_sectors;             /* Total sectors on device */
    uint32_t sector_size;               /* Bytes per sector (usually 512) */
    uint32_t flags;                     /* BLOCK_FLAG_* flags */
    const block_ops_t* ops;             /* Device operations */
    void* private_data;                 /* Driver-specific data */
    int index;                          /* Device index */
} block_device_t;

/* ============================================================================
 * Block Device Registration
 * ============================================================================ */

/**
 * Initialize block device subsystem
 *
 * @return BLOCK_OK on success, error code on failure
 */
int block_init(void);

/**
 * Register a block device
 *
 * @param dev   Block device to register
 *
 * @return BLOCK_OK on success, error code on failure
 */
int block_register_device(block_device_t* dev);

/**
 * Unregister a block device
 *
 * @param dev   Block device to unregister
 */
void block_unregister_device(block_device_t* dev);

/**
 * Get block device by name
 *
 * @param name  Device name (e.g., "vda")
 *
 * @return Block device, or NULL if not found
 */
block_device_t* block_get_device(const char* name);

/**
 * Get block device by index
 *
 * @param index Device index (0 to block_device_count()-1)
 *
 * @return Block device, or NULL if invalid index
 */
block_device_t* block_get_device_by_index(int index);

/**
 * Get number of registered block devices
 *
 * @return Device count
 */
int block_device_count(void);

/* ============================================================================
 * Block I/O Functions
 * ============================================================================ */

/**
 * Read sectors from block device
 *
 * @param dev       Block device
 * @param sector    Starting sector number
 * @param count     Number of sectors to read
 * @param buffer    Output buffer
 *
 * @return BLOCK_OK on success, negative error on failure
 */
int block_read(block_device_t* dev, uint64_t sector,
               uint32_t count, void* buffer);

/**
 * Write sectors to block device
 *
 * @param dev       Block device
 * @param sector    Starting sector number
 * @param count     Number of sectors to write
 * @param buffer    Input buffer
 *
 * @return BLOCK_OK on success, negative error on failure
 */
int block_write(block_device_t* dev, uint64_t sector,
                uint32_t count, const void* buffer);

/**
 * Read bytes from block device (handles unaligned reads)
 *
 * @param dev       Block device
 * @param offset    Byte offset from start of device
 * @param size      Number of bytes to read
 * @param buffer    Output buffer
 *
 * @return BLOCK_OK on success, negative error on failure
 */
int block_read_bytes(block_device_t* dev, uint64_t offset,
                     size_t size, void* buffer);

/**
 * Write bytes to block device (handles unaligned writes)
 *
 * @param dev       Block device
 * @param offset    Byte offset from start of device
 * @param size      Number of bytes to write
 * @param buffer    Input buffer
 *
 * @return BLOCK_OK on success, negative error on failure
 */
int block_write_bytes(block_device_t* dev, uint64_t offset,
                      size_t size, const void* buffer);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get device capacity in bytes
 *
 * @param dev   Block device
 *
 * @return Capacity in bytes
 */
static inline uint64_t block_capacity(block_device_t* dev) {
    return dev->total_sectors * dev->sector_size;
}

/**
 * Get device capacity in MB
 *
 * @param dev   Block device
 *
 * @return Capacity in megabytes
 */
static inline uint64_t block_capacity_mb(block_device_t* dev) {
    return (dev->total_sectors * dev->sector_size) / (1024 * 1024);
}

/**
 * Check if device is read-only
 *
 * @param dev   Block device
 *
 * @return true if read-only
 */
static inline bool block_is_readonly(block_device_t* dev) {
    return (dev->flags & BLOCK_FLAG_READONLY) != 0;
}

/* ============================================================================
 * Debug Functions
 * ============================================================================ */

/**
 * Print all registered block devices
 */
void block_print_devices(void);

/**
 * Run block subsystem tests
 *
 * @return 0 on success, negative on failure
 */
int block_run_tests(void);

#endif /* EMBODIOS_BLOCK_H */
