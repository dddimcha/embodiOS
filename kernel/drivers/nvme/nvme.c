/* EMBODIOS NVMe Driver Implementation
 *
 * High-performance NVMe storage driver for bare-metal operation.
 *
 * Implementation notes:
 * - Uses polling mode (no interrupts) for simplicity
 * - Single I/O queue for sequential access
 * - PRP (Physical Region Page) for data transfer
 * - Supports up to 4KB block sizes
 */

#include <embodios/nvme.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* Memory barriers - architecture-specific */
#if defined(__x86_64__) || defined(__i386__)
#define mb()    __asm__ volatile("mfence" ::: "memory")
#define wmb()   __asm__ volatile("sfence" ::: "memory")
#define rmb()   __asm__ volatile("lfence" ::: "memory")
#elif defined(__aarch64__)
#define mb()    __asm__ volatile("dsb sy" ::: "memory")
#define wmb()   __asm__ volatile("dsb st" ::: "memory")
#define rmb()   __asm__ volatile("dsb ld" ::: "memory")
#else
#define mb()    __asm__ volatile("" ::: "memory")
#define wmb()   __asm__ volatile("" ::: "memory")
#define rmb()   __asm__ volatile("" ::: "memory")
#endif

/* Global NVMe controller state */
static nvme_ctrl_t g_nvme = {0};

/* Performance statistics */
static struct {
    uint64_t commands_issued;
    uint64_t blocks_read;
    uint64_t blocks_written;
    uint64_t read_errors;
    uint64_t write_errors;
    uint64_t timeouts;
} nvme_stats = {0};

/* DMA buffers for admin commands */
static uint8_t admin_sq_buffer[NVME_ADMIN_QUEUE_SIZE * NVME_SQ_ENTRY_SIZE]
    __attribute__((aligned(4096)));
static uint8_t admin_cq_buffer[NVME_ADMIN_QUEUE_SIZE * NVME_CQ_ENTRY_SIZE]
    __attribute__((aligned(4096)));

/* DMA buffers for I/O commands */
static uint8_t io_sq_buffer[NVME_IO_QUEUE_SIZE * NVME_SQ_ENTRY_SIZE]
    __attribute__((aligned(4096)));
static uint8_t io_cq_buffer[NVME_IO_QUEUE_SIZE * NVME_CQ_ENTRY_SIZE]
    __attribute__((aligned(4096)));

/* Identify data buffer (4KB aligned for DMA) */
static uint8_t identify_buffer[4096] __attribute__((aligned(4096)));

/* I/O data buffer - supports multi-block operations (up to 32 blocks) */
#define NVME_MAX_IO_BLOCKS  32
#define NVME_IO_BUFFER_SIZE (NVME_MAX_IO_BLOCKS * 4096)
static uint8_t io_buffer[NVME_IO_BUFFER_SIZE] __attribute__((aligned(4096)));

/* PRP list for transfers larger than 2 pages */
static uint64_t prp_list[512] __attribute__((aligned(4096)));

/* ============================================================================
 * Register Access
 * ============================================================================ */

static inline uint32_t nvme_read32(volatile void *base, uint32_t offset)
{
    return *(volatile uint32_t *)((uintptr_t)base + offset);
}

static inline uint64_t nvme_read64(volatile void *base, uint32_t offset)
{
    return *(volatile uint64_t *)((uintptr_t)base + offset);
}

static inline void nvme_write32(volatile void *base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)((uintptr_t)base + offset) = value;
}

static inline void nvme_write64(volatile void *base, uint32_t offset, uint64_t value)
{
    *(volatile uint64_t *)((uintptr_t)base + offset) = value;
}

/* ============================================================================
 * Queue Operations
 * ============================================================================ */

/**
 * Submit a command to a submission queue
 */
static void nvme_submit_cmd(nvme_queue_t *queue, nvme_sqe_t *cmd)
{
    volatile nvme_sqe_t *sq = (volatile nvme_sqe_t *)queue->sq;

    /* Copy command to queue */
    memcpy((void *)&sq[queue->sq_tail], cmd, sizeof(nvme_sqe_t));
    wmb();

    /* Advance tail and ring doorbell */
    queue->sq_tail = (queue->sq_tail + 1) % queue->size;
    *queue->sq_doorbell = queue->sq_tail;
}

/**
 * Wait for completion and return status
 * Returns 0 on success, status code on failure
 */
static int nvme_wait_completion(nvme_queue_t *queue, uint16_t cid, uint32_t timeout_ms)
{
    volatile nvme_cqe_t *cq = (volatile nvme_cqe_t *)queue->cq;
    volatile nvme_cqe_t *entry;
    uint32_t wait = 0;
    uint16_t status;

    while (wait < timeout_ms * 1000) {
        rmb();
        entry = &cq[queue->cq_head];

        /* Check phase bit */
        if (NVME_CQE_STATUS_P(entry->status) == queue->cq_phase) {
            /* Found completion */
            if (entry->cid == cid) {
                status = entry->status;

                /* Advance CQ head */
                queue->cq_head = (queue->cq_head + 1) % queue->size;
                if (queue->cq_head == 0) {
                    queue->cq_phase ^= 1;  /* Toggle phase */
                }

                /* Update doorbell */
                *queue->cq_doorbell = queue->cq_head;

                /* Return status code */
                return NVME_CQE_STATUS_SC(status);
            }
        }

        /* Small delay */
        for (volatile int i = 0; i < 100; i++);
        wait++;
    }

    nvme_stats.timeouts++;
    return NVME_ERR_TIMEOUT;
}

/**
 * Get next command ID
 */
static uint16_t nvme_get_cid(nvme_queue_t *queue)
{
    return queue->cid++;
}

/* ============================================================================
 * Admin Commands
 * ============================================================================ */

/**
 * Send Identify command
 * @param cns   CNS value (0=namespace, 1=controller)
 * @param nsid  Namespace ID (for CNS=0)
 * @param data  Output buffer (4KB)
 * @return 0 on success, error code on failure
 */
static int nvme_identify(uint8_t cns, uint32_t nsid, void *data)
{
    nvme_sqe_t cmd = {0};
    uint16_t cid;
    int ret;

    cid = nvme_get_cid(&g_nvme.admin_queue);

    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.cid = cid;
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t)(uintptr_t)identify_buffer;
    cmd.cdw10 = cns;

    nvme_submit_cmd(&g_nvme.admin_queue, &cmd);
    ret = nvme_wait_completion(&g_nvme.admin_queue, cid, 5000);

    if (ret == 0) {
        memcpy(data, identify_buffer, 4096);
    }

    return ret;
}

/**
 * Create I/O Completion Queue
 */
static int nvme_create_io_cq(uint16_t qid, uint16_t size, volatile void *buffer)
{
    nvme_sqe_t cmd = {0};
    uint16_t cid;

    cid = nvme_get_cid(&g_nvme.admin_queue);

    cmd.opcode = NVME_ADMIN_CREATE_CQ;
    cmd.cid = cid;
    cmd.prp1 = (uint64_t)(uintptr_t)buffer;
    cmd.cdw10 = ((size - 1) << 16) | qid;
    cmd.cdw11 = 1;  /* Physically contiguous, interrupts disabled */

    nvme_submit_cmd(&g_nvme.admin_queue, &cmd);
    return nvme_wait_completion(&g_nvme.admin_queue, cid, 5000);
}

/**
 * Create I/O Submission Queue
 */
static int nvme_create_io_sq(uint16_t qid, uint16_t size, volatile void *buffer, uint16_t cqid)
{
    nvme_sqe_t cmd = {0};
    uint16_t cid;

    cid = nvme_get_cid(&g_nvme.admin_queue);

    cmd.opcode = NVME_ADMIN_CREATE_SQ;
    cmd.cid = cid;
    cmd.prp1 = (uint64_t)(uintptr_t)buffer;
    cmd.cdw10 = ((size - 1) << 16) | qid;
    cmd.cdw11 = (cqid << 16) | 1;  /* CQ ID + Physically contiguous */

    nvme_submit_cmd(&g_nvme.admin_queue, &cmd);
    return nvme_wait_completion(&g_nvme.admin_queue, cid, 5000);
}

/* ============================================================================
 * Controller Initialization
 * ============================================================================ */

/**
 * Wait for controller ready status
 */
static int nvme_wait_ready(bool enable, uint32_t timeout_ms)
{
    uint32_t wait = 0;
    uint32_t csts;

    while (wait < timeout_ms) {
        csts = nvme_read32(g_nvme.regs, NVME_REG_CSTS);

        if (enable) {
            if (csts & NVME_CSTS_RDY) return 0;
        } else {
            if (!(csts & NVME_CSTS_RDY)) return 0;
        }

        /* Check for fatal error */
        if (csts & NVME_CSTS_CFS) {
            console_printf("[NVMe] Controller fatal status!\n");
            return NVME_ERR_INIT;
        }

        /* Delay ~1ms */
        for (volatile int i = 0; i < 10000; i++);
        wait++;
    }

    return NVME_ERR_TIMEOUT;
}

/**
 * Reset and configure controller
 */
static int nvme_reset_controller(void)
{
    uint32_t cc;
    uint32_t to_ms;
    int ret;

    /* Read capabilities */
    g_nvme.cap = nvme_read64(g_nvme.regs, NVME_REG_CAP);
    g_nvme.vs = nvme_read32(g_nvme.regs, NVME_REG_VS);

    /* Calculate timeout (CAP.TO is in 500ms units) */
    to_ms = NVME_CAP_TO(g_nvme.cap) * 500;
    if (to_ms == 0) to_ms = 1000;

    /* Calculate doorbell stride */
    g_nvme.doorbell_stride = 4 << NVME_CAP_DSTRD(g_nvme.cap);

    console_printf("[NVMe] Version: %d.%d.%d\n",
                   (g_nvme.vs >> 16) & 0xFFFF,
                   (g_nvme.vs >> 8) & 0xFF,
                   g_nvme.vs & 0xFF);
    console_printf("[NVMe] Max Queue Entries: %d\n", (int)NVME_CAP_MQES(g_nvme.cap) + 1);
    console_printf("[NVMe] Timeout: %d ms\n", to_ms);

    /* Disable controller first */
    cc = nvme_read32(g_nvme.regs, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        nvme_write32(g_nvme.regs, NVME_REG_CC, 0);
        ret = nvme_wait_ready(false, to_ms);
        if (ret != 0) {
            console_printf("[NVMe] Failed to disable controller\n");
            return ret;
        }
    }

    /* Setup admin queues */
    g_nvme.admin_queue.sq = admin_sq_buffer;
    g_nvme.admin_queue.cq = admin_cq_buffer;
    g_nvme.admin_queue.size = NVME_ADMIN_QUEUE_SIZE;
    g_nvme.admin_queue.sq_tail = 0;
    g_nvme.admin_queue.cq_head = 0;
    g_nvme.admin_queue.cq_phase = 1;
    g_nvme.admin_queue.cid = 0;
    g_nvme.admin_queue.id = 0;

    /* Calculate doorbell addresses */
    g_nvme.admin_queue.sq_doorbell = (volatile uint32_t *)
        ((uintptr_t)g_nvme.regs + NVME_REG_SQ0TDBL);
    g_nvme.admin_queue.cq_doorbell = (volatile uint32_t *)
        ((uintptr_t)g_nvme.regs + NVME_REG_SQ0TDBL + g_nvme.doorbell_stride);

    /* Clear queue buffers */
    memset(admin_sq_buffer, 0, sizeof(admin_sq_buffer));
    memset(admin_cq_buffer, 0, sizeof(admin_cq_buffer));

    /* Configure admin queue attributes */
    nvme_write32(g_nvme.regs, NVME_REG_AQA,
                 NVME_AQA_ASQS(NVME_ADMIN_QUEUE_SIZE - 1) |
                 NVME_AQA_ACQS(NVME_ADMIN_QUEUE_SIZE - 1));

    /* Set admin queue base addresses */
    nvme_write64(g_nvme.regs, NVME_REG_ASQ, (uint64_t)(uintptr_t)admin_sq_buffer);
    nvme_write64(g_nvme.regs, NVME_REG_ACQ, (uint64_t)(uintptr_t)admin_cq_buffer);

    /* Configure and enable controller */
    cc = NVME_CC_EN |
         NVME_CC_CSS_NVM |
         NVME_CC_MPS(0) |           /* 4KB pages */
         NVME_CC_AMS_RR |
         NVME_CC_SHN_NONE |
         NVME_CC_IOSQES(NVME_SQ_ENTRY_SHIFT) |
         NVME_CC_IOCQES(NVME_CQ_ENTRY_SHIFT);

    nvme_write32(g_nvme.regs, NVME_REG_CC, cc);

    /* Wait for ready */
    ret = nvme_wait_ready(true, to_ms);
    if (ret != 0) {
        console_printf("[NVMe] Failed to enable controller\n");
        return ret;
    }

    console_printf("[NVMe] Controller enabled\n");
    return NVME_OK;
}

/**
 * Setup I/O queue
 */
static int nvme_setup_io_queue(void)
{
    int ret;

    /* Setup I/O queue structure */
    g_nvme.io_queue.sq = io_sq_buffer;
    g_nvme.io_queue.cq = io_cq_buffer;
    g_nvme.io_queue.size = NVME_IO_QUEUE_SIZE;
    g_nvme.io_queue.sq_tail = 0;
    g_nvme.io_queue.cq_head = 0;
    g_nvme.io_queue.cq_phase = 1;
    g_nvme.io_queue.cid = 0;
    g_nvme.io_queue.id = 1;

    /* Calculate doorbell addresses for queue 1 */
    g_nvme.io_queue.sq_doorbell = (volatile uint32_t *)
        ((uintptr_t)g_nvme.regs + NVME_REG_SQ0TDBL + 2 * g_nvme.doorbell_stride);
    g_nvme.io_queue.cq_doorbell = (volatile uint32_t *)
        ((uintptr_t)g_nvme.regs + NVME_REG_SQ0TDBL + 3 * g_nvme.doorbell_stride);

    /* Clear queue buffers */
    memset(io_sq_buffer, 0, sizeof(io_sq_buffer));
    memset(io_cq_buffer, 0, sizeof(io_cq_buffer));

    /* Create I/O Completion Queue */
    ret = nvme_create_io_cq(1, NVME_IO_QUEUE_SIZE, io_cq_buffer);
    if (ret != 0) {
        console_printf("[NVMe] Failed to create I/O CQ: %d\n", ret);
        return NVME_ERR_INIT;
    }

    /* Create I/O Submission Queue */
    ret = nvme_create_io_sq(1, NVME_IO_QUEUE_SIZE, io_sq_buffer, 1);
    if (ret != 0) {
        console_printf("[NVMe] Failed to create I/O SQ: %d\n", ret);
        return NVME_ERR_INIT;
    }

    console_printf("[NVMe] I/O queue created (depth=%d)\n", NVME_IO_QUEUE_SIZE);
    return NVME_OK;
}

/**
 * Probe NVMe device from PCI
 */
static int nvme_probe(pci_device_t *dev)
{
    uint64_t bar0;
    size_t bar_size;
    int ret;

    console_printf("[NVMe] Probing device %04x:%04x\n",
                   dev->vendor_id, dev->device_id);

    /* Get BAR0 address */
    bar0 = pci_bar_address(dev, 0);
    bar_size = pci_bar_size(dev, 0);

    if (bar0 == 0 || bar_size == 0) {
        console_printf("[NVMe] Invalid BAR0\n");
        return NVME_ERR_INIT;
    }

    console_printf("[NVMe] BAR0 at 0x%llx (size=%zu KB)\n",
                   (unsigned long long)bar0, bar_size / 1024);

    /* Enable memory access and bus mastering */
    pci_enable_memory(dev);
    pci_enable_bus_master(dev);

    /* Store PCI device reference */
    g_nvme.pci_dev = dev;

    /* Map registers (identity mapping in our kernel) */
    g_nvme.regs = (volatile void *)(uintptr_t)bar0;

    /* Reset and configure controller */
    ret = nvme_reset_controller();
    if (ret != NVME_OK) {
        return ret;
    }

    /* Identify controller */
    ret = nvme_identify(NVME_ID_CNS_CTRL, 0, &g_nvme.id_ctrl);
    if (ret != 0) {
        console_printf("[NVMe] Identify controller failed: %d\n", ret);
        return NVME_ERR_INIT;
    }

    /* Extract controller info */
    g_nvme.nn = g_nvme.id_ctrl.nn;

    /* Calculate max transfer size */
    if (g_nvme.id_ctrl.mdts > 0) {
        g_nvme.max_transfer = (1 << g_nvme.id_ctrl.mdts) * 4096;
    } else {
        g_nvme.max_transfer = 1024 * 1024;  /* Default 1MB */
    }

    /* Print controller info - trim whitespace */
    char model[41] = {0};
    memcpy(model, g_nvme.id_ctrl.mn, 40);
    for (int i = 39; i >= 0 && model[i] == ' '; i--) {
        model[i] = '\0';
    }

    char serial[21] = {0};
    memcpy(serial, g_nvme.id_ctrl.sn, 20);
    for (int i = 19; i >= 0 && serial[i] == ' '; i--) {
        serial[i] = '\0';
    }

    console_printf("[NVMe] Model: %s\n", model);
    console_printf("[NVMe] Serial: %s\n", serial);
    console_printf("[NVMe] Namespaces: %d\n", g_nvme.nn);

    /* Setup I/O queue */
    ret = nvme_setup_io_queue();
    if (ret != NVME_OK) {
        return ret;
    }

    /* Identify namespace 1 */
    g_nvme.nsid = 1;
    ret = nvme_identify(NVME_ID_CNS_NS, 1, &g_nvme.id_ns);
    if (ret != 0) {
        console_printf("[NVMe] Identify namespace failed: %d\n", ret);
        return NVME_ERR_INIT;
    }

    /* Get namespace size and block size */
    g_nvme.ns_size = g_nvme.id_ns.nsze;
    uint8_t lba_format = g_nvme.id_ns.flbas & 0xF;
    g_nvme.block_size = 1 << g_nvme.id_ns.lbaf[lba_format].lbads;

    uint64_t capacity_mb = (g_nvme.ns_size * g_nvme.block_size) / (1024 * 1024);
    console_printf("[NVMe] Namespace 1: %llu blocks x %d bytes = %llu MB\n",
                   (unsigned long long)g_nvme.ns_size,
                   g_nvme.block_size,
                   (unsigned long long)capacity_mb);

    g_nvme.initialized = true;
    return NVME_OK;
}

/* PCI driver registration */
static pci_driver_t nvme_driver = {
    .name = "nvme",
    .vendor_id = PCI_ANY_ID,
    .device_id = PCI_ANY_ID,
    .class_code = NVME_PCI_CLASS,
    .subclass = NVME_PCI_SUBCLASS,
    .probe = nvme_probe,
    .remove = NULL,
    .next = NULL
};

/* ============================================================================
 * Public API
 * ============================================================================ */

int nvme_init(void)
{
    int ret;

    console_printf("[NVMe] Initializing NVMe subsystem...\n");

    /* Make sure PCI is initialized */
    if (!pci_is_initialized()) {
        console_printf("[NVMe] PCI not initialized\n");
        return NVME_ERR_NOT_FOUND;
    }

    /* Register driver */
    ret = pci_register_driver(&nvme_driver);
    if (ret != PCI_OK) {
        console_printf("[NVMe] Driver registration failed\n");
        return NVME_ERR_INIT;
    }

    /* Check if we found a device */
    if (!g_nvme.initialized) {
        console_printf("[NVMe] No NVMe device found\n");
        return NVME_ERR_NOT_FOUND;
    }

    console_printf("[NVMe] Initialization complete\n");
    return NVME_OK;
}

bool nvme_is_ready(void)
{
    return g_nvme.initialized;
}

/**
 * Setup PRP list for multi-page transfers
 * Returns prp2 value (either second page address or PRP list address)
 */
static uint64_t nvme_setup_prp(uint64_t buffer_addr, uint32_t length)
{
    uint32_t offset = buffer_addr & 0xFFF;  /* Offset within first page */
    uint32_t first_page_len = 4096 - offset;
    uint32_t remaining;
    uint32_t prp_count;

    if (length <= first_page_len) {
        /* Fits in first page, no PRP2 needed */
        return 0;
    }

    remaining = length - first_page_len;
    if (remaining <= 4096) {
        /* Fits in two pages, PRP2 is second page address */
        return (buffer_addr & ~0xFFFULL) + 4096;
    }

    /* Need PRP list for more than 2 pages */
    prp_count = (remaining + 4095) / 4096;
    for (uint32_t i = 0; i < prp_count && i < 512; i++) {
        prp_list[i] = (buffer_addr & ~0xFFFULL) + (i + 1) * 4096;
    }

    return (uint64_t)(uintptr_t)prp_list;
}

int nvme_read(uint64_t lba, uint32_t count, void *buffer)
{
    nvme_sqe_t cmd = {0};
    uint16_t cid;
    int ret;
    uint32_t blocks_read = 0;
    uint32_t batch_size;
    uint32_t transfer_bytes;

    if (!g_nvme.initialized) {
        return NVME_ERR_INIT;
    }

    if (!buffer) {
        return NVME_ERR_INVALID;
    }

    /* Process in batches for better performance */
    while (blocks_read < count) {
        /* Calculate batch size (limited by buffer and max transfer) */
        batch_size = count - blocks_read;
        if (batch_size > NVME_MAX_IO_BLOCKS) {
            batch_size = NVME_MAX_IO_BLOCKS;
        }

        /* Limit by max transfer size */
        uint32_t max_blocks = g_nvme.max_transfer / g_nvme.block_size;
        if (batch_size > max_blocks && max_blocks > 0) {
            batch_size = max_blocks;
        }

        transfer_bytes = batch_size * g_nvme.block_size;

        memset(&cmd, 0, sizeof(cmd));
        cid = nvme_get_cid(&g_nvme.io_queue);

        cmd.opcode = NVME_CMD_READ;
        cmd.cid = cid;
        cmd.nsid = g_nvme.nsid;
        cmd.prp1 = (uint64_t)(uintptr_t)io_buffer;
        cmd.prp2 = nvme_setup_prp((uint64_t)(uintptr_t)io_buffer, transfer_bytes);
        cmd.cdw10 = (uint32_t)(lba + blocks_read);
        cmd.cdw11 = (uint32_t)((lba + blocks_read) >> 32);
        cmd.cdw12 = batch_size - 1;  /* NLB is 0-based */

        nvme_submit_cmd(&g_nvme.io_queue, &cmd);
        nvme_stats.commands_issued++;

        ret = nvme_wait_completion(&g_nvme.io_queue, cid, 5000);

        if (ret != 0) {
            nvme_stats.read_errors++;
            console_printf("[NVMe] Read failed at LBA %llu: %d\n",
                           (unsigned long long)(lba + blocks_read), ret);
            return (blocks_read > 0) ? (int)blocks_read : NVME_ERR_IO;
        }

        /* Copy data to user buffer */
        memcpy((uint8_t *)buffer + blocks_read * g_nvme.block_size,
               io_buffer, transfer_bytes);

        nvme_stats.blocks_read += batch_size;
        blocks_read += batch_size;
    }

    return (int)blocks_read;
}

int nvme_write(uint64_t lba, uint32_t count, const void *buffer)
{
    nvme_sqe_t cmd = {0};
    uint16_t cid;
    int ret;
    uint32_t blocks_written = 0;
    uint32_t batch_size;
    uint32_t transfer_bytes;

    if (!g_nvme.initialized) {
        return NVME_ERR_INIT;
    }

    if (!buffer) {
        return NVME_ERR_INVALID;
    }

    /* Process in batches for better performance */
    while (blocks_written < count) {
        /* Calculate batch size */
        batch_size = count - blocks_written;
        if (batch_size > NVME_MAX_IO_BLOCKS) {
            batch_size = NVME_MAX_IO_BLOCKS;
        }

        /* Limit by max transfer size */
        uint32_t max_blocks = g_nvme.max_transfer / g_nvme.block_size;
        if (batch_size > max_blocks && max_blocks > 0) {
            batch_size = max_blocks;
        }

        transfer_bytes = batch_size * g_nvme.block_size;

        /* Copy data to DMA buffer */
        memcpy(io_buffer,
               (const uint8_t *)buffer + blocks_written * g_nvme.block_size,
               transfer_bytes);

        memset(&cmd, 0, sizeof(cmd));
        cid = nvme_get_cid(&g_nvme.io_queue);

        cmd.opcode = NVME_CMD_WRITE;
        cmd.cid = cid;
        cmd.nsid = g_nvme.nsid;
        cmd.prp1 = (uint64_t)(uintptr_t)io_buffer;
        cmd.prp2 = nvme_setup_prp((uint64_t)(uintptr_t)io_buffer, transfer_bytes);
        cmd.cdw10 = (uint32_t)(lba + blocks_written);
        cmd.cdw11 = (uint32_t)((lba + blocks_written) >> 32);
        cmd.cdw12 = batch_size - 1;  /* NLB is 0-based */

        nvme_submit_cmd(&g_nvme.io_queue, &cmd);
        nvme_stats.commands_issued++;

        ret = nvme_wait_completion(&g_nvme.io_queue, cid, 5000);

        if (ret != 0) {
            nvme_stats.write_errors++;
            console_printf("[NVMe] Write failed at LBA %llu: %d\n",
                           (unsigned long long)(lba + blocks_written), ret);
            return (blocks_written > 0) ? (int)blocks_written : NVME_ERR_IO;
        }

        nvme_stats.blocks_written += batch_size;
        blocks_written += batch_size;
    }

    return (int)blocks_written;
}

int nvme_flush(void)
{
    nvme_sqe_t cmd = {0};
    uint16_t cid;
    int ret;

    if (!g_nvme.initialized) {
        return NVME_ERR_INIT;
    }

    cid = nvme_get_cid(&g_nvme.io_queue);

    cmd.opcode = NVME_CMD_FLUSH;
    cmd.cid = cid;
    cmd.nsid = g_nvme.nsid;

    nvme_submit_cmd(&g_nvme.io_queue, &cmd);
    ret = nvme_wait_completion(&g_nvme.io_queue, cid, 5000);

    if (ret != 0) {
        console_printf("[NVMe] Flush failed: %d\n", ret);
        return NVME_ERR_IO;
    }

    return NVME_OK;
}

void nvme_get_info(uint64_t *capacity, uint32_t *block_size,
                   char *model, size_t model_len)
{
    if (!g_nvme.initialized) {
        if (capacity) *capacity = 0;
        if (block_size) *block_size = 0;
        if (model && model_len > 0) model[0] = '\0';
        return;
    }

    if (capacity) {
        *capacity = g_nvme.ns_size * g_nvme.block_size;
    }

    if (block_size) {
        *block_size = g_nvme.block_size;
    }

    if (model && model_len > 0) {
        size_t copy_len = model_len - 1;
        if (copy_len > 40) copy_len = 40;
        memcpy(model, g_nvme.id_ctrl.mn, copy_len);
        model[copy_len] = '\0';

        /* Trim trailing spaces */
        for (size_t i = copy_len; i > 0; i--) {
            if (model[i-1] == ' ' || model[i-1] == '\0') {
                model[i-1] = '\0';
            } else {
                break;
            }
        }
    }
}

void nvme_print_info(void)
{
    if (!g_nvme.initialized) {
        console_printf("[NVMe] Not initialized\n");
        return;
    }

    char model[41] = {0};
    char serial[21] = {0};

    memcpy(model, g_nvme.id_ctrl.mn, 40);
    memcpy(serial, g_nvme.id_ctrl.sn, 20);

    /* Trim */
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';
    for (int i = 19; i >= 0 && serial[i] == ' '; i--) serial[i] = '\0';

    uint64_t capacity_mb = (g_nvme.ns_size * g_nvme.block_size) / (1024 * 1024);

    console_printf("\n[NVMe] Controller Information:\n");
    console_printf("  Model:          %s\n", model);
    console_printf("  Serial:         %s\n", serial);
    console_printf("  Version:        %d.%d.%d\n",
                   (g_nvme.vs >> 16) & 0xFFFF,
                   (g_nvme.vs >> 8) & 0xFF,
                   g_nvme.vs & 0xFF);
    console_printf("  Namespaces:     %d\n", g_nvme.nn);
    console_printf("  Max Transfer:   %d KB\n", g_nvme.max_transfer / 1024);
    console_printf("\n[NVMe] Namespace 1:\n");
    console_printf("  Size:           %llu blocks\n", (unsigned long long)g_nvme.ns_size);
    console_printf("  Block Size:     %d bytes\n", g_nvme.block_size);
    console_printf("  Capacity:       %llu MB\n", (unsigned long long)capacity_mb);

    console_printf("\n[NVMe] I/O Statistics:\n");
    console_printf("  Commands Issued: %llu\n", (unsigned long long)nvme_stats.commands_issued);
    console_printf("  Blocks Read:     %llu\n", (unsigned long long)nvme_stats.blocks_read);
    console_printf("  Blocks Written:  %llu\n", (unsigned long long)nvme_stats.blocks_written);
    console_printf("  Read Errors:     %llu\n", (unsigned long long)nvme_stats.read_errors);
    console_printf("  Write Errors:    %llu\n", (unsigned long long)nvme_stats.write_errors);
    console_printf("  Timeouts:        %llu\n", (unsigned long long)nvme_stats.timeouts);
    console_printf("\n");
}

int nvme_run_tests(void)
{
    int passed = 0;
    int failed = 0;
    uint8_t test_buf[512];
    int ret;

    console_printf("\n[NVMe] Running self-tests...\n");

    /* Test 1: Initialization */
    console_printf("  Test 1: Initialization... ");
    if (g_nvme.initialized) {
        console_printf("PASS\n");
        passed++;
    } else {
        console_printf("FAIL\n");
        failed++;
        goto done;
    }

    /* Test 2: Read first block */
    console_printf("  Test 2: Read block 0... ");
    ret = nvme_read(0, 1, test_buf);
    if (ret == 1) {
        console_printf("PASS\n");
        passed++;
    } else {
        console_printf("FAIL (%d)\n", ret);
        failed++;
    }

    /* Test 3: Read with large LBA */
    console_printf("  Test 3: Read block 1000... ");
    if (g_nvme.ns_size > 1000) {
        ret = nvme_read(1000, 1, test_buf);
        if (ret == 1) {
            console_printf("PASS\n");
            passed++;
        } else {
            console_printf("FAIL (%d)\n", ret);
            failed++;
        }
    } else {
        console_printf("SKIP (disk too small)\n");
    }

    /* Test 4: Flush */
    console_printf("  Test 4: Flush... ");
    ret = nvme_flush();
    if (ret == NVME_OK) {
        console_printf("PASS\n");
        passed++;
    } else {
        console_printf("FAIL (%d)\n", ret);
        failed++;
    }

done:
    console_printf("[NVMe] Tests complete: %d passed, %d failed\n\n",
                   passed, failed);

    return (failed == 0) ? 0 : -1;
}
