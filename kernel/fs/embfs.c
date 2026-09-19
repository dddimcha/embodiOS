/* EMBODIOS embfs - Minimal Persistent File System
 *
 * See kernel/include/embodios/embfs.h for the on-disk layout and the
 * reliability model (CRC32 + double-buffered atomic table commits).
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#include <embodios/embfs.h>
#include <embodios/console.h>
#include <embodios/kernel.h>
#include <embodios/mm.h>

/* ============================================================================
 * CRC32 (IEEE 802.3)
 * ============================================================================ */

static uint32_t crc32_table[256];
static bool crc32_table_ready = false;

static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = true;
}

uint32_t embfs_crc32(const void* data, size_t len)
{
    if (!crc32_table_ready) {
        crc32_init_table();
    }
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ============================================================================
 * Module State
 * ============================================================================ */

static block_device_t* g_dev = NULL;
static bool g_mounted = false;

/* In-memory copy of the active file table */
static struct embfs_table g_table;
static struct embfs_superblock g_sb;
static uint8_t g_sector_buf[EMBFS_TABLE_SECTORS * 512];  /* 4K scratch */

/* ============================================================================
 * Low-level Helpers
 * ============================================================================ */

static int embfs_flush(void)
{
    if (g_dev && g_dev->ops && g_dev->ops->flush) {
        return g_dev->ops->flush(g_dev);
    }
    return BLOCK_OK;
}

/* Bytes covered by the superblock CRC: magic..total_blocks */
#define EMBFS_SB_CRC_LEN  (8 + 4 + 4 + 8 + 8 + 4 + 4)

static uint32_t sb_crc(const struct embfs_superblock* sb)
{
    return embfs_crc32(sb, EMBFS_SB_CRC_LEN);
}

static uint32_t table_crc(const struct embfs_table* t)
{
    uint32_t crc = embfs_crc32(&t->count, sizeof(t->count));
    /* CRC over count || entries: continue the CRC chain manually */
    if (!crc32_table_ready) crc32_init_table();
    const uint8_t* p = (const uint8_t*)t->entries;
    crc ^= 0xFFFFFFFFU;
    for (size_t i = 0; i < sizeof(t->entries); i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

static bool sb_valid(const struct embfs_superblock* sb)
{
    return memcmp(sb->magic, EMBFS_MAGIC_SB, 8) == 0 &&
           sb->version == EMBFS_VERSION &&
           sb->crc32 == sb_crc(sb);
}

static bool table_valid(const struct embfs_table* t)
{
    return memcmp(t->magic, EMBFS_MAGIC_TABLE, 8) == 0 &&
           t->crc32 == table_crc(t);
}

/* Does this device look like a raw GGUF model disk? */
static bool dev_looks_like_gguf(block_device_t* dev)
{
    uint8_t buf[512];
    if (block_read(dev, 0, 1, buf) != BLOCK_OK) {
        return true;  /* Unreadable: stay away */
    }
    return memcmp(buf, "GGUF", 4) == 0;
}

static bool dev_is_blank(block_device_t* dev)
{
    uint8_t buf[512];
    if (block_read(dev, 0, 1, buf) != BLOCK_OK) {
        return false;
    }
    for (int i = 0; i < 512; i++) {
        if (buf[i] != 0) return false;
    }
    return true;
}

static bool dev_suitable(block_device_t* dev)
{
    if (!dev) return false;
    if (dev->flags & BLOCK_FLAG_READONLY) return false;
    if (dev->total_sectors < EMBFS_MIN_SECTORS) return false;
    return true;
}

/* ============================================================================
 * Mount / Format
 * ============================================================================ */

static int embfs_load_table(block_device_t* dev,
                            const struct embfs_superblock* sb)
{
    struct embfs_table* ta = (struct embfs_table*)g_sector_buf;
    static struct embfs_table tb;
    bool a_ok = false, b_ok = false;

    if (block_read(dev, EMBFS_SECTOR_TABLE_A, EMBFS_TABLE_SECTORS,
                   g_sector_buf) == BLOCK_OK) {
        a_ok = table_valid(ta);
    }
    if (block_read(dev, EMBFS_SECTOR_TABLE_B, EMBFS_TABLE_SECTORS,
                   &tb) == BLOCK_OK) {
        b_ok = table_valid(&tb);
    }

    if (!a_ok && !b_ok) {
        return EMBFS_ERR_CRC;
    }

    /* Pick the valid copy with the highest generation; prefer the one the
     * superblock points to on ties. */
    const struct embfs_table* pick = NULL;
    if (a_ok && b_ok) {
        if (ta->generation == tb.generation) {
            pick = (sb->active_table == 0) ? ta : &tb;
        } else {
            pick = (ta->generation > tb.generation) ? ta : &tb;
        }
    } else {
        pick = a_ok ? ta : &tb;
    }

    memcpy(&g_table, pick, sizeof(g_table));
    return EMBFS_OK;
}

int embfs_mount(void)
{
    g_mounted = false;
    g_dev = NULL;

    int ndev = block_device_count();
    for (int i = 0; i < ndev; i++) {
        block_device_t* dev = block_get_device_by_index(i);
        if (!dev_suitable(dev)) {
            continue;
        }
        if (dev_looks_like_gguf(dev)) {
            continue;  /* Never touch a model disk */
        }

        struct embfs_superblock sb;
        if (block_read(dev, EMBFS_SECTOR_SB, 1, &sb) != BLOCK_OK) {
            continue;
        }
        if (!sb_valid(&sb)) {
            continue;  /* Not an embfs volume */
        }

        if (embfs_load_table(dev, &sb) != EMBFS_OK) {
            console_printf("[embfs] %s: superblock OK but no valid table\n",
                           dev->name);
            continue;
        }

        g_dev = dev;
        g_sb = sb;
        g_mounted = true;
        console_printf("[embfs] Mounted on %s: %u file(s), %llu MB data area\n",
                       dev->name, g_table.count,
                       (uint64_t)g_sb.total_blocks * EMBFS_BLOCK_SIZE / (1024*1024));
        return EMBFS_OK;
    }

    return (ndev > 0) ? EMBFS_ERR_NO_FS : EMBFS_ERR_NO_DISK;
}

int embfs_format(bool force)
{
    int ndev = block_device_count();
    block_device_t* dev = NULL;

    for (int i = 0; i < ndev; i++) {
        block_device_t* cand = block_get_device_by_index(i);
        if (!dev_suitable(cand) || dev_looks_like_gguf(cand)) {
            continue;
        }

        struct embfs_superblock sb;
        bool has_embfs = false;
        if (block_read(cand, EMBFS_SECTOR_SB, 1, &sb) == BLOCK_OK) {
            has_embfs = sb_valid(&sb);
        }
        if (has_embfs && !force) {
            continue;  /* Already formatted, keep looking */
        }
        if (!has_embfs && !dev_is_blank(cand)) {
            /* Unknown data on device: only touch it when forced */
            if (!force) continue;
        }
        dev = cand;
        break;
    }

    if (!dev) {
        return (ndev > 0) ? EMBFS_ERR_INVALID : EMBFS_ERR_NO_DISK;
    }

    /* Zero both table regions */
    memset(g_sector_buf, 0, sizeof(g_sector_buf));
    if (block_write(dev, EMBFS_SECTOR_TABLE_A, EMBFS_TABLE_SECTORS,
                    g_sector_buf) != BLOCK_OK ||
        block_write(dev, EMBFS_SECTOR_TABLE_B, EMBFS_TABLE_SECTORS,
                    g_sector_buf) != BLOCK_OK) {
        return EMBFS_ERR_IO;
    }

    /* Empty table, generation 1, copy A active */
    memset(&g_table, 0, sizeof(g_table));
    memcpy(g_table.magic, EMBFS_MAGIC_TABLE, 8);
    g_table.generation = 1;
    g_table.count = 0;
    g_table.crc32 = table_crc(&g_table);

    if (block_write(dev, EMBFS_SECTOR_TABLE_A, EMBFS_TABLE_SECTORS,
                    &g_table) != BLOCK_OK) {
        return EMBFS_ERR_IO;
    }

    memset(&g_sb, 0, sizeof(g_sb));
    memcpy(g_sb.magic, EMBFS_MAGIC_SB, 8);
    g_sb.version = EMBFS_VERSION;
    g_sb.active_table = 0;
    g_sb.generation = 1;
    g_sb.total_sectors = dev->total_sectors;
    g_sb.data_start = EMBFS_DATA_SECTOR;
    g_sb.total_blocks = (uint32_t)((dev->total_sectors - EMBFS_DATA_SECTOR) *
                                   512 / EMBFS_BLOCK_SIZE);
    g_sb.crc32 = sb_crc(&g_sb);

    if (block_write(dev, EMBFS_SECTOR_SB, 1, &g_sb) != BLOCK_OK) {
        return EMBFS_ERR_IO;
    }

    /* Flush everything to stable storage */
    g_dev = dev;
    int fret = embfs_flush();
    g_dev = NULL;
    if (fret != BLOCK_OK) {
        return EMBFS_ERR_IO;
    }

    console_printf("[embfs] Formatted %s: %u data blocks (%llu MB)\n",
                   dev->name, g_sb.total_blocks,
                   (uint64_t)g_sb.total_blocks * EMBFS_BLOCK_SIZE / (1024*1024));
    return EMBFS_OK;
}

int embfs_ensure(void)
{
    if (g_mounted) {
        return EMBFS_OK;
    }
    int ret = embfs_mount();
    if (ret == EMBFS_ERR_NO_FS) {
        /* Blank suitable disk present: auto-format so fsave just works */
        ret = embfs_format(false);
        if (ret == EMBFS_OK) {
            ret = embfs_mount();
        }
    }
    return ret;
}

bool embfs_mounted(void)
{
    return g_mounted;
}

/* ============================================================================
 * Space Allocation (first-fit over the in-memory table)
 * ============================================================================ */

static bool range_free(uint32_t start, uint32_t blocks,
                       const struct embfs_entry* ignore)
{
    for (int i = 0; i < EMBFS_MAX_FILES; i++) {
        const struct embfs_entry* e = &g_table.entries[i];
        if (!e->used || e == ignore) continue;
        if (start < e->start_block + e->blocks &&
            e->start_block < start + blocks) {
            return false;  /* Overlap */
        }
    }
    return true;
}

static int alloc_blocks(uint32_t blocks, const struct embfs_entry* ignore,
                        uint32_t* out_start)
{
    if (blocks == 0 || blocks > g_sb.total_blocks) {
        return EMBFS_ERR_NOSPACE;
    }
    for (uint32_t b = 0; b + blocks <= g_sb.total_blocks; b++) {
        if (range_free(b, blocks, ignore)) {
            *out_start = b;
            return EMBFS_OK;
        }
    }
    return EMBFS_ERR_NOSPACE;
}

static struct embfs_entry* find_entry(const char* name)
{
    for (int i = 0; i < EMBFS_MAX_FILES; i++) {
        struct embfs_entry* e = &g_table.entries[i];
        if (e->used && strcmp(e->name, name) == 0) {
            return e;
        }
    }
    return NULL;
}

/* ============================================================================
 * Atomic Table Commit
 * ============================================================================ */

static int embfs_commit(void)
{
    uint32_t inactive = 1 - g_sb.active_table;
    uint64_t sector = (inactive == 0) ? EMBFS_SECTOR_TABLE_A
                                      : EMBFS_SECTOR_TABLE_B;

    /* Recount and re-checksum */
    uint32_t count = 0;
    for (int i = 0; i < EMBFS_MAX_FILES; i++) {
        if (g_table.entries[i].used) count++;
    }
    g_table.count = count;
    g_table.generation = g_sb.generation + 1;
    g_table.crc32 = table_crc(&g_table);

    /* 1. Write new table to the inactive copy */
    if (block_write(g_dev, sector, EMBFS_TABLE_SECTORS, &g_table) != BLOCK_OK) {
        return EMBFS_ERR_IO;
    }
    if (embfs_flush() != BLOCK_OK) {
        return EMBFS_ERR_IO;
    }

    /* 2. Flip the superblock to the new table (atomic commit point) */
    g_sb.active_table = inactive;
    g_sb.generation = g_table.generation;
    g_sb.crc32 = sb_crc(&g_sb);
    if (block_write(g_dev, EMBFS_SECTOR_SB, 1, &g_sb) != BLOCK_OK) {
        return EMBFS_ERR_IO;
    }
    if (embfs_flush() != BLOCK_OK) {
        return EMBFS_ERR_IO;
    }

    return EMBFS_OK;
}

/* ============================================================================
 * File Operations
 * ============================================================================ */

static bool name_valid(const char* name)
{
    if (!name || !name[0]) return false;
    size_t len = strlen(name);
    if (len >= EMBFS_NAME_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c < 32 || c == '/' || c == 127) return false;
    }
    return true;
}

int embfs_write(const char* name, const void* data, uint32_t size)
{
    if (!name_valid(name) || (!data && size > 0)) {
        return EMBFS_ERR_INVALID;
    }
    int ret = embfs_ensure();
    if (ret != EMBFS_OK) return ret;

    uint32_t blocks = (size + EMBFS_BLOCK_SIZE - 1) / EMBFS_BLOCK_SIZE;
    if (blocks == 0) blocks = 1;

    struct embfs_entry* e = find_entry(name);
    uint32_t start = 0;

    if (e && blocks <= e->blocks) {
        /* Fits in the existing allocation: overwrite in place */
        start = e->start_block;
    } else {
        ret = alloc_blocks(blocks, e, &start);
        if (ret != EMBFS_OK) return ret;
    }

    /* Stage data in a padded, block-aligned buffer */
    size_t padded = (size_t)blocks * EMBFS_BLOCK_SIZE;
    uint8_t* buf = (uint8_t*)heap_alloc(padded);
    if (!buf) return EMBFS_ERR_NOMEM;
    memset(buf, 0, padded);
    if (size > 0) memcpy(buf, data, size);

    uint64_t sector = (uint64_t)g_sb.data_start + (uint64_t)start * 8;
    int bret = block_write(g_dev, sector, blocks * 8, buf);
    heap_free(buf);
    if (bret != BLOCK_OK) return EMBFS_ERR_IO;
    if (embfs_flush() != BLOCK_OK) return EMBFS_ERR_IO;

    /* Update table and commit (data is already on stable storage) */
    if (!e) {
        for (int i = 0; i < EMBFS_MAX_FILES; i++) {
            if (!g_table.entries[i].used) {
                e = &g_table.entries[i];
                break;
            }
        }
        if (!e) return EMBFS_ERR_FULL;
        memset(e, 0, sizeof(*e));
        e->used = 1;
        strncpy(e->name, name, EMBFS_NAME_LEN - 1);
    }
    e->start_block = start;
    e->blocks = blocks;
    e->size = size;
    e->crc32 = embfs_crc32(data, size);

    return embfs_commit();
}

int embfs_read(const char* name, void* buf, uint32_t buf_size,
               uint32_t* out_size)
{
    if (!name_valid(name)) {
        return EMBFS_ERR_INVALID;
    }
    int ret = embfs_ensure();
    if (ret != EMBFS_OK) return ret;

    struct embfs_entry* e = find_entry(name);
    if (!e) return EMBFS_ERR_NOT_FOUND;

    if (out_size) *out_size = e->size;
    if (!buf) return EMBFS_OK;
    if (buf_size < e->size) return EMBFS_ERR_INVALID;

    size_t padded = (size_t)e->blocks * EMBFS_BLOCK_SIZE;
    uint8_t* tmp = (uint8_t*)heap_alloc(padded);
    if (!tmp) return EMBFS_ERR_NOMEM;

    uint64_t sector = (uint64_t)g_sb.data_start + (uint64_t)e->start_block * 8;
    int bret = block_read(g_dev, sector, e->blocks * 8, tmp);
    if (bret != BLOCK_OK) {
        heap_free(tmp);
        return EMBFS_ERR_IO;
    }

    if (embfs_crc32(tmp, e->size) != e->crc32) {
        heap_free(tmp);
        return EMBFS_ERR_CRC;
    }

    if (e->size > 0) memcpy(buf, tmp, e->size);
    heap_free(tmp);
    return EMBFS_OK;
}

int embfs_delete(const char* name)
{
    if (!name_valid(name)) {
        return EMBFS_ERR_INVALID;
    }
    int ret = embfs_ensure();
    if (ret != EMBFS_OK) return ret;

    struct embfs_entry* e = find_entry(name);
    if (!e) return EMBFS_ERR_NOT_FOUND;

    memset(e, 0, sizeof(*e));  /* Space is reclaimable immediately */
    return embfs_commit();
}

bool embfs_exists(const char* name)
{
    if (!name_valid(name)) return false;
    if (embfs_ensure() != EMBFS_OK) return false;
    return find_entry(name) != NULL;
}

/* ============================================================================
 * Info / Listing
 * ============================================================================ */

void embfs_list(void)
{
    if (embfs_ensure() != EMBFS_OK) {
        console_printf("embfs: no volume (attach a disk: "
                       "-drive file=disk.img,format=raw,if=virtio)\n");
        return;
    }

    console_printf("\nembfs on %s: %u file(s)\n", g_dev->name, g_table.count);
    console_printf("  %-24s %10s  %s\n", "NAME", "SIZE", "BLOCKS");
    for (int i = 0; i < EMBFS_MAX_FILES; i++) {
        struct embfs_entry* e = &g_table.entries[i];
        if (e->used) {
            console_printf("  %-24s %10u  %u @ blk %u\n",
                           e->name, e->size, e->blocks, e->start_block);
        }
    }
    console_printf("\n");
}

void embfs_df(void)
{
    if (embfs_ensure() != EMBFS_OK) {
        console_printf("embfs: no volume mounted\n");
        return;
    }

    uint32_t used_blocks = 0;
    for (int i = 0; i < EMBFS_MAX_FILES; i++) {
        if (g_table.entries[i].used) {
            used_blocks += g_table.entries[i].blocks;
        }
    }

    uint64_t total = (uint64_t)g_sb.total_blocks * EMBFS_BLOCK_SIZE;
    uint64_t used = (uint64_t)used_blocks * EMBFS_BLOCK_SIZE;

    console_printf("\nembfs on %s:\n", g_dev->name);
    console_printf("  Total:     %llu KB (%u blocks)\n",
                   total / 1024, g_sb.total_blocks);
    console_printf("  Used:      %llu KB (%u blocks)\n", used / 1024, used_blocks);
    console_printf("  Free:      %llu KB\n", (total - used) / 1024);
    console_printf("  Files:     %u / %u\n", g_table.count, EMBFS_MAX_FILES);
    console_printf("  Generation: %llu (table %s)\n\n", g_sb.generation,
                   g_sb.active_table == 0 ? "A" : "B");
}
