/* EMBODIOS BCM I2C Driver Implementation
 *
 * Inter-Integrated Circuit (I2C) driver for BCM2712 (Raspberry Pi 5)
 * and compatible ARM64 platforms. Supports sensor and peripheral
 * communication for industrial, robotics, and IoT applications.
 */

#include <embodios/i2c.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <arch/aarch64/io.h>

/* Debug output (uncomment to enable) */
/* #define I2C_DEBUG 1 */

/* ============================================================================
 * Module State
 * ============================================================================ */

/**
 * I2C controller state structure
 * Maintains controller configuration, state, and statistics
 */
typedef struct i2c_controller {
    volatile void *base;        /* Register base address */
    i2c_config_t config;        /* Configuration parameters */
    i2c_state_t state;          /* Controller state */
    i2c_stats_t stats;          /* Statistics counters */
    bool initialized;           /* Initialization flag */
} i2c_controller_t;

/* Global I2C controller instances (BCM2712 has 8 controllers) */
static i2c_controller_t g_i2c_controllers[I2C_MAX_CONTROLLERS] = {0};

/* Base address lookup table */
static const uint64_t i2c_base_addresses[I2C_MAX_CONTROLLERS] = {
    BCM2712_I2C0_BASE,
    BCM2712_I2C1_BASE,
    BCM2712_I2C2_BASE,
    BCM2712_I2C3_BASE,
    BCM2712_I2C4_BASE,
    BCM2712_I2C5_BASE,
    BCM2712_I2C6_BASE,
    BCM2712_I2C7_BASE,
};

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Validate controller number
 */
static inline bool validate_controller(uint8_t controller)
{
    return (controller < I2C_MAX_CONTROLLERS);
}

/**
 * Get register address for controller
 */
static inline volatile uint32_t *get_reg(uint8_t controller, uint32_t offset)
{
    return (volatile uint32_t *)((uintptr_t)g_i2c_controllers[controller].base + offset);
}

/**
 * Wait for transfer completion or timeout
 */
static int wait_for_done(uint8_t controller, uint32_t timeout_ms)
{
    volatile uint32_t *status_reg = get_reg(controller, I2C_S);
    uint32_t timeout_count = timeout_ms * 1000;  /* Approximate microsecond delay */

    while (timeout_count > 0) {
        uint32_t status = mmio_read32(status_reg);

        /* Check for errors */
        if (status & I2C_S_ERR) {
            /* Clear error flag */
            mmio_write32(status_reg, I2C_S_ERR);
            g_i2c_controllers[controller].stats.nak_errors++;
            return I2C_ERR_NAK;
        }

        if (status & I2C_S_CLKT) {
            /* Clear clock stretch timeout flag */
            mmio_write32(status_reg, I2C_S_CLKT);
            g_i2c_controllers[controller].stats.clk_stretch_errors++;
            return I2C_ERR_CLKT;
        }

        /* Check if transfer is done */
        if (status & I2C_S_DONE) {
            /* Clear done flag */
            mmio_write32(status_reg, I2C_S_DONE);
            return I2C_OK;
        }

        timeout_count--;
    }

    g_i2c_controllers[controller].stats.timeout_errors++;
    return I2C_ERR_TIMEOUT;
}

/**
 * Calculate clock divider for desired speed
 */
static uint16_t calculate_divider(uint32_t speed_hz)
{
    /* BCM2712 core clock is typically 250 MHz */
    const uint32_t core_clock = 250000000;
    uint32_t divider;

    if (speed_hz == 0) {
        speed_hz = I2C_SPEED_DEFAULT;
    }

    /* Divider = CORE_CLK / desired_speed */
    divider = core_clock / speed_hz;

    /* Clamp to valid range */
    if (divider < 2) {
        divider = 2;
    }
    if (divider > 0xFFFE) {
        divider = 0xFFFE;
    }

    return (uint16_t)divider;
}

/**
 * Validate address based on addressing mode
 */
static int validate_address(uint16_t addr, bool addr_10bit)
{
    if (addr_10bit) {
        if (addr > I2C_MAX_10BIT_ADDR) {
            return I2C_ERR_ADDR_INVALID;
        }
    } else {
        if (addr > I2C_MAX_7BIT_ADDR) {
            return I2C_ERR_ADDR_INVALID;
        }
    }

    return I2C_OK;
}

/**
 * Clear FIFO
 */
static void clear_fifo(uint8_t controller)
{
    volatile uint32_t *control_reg = get_reg(controller, I2C_C);
    uint32_t control = mmio_read32(control_reg);

    /* Set clear FIFO bits */
    control |= I2C_C_CLEAR;
    mmio_write32(control_reg, control);
}

/* ============================================================================
 * Initialization and Configuration
 * ============================================================================ */

/**
 * Initialize I2C controller
 */
int i2c_init(uint8_t controller, const i2c_config_t *config)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    console_printf("[I2C%d] Initializing BCM2712 I2C controller...\n", controller);

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    /* Initialize device structure */
    memset(ctrl, 0, sizeof(i2c_controller_t));

    /* Map register base address */
    ctrl->base = (volatile void *)i2c_base_addresses[controller];

    /* Setup configuration */
    if (config) {
        ctrl->config = *config;
    } else {
        /* Default configuration */
        ctrl->config.speed = I2C_SPEED_DEFAULT;
        ctrl->config.timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
        ctrl->config.use_dma = false;
        ctrl->config.addr_10bit = false;
        ctrl->config.retries = I2C_MAX_RETRIES;
    }

    /* Calculate and set clock divider */
    uint16_t divider = calculate_divider(ctrl->config.speed);
    volatile uint32_t *div_reg = get_reg(controller, I2C_DIV);
    mmio_write32(div_reg, divider);

    /* Set clock stretch timeout (use default value) */
    volatile uint32_t *clkt_reg = get_reg(controller, I2C_CLKT);
    mmio_write32(clkt_reg, 0x40);  /* Default timeout value */

    /* Clear FIFO */
    clear_fifo(controller);

    /* Clear any pending status flags */
    volatile uint32_t *status_reg = get_reg(controller, I2C_S);
    mmio_write32(status_reg, I2C_S_CLKT | I2C_S_ERR | I2C_S_DONE);

    /* Enable I2C controller */
    volatile uint32_t *control_reg = get_reg(controller, I2C_C);
    mmio_write32(control_reg, I2C_C_I2CEN);

    /* Initialize statistics */
    ctrl->stats.tx_msgs = 0;
    ctrl->stats.rx_msgs = 0;
    ctrl->stats.tx_bytes = 0;
    ctrl->stats.rx_bytes = 0;
    ctrl->stats.errors = 0;
    ctrl->stats.nak_errors = 0;
    ctrl->stats.timeout_errors = 0;
    ctrl->stats.clk_stretch_errors = 0;
    ctrl->stats.retries = 0;

    ctrl->state = I2C_STATE_IDLE;
    ctrl->initialized = true;

    console_printf("[I2C%d] Driver initialized successfully\n", controller);
    console_printf("[I2C%d] Base: 0x%llX, Speed: %d Hz, Divider: %d\n",
                   controller, i2c_base_addresses[controller],
                   ctrl->config.speed, divider);

    return I2C_OK;
}

/**
 * Shutdown I2C controller
 */
void i2c_shutdown(uint8_t controller)
{
    if (!validate_controller(controller)) {
        return;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return;
    }

    console_printf("[I2C%d] Shutting down I2C controller...\n", controller);
    console_printf("[I2C%d] Statistics: tx_msgs=%llu, rx_msgs=%llu, tx_bytes=%llu, rx_bytes=%llu\n",
                   controller, ctrl->stats.tx_msgs, ctrl->stats.rx_msgs,
                   ctrl->stats.tx_bytes, ctrl->stats.rx_bytes);
    console_printf("[I2C%d] Errors: total=%llu, nak=%llu, timeout=%llu, clk_stretch=%llu\n",
                   controller, ctrl->stats.errors, ctrl->stats.nak_errors,
                   ctrl->stats.timeout_errors, ctrl->stats.clk_stretch_errors);

    /* Disable I2C controller */
    volatile uint32_t *control_reg = get_reg(controller, I2C_C);
    mmio_write32(control_reg, 0);

    ctrl->state = I2C_STATE_DISABLED;
    ctrl->initialized = false;

    console_printf("[I2C%d] Driver shutdown complete\n", controller);
}

/**
 * Check if I2C controller is initialized
 */
bool i2c_is_initialized(uint8_t controller)
{
    if (!validate_controller(controller)) {
        return false;
    }

    return g_i2c_controllers[controller].initialized;
}

/**
 * Get current I2C controller state
 */
i2c_state_t i2c_get_state(uint8_t controller)
{
    if (!validate_controller(controller)) {
        return I2C_STATE_DISABLED;
    }

    return g_i2c_controllers[controller].state;
}

/* ============================================================================
 * Configuration Functions
 * ============================================================================ */

/**
 * Set I2C bus speed
 */
int i2c_set_speed(uint8_t controller, uint32_t speed)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return I2C_ERR_NOT_INIT;
    }

    /* Calculate and set clock divider */
    uint16_t divider = calculate_divider(speed);
    volatile uint32_t *div_reg = get_reg(controller, I2C_DIV);
    mmio_write32(div_reg, divider);

    ctrl->config.speed = speed;

#ifdef I2C_DEBUG
    console_printf("[I2C%d] Speed set to %d Hz (divider: %d)\n", controller, speed, divider);
#endif

    return I2C_OK;
}

/**
 * Get current I2C bus speed
 */
uint32_t i2c_get_speed(uint8_t controller)
{
    if (!validate_controller(controller)) {
        return 0;
    }

    if (!g_i2c_controllers[controller].initialized) {
        return 0;
    }

    return g_i2c_controllers[controller].config.speed;
}

/**
 * Set I2C timeout
 */
int i2c_set_timeout(uint8_t controller, uint32_t timeout_ms)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return I2C_ERR_NOT_INIT;
    }

    ctrl->config.timeout_ms = timeout_ms;

    return I2C_OK;
}

/* ============================================================================
 * Data Transfer Functions
 * ============================================================================ */

/**
 * Write data to I2C slave device
 */
int i2c_write(uint8_t controller, uint16_t addr, const uint8_t *buf, uint16_t len)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        ctrl->stats.errors++;
        return I2C_ERR_NOT_INIT;
    }

    if (!buf || len == 0) {
        ctrl->stats.errors++;
        return I2C_ERR_INVALID;
    }

    if (len > I2C_MAX_TRANSFER_SIZE) {
        ctrl->stats.errors++;
        return I2C_ERR_DATA_SIZE;
    }

    int ret = validate_address(addr, ctrl->config.addr_10bit);
    if (ret != I2C_OK) {
        ctrl->stats.errors++;
        return ret;
    }

    ctrl->state = I2C_STATE_ACTIVE;

    /* Set slave address */
    volatile uint32_t *addr_reg = get_reg(controller, I2C_A);
    mmio_write32(addr_reg, addr);

    /* Set data length */
    volatile uint32_t *dlen_reg = get_reg(controller, I2C_DLEN);
    mmio_write32(dlen_reg, len);

    /* Clear FIFO */
    clear_fifo(controller);

    /* Clear status flags */
    volatile uint32_t *status_reg = get_reg(controller, I2C_S);
    mmio_write32(status_reg, I2C_S_CLKT | I2C_S_ERR | I2C_S_DONE);

    /* Start write transfer */
    volatile uint32_t *control_reg = get_reg(controller, I2C_C);
    mmio_write32(control_reg, I2C_C_I2CEN | I2C_C_ST);

    /* Write data to FIFO */
    volatile uint32_t *fifo_reg = get_reg(controller, I2C_FIFO);
    uint16_t bytes_written = 0;

    while (bytes_written < len) {
        uint32_t status = mmio_read32(status_reg);

        /* Check for errors */
        if (status & I2C_S_ERR) {
            mmio_write32(status_reg, I2C_S_ERR);
            ctrl->stats.errors++;
            ctrl->stats.nak_errors++;
            ctrl->state = I2C_STATE_ERROR;
            return I2C_ERR_NAK;
        }

        if (status & I2C_S_CLKT) {
            mmio_write32(status_reg, I2C_S_CLKT);
            ctrl->stats.errors++;
            ctrl->stats.clk_stretch_errors++;
            ctrl->state = I2C_STATE_ERROR;
            return I2C_ERR_CLKT;
        }

        /* Check if FIFO can accept data */
        if (status & I2C_S_TXD) {
            mmio_write32(fifo_reg, buf[bytes_written]);
            bytes_written++;
        }
    }

    /* Wait for transfer completion */
    ret = wait_for_done(controller, ctrl->config.timeout_ms);

    if (ret == I2C_OK) {
        ctrl->stats.tx_msgs++;
        ctrl->stats.tx_bytes += len;
        ctrl->state = I2C_STATE_IDLE;

#ifdef I2C_DEBUG
        console_printf("[I2C%d] Write %d bytes to 0x%02X\n", controller, len, addr);
#endif

        return len;
    } else {
        ctrl->stats.errors++;
        ctrl->state = I2C_STATE_ERROR;
        return ret;
    }
}

/**
 * Read data from I2C slave device
 */
int i2c_read(uint8_t controller, uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        ctrl->stats.errors++;
        return I2C_ERR_NOT_INIT;
    }

    if (!buf || len == 0) {
        ctrl->stats.errors++;
        return I2C_ERR_INVALID;
    }

    if (len > I2C_MAX_TRANSFER_SIZE) {
        ctrl->stats.errors++;
        return I2C_ERR_DATA_SIZE;
    }

    int ret = validate_address(addr, ctrl->config.addr_10bit);
    if (ret != I2C_OK) {
        ctrl->stats.errors++;
        return ret;
    }

    ctrl->state = I2C_STATE_ACTIVE;

    /* Set slave address */
    volatile uint32_t *addr_reg = get_reg(controller, I2C_A);
    mmio_write32(addr_reg, addr);

    /* Set data length */
    volatile uint32_t *dlen_reg = get_reg(controller, I2C_DLEN);
    mmio_write32(dlen_reg, len);

    /* Clear FIFO */
    clear_fifo(controller);

    /* Clear status flags */
    volatile uint32_t *status_reg = get_reg(controller, I2C_S);
    mmio_write32(status_reg, I2C_S_CLKT | I2C_S_ERR | I2C_S_DONE);

    /* Start read transfer */
    volatile uint32_t *control_reg = get_reg(controller, I2C_C);
    mmio_write32(control_reg, I2C_C_I2CEN | I2C_C_ST | I2C_C_READ);

    /* Read data from FIFO */
    volatile uint32_t *fifo_reg = get_reg(controller, I2C_FIFO);
    uint16_t bytes_read = 0;

    while (bytes_read < len) {
        uint32_t status = mmio_read32(status_reg);

        /* Check for errors */
        if (status & I2C_S_ERR) {
            mmio_write32(status_reg, I2C_S_ERR);
            ctrl->stats.errors++;
            ctrl->stats.nak_errors++;
            ctrl->state = I2C_STATE_ERROR;
            return I2C_ERR_NAK;
        }

        if (status & I2C_S_CLKT) {
            mmio_write32(status_reg, I2C_S_CLKT);
            ctrl->stats.errors++;
            ctrl->stats.clk_stretch_errors++;
            ctrl->state = I2C_STATE_ERROR;
            return I2C_ERR_CLKT;
        }

        /* Check if FIFO has data */
        if (status & I2C_S_RXD) {
            buf[bytes_read] = (uint8_t)mmio_read32(fifo_reg);
            bytes_read++;
        }

        /* Check if transfer is done */
        if (status & I2C_S_DONE) {
            break;
        }
    }

    /* Wait for transfer completion */
    ret = wait_for_done(controller, ctrl->config.timeout_ms);

    if (ret == I2C_OK || bytes_read > 0) {
        ctrl->stats.rx_msgs++;
        ctrl->stats.rx_bytes += bytes_read;
        ctrl->state = I2C_STATE_IDLE;

#ifdef I2C_DEBUG
        console_printf("[I2C%d] Read %d bytes from 0x%02X\n", controller, bytes_read, addr);
#endif

        return bytes_read;
    } else {
        ctrl->stats.errors++;
        ctrl->state = I2C_STATE_ERROR;
        return ret;
    }
}

/**
 * Write then read from I2C slave (combined transaction)
 */
int i2c_write_read(uint8_t controller, uint16_t addr,
                   const uint8_t *wbuf, uint16_t wlen,
                   uint8_t *rbuf, uint16_t rlen)
{
    int ret;

    /* Perform write */
    ret = i2c_write(controller, addr, wbuf, wlen);
    if (ret < 0) {
        return ret;
    }

    /* Perform read */
    ret = i2c_read(controller, addr, rbuf, rlen);
    if (ret < 0) {
        return ret;
    }

    return I2C_OK;
}

/**
 * Transfer multiple I2C messages atomically
 */
int i2c_transfer(uint8_t controller, i2c_msg_t *msgs, uint16_t num_msgs)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return I2C_ERR_NOT_INIT;
    }

    if (!msgs || num_msgs == 0) {
        return I2C_ERR_INVALID;
    }

    int ret;

    for (uint16_t i = 0; i < num_msgs; i++) {
        i2c_msg_t *msg = &msgs[i];

        if (msg->flags & I2C_M_RD) {
            /* Read message */
            ret = i2c_read(controller, msg->addr, msg->buf, msg->len);
        } else {
            /* Write message */
            ret = i2c_write(controller, msg->addr, msg->buf, msg->len);
        }

        if (ret < 0) {
            return ret;
        }
    }

    return I2C_OK;
}

/* ============================================================================
 * Register Access Helpers
 * ============================================================================ */

/**
 * Write byte to device register
 */
int i2c_write_reg_byte(uint8_t controller, uint16_t addr, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_write(controller, addr, buf, 2);
}

/**
 * Read byte from device register
 */
int i2c_read_reg_byte(uint8_t controller, uint16_t addr, uint8_t reg, uint8_t *value)
{
    int ret;

    /* Write register address */
    ret = i2c_write(controller, addr, &reg, 1);
    if (ret < 0) {
        return ret;
    }

    /* Read value */
    ret = i2c_read(controller, addr, value, 1);
    if (ret < 0) {
        return ret;
    }

    return I2C_OK;
}

/**
 * Write word (16-bit) to device register
 */
int i2c_write_reg_word(uint8_t controller, uint16_t addr, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_write(controller, addr, buf, 3);
}

/**
 * Read word (16-bit) from device register
 */
int i2c_read_reg_word(uint8_t controller, uint16_t addr, uint8_t reg, uint16_t *value)
{
    uint8_t buf[2];
    int ret;

    /* Write register address */
    ret = i2c_write(controller, addr, &reg, 1);
    if (ret < 0) {
        return ret;
    }

    /* Read value */
    ret = i2c_read(controller, addr, buf, 2);
    if (ret < 0) {
        return ret;
    }

    *value = ((uint16_t)buf[0] << 8) | buf[1];

    return I2C_OK;
}

/**
 * Write buffer to device register
 */
int i2c_write_reg_buf(uint8_t controller, uint16_t addr, uint8_t reg,
                      const uint8_t *buf, uint16_t len)
{
    if (!buf || len == 0 || len > (I2C_MAX_TRANSFER_SIZE - 1)) {
        return I2C_ERR_INVALID;
    }

    /* Allocate temporary buffer for register + data */
    uint8_t temp_buf[I2C_MAX_TRANSFER_SIZE];
    temp_buf[0] = reg;

    for (uint16_t i = 0; i < len; i++) {
        temp_buf[i + 1] = buf[i];
    }

    return i2c_write(controller, addr, temp_buf, len + 1);
}

/**
 * Read buffer from device register
 */
int i2c_read_reg_buf(uint8_t controller, uint16_t addr, uint8_t reg,
                     uint8_t *buf, uint16_t len)
{
    int ret;

    /* Write register address */
    ret = i2c_write(controller, addr, &reg, 1);
    if (ret < 0) {
        return ret;
    }

    /* Read data */
    return i2c_read(controller, addr, buf, len);
}

/* ============================================================================
 * Device Detection
 * ============================================================================ */

/**
 * Scan I2C bus for devices
 */
int i2c_scan(uint8_t controller, uint16_t *devices, uint16_t max_devices)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return I2C_ERR_NOT_INIT;
    }

    if (!devices || max_devices == 0) {
        return I2C_ERR_INVALID;
    }

    uint16_t found = 0;

    console_printf("[I2C%d] Scanning bus...\n", controller);

    /* Scan 7-bit address range (skip reserved addresses) */
    for (uint16_t addr = 0x08; addr <= I2C_MAX_7BIT_ADDR; addr++) {
        /* Try to write 0 bytes (probe device) */
        uint8_t dummy = 0;
        int ret = i2c_write(controller, addr, &dummy, 0);

        if (ret >= 0) {
            /* Device responded */
            if (found < max_devices) {
                devices[found] = addr;
                found++;
            }
            console_printf("[I2C%d] Found device at 0x%02X\n", controller, addr);
        }
    }

    console_printf("[I2C%d] Scan complete, found %d device(s)\n", controller, found);

    return found;
}

/**
 * Probe for device at specific address
 */
bool i2c_probe_device(uint8_t controller, uint16_t addr)
{
    if (!validate_controller(controller)) {
        return false;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return false;
    }

    /* Try to write 0 bytes (probe device) */
    uint8_t dummy = 0;
    int ret = i2c_write(controller, addr, &dummy, 0);

    return (ret >= 0);
}

/* ============================================================================
 * Statistics and Diagnostics
 * ============================================================================ */

/**
 * Get I2C controller statistics
 */
int i2c_get_stats(uint8_t controller, i2c_stats_t *stats)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return I2C_ERR_NOT_INIT;
    }

    if (!stats) {
        return I2C_ERR_INVALID;
    }

    *stats = ctrl->stats;

    return I2C_OK;
}

/**
 * Reset I2C controller statistics
 */
void i2c_reset_stats(uint8_t controller)
{
    if (!validate_controller(controller)) {
        return;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return;
    }

    ctrl->stats.tx_msgs = 0;
    ctrl->stats.rx_msgs = 0;
    ctrl->stats.tx_bytes = 0;
    ctrl->stats.rx_bytes = 0;
    ctrl->stats.errors = 0;
    ctrl->stats.nak_errors = 0;
    ctrl->stats.timeout_errors = 0;
    ctrl->stats.clk_stretch_errors = 0;
    ctrl->stats.retries = 0;
}

/**
 * Reset I2C controller (recovery from error state)
 */
int i2c_reset(uint8_t controller)
{
    if (!validate_controller(controller)) {
        return I2C_ERR_NO_DEVICE;
    }

    i2c_controller_t *ctrl = &g_i2c_controllers[controller];

    if (!ctrl->initialized) {
        return I2C_ERR_NOT_INIT;
    }

    console_printf("[I2C%d] Resetting controller...\n", controller);

    /* Disable controller */
    volatile uint32_t *control_reg = get_reg(controller, I2C_C);
    mmio_write32(control_reg, 0);

    /* Clear FIFO */
    clear_fifo(controller);

    /* Clear status flags */
    volatile uint32_t *status_reg = get_reg(controller, I2C_S);
    mmio_write32(status_reg, I2C_S_CLKT | I2C_S_ERR | I2C_S_DONE);

    /* Re-enable controller */
    mmio_write32(control_reg, I2C_C_I2CEN);

    ctrl->state = I2C_STATE_IDLE;

    console_printf("[I2C%d] Reset complete\n", controller);

    return I2C_OK;
}
