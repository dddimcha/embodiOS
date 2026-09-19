/* EMBODIOS embfs - Minimal Persistent File System
 *
 * Flat file table stored in a reserved region at the start of a raw
 * virtio-blk disk. Designed for config/small-file persistence in QEMU.
 *
 * On-disk layout (512-byte sectors, 4K data blocks):
 *   sector 0        Superblock (magic "EMBFS001", CRC32, active table ptr)
 *   sector 1..8     File table copy A (magic "EMBFTAB1", generation, CRC32)
 *   sector 9..16    File table copy B (double-buffer for atomic commits)
 *   sector 17..2047 Reserved (zero)
 *   sector 2048..   Data area (4K-aligned blocks, first-fit allocation)
 *
 * Reliability:
 *  - Superblock and both table copies are CRC32-protected.
 *  - Table commits are atomic: new table is written to the inactive copy,
 *    flushed, then the superblock is flipped to it. A crash mid-commit
 *    leaves the previous valid (superblock, table) pair intact.
 *  - Mount picks the valid table copy with the highest generation.
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#ifndef EMBODIOS_EMBFS_H
#define EMBODIOS_EMBFS_H

#include <embodios/types.h>
#include <embodios/block.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define EMBFS_MAGIC_SB      "EMBFS001"
#define EMBFS_MAGIC_TABLE   "EMBFTAB1"
#define EMBFS_VERSION       1

#define EMBFS_MAX_FILES     64
#define EMBFS_NAME_LEN      32
#define EMBFS_BLOCK_SIZE    4096            /* Data allocation unit */

#define EMBFS_SECTOR_SB         0           /* Superblock sector */
#define EMBFS_SECTOR_TABLE_A    1           /* Table copy A: sectors 1..8 */
#define EMBFS_SECTOR_TABLE_B    9           /* Table copy B: sectors 9..16 */
#define EMBFS_TABLE_SECTORS     8           /* 4096 bytes per table copy */
#define EMBFS_DATA_SECTOR       2048        /* Data starts at 1MB */
#define EMBFS_MIN_SECTORS       (EMBFS_DATA_SECTOR + 8)

/* Error codes */
#define EMBFS_OK            0
#define EMBFS_ERR_IO        -1
#define EMBFS_ERR_NOMEM     -2
#define EMBFS_ERR_INVALID   -3
#define EMBFS_ERR_NOT_FOUND -4
#define EMBFS_ERR_NO_DISK   -5      /* No suitable block device */
#define EMBFS_ERR_NO_FS     -6      /* Device has no embfs volume */
#define EMBFS_ERR_FULL      -7      /* File table full */
#define EMBFS_ERR_NOSPACE   -8      /* Data area full */
#define EMBFS_ERR_EXISTS    -9
#define EMBFS_ERR_CRC       -10     /* Checksum mismatch */
#define EMBFS_ERR_READONLY  -11

/* ============================================================================
 * On-disk Structures (little-endian, packed)
 * ============================================================================ */

struct embfs_superblock {
    char     magic[8];          /* EMBFS_MAGIC_SB */
    uint32_t version;
    uint32_t active_table;      /* 0 = copy A, 1 = copy B */
    uint64_t generation;        /* Table generation counter */
    uint64_t total_sectors;     /* Device capacity at format time */
    uint32_t data_start;        /* First data sector (EMBFS_DATA_SECTOR) */
    uint32_t total_blocks;      /* Number of 4K data blocks */
    uint32_t crc32;             /* CRC32 of all fields above */
    uint8_t  reserved[512 - 8 - 4 - 4 - 8 - 8 - 4 - 4 - 4];
} __packed;

struct embfs_entry {
    char     name[EMBFS_NAME_LEN];  /* NUL-terminated file name */
    uint32_t size;                  /* Bytes used */
    uint32_t start_block;           /* First 4K block (from data start) */
    uint32_t blocks;                /* Allocated 4K blocks */
    uint32_t crc32;                 /* CRC32 of file data */
    uint32_t used;                  /* 1 = entry in use */
    uint32_t reserved;
} __packed;                         /* 56 bytes */

struct embfs_table {
    char     magic[8];              /* EMBFS_MAGIC_TABLE */
    uint64_t generation;            /* Commit generation */
    uint32_t count;                 /* Number of used entries */
    uint32_t crc32;                 /* CRC32 of count + entries */
    struct embfs_entry entries[EMBFS_MAX_FILES];
    uint8_t  reserved[EMBFS_TABLE_SECTORS * 512
                      - 8 - 8 - 4 - 4
                      - EMBFS_MAX_FILES * (int)sizeof(struct embfs_entry)];
} __packed;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Scan block devices for an embfs volume and mount it.
 * Skips read-only devices and devices whose first bytes look like a
 * GGUF model (to never clobber a model disk).
 *
 * @return EMBFS_OK if a volume was mounted, EMBFS_ERR_NO_DISK / EMBFS_ERR_NO_FS
 */
int embfs_mount(void);

/**
 * Format the first suitable block device with a fresh embfs volume.
 * Refuses to format devices containing a GGUF model or a valid embfs
 * volume unless force is set.
 */
int embfs_format(bool force);

/**
 * Ensure an embfs volume is available: mount existing, or auto-format a
 * suitable (writable, non-GGUF) device. Used lazily by file commands so
 * a fresh blank disk "just works".
 */
int embfs_ensure(void);

/** True if an embfs volume is currently mounted. */
bool embfs_mounted(void);

/** Create or overwrite a file. Data CRC32 is stored and verified on read. */
int embfs_write(const char* name, const void* data, uint32_t size);

/**
 * Read a file. If buf is NULL, only returns the size.
 * Verifies the stored CRC32 (EMBFS_ERR_CRC on mismatch).
 */
int embfs_read(const char* name, void* buf, uint32_t buf_size,
               uint32_t* out_size);

/** Delete a file. */
int embfs_delete(const char* name);

/** True if a file exists. */
bool embfs_exists(const char* name);

/** Print file listing (fls). */
void embfs_list(void);

/** Print usage statistics (df). */
void embfs_df(void);

/** CRC32 (IEEE 802.3 polynomial, table-driven). */
uint32_t embfs_crc32(const void* data, size_t len);

#endif /* EMBODIOS_EMBFS_H */
