/* EMBODIOS EtherCAT Slave Protocol
 *
 * EtherCAT slave implementation for industrial automation integration.
 * Provides real-time process data exchange and state machine management.
 *
 * Features:
 * - EtherCAT frame processing (Ethernet/EtherCAT/Datagram)
 * - Slave state machine (INIT → PREOP → SAFEOP → OP)
 * - Process Data Objects (PDO) exchange
 * - Mailbox communication (CoE)
 * - Distributed clocks synchronization
 * - Sub-microsecond cycle times
 */

#ifndef EMBODIOS_ETHERCAT_H
#define EMBODIOS_ETHERCAT_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Protocol Numbers and Constants
 * ============================================================================ */

/* EtherCAT EtherType */
#define ECAT_ETHERTYPE          0x88A4  /* EtherCAT protocol */

/* Frame limits */
#define ECAT_MAX_FRAME_SIZE     1514    /* Maximum Ethernet frame */
#define ECAT_MIN_FRAME_SIZE     60      /* Minimum Ethernet frame */
#define ECAT_MAX_DATAGRAMS      15      /* Max datagrams per frame */
#define ECAT_MAX_DATA_SIZE      1486    /* Max data in single frame */

/* EtherCAT frame header size */
#define ECAT_HEADER_SIZE        2       /* Length and type fields */
#define ECAT_DATAGRAM_HEADER    10      /* Datagram header size */

/* Protocol types */
#define ECAT_TYPE_DATAGRAM      0x01    /* EtherCAT datagram */
#define ECAT_TYPE_NWV           0x04    /* Network variables */
#define ECAT_TYPE_MAILBOX       0x05    /* Mailbox gateway */

/* ============================================================================
 * EtherCAT Commands
 * ============================================================================ */

/* Physical memory access */
#define ECAT_CMD_NOP            0x00    /* No operation */
#define ECAT_CMD_APRD           0x01    /* Auto increment physical read */
#define ECAT_CMD_APWR           0x02    /* Auto increment physical write */
#define ECAT_CMD_APRW           0x03    /* Auto increment physical r/w */
#define ECAT_CMD_FPRD           0x04    /* Configured address physical read */
#define ECAT_CMD_FPWR           0x05    /* Configured address physical write */
#define ECAT_CMD_FPRW           0x06    /* Configured address physical r/w */
#define ECAT_CMD_BRD            0x07    /* Broadcast read */
#define ECAT_CMD_BWR            0x08    /* Broadcast write */
#define ECAT_CMD_BRW            0x09    /* Broadcast read/write */

/* Logical memory access */
#define ECAT_CMD_LRD            0x0A    /* Logical read */
#define ECAT_CMD_LWR            0x0B    /* Logical write */
#define ECAT_CMD_LRW            0x0C    /* Logical read/write */

/* Addressing read/write */
#define ECAT_CMD_ARMW           0x0D    /* Auto increment read multiple write */
#define ECAT_CMD_FRMW           0x0E    /* Configured read multiple write */

/* ============================================================================
 * State Machine States
 * ============================================================================ */

/* AL (Application Layer) States */
#define ECAT_STATE_INIT         0x01    /* Init state */
#define ECAT_STATE_PREOP        0x02    /* Pre-operational */
#define ECAT_STATE_BOOT         0x03    /* Bootstrap */
#define ECAT_STATE_SAFEOP       0x04    /* Safe-operational */
#define ECAT_STATE_OP           0x08    /* Operational */

/* State transition flags */
#define ECAT_STATE_ERROR        0x10    /* Error flag */
#define ECAT_STATE_ACK          0x10    /* Acknowledge error */

/* AL Status Code (error codes) */
#define ECAT_AL_STATUS_OK                   0x0000  /* No error */
#define ECAT_AL_STATUS_UNSPECIFIED          0x0001  /* Unspecified error */
#define ECAT_AL_STATUS_NO_MEMORY            0x0002  /* No memory */
#define ECAT_AL_STATUS_INVALID_SETUP        0x0004  /* Invalid device setup */
#define ECAT_AL_STATUS_INVALID_MAILBOX      0x0006  /* Invalid mailbox config */
#define ECAT_AL_STATUS_INVALID_SYNC         0x0007  /* Invalid sync manager */
#define ECAT_AL_STATUS_WATCHDOG             0x001B  /* Sync manager watchdog */
#define ECAT_AL_STATUS_INVALID_INPUT        0x001D  /* Invalid input mapping */
#define ECAT_AL_STATUS_INVALID_OUTPUT       0x001E  /* Invalid output mapping */
#define ECAT_AL_STATUS_SYNC_ERROR           0x001F  /* Synchronization error */

/* ============================================================================
 * Register Addresses (ESC - EtherCAT Slave Controller)
 * ============================================================================ */

/* Information registers */
#define ECAT_REG_TYPE                   0x0000  /* Type */
#define ECAT_REG_REVISION               0x0001  /* Revision */
#define ECAT_REG_BUILD                  0x0002  /* Build */
#define ECAT_REG_FMMU_COUNT             0x0004  /* FMMUs supported */
#define ECAT_REG_SYNC_COUNT             0x0005  /* Sync managers supported */
#define ECAT_REG_RAM_SIZE               0x0006  /* RAM size */
#define ECAT_REG_PORT_DESC              0x0007  /* Port descriptor */
#define ECAT_REG_FEATURES               0x0008  /* ESC features */

/* Configuration registers */
#define ECAT_REG_STATION_ADDR           0x0010  /* Configured station address */
#define ECAT_REG_STATION_ALIAS          0x0012  /* Configured station alias */

/* DL (Data Link) registers */
#define ECAT_REG_DL_CONTROL             0x0100  /* DL control */
#define ECAT_REG_DL_STATUS              0x0110  /* DL status */
#define ECAT_REG_AL_CONTROL             0x0120  /* AL control */
#define ECAT_REG_AL_STATUS              0x0130  /* AL status */
#define ECAT_REG_AL_STATUS_CODE         0x0134  /* AL status code */

/* PDI (Process Data Interface) registers */
#define ECAT_REG_PDI_CONTROL            0x0140  /* PDI control */
#define ECAT_REG_PDI_CONFIG             0x0150  /* PDI configuration */
#define ECAT_REG_PDI_CONFIG_EXT         0x0152  /* PDI extended config */

/* Interrupt registers */
#define ECAT_REG_IRQ_MASK               0x0200  /* Interrupt mask */
#define ECAT_REG_IRQ_REQUEST            0x0210  /* Interrupt request */

/* Error counters */
#define ECAT_REG_RX_ERROR_COUNT         0x0300  /* RX error counter base */
#define ECAT_REG_LOST_LINK_COUNT        0x0310  /* Lost link counter base */

/* Watchdog */
#define ECAT_REG_WD_DIVIDER             0x0400  /* Watchdog divider */
#define ECAT_REG_WD_TIME_PDI            0x0410  /* PDI watchdog time */
#define ECAT_REG_WD_TIME_PROC           0x0420  /* Process data watchdog time */

/* FMMU (Fieldbus Memory Management Unit) */
#define ECAT_REG_FMMU_BASE              0x0600  /* FMMU 0 base */
#define ECAT_FMMU_SIZE                  16      /* FMMU entry size */
#define ECAT_FMMU_MAX                   16      /* Max FMMU entries */

/* Sync Manager */
#define ECAT_REG_SM_BASE                0x0800  /* Sync Manager 0 base */
#define ECAT_SM_SIZE                    8       /* SM entry size */
#define ECAT_SM_MAX                     16      /* Max SM entries */

/* DC (Distributed Clock) */
#define ECAT_REG_DC_RECV_TIME           0x0900  /* DC receive time port 0 */
#define ECAT_REG_DC_SYSTEM_TIME         0x0910  /* DC system time */
#define ECAT_REG_DC_RECV_TIME_OFFSET    0x0920  /* System time offset */
#define ECAT_REG_DC_SYSTEM_TIME_DELAY   0x0928  /* System time delay */
#define ECAT_REG_DC_SYSTEM_TIME_DIFF    0x092C  /* System time difference */
#define ECAT_REG_DC_SPEED_COUNT_START   0x0930  /* Speed counter start */
#define ECAT_REG_DC_SPEED_COUNT_DIFF    0x0932  /* Speed counter diff */
#define ECAT_REG_DC_FILTER_DEPTH        0x0934  /* System time diff filter depth */
#define ECAT_REG_DC_CYCLIC_UNIT         0x09A0  /* Cyclic unit control */
#define ECAT_REG_DC_ASSIGN_ACTIVATE     0x09A8  /* Assign activate */
#define ECAT_REG_DC_SYNC0_CYCLE         0x09A0  /* SYNC0 cycle time */
#define ECAT_REG_DC_SYNC1_CYCLE         0x09A4  /* SYNC1 cycle time */

/* SII (Slave Information Interface) EEPROM */
#define ECAT_REG_SII_CONFIG             0x0500  /* SII config/status */
#define ECAT_REG_SII_CONTROL            0x0502  /* SII control/status */
#define ECAT_REG_SII_ADDRESS            0x0504  /* SII address */
#define ECAT_REG_SII_DATA               0x0508  /* SII data */

/* ============================================================================
 * Protocol Headers
 * ============================================================================ */

/* EtherCAT frame header */
typedef struct ecat_header {
    uint16_t length_type;       /* Length (11 bits) and type (4 bits, reserved 1 bit) */
} __packed ecat_header_t;

/* EtherCAT datagram header */
typedef struct ecat_datagram {
    uint8_t  cmd;               /* Command type */
    uint8_t  idx;               /* Index (for multiple datagrams) */
    uint32_t addr;              /* Address (physical or logical) */
    uint16_t len_flags;         /* Length (11 bits) + flags (5 bits) */
    uint16_t irq;               /* Interrupt */
    /* Followed by data and working counter (2 bytes) */
} __packed ecat_datagram_t;

/* Datagram flags (in len_flags field, high 5 bits) */
#define ECAT_FLAG_MORE              0x8000  /* More datagrams follow */
#define ECAT_FLAG_CIRCULATED        0x4000  /* Frame has circulated */

/* Working counter access macros */
#define ECAT_WKC_OFFSET(len)        ((len))  /* WKC is after data */

/* FMMU (Fieldbus Memory Management Unit) configuration */
typedef struct ecat_fmmu {
    uint32_t logical_start;     /* Logical start address */
    uint16_t length;            /* Length in bytes */
    uint8_t  logical_start_bit; /* Start bit in logical address */
    uint8_t  logical_end_bit;   /* End bit in logical address */
    uint16_t physical_start;    /* Physical start address */
    uint8_t  physical_start_bit;/* Start bit in physical address */
    uint8_t  type;              /* Read/write enable */
    uint8_t  activate;          /* Activation state */
    uint8_t  reserved[3];       /* Reserved */
} __packed ecat_fmmu_t;

/* FMMU types */
#define ECAT_FMMU_TYPE_READ         0x01    /* Read mapping */
#define ECAT_FMMU_TYPE_WRITE        0x02    /* Write mapping */
#define ECAT_FMMU_TYPE_READWRITE    0x03    /* Read/write mapping */

/* Sync Manager configuration */
typedef struct ecat_sm {
    uint16_t physical_start;    /* Physical start address */
    uint16_t length;            /* Length in bytes */
    uint8_t  control;           /* Control register */
    uint8_t  status;            /* Status register */
    uint8_t  activate;          /* Activation state */
    uint8_t  pdi_control;       /* PDI control */
} __packed ecat_sm_t;

/* Sync Manager control bits */
#define ECAT_SM_CTRL_MODE           0x03    /* Operation mode mask */
#define ECAT_SM_CTRL_MODE_BUFFERED  0x00    /* Buffered */
#define ECAT_SM_CTRL_MODE_MAILBOX   0x02    /* Mailbox */
#define ECAT_SM_CTRL_DIRECTION      0x04    /* Direction: 0=read, 1=write */
#define ECAT_SM_CTRL_ECAT_IRQ       0x08    /* EtherCAT interrupt enable */
#define ECAT_SM_CTRL_PDI_IRQ        0x10    /* PDI interrupt enable */
#define ECAT_SM_CTRL_WD_ENABLE      0x40    /* Watchdog enable */

/* Sync Manager standard assignments */
#define ECAT_SM_MBOX_OUT            0       /* Mailbox out (master to slave) */
#define ECAT_SM_MBOX_IN             1       /* Mailbox in (slave to master) */
#define ECAT_SM_PROC_OUT            2       /* Process data out */
#define ECAT_SM_PROC_IN             3       /* Process data in */

/* ============================================================================
 * Mailbox Protocol
 * ============================================================================ */

/* Mailbox header */
typedef struct ecat_mailbox_header {
    uint16_t length;            /* Data length */
    uint16_t address;           /* Slave address */
    uint8_t  channel_flags;     /* Channel and priority */
    uint8_t  type;              /* Mailbox type */
} __packed ecat_mailbox_header_t;

/* Mailbox types */
#define ECAT_MBOX_TYPE_ERR          0x00    /* Error */
#define ECAT_MBOX_TYPE_AOE          0x01    /* ADS over EtherCAT */
#define ECAT_MBOX_TYPE_EOE          0x02    /* Ethernet over EtherCAT */
#define ECAT_MBOX_TYPE_COE          0x03    /* CANopen over EtherCAT */
#define ECAT_MBOX_TYPE_FOE          0x04    /* File over EtherCAT */
#define ECAT_MBOX_TYPE_SOE          0x05    /* Servo over EtherCAT */
#define ECAT_MBOX_TYPE_VOE          0x0F    /* Vendor specific */

/* CoE (CANopen over EtherCAT) */
#define ECAT_COE_TYPE_EMERGENCY     0x01    /* Emergency */
#define ECAT_COE_TYPE_SDO_REQ       0x02    /* SDO request */
#define ECAT_COE_TYPE_SDO_RESP      0x03    /* SDO response */
#define ECAT_COE_TYPE_SDO_INFO      0x08    /* SDO information */

/* ============================================================================
 * Slave Configuration
 * ============================================================================ */

typedef struct ecat_slave_config {
    uint16_t station_address;   /* Configured station address */
    uint16_t station_alias;     /* Station alias */
    uint32_t vendor_id;         /* Vendor ID */
    uint32_t product_code;      /* Product code */
    uint32_t revision;          /* Revision number */
    uint32_t serial;            /* Serial number */
    uint8_t  port_count;        /* Number of ports */
    uint8_t  fmmu_count;        /* Number of FMMUs */
    uint8_t  sm_count;          /* Number of sync managers */
    uint8_t  dc_supported;      /* Distributed clocks supported */

    /* Process data */
    uint16_t input_size;        /* Input PDO size (bytes) */
    uint16_t output_size;       /* Output PDO size (bytes) */
    uint8_t  *input_data;       /* Pointer to input PDO data */
    uint8_t  *output_data;      /* Pointer to output PDO data */

    /* Mailbox */
    uint16_t mbox_out_addr;     /* Mailbox out address */
    uint16_t mbox_out_size;     /* Mailbox out size */
    uint16_t mbox_in_addr;      /* Mailbox in address */
    uint16_t mbox_in_size;      /* Mailbox in size */

    bool     mailbox_supported; /* Mailbox communication */
    bool     coe_supported;     /* CANopen over EtherCAT */
    bool     foe_supported;     /* File over EtherCAT */
    bool     eoe_supported;     /* Ethernet over EtherCAT */
    bool     soe_supported;     /* Servo over EtherCAT */
} ecat_slave_config_t;

/* ============================================================================
 * Slave State and Context
 * ============================================================================ */

typedef struct ecat_slave {
    /* Configuration */
    ecat_slave_config_t config;

    /* State machine */
    uint8_t  al_state;          /* Current AL state */
    uint8_t  requested_state;   /* Requested AL state */
    uint16_t al_status_code;    /* AL status code (error) */
    uint32_t state_change_time; /* Last state change timestamp */

    /* Register memory */
    uint8_t  *registers;        /* ESC register space */
    size_t   register_size;     /* Size of register space */

    /* FMMU configuration */
    ecat_fmmu_t fmmu[ECAT_FMMU_MAX];

    /* Sync Manager configuration */
    ecat_sm_t sm[ECAT_SM_MAX];

    /* Distributed Clock */
    uint64_t dc_system_time;    /* DC system time (nanoseconds) */
    int32_t  dc_time_offset;    /* Time offset from master */
    bool     dc_sync_active;    /* DC synchronization active */

    /* Watchdog */
    uint16_t wd_divider;        /* Watchdog divider */
    uint16_t wd_time_pdi;       /* PDI watchdog time */
    uint16_t wd_time_proc;      /* Process data watchdog time */
    uint32_t wd_last_trigger;   /* Last watchdog trigger */

    /* Mailbox buffers */
    uint8_t  *mbox_out_buf;     /* Mailbox out buffer */
    uint8_t  *mbox_in_buf;      /* Mailbox in buffer */
    bool     mbox_out_ready;    /* Data available in out mailbox */
    bool     mbox_in_ready;     /* Space available in in mailbox */

    /* Network interface */
    void     *netif;            /* Network interface handle */
    uint8_t  mac_addr[6];       /* MAC address */

    bool     active;            /* Slave active */
} ecat_slave_t;

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct ecat_stats {
    uint64_t frames_received;       /* Frames received */
    uint64_t frames_sent;           /* Frames sent */
    uint64_t datagrams_processed;   /* Datagrams processed */
    uint64_t bytes_received;        /* Bytes received */
    uint64_t bytes_sent;            /* Bytes sent */

    uint64_t aprd_count;            /* APRD commands */
    uint64_t apwr_count;            /* APWR commands */
    uint64_t fprd_count;            /* FPRD commands */
    uint64_t fpwr_count;            /* FPWR commands */
    uint64_t brd_count;             /* BRD commands */
    uint64_t bwr_count;             /* BWR commands */
    uint64_t lrd_count;             /* LRD commands */
    uint64_t lwr_count;             /* LWR commands */
    uint64_t lrw_count;             /* LRW commands */

    uint64_t state_transitions;     /* State machine transitions */
    uint64_t state_init;            /* Time in INIT state */
    uint64_t state_preop;           /* Time in PREOP state */
    uint64_t state_safeop;          /* Time in SAFEOP state */
    uint64_t state_op;              /* Time in OP state */

    uint64_t pdo_cycles;            /* PDO exchange cycles */
    uint64_t mailbox_sent;          /* Mailbox messages sent */
    uint64_t mailbox_received;      /* Mailbox messages received */

    uint64_t errors;                /* Total errors */
    uint64_t wkc_errors;            /* Working counter errors */
    uint64_t frame_errors;          /* Frame errors */
    uint64_t watchdog_triggers;     /* Watchdog triggers */
    uint64_t dc_sync_errors;        /* DC synchronization errors */
} ecat_stats_t;

/* ============================================================================
 * Function Declarations
 * ============================================================================ */

/* Initialization and cleanup */
ecat_slave_t *ecat_slave_create(const ecat_slave_config_t *config);
void ecat_slave_destroy(ecat_slave_t *slave);
int ecat_slave_init(ecat_slave_t *slave);
int ecat_slave_reset(ecat_slave_t *slave);

/* State machine */
int ecat_slave_set_state(ecat_slave_t *slave, uint8_t state);
uint8_t ecat_slave_get_state(const ecat_slave_t *slave);
int ecat_slave_transition(ecat_slave_t *slave);
const char *ecat_state_string(uint8_t state);

/* Frame processing */
int ecat_process_frame(ecat_slave_t *slave, const uint8_t *frame, size_t len);
int ecat_process_datagram(ecat_slave_t *slave, const ecat_datagram_t *dg,
                          const uint8_t *data, size_t len);
int ecat_build_response(ecat_slave_t *slave, uint8_t *frame, size_t *len);

/* Register access */
uint8_t ecat_reg_read8(ecat_slave_t *slave, uint16_t addr);
uint16_t ecat_reg_read16(ecat_slave_t *slave, uint16_t addr);
uint32_t ecat_reg_read32(ecat_slave_t *slave, uint16_t addr);
void ecat_reg_write8(ecat_slave_t *slave, uint16_t addr, uint8_t value);
void ecat_reg_write16(ecat_slave_t *slave, uint16_t addr, uint16_t value);
void ecat_reg_write32(ecat_slave_t *slave, uint16_t addr, uint32_t value);

/* FMMU configuration */
int ecat_fmmu_config(ecat_slave_t *slave, uint8_t fmmu_idx, const ecat_fmmu_t *fmmu);
int ecat_fmmu_activate(ecat_slave_t *slave, uint8_t fmmu_idx);
int ecat_fmmu_deactivate(ecat_slave_t *slave, uint8_t fmmu_idx);

/* Sync Manager configuration */
int ecat_sm_config(ecat_slave_t *slave, uint8_t sm_idx, const ecat_sm_t *sm);
int ecat_sm_activate(ecat_slave_t *slave, uint8_t sm_idx);
int ecat_sm_deactivate(ecat_slave_t *slave, uint8_t sm_idx);

/* Process Data Objects (PDO) */
int ecat_pdo_exchange(ecat_slave_t *slave);
int ecat_pdo_read(ecat_slave_t *slave, uint8_t *data, size_t len);
int ecat_pdo_write(ecat_slave_t *slave, const uint8_t *data, size_t len);

/* Mailbox */
int ecat_mailbox_send(ecat_slave_t *slave, uint8_t type, const uint8_t *data, size_t len);
int ecat_mailbox_receive(ecat_slave_t *slave, uint8_t *type, uint8_t *data, size_t *len);
int ecat_mailbox_process(ecat_slave_t *slave);

/* Distributed Clock */
int ecat_dc_init(ecat_slave_t *slave);
int ecat_dc_sync(ecat_slave_t *slave, uint64_t master_time);
uint64_t ecat_dc_get_time(const ecat_slave_t *slave);
int ecat_dc_set_sync_mode(ecat_slave_t *slave, bool enabled);

/* Watchdog */
int ecat_watchdog_init(ecat_slave_t *slave, uint16_t divider, uint16_t time_pdi, uint16_t time_proc);
int ecat_watchdog_trigger(ecat_slave_t *slave);
int ecat_watchdog_check(ecat_slave_t *slave);

/* Statistics */
void ecat_get_stats(const ecat_slave_t *slave, ecat_stats_t *stats);
void ecat_reset_stats(ecat_slave_t *slave);

/* Utility functions */
uint16_t ecat_crc16(const uint8_t *data, size_t len);
const char *ecat_cmd_string(uint8_t cmd);
const char *ecat_error_string(uint16_t error_code);

/* Network interface binding */
int ecat_bind_netif(ecat_slave_t *slave, void *netif);
int ecat_unbind_netif(ecat_slave_t *slave);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_ETHERCAT_H */
