/* EMBODIOS I2C Bus Driver Interface
 *
 * Inter-Integrated Circuit (I2C) driver for sensor and peripheral
 * communication. Supports BCM2712 (Raspberry Pi 5) I2C controllers
 * for industrial, robotics, and IoT applications.
 *
 * Features:
 * - Multi-master I2C bus support
 * - Standard (100kHz), Fast (400kHz), Fast-Plus (1MHz) modes
 * - 7-bit and 10-bit addressing
 * - BCM2712 hardware controller integration
 * - DMA support for bulk transfers
 * - Clock stretching and error recovery
 */

#ifndef EMBODIOS_I2C_H
#define EMBODIOS_I2C_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * BCM2712 I2C Register Definitions
 * ============================================================================ */

/* BCM2712 I2C Base Addresses */
#define BCM2712_I2C0_BASE       0x107D001000UL  /* I2C0 controller */
#define BCM2712_I2C1_BASE       0x107D001100UL  /* I2C1 controller */
#define BCM2712_I2C2_BASE       0x107D001200UL  /* I2C2 controller */
#define BCM2712_I2C3_BASE       0x107D001300UL  /* I2C3 controller */
#define BCM2712_I2C4_BASE       0x107D001400UL  /* I2C4 controller */
#define BCM2712_I2C5_BASE       0x107D001500UL  /* I2C5 controller */
#define BCM2712_I2C6_BASE       0x107D001600UL  /* I2C6 controller */
#define BCM2712_I2C7_BASE       0x107D001700UL  /* I2C7 controller */

/* BCM2712 I2C Register Offsets */
#define I2C_C                   0x00    /* Control register */
#define I2C_S                   0x04    /* Status register */
#define I2C_DLEN                0x08    /* Data length register */
#define I2C_A                   0x0C    /* Slave address register */
#define I2C_FIFO                0x10    /* Data FIFO register */
#define I2C_DIV                 0x14    /* Clock divider register */
#define I2C_DEL                 0x18    /* Data delay register */
#define I2C_CLKT                0x1C    /* Clock stretch timeout register */

/* I2C Control Register (I2C_C) Bits */
#define I2C_C_I2CEN             (1 << 15)   /* I2C enable */
#define I2C_C_INTR              (1 << 10)   /* Interrupt on RX */
#define I2C_C_INTT              (1 << 9)    /* Interrupt on TX */
#define I2C_C_INTD              (1 << 8)    /* Interrupt on DONE */
#define I2C_C_ST                (1 << 7)    /* Start transfer */
#define I2C_C_CLEAR             (1 << 4)    /* Clear FIFO */
#define I2C_C_READ              (1 << 0)    /* Read transfer */

/* I2C Status Register (I2C_S) Bits */
#define I2C_S_CLKT              (1 << 9)    /* Clock stretch timeout */
#define I2C_S_ERR               (1 << 8)    /* ACK error */
#define I2C_S_RXF               (1 << 7)    /* RX FIFO full */
#define I2C_S_TXE               (1 << 6)    /* TX FIFO empty */
#define I2C_S_RXD               (1 << 5)    /* RX FIFO has data */
#define I2C_S_TXD               (1 << 4)    /* TX FIFO can accept data */
#define I2C_S_RXR               (1 << 3)    /* RX FIFO needs reading */
#define I2C_S_TXW               (1 << 2)    /* TX FIFO needs writing */
#define I2C_S_DONE              (1 << 1)    /* Transfer done */
#define I2C_S_TA                (1 << 0)    /* Transfer active */

/* I2C FIFO Size */
#define I2C_FIFO_SIZE           16      /* 16-byte hardware FIFO */

/* ============================================================================
 * I2C Protocol Constants
 * ============================================================================ */

/* I2C Speed Modes */
#define I2C_SPEED_STANDARD      100000  /* 100 kHz - Standard mode */
#define I2C_SPEED_FAST          400000  /* 400 kHz - Fast mode */
#define I2C_SPEED_FAST_PLUS     1000000 /* 1 MHz - Fast-plus mode */

/* Default speed */
#define I2C_SPEED_DEFAULT       I2C_SPEED_FAST

/* I2C Addressing Modes */
#define I2C_ADDR_7BIT           0       /* 7-bit addressing */
#define I2C_ADDR_10BIT          1       /* 10-bit addressing */

/* I2C Address Limits */
#define I2C_MAX_7BIT_ADDR       0x7F    /* Maximum 7-bit address */
#define I2C_MAX_10BIT_ADDR      0x3FF   /* Maximum 10-bit address */

/* Reserved I2C Addresses (7-bit) */
#define I2C_ADDR_GENERAL_CALL   0x00    /* General call address */
#define I2C_ADDR_START_BYTE     0x01    /* Start byte */
#define I2C_ADDR_RESERVED_MIN   0x00    /* Reserved range start */
#define I2C_ADDR_RESERVED_MAX   0x07    /* Reserved range end */

/* ============================================================================
 * I2C Buffer Sizes
 * ============================================================================ */

#define I2C_MAX_TRANSFER_SIZE   65535   /* Maximum single transfer (limited by DLEN) */
#define I2C_DEFAULT_TIMEOUT_MS  1000    /* Default timeout in milliseconds */
#define I2C_MAX_RETRIES         3       /* Maximum retry attempts */
#define I2C_MAX_CONTROLLERS     8       /* Maximum I2C controllers (BCM2712) */

/* ============================================================================
 * I2C Message Structure
 * ============================================================================ */

/**
 * I2C message flags
 */
#define I2C_M_RD                0x0001  /* Read data from slave to master */
#define I2C_M_TEN               0x0010  /* 10-bit addressing */
#define I2C_M_NOSTART           0x0020  /* No start condition (repeated start) */
#define I2C_M_IGNORE_NAK        0x0040  /* Ignore NAK from slave */
#define I2C_M_NO_RD_ACK         0x0080  /* Don't ACK read data */

/**
 * I2C message structure
 * Represents a single I2C transaction
 */
typedef struct i2c_msg {
    uint16_t addr;          /* Slave address (7-bit or 10-bit) */
    uint16_t flags;         /* Message flags (I2C_M_*) */
    uint16_t len;           /* Message length in bytes */
    uint8_t  *buf;          /* Data buffer pointer */
} i2c_msg_t;

/* ============================================================================
 * I2C Device Configuration
 * ============================================================================ */

/**
 * I2C controller configuration
 */
typedef struct i2c_config {
    uint32_t speed;         /* Bus speed in Hz (100k, 400k, 1M) */
    uint32_t timeout_ms;    /* Transaction timeout in milliseconds */
    bool use_dma;           /* Enable DMA for large transfers */
    bool addr_10bit;        /* Enable 10-bit addressing mode */
    uint8_t retries;        /* Number of retry attempts on error */
} i2c_config_t;

/* ============================================================================
 * I2C Device Statistics
 * ============================================================================ */

/**
 * I2C bus statistics and error counters
 */
typedef struct i2c_stats {
    uint64_t tx_msgs;       /* Messages transmitted */
    uint64_t rx_msgs;       /* Messages received */
    uint64_t tx_bytes;      /* Bytes transmitted */
    uint64_t rx_bytes;      /* Bytes received */
    uint64_t errors;        /* Total errors */
    uint64_t nak_errors;    /* NAK/ACK errors */
    uint64_t timeout_errors;/* Timeout errors */
    uint64_t clk_stretch_errors; /* Clock stretch timeout errors */
    uint64_t retries;       /* Retry attempts */
} i2c_stats_t;

/* ============================================================================
 * I2C Controller State
 * ============================================================================ */

/**
 * I2C controller state
 */
typedef enum i2c_state {
    I2C_STATE_IDLE = 0,     /* Controller idle */
    I2C_STATE_ACTIVE,       /* Transfer in progress */
    I2C_STATE_ERROR,        /* Error state */
    I2C_STATE_DISABLED,     /* Controller disabled */
} i2c_state_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define I2C_OK                  0       /* Success */
#define I2C_ERR_NOT_INIT       -1       /* Not initialized */
#define I2C_ERR_NOMEM          -2       /* Out of memory */
#define I2C_ERR_INVALID        -3       /* Invalid parameter */
#define I2C_ERR_TIMEOUT        -4       /* Operation timeout */
#define I2C_ERR_BUSY           -5       /* Controller busy */
#define I2C_ERR_NO_DEVICE      -6       /* No I2C controller found */
#define I2C_ERR_IO             -7       /* I/O error */
#define I2C_ERR_NAK            -8       /* NAK received (no ACK) */
#define I2C_ERR_CLKT           -9       /* Clock stretch timeout */
#define I2C_ERR_ADDR_INVALID   -10      /* Invalid address */
#define I2C_ERR_DATA_SIZE      -11      /* Invalid data size */

/* ============================================================================
 * Public API - Initialization
 * ============================================================================ */

/**
 * Initialize I2C controller
 * @param controller    Controller number (0-7 for BCM2712)
 * @param config        Configuration parameters (NULL for defaults)
 * @return I2C_OK on success, error code on failure
 */
int i2c_init(uint8_t controller, const i2c_config_t *config);

/**
 * Shutdown I2C controller
 * @param controller    Controller number (0-7)
 */
void i2c_shutdown(uint8_t controller);

/**
 * Check if I2C controller is initialized
 * @param controller    Controller number (0-7)
 * @return true if initialized and ready
 */
bool i2c_is_initialized(uint8_t controller);

/**
 * Get current I2C controller state
 * @param controller    Controller number (0-7)
 * @return Current controller state
 */
i2c_state_t i2c_get_state(uint8_t controller);

/* ============================================================================
 * Public API - Configuration
 * ============================================================================ */

/**
 * Set I2C bus speed
 * @param controller    Controller number (0-7)
 * @param speed         Bus speed in Hz (100k, 400k, 1M)
 * @return I2C_OK on success, error code on failure
 */
int i2c_set_speed(uint8_t controller, uint32_t speed);

/**
 * Get current I2C bus speed
 * @param controller    Controller number (0-7)
 * @return Bus speed in Hz, or 0 if not initialized
 */
uint32_t i2c_get_speed(uint8_t controller);

/**
 * Set I2C timeout
 * @param controller    Controller number (0-7)
 * @param timeout_ms    Timeout in milliseconds
 * @return I2C_OK on success, error code on failure
 */
int i2c_set_timeout(uint8_t controller, uint32_t timeout_ms);

/* ============================================================================
 * Public API - Data Transfer
 * ============================================================================ */

/**
 * Write data to I2C slave device
 * @param controller    Controller number (0-7)
 * @param addr          Slave address (7-bit or 10-bit)
 * @param buf           Data buffer to write
 * @param len           Number of bytes to write
 * @return Number of bytes written on success, negative error code on failure
 */
int i2c_write(uint8_t controller, uint16_t addr, const uint8_t *buf, uint16_t len);

/**
 * Read data from I2C slave device
 * @param controller    Controller number (0-7)
 * @param addr          Slave address (7-bit or 10-bit)
 * @param buf           Buffer to store read data
 * @param len           Number of bytes to read
 * @return Number of bytes read on success, negative error code on failure
 */
int i2c_read(uint8_t controller, uint16_t addr, uint8_t *buf, uint16_t len);

/**
 * Write then read from I2C slave (combined transaction)
 * Common pattern for reading registers: write register address, then read data
 * @param controller    Controller number (0-7)
 * @param addr          Slave address (7-bit or 10-bit)
 * @param wbuf          Data buffer to write
 * @param wlen          Number of bytes to write
 * @param rbuf          Buffer to store read data
 * @param rlen          Number of bytes to read
 * @return I2C_OK on success, error code on failure
 */
int i2c_write_read(uint8_t controller, uint16_t addr,
                   const uint8_t *wbuf, uint16_t wlen,
                   uint8_t *rbuf, uint16_t rlen);

/**
 * Transfer multiple I2C messages atomically
 * @param controller    Controller number (0-7)
 * @param msgs          Array of I2C messages
 * @param num_msgs      Number of messages in array
 * @return I2C_OK on success, error code on failure
 */
int i2c_transfer(uint8_t controller, i2c_msg_t *msgs, uint16_t num_msgs);

/* ============================================================================
 * Public API - Register Access Helpers
 * ============================================================================ */

/**
 * Write byte to device register
 * @param controller    Controller number (0-7)
 * @param addr          Slave address
 * @param reg           Register address
 * @param value         Value to write
 * @return I2C_OK on success, error code on failure
 */
int i2c_write_reg_byte(uint8_t controller, uint16_t addr, uint8_t reg, uint8_t value);

/**
 * Read byte from device register
 * @param controller    Controller number (0-7)
 * @param addr          Slave address
 * @param reg           Register address
 * @param value         Pointer to store read value
 * @return I2C_OK on success, error code on failure
 */
int i2c_read_reg_byte(uint8_t controller, uint16_t addr, uint8_t reg, uint8_t *value);

/**
 * Write word (16-bit) to device register
 * @param controller    Controller number (0-7)
 * @param addr          Slave address
 * @param reg           Register address
 * @param value         Value to write
 * @return I2C_OK on success, error code on failure
 */
int i2c_write_reg_word(uint8_t controller, uint16_t addr, uint8_t reg, uint16_t value);

/**
 * Read word (16-bit) from device register
 * @param controller    Controller number (0-7)
 * @param addr          Slave address
 * @param reg           Register address
 * @param value         Pointer to store read value
 * @return I2C_OK on success, error code on failure
 */
int i2c_read_reg_word(uint8_t controller, uint16_t addr, uint8_t reg, uint16_t *value);

/**
 * Write buffer to device register
 * @param controller    Controller number (0-7)
 * @param addr          Slave address
 * @param reg           Register address
 * @param buf           Data buffer to write
 * @param len           Number of bytes to write
 * @return I2C_OK on success, error code on failure
 */
int i2c_write_reg_buf(uint8_t controller, uint16_t addr, uint8_t reg,
                      const uint8_t *buf, uint16_t len);

/**
 * Read buffer from device register
 * @param controller    Controller number (0-7)
 * @param addr          Slave address
 * @param reg           Register address
 * @param buf           Buffer to store read data
 * @param len           Number of bytes to read
 * @return I2C_OK on success, error code on failure
 */
int i2c_read_reg_buf(uint8_t controller, uint16_t addr, uint8_t reg,
                     uint8_t *buf, uint16_t len);

/* ============================================================================
 * Public API - Device Detection
 * ============================================================================ */

/**
 * Scan I2C bus for devices
 * @param controller    Controller number (0-7)
 * @param devices       Array to store found device addresses
 * @param max_devices   Maximum number of devices to find
 * @return Number of devices found, negative error code on failure
 */
int i2c_scan(uint8_t controller, uint16_t *devices, uint16_t max_devices);

/**
 * Probe for device at specific address
 * @param controller    Controller number (0-7)
 * @param addr          Device address to probe
 * @return true if device responds, false otherwise
 */
bool i2c_probe_device(uint8_t controller, uint16_t addr);

/* ============================================================================
 * Public API - Statistics and Diagnostics
 * ============================================================================ */

/**
 * Get I2C controller statistics
 * @param controller    Controller number (0-7)
 * @param stats         Pointer to stats structure to fill
 * @return I2C_OK on success, error code on failure
 */
int i2c_get_stats(uint8_t controller, i2c_stats_t *stats);

/**
 * Reset I2C controller statistics
 * @param controller    Controller number (0-7)
 */
void i2c_reset_stats(uint8_t controller);

/**
 * Reset I2C controller (recovery from error state)
 * @param controller    Controller number (0-7)
 * @return I2C_OK on success, error code on failure
 */
int i2c_reset(uint8_t controller);

/* ============================================================================
 * Common I2C Device Addresses
 * ============================================================================ */

/* Common sensor I2C addresses (7-bit) */
#define I2C_ADDR_MPU6050        0x68    /* MPU6050 IMU (default) */
#define I2C_ADDR_MPU6050_ALT    0x69    /* MPU6050 IMU (alternate) */
#define I2C_ADDR_BMP280         0x76    /* BMP280 pressure/temp sensor */
#define I2C_ADDR_BMP280_ALT     0x77    /* BMP280 alternate */
#define I2C_ADDR_BME280         0x76    /* BME280 humidity/pressure/temp */
#define I2C_ADDR_BME280_ALT     0x77    /* BME280 alternate */
#define I2C_ADDR_ADS1115        0x48    /* ADS1115 ADC (default) */
#define I2C_ADDR_PCA9685        0x40    /* PCA9685 PWM driver (default) */
#define I2C_ADDR_MCP23017       0x20    /* MCP23017 I/O expander (default) */
#define I2C_ADDR_DS1307         0x68    /* DS1307 RTC */
#define I2C_ADDR_AT24C32        0x50    /* AT24C32 EEPROM (default) */

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_I2C_H */
