/* EMBODIOS NVMe Driver Interface
 *
 * NVMe (Non-Volatile Memory Express) driver for high-performance
 * SSD storage access. Provides basic read/write functionality.
 *
 * Features:
 * - PCI device detection and initialization
 * - Admin and I/O queue management
 * - Basic read/write commands
 * - Namespace identification
 */

#ifndef _EMBODIOS_NVME_H
#define _EMBODIOS_NVME_H

#include <embodios/types.h>
#include <embodios/pci.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVMe PCI Class/Subclass/ProgIF */
#define NVME_PCI_CLASS          0x01    /* Storage */
#define NVME_PCI_SUBCLASS       0x08    /* NVMe */
#define NVME_PCI_PROGIF         0x02    /* NVMe I/O controller */

/* NVMe Controller Registers (BAR0) */
#define NVME_REG_CAP            0x00    /* Controller Capabilities */
#define NVME_REG_VS             0x08    /* Version */
#define NVME_REG_INTMS          0x0C    /* Interrupt Mask Set */
#define NVME_REG_INTMC          0x10    /* Interrupt Mask Clear */
#define NVME_REG_CC             0x14    /* Controller Configuration */
#define NVME_REG_CSTS           0x1C    /* Controller Status */
#define NVME_REG_NSSR           0x20    /* NVM Subsystem Reset */
#define NVME_REG_AQA            0x24    /* Admin Queue Attributes */
#define NVME_REG_ASQ            0x28    /* Admin Submission Queue Base */
#define NVME_REG_ACQ            0x30    /* Admin Completion Queue Base */
#define NVME_REG_CMBLOC         0x38    /* Controller Memory Buffer Location */
#define NVME_REG_CMBSZ          0x3C    /* Controller Memory Buffer Size */
#define NVME_REG_SQ0TDBL        0x1000  /* Submission Queue 0 Tail Doorbell */

/* Controller Capabilities (CAP) fields */
#define NVME_CAP_MQES(cap)      ((cap) & 0xFFFF)           /* Max Queue Entries */
#define NVME_CAP_CQR(cap)       (((cap) >> 16) & 0x1)      /* Contiguous Queues Required */
#define NVME_CAP_AMS(cap)       (((cap) >> 17) & 0x3)      /* Arbitration Mechanism */
#define NVME_CAP_TO(cap)        (((cap) >> 24) & 0xFF)     /* Timeout (500ms units) */
#define NVME_CAP_DSTRD(cap)     (((cap) >> 32) & 0xF)      /* Doorbell Stride */
#define NVME_CAP_NSSRS(cap)     (((cap) >> 36) & 0x1)      /* NVM Subsystem Reset */
#define NVME_CAP_CSS(cap)       (((cap) >> 37) & 0xFF)     /* Command Sets Supported */
#define NVME_CAP_MPSMIN(cap)    (((cap) >> 48) & 0xF)      /* Min Page Size */
#define NVME_CAP_MPSMAX(cap)    (((cap) >> 52) & 0xF)      /* Max Page Size */

/* Controller Configuration (CC) fields */
#define NVME_CC_EN              (1 << 0)    /* Enable */
#define NVME_CC_CSS_NVM         (0 << 4)    /* NVM Command Set */
#define NVME_CC_MPS(n)          (((n) & 0xF) << 7)  /* Memory Page Size (2^(12+n)) */
#define NVME_CC_AMS_RR          (0 << 11)   /* Round Robin */
#define NVME_CC_SHN_NONE        (0 << 14)   /* No Shutdown */
#define NVME_CC_SHN_NORMAL      (1 << 14)   /* Normal Shutdown */
#define NVME_CC_SHN_ABRUPT      (2 << 14)   /* Abrupt Shutdown */
#define NVME_CC_IOSQES(n)       (((n) & 0xF) << 16) /* I/O SQ Entry Size (2^n) */
#define NVME_CC_IOCQES(n)       (((n) & 0xF) << 20) /* I/O CQ Entry Size (2^n) */

/* Controller Status (CSTS) fields */
#define NVME_CSTS_RDY           (1 << 0)    /* Ready */
#define NVME_CSTS_CFS           (1 << 1)    /* Controller Fatal Status */
#define NVME_CSTS_SHST_MASK     (3 << 2)    /* Shutdown Status */
#define NVME_CSTS_SHST_NORMAL   (0 << 2)    /* Normal operation */
#define NVME_CSTS_SHST_OCCUR    (1 << 2)    /* Shutdown processing occurring */
#define NVME_CSTS_SHST_COMPLETE (2 << 2)    /* Shutdown processing complete */
#define NVME_CSTS_NSSRO         (1 << 4)    /* NVM Subsystem Reset Occurred */

/* Admin Queue Attributes (AQA) */
#define NVME_AQA_ASQS(n)        ((n) & 0xFFF)           /* Admin SQ Size */
#define NVME_AQA_ACQS(n)        (((n) & 0xFFF) << 16)   /* Admin CQ Size */

/* NVMe Command Opcodes - Admin Commands */
#define NVME_ADMIN_DELETE_SQ    0x00
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_GET_LOG      0x02
#define NVME_ADMIN_DELETE_CQ    0x04
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_ABORT        0x08
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0A
#define NVME_ADMIN_ASYNC_EVENT  0x0C
#define NVME_ADMIN_FW_COMMIT    0x10
#define NVME_ADMIN_FW_DOWNLOAD  0x11
#define NVME_ADMIN_FORMAT_NVM   0x80
#define NVME_ADMIN_SECURITY_SEND 0x81
#define NVME_ADMIN_SECURITY_RECV 0x82

/* NVMe Command Opcodes - NVM Commands (I/O) */
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02
#define NVME_CMD_WRITE_UNCOR    0x04
#define NVME_CMD_COMPARE        0x05
#define NVME_CMD_WRITE_ZEROS    0x08
#define NVME_CMD_DSM            0x09    /* Dataset Management */
#define NVME_CMD_VERIFY         0x0C
#define NVME_CMD_RESV_REG       0x0D    /* Reservation Register */
#define NVME_CMD_RESV_REPORT    0x0E    /* Reservation Report */
#define NVME_CMD_RESV_ACQUIRE   0x11    /* Reservation Acquire */
#define NVME_CMD_RESV_RELEASE   0x15    /* Reservation Release */

/* Identify CNS values */
#define NVME_ID_CNS_NS          0x00    /* Namespace */
#define NVME_ID_CNS_CTRL        0x01    /* Controller */
#define NVME_ID_CNS_NS_ACTIVE   0x02    /* Active Namespace List */

/* Queue entry sizes */
#define NVME_SQ_ENTRY_SIZE      64      /* Submission Queue Entry Size */
#define NVME_CQ_ENTRY_SIZE      16      /* Completion Queue Entry Size */
#define NVME_SQ_ENTRY_SHIFT     6       /* log2(64) */
#define NVME_CQ_ENTRY_SHIFT     4       /* log2(16) */

/* Default queue sizes */
#define NVME_ADMIN_QUEUE_SIZE   32      /* Admin queue entries */
#define NVME_IO_QUEUE_SIZE      256     /* I/O queue entries */

/* Block size */
#define NVME_BLOCK_SIZE         512     /* Default logical block size */
#define NVME_MAX_BLOCK_SIZE     4096    /* Maximum supported block size */

/* NVMe Submission Queue Entry (Command) - 64 bytes */
typedef struct nvme_sqe {
    /* DW0: Command Dword 0 */
    uint8_t  opcode;        /* Opcode */
    uint8_t  flags;         /* Fused operation, PSDT */
    uint16_t cid;           /* Command Identifier */

    /* DW1: Namespace Identifier */
    uint32_t nsid;

    /* DW2-3: Reserved */
    uint32_t rsvd2;
    uint32_t rsvd3;

    /* DW4-5: Metadata Pointer */
    uint64_t mptr;

    /* DW6-9: Data Pointer (PRP or SGL) */
    uint64_t prp1;          /* PRP Entry 1 */
    uint64_t prp2;          /* PRP Entry 2 or PRP List pointer */

    /* DW10-15: Command Specific */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

/* NVMe Completion Queue Entry - 16 bytes */
typedef struct nvme_cqe {
    /* DW0: Command Specific */
    uint32_t result;

    /* DW1: Reserved */
    uint32_t rsvd;

    /* DW2: SQ Head Pointer + SQ Identifier */
    uint16_t sq_head;       /* SQ Head Pointer */
    uint16_t sq_id;         /* SQ Identifier */

    /* DW3: Command Identifier + Status */
    uint16_t cid;           /* Command Identifier */
    uint16_t status;        /* Status Field + Phase Tag */
} __attribute__((packed)) nvme_cqe_t;

/* Status field macros */
#define NVME_CQE_STATUS_P(s)    ((s) & 0x1)         /* Phase Tag */
#define NVME_CQE_STATUS_SC(s)   (((s) >> 1) & 0xFF) /* Status Code */
#define NVME_CQE_STATUS_SCT(s)  (((s) >> 9) & 0x7)  /* Status Code Type */
#define NVME_CQE_STATUS_MORE(s) (((s) >> 14) & 0x1) /* More */
#define NVME_CQE_STATUS_DNR(s)  (((s) >> 15) & 0x1) /* Do Not Retry */

/* Status Code Types */
#define NVME_SCT_GENERIC        0x0     /* Generic Command Status */
#define NVME_SCT_SPECIFIC       0x1     /* Command Specific Status */
#define NVME_SCT_MEDIA          0x2     /* Media and Data Integrity Errors */
#define NVME_SCT_PATH           0x3     /* Path Related Status */
#define NVME_SCT_VENDOR         0x7     /* Vendor Specific */

/* Generic Status Codes */
#define NVME_SC_SUCCESS         0x00
#define NVME_SC_INVALID_OPCODE  0x01
#define NVME_SC_INVALID_FIELD   0x02
#define NVME_SC_CMD_ID_CONFLICT 0x03
#define NVME_SC_DATA_XFER_ERR   0x04
#define NVME_SC_POWER_LOSS      0x05
#define NVME_SC_INTERNAL_ERR    0x06
#define NVME_SC_ABORT_REQ       0x07
#define NVME_SC_ABORT_SQDELETE  0x08
#define NVME_SC_FUSED_FAIL      0x09
#define NVME_SC_FUSED_MISSING   0x0A
#define NVME_SC_INVALID_NS      0x0B
#define NVME_SC_CMD_SEQ_ERR     0x0C
#define NVME_SC_INVALID_SGL     0x0D
#define NVME_SC_INVALID_SGL_CNT 0x0E
#define NVME_SC_DATA_SGL_LEN    0x0F
#define NVME_SC_MD_SGL_LEN      0x10
#define NVME_SC_SGL_TYPE        0x11
#define NVME_SC_LBA_RANGE       0x80
#define NVME_SC_CAP_EXCEEDED    0x81
#define NVME_SC_NS_NOT_READY    0x82
#define NVME_SC_RESERVATION     0x83
#define NVME_SC_FORMAT_IN_PROG  0x84

/* NVMe Identify Controller structure - 4096 bytes (partial) */
typedef struct nvme_id_ctrl {
    uint16_t vid;           /* PCI Vendor ID */
    uint16_t ssvid;         /* PCI Subsystem Vendor ID */
    char     sn[20];        /* Serial Number */
    char     mn[40];        /* Model Number */
    char     fr[8];         /* Firmware Revision */
    uint8_t  rab;           /* Recommended Arbitration Burst */
    uint8_t  ieee[3];       /* IEEE OUI Identifier */
    uint8_t  cmic;          /* Controller Multi-Path I/O */
    uint8_t  mdts;          /* Maximum Data Transfer Size */
    uint16_t cntlid;        /* Controller ID */
    uint32_t ver;           /* Version */
    uint32_t rtd3r;         /* RTD3 Resume Latency */
    uint32_t rtd3e;         /* RTD3 Entry Latency */
    uint32_t oaes;          /* Optional Async Events */
    uint32_t ctratt;        /* Controller Attributes */
    uint8_t  rsvd100[156];  /* Reserved bytes 100-255 */
    uint8_t  oacs[2];       /* Optional Admin Commands */
    uint8_t  acl;           /* Abort Command Limit */
    uint8_t  aerl;          /* Async Event Request Limit */
    uint8_t  frmw;          /* Firmware Updates */
    uint8_t  lpa;           /* Log Page Attributes */
    uint8_t  elpe;          /* Error Log Page Entries */
    uint8_t  npss;          /* Number of Power States */
    uint8_t  avscc;         /* Admin Vendor Specific Command Config */
    uint8_t  apsta;         /* Autonomous Power State Transition */
    uint16_t wctemp;        /* Warning Composite Temp Threshold */
    uint16_t cctemp;        /* Critical Composite Temp Threshold */
    uint8_t  rsvd270[242];  /* Reserved bytes 270-511 */
    uint8_t  sqes;          /* Submission Queue Entry Size */
    uint8_t  cqes;          /* Completion Queue Entry Size */
    uint16_t maxcmd;        /* Max Outstanding Commands */
    uint32_t nn;            /* Number of Namespaces */
    uint16_t oncs;          /* Optional NVM Command Support */
    uint16_t fuses;         /* Fused Operation Support */
    uint8_t  fna;           /* Format NVM Attributes */
    uint8_t  vwc;           /* Volatile Write Cache */
    uint16_t awun;          /* Atomic Write Unit Normal */
    uint16_t awupf;         /* Atomic Write Unit Power Fail */
    uint8_t  nvscc;         /* NVM Vendor Specific Config */
    uint8_t  rsvd531[177];  /* Reserved */
    uint8_t  rsvd708[1344]; /* Reserved */
    uint8_t  vs[1024];      /* Vendor Specific */
    uint8_t  rsvd3072[1024];/* Reserved */
} __attribute__((packed)) nvme_id_ctrl_t;

/* NVMe Identify Namespace structure - 4096 bytes (partial) */
typedef struct nvme_id_ns {
    uint64_t nsze;          /* Namespace Size (blocks) */
    uint64_t ncap;          /* Namespace Capacity */
    uint64_t nuse;          /* Namespace Utilization */
    uint8_t  nsfeat;        /* Namespace Features */
    uint8_t  nlbaf;         /* Number of LBA Formats */
    uint8_t  flbas;         /* Formatted LBA Size */
    uint8_t  mc;            /* Metadata Capabilities */
    uint8_t  dpc;           /* Data Protection Capabilities */
    uint8_t  dps;           /* Data Protection Settings */
    uint8_t  nmic;          /* Namespace Multi-path I/O */
    uint8_t  rescap;        /* Reservation Capabilities */
    uint8_t  fpi;           /* Format Progress Indicator */
    uint8_t  dlfeat;        /* Deallocate Logical Block */
    uint16_t nawun;         /* Namespace Atomic Write Unit Normal */
    uint16_t nawupf;        /* Namespace Atomic Write Unit Power Fail */
    uint16_t nacwu;         /* Namespace Atomic Compare & Write Unit */
    uint16_t nabsn;         /* Namespace Atomic Boundary Size Normal */
    uint16_t nabo;          /* Namespace Atomic Boundary Offset */
    uint16_t nabspf;        /* Namespace Atomic Boundary Size Power Fail */
    uint16_t noiob;         /* Namespace Optimal I/O Boundary */
    uint8_t  nvmcap[16];    /* NVM Capacity */
    uint8_t  rsvd64[40];    /* Reserved */
    uint8_t  nguid[16];     /* Namespace GUID */
    uint8_t  eui64[8];      /* IEEE Extended Unique Identifier */
    struct {
        uint16_t ms;        /* Metadata Size */
        uint8_t  lbads;     /* LBA Data Size (2^n) */
        uint8_t  rp;        /* Relative Performance */
    } lbaf[16];             /* LBA Format Support */
    uint8_t  rsvd192[192];  /* Reserved */
    uint8_t  vs[3712];      /* Vendor Specific */
} __attribute__((packed)) nvme_id_ns_t;

/* NVMe Queue structure */
typedef struct nvme_queue {
    volatile void *sq;      /* Submission Queue */
    volatile void *cq;      /* Completion Queue */
    volatile uint32_t *sq_doorbell;  /* SQ Tail Doorbell */
    volatile uint32_t *cq_doorbell;  /* CQ Head Doorbell */
    uint16_t sq_tail;       /* SQ Tail index */
    uint16_t cq_head;       /* CQ Head index */
    uint16_t size;          /* Queue depth */
    uint16_t cid;           /* Next Command ID */
    uint8_t  cq_phase;      /* Expected phase bit */
    uint8_t  id;            /* Queue ID */
} nvme_queue_t;

/* NVMe Controller structure */
typedef struct nvme_ctrl {
    pci_device_t *pci_dev;  /* PCI device */
    volatile void *regs;    /* Memory-mapped registers (BAR0) */
    uint64_t cap;           /* Controller Capabilities */
    uint32_t vs;            /* Version */

    /* Queues */
    nvme_queue_t admin_queue;   /* Admin queue */
    nvme_queue_t io_queue;      /* I/O queue */

    /* Controller info */
    nvme_id_ctrl_t id_ctrl;     /* Identify Controller data */
    uint32_t nn;                /* Number of namespaces */
    uint32_t max_transfer;      /* Max data transfer size (bytes) */
    uint32_t doorbell_stride;   /* Doorbell stride (bytes) */

    /* Namespace 1 info (primary namespace) */
    nvme_id_ns_t id_ns;         /* Identify Namespace data */
    uint64_t ns_size;           /* Namespace size (blocks) */
    uint32_t block_size;        /* Logical block size (bytes) */
    uint32_t nsid;              /* Active namespace ID */

    bool initialized;
} nvme_ctrl_t;

/* Error codes */
#define NVME_OK             0
#define NVME_ERR_NOT_FOUND  -1
#define NVME_ERR_INIT       -2
#define NVME_ERR_TIMEOUT    -3
#define NVME_ERR_IO         -4
#define NVME_ERR_NOMEM      -5
#define NVME_ERR_INVALID    -6

/* Public API */

/**
 * Initialize the NVMe subsystem
 * Scans for NVMe controllers and initializes the first one found
 * @return NVME_OK on success, error code on failure
 */
int nvme_init(void);

/**
 * Check if NVMe is initialized and ready
 * @return true if ready, false otherwise
 */
bool nvme_is_ready(void);

/**
 * Read blocks from NVMe storage
 * @param lba       Starting logical block address
 * @param count     Number of blocks to read
 * @param buffer    Buffer to store data (must be at least count * block_size)
 * @return Number of blocks read, or negative error code
 */
int nvme_read(uint64_t lba, uint32_t count, void *buffer);

/**
 * Write blocks to NVMe storage
 * @param lba       Starting logical block address
 * @param count     Number of blocks to write
 * @param buffer    Data to write (must be at least count * block_size)
 * @return Number of blocks written, or negative error code
 */
int nvme_write(uint64_t lba, uint32_t count, const void *buffer);

/**
 * Flush cached data to NVMe storage
 * @return NVME_OK on success, error code on failure
 */
int nvme_flush(void);

/**
 * Get NVMe controller information
 * @param capacity      Total capacity in bytes (output, can be NULL)
 * @param block_size    Logical block size in bytes (output, can be NULL)
 * @param model         Model string buffer (output, can be NULL)
 * @param model_len     Length of model buffer
 */
void nvme_get_info(uint64_t *capacity, uint32_t *block_size,
                   char *model, size_t model_len);

/**
 * Print NVMe status and information
 */
void nvme_print_info(void);

/**
 * Run NVMe self-tests
 * @return 0 on success, -1 on failure
 */
int nvme_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMBODIOS_NVME_H */
