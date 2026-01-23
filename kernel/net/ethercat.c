/* EMBODIOS EtherCAT Slave Protocol Implementation
 *
 * Real-time industrial Ethernet protocol for factory automation.
 */

#include <embodios/ethercat.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* ============================================================================
 * Global State and Statistics
 * ============================================================================ */

static ecat_stats_t global_stats;
static bool ethercat_initialized = false;

/* ============================================================================
 * Byte Order Conversion (EtherCAT uses little-endian)
 * ============================================================================ */

static inline uint16_t ecat_le16_to_cpu(uint16_t val)
{
    /* On x86, little-endian is native */
    return val;
}

static inline uint32_t ecat_le32_to_cpu(uint32_t val)
{
    return val;
}

static inline uint16_t ecat_cpu_to_le16(uint16_t val)
{
    return val;
}

static inline uint32_t ecat_cpu_to_le32(uint32_t val)
{
    return val;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

uint16_t ecat_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];

        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

const char *ecat_cmd_string(uint8_t cmd)
{
    switch (cmd) {
        case ECAT_CMD_NOP:      return "NOP";
        case ECAT_CMD_APRD:     return "APRD";
        case ECAT_CMD_APWR:     return "APWR";
        case ECAT_CMD_APRW:     return "APRW";
        case ECAT_CMD_FPRD:     return "FPRD";
        case ECAT_CMD_FPWR:     return "FPWR";
        case ECAT_CMD_FPRW:     return "FPRW";
        case ECAT_CMD_BRD:      return "BRD";
        case ECAT_CMD_BWR:      return "BWR";
        case ECAT_CMD_BRW:      return "BRW";
        case ECAT_CMD_LRD:      return "LRD";
        case ECAT_CMD_LWR:      return "LWR";
        case ECAT_CMD_LRW:      return "LRW";
        case ECAT_CMD_ARMW:     return "ARMW";
        case ECAT_CMD_FRMW:     return "FRMW";
        default:                return "UNKNOWN";
    }
}

const char *ecat_state_string(uint8_t state)
{
    switch (state & 0x0F) {
        case ECAT_STATE_INIT:   return "INIT";
        case ECAT_STATE_PREOP:  return "PREOP";
        case ECAT_STATE_BOOT:   return "BOOT";
        case ECAT_STATE_SAFEOP: return "SAFEOP";
        case ECAT_STATE_OP:     return "OP";
        default:                return "INVALID";
    }
}

const char *ecat_error_string(uint16_t error_code)
{
    switch (error_code) {
        case ECAT_AL_STATUS_OK:                 return "No error";
        case ECAT_AL_STATUS_UNSPECIFIED:        return "Unspecified error";
        case ECAT_AL_STATUS_NO_MEMORY:          return "No memory";
        case ECAT_AL_STATUS_INVALID_SETUP:      return "Invalid device setup";
        case ECAT_AL_STATUS_INVALID_MAILBOX:    return "Invalid mailbox config";
        case ECAT_AL_STATUS_INVALID_SYNC:       return "Invalid sync manager";
        case ECAT_AL_STATUS_WATCHDOG:           return "Watchdog timeout";
        case ECAT_AL_STATUS_INVALID_INPUT:      return "Invalid input mapping";
        case ECAT_AL_STATUS_INVALID_OUTPUT:     return "Invalid output mapping";
        case ECAT_AL_STATUS_SYNC_ERROR:         return "Synchronization error";
        default:                                return "Unknown error";
    }
}

/* ============================================================================
 * Register Access Functions
 * ============================================================================ */

uint8_t ecat_reg_read8(ecat_slave_t *slave, uint16_t addr)
{
    if (!slave || !slave->registers) return 0;
    if (addr >= slave->register_size) return 0;

    return slave->registers[addr];
}

uint16_t ecat_reg_read16(ecat_slave_t *slave, uint16_t addr)
{
    if (!slave || !slave->registers) return 0;
    if (addr + 1 >= slave->register_size) return 0;

    uint16_t val = *(uint16_t *)&slave->registers[addr];
    return ecat_le16_to_cpu(val);
}

uint32_t ecat_reg_read32(ecat_slave_t *slave, uint16_t addr)
{
    if (!slave || !slave->registers) return 0;
    if (addr + 3 >= slave->register_size) return 0;

    uint32_t val = *(uint32_t *)&slave->registers[addr];
    return ecat_le32_to_cpu(val);
}

void ecat_reg_write8(ecat_slave_t *slave, uint16_t addr, uint8_t value)
{
    if (!slave || !slave->registers) return;
    if (addr >= slave->register_size) return;

    slave->registers[addr] = value;
}

void ecat_reg_write16(ecat_slave_t *slave, uint16_t addr, uint16_t value)
{
    if (!slave || !slave->registers) return;
    if (addr + 1 >= slave->register_size) return;

    uint16_t val_le = ecat_cpu_to_le16(value);
    *(uint16_t *)&slave->registers[addr] = val_le;

    /* Handle special registers */
    if (addr == ECAT_REG_AL_CONTROL) {
        /* AL_CONTROL write triggers state transition */
        uint8_t requested_state = (uint8_t)(value & 0x0F);
        slave->requested_state = requested_state;
        /* Transition will be processed by ecat_slave_transition() */
    }
}

void ecat_reg_write32(ecat_slave_t *slave, uint16_t addr, uint32_t value)
{
    if (!slave || !slave->registers) return;
    if (addr + 3 >= slave->register_size) return;

    uint32_t val_le = ecat_cpu_to_le32(value);
    *(uint32_t *)&slave->registers[addr] = val_le;
}

/* ============================================================================
 * Datagram Processing Helpers
 * ============================================================================ */

static int process_register_read(ecat_slave_t *slave, uint16_t addr,
                                 uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        data[i] = ecat_reg_read8(slave, addr + i);
    }
    return len;
}

static int process_register_write(ecat_slave_t *slave, uint16_t addr,
                                  const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        ecat_reg_write8(slave, addr + i, data[i]);
    }
    return len;
}

static int process_aprd(ecat_slave_t *slave, uint32_t addr, uint8_t *data, uint16_t len)
{
    /* Auto-increment physical read - read from this slave, increment address */
    uint16_t reg_addr = (uint16_t)(addr & 0xFFFF);

    process_register_read(slave, reg_addr, data, len);
    global_stats.aprd_count++;

    return 1;  /* Working counter increment */
}

static int process_apwr(ecat_slave_t *slave, uint32_t addr, const uint8_t *data, uint16_t len)
{
    /* Auto-increment physical write */
    uint16_t reg_addr = (uint16_t)(addr & 0xFFFF);

    process_register_write(slave, reg_addr, data, len);
    global_stats.apwr_count++;

    return 1;  /* Working counter increment */
}

static int process_fprd(ecat_slave_t *slave, uint32_t addr, uint8_t *data, uint16_t len)
{
    /* Configured address physical read */
    uint16_t station = (uint16_t)((addr >> 16) & 0xFFFF);
    uint16_t reg_addr = (uint16_t)(addr & 0xFFFF);

    /* Check if this datagram is for us */
    if (station != slave->config.station_address &&
        station != slave->config.station_alias) {
        return 0;  /* Not for us, don't increment WKC */
    }

    process_register_read(slave, reg_addr, data, len);
    global_stats.fprd_count++;

    return 1;  /* Working counter increment */
}

static int process_fpwr(ecat_slave_t *slave, uint32_t addr, const uint8_t *data, uint16_t len)
{
    /* Configured address physical write */
    uint16_t station = (uint16_t)((addr >> 16) & 0xFFFF);
    uint16_t reg_addr = (uint16_t)(addr & 0xFFFF);

    /* Check if this datagram is for us */
    if (station != slave->config.station_address &&
        station != slave->config.station_alias) {
        return 0;  /* Not for us */
    }

    process_register_write(slave, reg_addr, data, len);
    global_stats.fpwr_count++;

    return 1;  /* Working counter increment */
}

static int process_brd(ecat_slave_t *slave, uint32_t addr, uint8_t *data, uint16_t len)
{
    /* Broadcast read - all slaves respond */
    uint16_t reg_addr = (uint16_t)(addr & 0xFFFF);

    process_register_read(slave, reg_addr, data, len);
    global_stats.brd_count++;

    return 1;  /* Working counter increment */
}

static int process_bwr(ecat_slave_t *slave, uint32_t addr, const uint8_t *data, uint16_t len)
{
    /* Broadcast write - all slaves write */
    uint16_t reg_addr = (uint16_t)(addr & 0xFFFF);

    process_register_write(slave, reg_addr, data, len);
    global_stats.bwr_count++;

    return 1;  /* Working counter increment */
}

static int process_lrd(ecat_slave_t *slave, uint32_t addr, uint8_t *data, uint16_t len)
{
    /* Logical read - FMMU mapping required */
    /* TODO: Implement FMMU mapping in subtask-2-3 */
    (void)slave;
    (void)addr;
    (void)data;
    (void)len;

    global_stats.lrd_count++;
    return 0;  /* Not implemented yet */
}

static int process_lwr(ecat_slave_t *slave, uint32_t addr, const uint8_t *data, uint16_t len)
{
    /* Logical write - FMMU mapping required */
    /* TODO: Implement FMMU mapping in subtask-2-3 */
    (void)slave;
    (void)addr;
    (void)data;
    (void)len;

    global_stats.lwr_count++;
    return 0;  /* Not implemented yet */
}

static int process_lrw(ecat_slave_t *slave, uint32_t addr, uint8_t *data, uint16_t len)
{
    /* Logical read/write - FMMU mapping required */
    /* TODO: Implement FMMU mapping in subtask-2-3 */
    (void)slave;
    (void)addr;
    (void)data;
    (void)len;

    global_stats.lrw_count++;
    return 0;  /* Not implemented yet */
}

/* ============================================================================
 * Frame Processing Functions
 * ============================================================================ */

int ecat_process_datagram(ecat_slave_t *slave, const ecat_datagram_t *dg,
                          const uint8_t *data, size_t len)
{
    if (!slave || !dg || !data) return -1;

    /* Extract datagram fields */
    uint8_t cmd = dg->cmd;
    uint32_t addr = ecat_le32_to_cpu(dg->addr);
    uint16_t len_flags = ecat_le16_to_cpu(dg->len_flags);
    uint16_t data_len = len_flags & 0x07FF;  /* Lower 11 bits */

    /* Validate data length */
    if (data_len > len - 2) {  /* -2 for working counter */
        global_stats.frame_errors++;
        return -1;
    }

    /* Working counter is at end of data */
    uint16_t *wkc_ptr = (uint16_t *)(data + data_len);
    uint16_t wkc = ecat_le16_to_cpu(*wkc_ptr);
    int wkc_increment = 0;

    /* Process command */
    switch (cmd) {
        case ECAT_CMD_NOP:
            /* No operation */
            break;

        case ECAT_CMD_APRD:
            wkc_increment = process_aprd(slave, addr, (uint8_t *)data, data_len);
            break;

        case ECAT_CMD_APWR:
            wkc_increment = process_apwr(slave, addr, data, data_len);
            break;

        case ECAT_CMD_APRW:
            /* Read/write: read first, then write */
            wkc_increment = process_aprd(slave, addr, (uint8_t *)data, data_len);
            if (wkc_increment > 0) {
                wkc_increment = process_apwr(slave, addr, data, data_len);
            }
            break;

        case ECAT_CMD_FPRD:
            wkc_increment = process_fprd(slave, addr, (uint8_t *)data, data_len);
            break;

        case ECAT_CMD_FPWR:
            wkc_increment = process_fpwr(slave, addr, data, data_len);
            break;

        case ECAT_CMD_FPRW:
            /* Read/write: read first, then write */
            wkc_increment = process_fprd(slave, addr, (uint8_t *)data, data_len);
            if (wkc_increment > 0) {
                wkc_increment = process_fpwr(slave, addr, data, data_len);
            }
            break;

        case ECAT_CMD_BRD:
            wkc_increment = process_brd(slave, addr, (uint8_t *)data, data_len);
            break;

        case ECAT_CMD_BWR:
            wkc_increment = process_bwr(slave, addr, data, data_len);
            break;

        case ECAT_CMD_BRW:
            /* Read/write: read first, then write */
            wkc_increment = process_brd(slave, addr, (uint8_t *)data, data_len);
            if (wkc_increment > 0) {
                wkc_increment = process_bwr(slave, addr, data, data_len);
            }
            break;

        case ECAT_CMD_LRD:
            wkc_increment = process_lrd(slave, addr, (uint8_t *)data, data_len);
            break;

        case ECAT_CMD_LWR:
            wkc_increment = process_lwr(slave, addr, data, data_len);
            break;

        case ECAT_CMD_LRW:
            wkc_increment = process_lrw(slave, addr, (uint8_t *)data, data_len);
            break;

        case ECAT_CMD_ARMW:
        case ECAT_CMD_FRMW:
            /* Multiple write commands - not commonly used */
            break;

        default:
            /* Unknown command */
            global_stats.frame_errors++;
            return -1;
    }

    /* Update working counter */
    if (wkc_increment > 0) {
        wkc += wkc_increment;
        *wkc_ptr = ecat_cpu_to_le16(wkc);
    }

    global_stats.datagrams_processed++;
    return 0;
}

int ecat_process_frame(ecat_slave_t *slave, const uint8_t *frame, size_t len)
{
    if (!slave || !frame) return -1;
    if (len < sizeof(ecat_header_t)) return -1;

    /* Parse EtherCAT header */
    const ecat_header_t *hdr = (const ecat_header_t *)frame;
    uint16_t length_type = ecat_le16_to_cpu(hdr->length_type);

    uint16_t frame_len = length_type & 0x07FF;  /* Lower 11 bits */
    uint8_t frame_type = (length_type >> 12) & 0x0F;  /* Bits 12-15 */

    /* Validate frame type */
    if (frame_type != ECAT_TYPE_DATAGRAM) {
        /* Not a datagram frame */
        global_stats.frame_errors++;
        return -1;
    }

    /* Validate frame length */
    if (frame_len + sizeof(ecat_header_t) > len) {
        global_stats.frame_errors++;
        return -1;
    }

    /* Process datagrams */
    const uint8_t *ptr = frame + sizeof(ecat_header_t);
    size_t remaining = frame_len;
    int datagram_count = 0;

    while (remaining >= sizeof(ecat_datagram_t)) {
        const ecat_datagram_t *dg = (const ecat_datagram_t *)ptr;

        /* Get datagram data length */
        uint16_t len_flags = ecat_le16_to_cpu(dg->len_flags);
        uint16_t data_len = len_flags & 0x07FF;

        /* Check if we have enough data */
        size_t dg_total_len = sizeof(ecat_datagram_t) + data_len + 2;  /* +2 for WKC */
        if (dg_total_len > remaining) {
            global_stats.frame_errors++;
            break;
        }

        /* Process this datagram */
        const uint8_t *dg_data = ptr + sizeof(ecat_datagram_t);
        int ret = ecat_process_datagram(slave, dg, dg_data, data_len + 2);
        if (ret < 0) {
            global_stats.frame_errors++;
        }

        datagram_count++;

        /* Check if more datagrams follow */
        if (!(len_flags & ECAT_FLAG_MORE)) {
            break;
        }

        /* Move to next datagram */
        ptr += dg_total_len;
        remaining -= dg_total_len;

        /* Safety check - prevent infinite loop */
        if (datagram_count >= ECAT_MAX_DATAGRAMS) {
            break;
        }
    }

    global_stats.frames_received++;
    global_stats.bytes_received += len;

    return datagram_count;
}

int ecat_build_response(ecat_slave_t *slave, uint8_t *frame, size_t *len)
{
    /* Response is built in-place during ecat_process_frame */
    /* This function is for future mailbox responses */
    /* TODO: Implement mailbox response building in subtask-2-4 */

    if (!slave || !frame || !len) return -1;

    *len = 0;
    return 0;
}

/* ============================================================================
 * Slave Lifecycle Functions
 * ============================================================================ */

ecat_slave_t *ecat_slave_create(const ecat_slave_config_t *config)
{
    if (!config) return NULL;

    ecat_slave_t *slave = (ecat_slave_t *)kzalloc(sizeof(ecat_slave_t));
    if (!slave) return NULL;

    /* Copy configuration */
    memcpy(&slave->config, config, sizeof(ecat_slave_config_t));

    /* Allocate register space (typical ESC has 64KB) */
    slave->register_size = 65536;
    slave->registers = (uint8_t *)kzalloc(slave->register_size);
    if (!slave->registers) {
        kfree(slave);
        return NULL;
    }

    /* Initialize state */
    slave->al_state = ECAT_STATE_INIT;
    slave->requested_state = ECAT_STATE_INIT;
    slave->al_status_code = ECAT_AL_STATUS_OK;
    slave->active = false;

    if (!ethercat_initialized) {
        memset(&global_stats, 0, sizeof(global_stats));
        ethercat_initialized = true;
    }

    return slave;
}

void ecat_slave_destroy(ecat_slave_t *slave)
{
    if (!slave) return;

    if (slave->registers) {
        kfree(slave->registers);
    }

    if (slave->mbox_out_buf) {
        kfree(slave->mbox_out_buf);
    }

    if (slave->mbox_in_buf) {
        kfree(slave->mbox_in_buf);
    }

    kfree(slave);
}

int ecat_slave_init(ecat_slave_t *slave)
{
    if (!slave) return -1;

    /* Initialize ESC registers */
    memset(slave->registers, 0, slave->register_size);

    /* Set device information in registers */
    ecat_reg_write16(slave, ECAT_REG_STATION_ADDR, slave->config.station_address);
    ecat_reg_write16(slave, ECAT_REG_STATION_ALIAS, slave->config.station_alias);
    ecat_reg_write8(slave, ECAT_REG_FMMU_COUNT, slave->config.fmmu_count);
    ecat_reg_write8(slave, ECAT_REG_SYNC_COUNT, slave->config.sm_count);

    /* Initialize state machine */
    slave->al_state = ECAT_STATE_INIT;
    slave->requested_state = ECAT_STATE_INIT;
    slave->al_status_code = ECAT_AL_STATUS_OK;

    /* Update AL status register */
    ecat_reg_write16(slave, ECAT_REG_AL_STATUS, slave->al_state);
    ecat_reg_write16(slave, ECAT_REG_AL_STATUS_CODE, slave->al_status_code);

    slave->active = true;
    return 0;
}

int ecat_slave_reset(ecat_slave_t *slave)
{
    if (!slave) return -1;

    /* Reset state machine */
    slave->al_state = ECAT_STATE_INIT;
    slave->requested_state = ECAT_STATE_INIT;
    slave->al_status_code = ECAT_AL_STATUS_OK;

    /* Update registers */
    ecat_reg_write16(slave, ECAT_REG_AL_STATUS, slave->al_state);
    ecat_reg_write16(slave, ECAT_REG_AL_STATUS_CODE, slave->al_status_code);

    /* Deactivate all FMMUs */
    for (int i = 0; i < ECAT_FMMU_MAX; i++) {
        slave->fmmu[i].activate = 0;
    }

    /* Deactivate all sync managers */
    for (int i = 0; i < ECAT_SM_MAX; i++) {
        slave->sm[i].activate = 0;
    }

    return 0;
}

/* ============================================================================
 * State Machine Functions
 * ============================================================================ */

static bool is_valid_state_transition(uint8_t from_state, uint8_t to_state)
{
    /* Clear error flags for comparison */
    from_state &= 0x0F;
    to_state &= 0x0F;

    /* INIT can transition to any state */
    if (from_state == ECAT_STATE_INIT) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_PREOP ||
                to_state == ECAT_STATE_BOOT ||
                to_state == ECAT_STATE_SAFEOP ||
                to_state == ECAT_STATE_OP);
    }

    /* PREOP can go to INIT, BOOT, SAFEOP */
    if (from_state == ECAT_STATE_PREOP) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_PREOP ||
                to_state == ECAT_STATE_BOOT ||
                to_state == ECAT_STATE_SAFEOP);
    }

    /* BOOT can only go to INIT */
    if (from_state == ECAT_STATE_BOOT) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_BOOT);
    }

    /* SAFEOP can go to INIT, PREOP, OP */
    if (from_state == ECAT_STATE_SAFEOP) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_PREOP ||
                to_state == ECAT_STATE_SAFEOP ||
                to_state == ECAT_STATE_OP);
    }

    /* OP can go to INIT, SAFEOP */
    if (from_state == ECAT_STATE_OP) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_SAFEOP ||
                to_state == ECAT_STATE_OP);
    }

    return false;
}

static int state_enter_init(ecat_slave_t *slave)
{
    /* INIT state - minimal functionality */
    /* All sync managers and FMMUs are deactivated */

    /* Deactivate all FMMUs */
    for (int i = 0; i < ECAT_FMMU_MAX; i++) {
        slave->fmmu[i].activate = 0;
    }

    /* Deactivate all sync managers */
    for (int i = 0; i < ECAT_SM_MAX; i++) {
        slave->sm[i].activate = 0;
    }

    return 0;
}

static int state_enter_preop(ecat_slave_t *slave)
{
    /* PREOP state - mailbox communication allowed */
    /* Sync managers for mailbox can be activated */

    if (slave->config.mailbox_supported) {
        /* Activate mailbox sync managers */
        if (slave->config.mbox_out_size > 0) {
            slave->sm[ECAT_SM_MBOX_OUT].activate = 1;
        }
        if (slave->config.mbox_in_size > 0) {
            slave->sm[ECAT_SM_MBOX_IN].activate = 1;
        }
    }

    return 0;
}

static int state_enter_safeop(ecat_slave_t *slave)
{
    /* SAFEOP state - process data communication enabled, outputs cleared */
    /* Sync managers for process data can be activated */

    /* Validate sync manager configuration for process data */
    if (slave->config.input_size > 0) {
        if (slave->sm[ECAT_SM_PROC_IN].length == 0) {
            slave->al_status_code = ECAT_AL_STATUS_INVALID_SYNC;
            return -1;
        }
    }

    if (slave->config.output_size > 0) {
        if (slave->sm[ECAT_SM_PROC_OUT].length == 0) {
            slave->al_status_code = ECAT_AL_STATUS_INVALID_SYNC;
            return -1;
        }
    }

    /* Activate process data sync managers */
    if (slave->config.input_size > 0) {
        slave->sm[ECAT_SM_PROC_IN].activate = 1;
    }

    if (slave->config.output_size > 0) {
        slave->sm[ECAT_SM_PROC_OUT].activate = 1;
        /* Clear output data in SAFEOP */
        if (slave->config.output_data) {
            memset(slave->config.output_data, 0, slave->config.output_size);
        }
    }

    return 0;
}

static int state_enter_op(ecat_slave_t *slave)
{
    /* OP state - full operation, all outputs active */
    /* No additional configuration needed, outputs can now be set */

    (void)slave;  /* All configuration already done in SAFEOP */

    return 0;
}

int ecat_slave_set_state(ecat_slave_t *slave, uint8_t state)
{
    if (!slave) return -1;

    /* Store requested state */
    slave->requested_state = state & 0x0F;

    /* Update AL control register */
    ecat_reg_write16(slave, ECAT_REG_AL_CONTROL, state);

    return 0;
}

uint8_t ecat_slave_get_state(const ecat_slave_t *slave)
{
    if (!slave) return 0;

    return slave->al_state;
}

int ecat_slave_transition(ecat_slave_t *slave)
{
    if (!slave) return -1;

    uint8_t current = slave->al_state & 0x0F;
    uint8_t requested = slave->requested_state & 0x0F;

    /* No transition needed */
    if (current == requested) {
        return 0;
    }

    /* Validate transition */
    if (!is_valid_state_transition(current, requested)) {
        slave->al_status_code = ECAT_AL_STATUS_INVALID_SETUP;
        slave->al_state = current | ECAT_STATE_ERROR;
        ecat_reg_write16(slave, ECAT_REG_AL_STATUS, slave->al_state);
        ecat_reg_write16(slave, ECAT_REG_AL_STATUS_CODE, slave->al_status_code);
        return -1;
    }

    /* Perform state transition */
    int ret = 0;

    switch (requested) {
        case ECAT_STATE_INIT:
            ret = state_enter_init(slave);
            break;

        case ECAT_STATE_PREOP:
            ret = state_enter_preop(slave);
            break;

        case ECAT_STATE_BOOT:
            /* Boot state - firmware update mode */
            /* Similar to PREOP but with bootstrap mailbox */
            ret = 0;
            break;

        case ECAT_STATE_SAFEOP:
            ret = state_enter_safeop(slave);
            break;

        case ECAT_STATE_OP:
            ret = state_enter_op(slave);
            break;

        default:
            slave->al_status_code = ECAT_AL_STATUS_INVALID_SETUP;
            ret = -1;
            break;
    }

    /* Update state based on transition result */
    if (ret == 0) {
        /* Successful transition */
        slave->al_state = requested;
        slave->al_status_code = ECAT_AL_STATUS_OK;
        global_stats.state_transitions++;

        /* Update state-specific statistics */
        switch (requested) {
            case ECAT_STATE_INIT:
                global_stats.state_init++;
                break;
            case ECAT_STATE_PREOP:
                global_stats.state_preop++;
                break;
            case ECAT_STATE_SAFEOP:
                global_stats.state_safeop++;
                break;
            case ECAT_STATE_OP:
                global_stats.state_op++;
                break;
        }
    } else {
        /* Failed transition - set error flag */
        slave->al_state = current | ECAT_STATE_ERROR;
    }

    /* Update AL status registers */
    ecat_reg_write16(slave, ECAT_REG_AL_STATUS, slave->al_state);
    ecat_reg_write16(slave, ECAT_REG_AL_STATUS_CODE, slave->al_status_code);

    return ret;
}

/* ============================================================================
 * FMMU Configuration (Stubs for subtask-2-3)
 * ============================================================================ */

int ecat_fmmu_config(ecat_slave_t *slave, uint8_t fmmu_idx, const ecat_fmmu_t *fmmu)
{
    /* TODO: Implement in subtask-2-3 */
    if (!slave || fmmu_idx >= ECAT_FMMU_MAX || !fmmu) return -1;

    memcpy(&slave->fmmu[fmmu_idx], fmmu, sizeof(ecat_fmmu_t));
    return 0;
}

int ecat_fmmu_activate(ecat_slave_t *slave, uint8_t fmmu_idx)
{
    /* TODO: Implement in subtask-2-3 */
    if (!slave || fmmu_idx >= ECAT_FMMU_MAX) return -1;

    slave->fmmu[fmmu_idx].activate = 1;
    return 0;
}

int ecat_fmmu_deactivate(ecat_slave_t *slave, uint8_t fmmu_idx)
{
    /* TODO: Implement in subtask-2-3 */
    if (!slave || fmmu_idx >= ECAT_FMMU_MAX) return -1;

    slave->fmmu[fmmu_idx].activate = 0;
    return 0;
}

/* ============================================================================
 * Sync Manager Configuration (Stubs for subtask-2-3)
 * ============================================================================ */

int ecat_sm_config(ecat_slave_t *slave, uint8_t sm_idx, const ecat_sm_t *sm)
{
    /* TODO: Implement in subtask-2-3 */
    if (!slave || sm_idx >= ECAT_SM_MAX || !sm) return -1;

    memcpy(&slave->sm[sm_idx], sm, sizeof(ecat_sm_t));
    return 0;
}

int ecat_sm_activate(ecat_slave_t *slave, uint8_t sm_idx)
{
    /* TODO: Implement in subtask-2-3 */
    if (!slave || sm_idx >= ECAT_SM_MAX) return -1;

    slave->sm[sm_idx].activate = 1;
    return 0;
}

int ecat_sm_deactivate(ecat_slave_t *slave, uint8_t sm_idx)
{
    /* TODO: Implement in subtask-2-3 */
    if (!slave || sm_idx >= ECAT_SM_MAX) return -1;

    slave->sm[sm_idx].activate = 0;
    return 0;
}

/* ============================================================================
 * PDO Functions
 * ============================================================================ */

int ecat_pdo_exchange(ecat_slave_t *slave)
{
    if (!slave) return -1;

    /* PDO exchange only possible in SAFEOP and OP states */
    uint8_t state = slave->al_state & 0x0F;
    if (state != ECAT_STATE_SAFEOP && state != ECAT_STATE_OP) {
        return -1;
    }

    /* Exchange output PDOs (master -> slave, RxPDO) */
    if (slave->config.output_size > 0 && slave->config.output_data) {
        /* Check if output sync manager is active */
        if (slave->sm[ECAT_SM_PROC_OUT].activate) {
            uint16_t phys_addr = slave->sm[ECAT_SM_PROC_OUT].physical_start;
            uint16_t sm_len = slave->sm[ECAT_SM_PROC_OUT].length;

            /* Validate size */
            if (sm_len < slave->config.output_size) {
                return -1;
            }

            /* Copy from sync manager memory to application output buffer */
            if (phys_addr + slave->config.output_size <= slave->register_size) {
                memcpy(slave->config.output_data,
                       &slave->registers[phys_addr],
                       slave->config.output_size);
            }
        }
    }

    /* Exchange input PDOs (slave -> master, TxPDO) */
    if (slave->config.input_size > 0 && slave->config.input_data) {
        /* Check if input sync manager is active */
        if (slave->sm[ECAT_SM_PROC_IN].activate) {
            uint16_t phys_addr = slave->sm[ECAT_SM_PROC_IN].physical_start;
            uint16_t sm_len = slave->sm[ECAT_SM_PROC_IN].length;

            /* Validate size */
            if (sm_len < slave->config.input_size) {
                return -1;
            }

            /* Copy from application input buffer to sync manager memory */
            if (phys_addr + slave->config.input_size <= slave->register_size) {
                memcpy(&slave->registers[phys_addr],
                       slave->config.input_data,
                       slave->config.input_size);
            }
        }
    }

    /* Update statistics */
    global_stats.pdo_cycles++;

    return 0;
}

int ecat_pdo_read(ecat_slave_t *slave, uint8_t *data, size_t len)
{
    if (!slave || !data) return -1;

    /* Read input PDO data (slave -> master, TxPDO) */
    if (!slave->config.input_data || slave->config.input_size == 0) {
        return 0;  /* No input data configured */
    }

    /* Determine actual read size */
    size_t read_size = (len < slave->config.input_size) ? len : slave->config.input_size;

    /* Copy input PDO data to buffer */
    memcpy(data, slave->config.input_data, read_size);

    return (int)read_size;
}

int ecat_pdo_write(ecat_slave_t *slave, const uint8_t *data, size_t len)
{
    if (!slave || !data) return -1;

    /* Write output PDO data (master -> slave, RxPDO) */
    if (!slave->config.output_data || slave->config.output_size == 0) {
        return 0;  /* No output data configured */
    }

    /* Determine actual write size */
    size_t write_size = (len < slave->config.output_size) ? len : slave->config.output_size;

    /* Copy data to output PDO buffer */
    memcpy(slave->config.output_data, data, write_size);

    return (int)write_size;
}

/* ============================================================================
 * Mailbox Functions (Stubs for subtask-2-4)
 * ============================================================================ */

int ecat_mailbox_send(ecat_slave_t *slave, uint8_t type, const uint8_t *data, size_t len)
{
    /* TODO: Implement in subtask-2-4 */
    (void)slave;
    (void)type;
    (void)data;
    (void)len;

    return 0;
}

int ecat_mailbox_receive(ecat_slave_t *slave, uint8_t *type, uint8_t *data, size_t *len)
{
    /* TODO: Implement in subtask-2-4 */
    (void)slave;
    (void)type;
    (void)data;
    (void)len;

    return 0;
}

int ecat_mailbox_process(ecat_slave_t *slave)
{
    /* TODO: Implement in subtask-2-4 */
    (void)slave;

    return 0;
}

/* ============================================================================
 * Distributed Clock Functions (Stubs for subtask-2-4)
 * ============================================================================ */

int ecat_dc_init(ecat_slave_t *slave)
{
    /* TODO: Implement in subtask-2-4 */
    if (!slave) return -1;

    slave->dc_system_time = 0;
    slave->dc_time_offset = 0;
    slave->dc_sync_active = false;
    return 0;
}

int ecat_dc_sync(ecat_slave_t *slave, uint64_t master_time)
{
    /* TODO: Implement in subtask-2-4 */
    if (!slave) return -1;

    slave->dc_system_time = master_time;
    return 0;
}

uint64_t ecat_dc_get_time(const ecat_slave_t *slave)
{
    if (!slave) return 0;

    return slave->dc_system_time;
}

int ecat_dc_set_sync_mode(ecat_slave_t *slave, bool enabled)
{
    /* TODO: Implement in subtask-2-4 */
    if (!slave) return -1;

    slave->dc_sync_active = enabled;
    return 0;
}

/* ============================================================================
 * Watchdog Functions (Stubs for subtask-2-4)
 * ============================================================================ */

int ecat_watchdog_init(ecat_slave_t *slave, uint16_t divider, uint16_t time_pdi, uint16_t time_proc)
{
    /* TODO: Implement in subtask-2-4 */
    if (!slave) return -1;

    slave->wd_divider = divider;
    slave->wd_time_pdi = time_pdi;
    slave->wd_time_proc = time_proc;
    return 0;
}

int ecat_watchdog_trigger(ecat_slave_t *slave)
{
    /* TODO: Implement in subtask-2-4 */
    if (!slave) return -1;

    slave->wd_last_trigger = 0;  /* TODO: Use actual time */
    return 0;
}

int ecat_watchdog_check(ecat_slave_t *slave)
{
    /* TODO: Implement in subtask-2-4 */
    if (!slave) return -1;

    return 0;
}

/* ============================================================================
 * Statistics Functions
 * ============================================================================ */

void ecat_get_stats(const ecat_slave_t *slave, ecat_stats_t *stats)
{
    if (!stats) return;

    (void)slave;  /* Using global stats for now */

    /* Return global stats */
    memcpy(stats, &global_stats, sizeof(ecat_stats_t));
}

void ecat_reset_stats(ecat_slave_t *slave)
{
    (void)slave;

    memset(&global_stats, 0, sizeof(global_stats));
}

/* ============================================================================
 * Network Interface Binding (Stubs for subtask-2-4)
 * ============================================================================ */

int ecat_bind_netif(ecat_slave_t *slave, void *netif)
{
    /* TODO: Implement in subtask-2-4 */
    if (!slave) return -1;

    slave->netif = netif;
    return 0;
}

int ecat_unbind_netif(ecat_slave_t *slave)
{
    /* TODO: Implement in subtask-2-4 */
    if (!slave) return -1;

    slave->netif = NULL;
    return 0;
}
