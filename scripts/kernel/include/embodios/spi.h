/* EMBODIOS SPI Driver Interface
 *
 * Serial Peripheral Interface (SPI) driver for BCM2712 (Raspberry Pi 5)
 * and compatible ARM64 platforms. Provides high-speed synchronous serial
 * communication for sensors, displays, and peripheral devices.
 *
 * Features:
 * - Full-duplex SPI communication (simultaneous TX/RX)
 * - Configurable clock speeds (up to 125 MHz)
 * - SPI modes 0-3 (CPOL/CPHA configuration)
 * - Multiple chip select lines (CE0, CE1)
 * - Configurable bit order (MSB/LSB first)
 * - DMA support for large transfers
 * - Polling and interrupt-driven operation
 */

#ifndef EMBODIOS_SPI_H
#define EMBODIOS_SPI_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * BCM2712 SPI Hardware Constants
 * ============================================================================ */

/* BCM2712 SPI0 Base Address (Raspberry Pi 5) */
#define BCM2712_SPI0_BASE       0x107D508000ULL  /* SPI0 register base */
#define BCM2712_SPI1_BASE       0x107D509000ULL  /* SPI1 register base (auxiliary) */

/* SPI Controller Count */
#define SPI_CONTROLLER_COUNT    2       /* SPI0 and SPI1 */
#define SPI_DEFAULT_CONTROLLER  0       /* SPI0 is primary */

/* SPI Chip Select Lines */
#define SPI_CS_COUNT            2       /* CE0 and CE1 */
#define SPI_CS0                 0       /* Chip select 0 */
#define SPI_CS1                 1       /* Chip select 1 */
#define SPI_CS_NONE             3       /* No chip select (manual control) */

/* ============================================================================
 * BCM2712 SPI Register Offsets
 * ============================================================================ */

/* Control and Status Register */
#define SPI_CS                  0x00    /* Control/Status register */

/* FIFO Data Register */
#define SPI_FIFO                0x04    /* TX/RX FIFO */

/* Clock Divider Register */
#define SPI_CLK                 0x08    /* Clock divider */

/* Data Length Register */
#define SPI_DLEN                0x0C    /* Data length (DMA mode) */

/* LTOH Register */
#define SPI_LTOH                0x10    /* LoSSI output hold delay */

/* DMA Control Register */
#define SPI_DC                  0x14    /* DMA DREQ controls */

/* ============================================================================
 * SPI Control/Status Register Bit Definitions (SPI_CS)
 * ============================================================================ */

/* CS Register Bits */
#define SPI_CS_LEN_LONG         (1 << 25)   /* Enable long data word */
#define SPI_CS_DMA_LEN          (1 << 24)   /* Enable DMA mode */
#define SPI_CS_CSPOL2           (1 << 23)   /* Chip select 2 polarity */
#define SPI_CS_CSPOL1           (1 << 22)   /* Chip select 1 polarity */
#define SPI_CS_CSPOL0           (1 << 21)   /* Chip select 0 polarity */
#define SPI_CS_RXF              (1 << 20)   /* RX FIFO full */
#define SPI_CS_RXR              (1 << 19)   /* RX FIFO needs reading */
#define SPI_CS_TXD              (1 << 18)   /* TX FIFO can accept data */
#define SPI_CS_RXD              (1 << 17)   /* RX FIFO contains data */
#define SPI_CS_DONE             (1 << 16)   /* Transfer done */
#define SPI_CS_LEN              (1 << 13)   /* LoSSI enable */
#define SPI_CS_REN              (1 << 12)   /* Read enable (bidirectional mode) */
#define SPI_CS_ADCS             (1 << 11)   /* Automatically deassert CS */
#define SPI_CS_INTR             (1 << 10)   /* Interrupt on RXR */
#define SPI_CS_INTD             (1 << 9)    /* Interrupt on DONE */
#define SPI_CS_DMAEN            (1 << 8)    /* DMA enable */
#define SPI_CS_TA               (1 << 7)    /* Transfer active */
#define SPI_CS_CSPOL            (1 << 6)    /* Chip select polarity */
#define SPI_CS_CLEAR_RX         (1 << 5)    /* Clear RX FIFO */
#define SPI_CS_CLEAR_TX         (1 << 4)    /* Clear TX FIFO */
#define SPI_CS_CPOL             (1 << 3)    /* Clock polarity */
#define SPI_CS_CPHA             (1 << 2)    /* Clock phase */
#define SPI_CS_CS_MASK          0x3         /* Chip select bits mask */

/* FIFO Sizes */
#define SPI_FIFO_SIZE           64      /* TX/RX FIFO depth (bytes) */
#define SPI_FIFO_THRESHOLD      32      /* FIFO threshold for DMA */

/* ============================================================================
 * SPI Clock Configuration
 * ============================================================================ */

/* BCM2712 Core Clock (reference for SPI clock divider) */
#define SPI_CORE_CLOCK_HZ       250000000   /* 250 MHz core clock */

/* Common SPI Clock Speeds */
#define SPI_CLOCK_125MHZ        125000000   /* 125 MHz (max speed) */
#define SPI_CLOCK_62_5MHZ       62500000    /* 62.5 MHz */
#define SPI_CLOCK_31_25MHZ      31250000    /* 31.25 MHz */
#define SPI_CLOCK_15_625MHZ     15625000    /* 15.625 MHz */
#define SPI_CLOCK_10MHZ         10000000    /* 10 MHz */
#define SPI_CLOCK_5MHZ          5000000     /* 5 MHz */
#define SPI_CLOCK_1MHZ          1000000     /* 1 MHz */
#define SPI_CLOCK_500KHZ        500000      /* 500 kHz */
#define SPI_CLOCK_100KHZ        100000      /* 100 kHz */

/* Default SPI Clock Speed */
#define SPI_CLOCK_DEFAULT       SPI_CLOCK_1MHZ

/* Clock Divider Limits */
#define SPI_CLK_MIN_DIVIDER     2       /* Minimum divider value */
#define SPI_CLK_MAX_DIVIDER     65536   /* Maximum divider value (16-bit) */

/* ============================================================================
 * SPI Transfer Modes
 * ============================================================================ */

/**
 * SPI mode configuration (CPOL/CPHA)
 * Defines clock polarity and phase for different device requirements
 */
typedef enum spi_mode {
    SPI_MODE_0 = 0,     /* CPOL=0, CPHA=0: Clock idle low, sample on leading edge */
    SPI_MODE_1 = 1,     /* CPOL=0, CPHA=1: Clock idle low, sample on trailing edge */
    SPI_MODE_2 = 2,     /* CPOL=1, CPHA=0: Clock idle high, sample on leading edge */
    SPI_MODE_3 = 3,     /* CPOL=1, CPHA=1: Clock idle high, sample on trailing edge */
} spi_mode_t;

/* ============================================================================
 * SPI Bit Order
 * ============================================================================ */

/**
 * SPI data bit order
 * Most devices use MSB first, but some require LSB first
 */
typedef enum spi_bit_order {
    SPI_BIT_ORDER_MSB_FIRST = 0,    /* Most significant bit first (standard) */
    SPI_BIT_ORDER_LSB_FIRST = 1,    /* Least significant bit first */
} spi_bit_order_t;

/* ============================================================================
 * SPI Configuration Structure
 * ============================================================================ */

/**
 * SPI controller configuration parameters
 */
typedef struct spi_config {
    uint8_t         controller;     /* SPI controller number (0 or 1) */
    uint8_t         chip_select;    /* Chip select line (0, 1, or SPI_CS_NONE) */
    uint32_t        clock_hz;       /* Clock speed in Hz */
    spi_mode_t      mode;           /* SPI mode (0-3) */
    spi_bit_order_t bit_order;      /* Bit transmission order */
    bool            cs_polarity;    /* Chip select active high (true) or low (false) */
    bool            use_dma;        /* Enable DMA for transfers */
} spi_config_t;

/* ============================================================================
 * SPI Transfer Structure
 * ============================================================================ */

/**
 * SPI transfer descriptor
 * Defines a single SPI transaction with TX and/or RX data
 */
typedef struct spi_transfer {
    const uint8_t  *tx_buf;         /* Transmit buffer (NULL for RX-only) */
    uint8_t        *rx_buf;         /* Receive buffer (NULL for TX-only) */
    uint32_t        len;            /* Transfer length in bytes */
    uint32_t        delay_usecs;    /* Delay after transfer (microseconds) */
    uint8_t         cs_change;      /* Deassert CS after transfer */
    uint8_t         bits_per_word;  /* Bits per word (8, 16, 32) - 8 is default */
    uint32_t        speed_hz;       /* Override clock speed for this transfer */
} spi_transfer_t;

/* ============================================================================
 * SPI Device Handle
 * ============================================================================ */

/**
 * SPI device handle
 * Opaque handle for configured SPI device
 */
typedef struct spi_device {
    uint8_t         controller;     /* SPI controller number */
    uint8_t         chip_select;    /* Associated chip select */
    uint32_t        clock_hz;       /* Current clock speed */
    spi_mode_t      mode;           /* Current SPI mode */
    bool            initialized;    /* Device initialized flag */
} spi_device_t;

/* ============================================================================
 * SPI Statistics
 * ============================================================================ */

/**
 * SPI subsystem statistics and performance counters
 */
typedef struct spi_stats {
    uint64_t transfers;         /* Total number of transfers */
    uint64_t tx_bytes;          /* Bytes transmitted */
    uint64_t rx_bytes;          /* Bytes received */
    uint64_t tx_errors;         /* Transmit errors */
    uint64_t rx_errors;         /* Receive errors */
    uint64_t fifo_overruns;     /* RX FIFO overrun events */
    uint64_t fifo_underruns;    /* TX FIFO underrun events */
    uint64_t timeouts;          /* Transfer timeout events */
    uint64_t dma_transfers;     /* Transfers using DMA */
} spi_stats_t;

/* ============================================================================
 * SPI Controller State
 * ============================================================================ */

/**
 * SPI controller state
 */
typedef enum spi_state {
    SPI_STATE_IDLE = 0,         /* Controller idle */
    SPI_STATE_BUSY,             /* Transfer in progress */
    SPI_STATE_ERROR,            /* Error state */
    SPI_STATE_DISABLED,         /* Controller disabled */
} spi_state_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define SPI_OK                  0       /* Success */
#define SPI_ERR_NOT_INIT       -1       /* Not initialized */
#define SPI_ERR_NOMEM          -2       /* Out of memory */
#define SPI_ERR_INVALID        -3       /* Invalid parameter */
#define SPI_ERR_TIMEOUT        -4       /* Transfer timeout */
#define SPI_ERR_BUSY           -5       /* Controller busy */
#define SPI_ERR_NO_DEVICE      -6       /* Invalid device/controller */
#define SPI_ERR_IO             -7       /* I/O error */
#define SPI_ERR_FIFO_OVERRUN   -8       /* RX FIFO overrun */
#define SPI_ERR_FIFO_UNDERRUN  -9       /* TX FIFO underrun */
#define SPI_ERR_INVALID_MODE   -10      /* Invalid SPI mode */
#define SPI_ERR_INVALID_CLOCK  -11      /* Invalid clock speed */

/* Transfer Timeouts */
#define SPI_TIMEOUT_DEFAULT     1000    /* Default timeout (ms) */
#define SPI_TIMEOUT_SHORT       100     /* Short timeout (ms) */
#define SPI_TIMEOUT_LONG        5000    /* Long timeout (ms) */

/* ============================================================================
 * Public API - Initialization
 * ============================================================================ */

/**
 * Initialize SPI subsystem
 * Initializes specified SPI controller with default configuration
 * @param controller SPI controller number (0 or 1)
 * @return SPI_OK on success, error code on failure
 */
int spi_init(uint8_t controller);

/**
 * Initialize SPI subsystem with custom configuration
 * @param config Configuration parameters
 * @return SPI_OK on success, error code on failure
 */
int spi_init_config(const spi_config_t *config);

/**
 * Shutdown SPI subsystem
 * Stops the controller and releases resources
 * @param controller SPI controller number (0 or 1)
 */
void spi_shutdown(uint8_t controller);

/**
 * Check if SPI subsystem is initialized
 * @param controller SPI controller number (0 or 1)
 * @return true if initialized and ready
 */
bool spi_is_initialized(uint8_t controller);

/**
 * Get current SPI controller state
 * @param controller SPI controller number (0 or 1)
 * @return Current controller state
 */
spi_state_t spi_get_state(uint8_t controller);

/* ============================================================================
 * Public API - Configuration
 * ============================================================================ */

/**
 * Set SPI clock frequency
 * @param controller SPI controller number (0 or 1)
 * @param clock_hz   Desired clock frequency in Hz
 * @return Actual clock frequency set, or negative error code
 */
int spi_set_clock(uint8_t controller, uint32_t clock_hz);

/**
 * Get current SPI clock frequency
 * @param controller SPI controller number (0 or 1)
 * @return Current clock frequency in Hz, or 0 on error
 */
uint32_t spi_get_clock(uint8_t controller);

/**
 * Set SPI mode (CPOL/CPHA)
 * @param controller SPI controller number (0 or 1)
 * @param mode       SPI mode (0-3)
 * @return SPI_OK on success, error code on failure
 */
int spi_set_mode(uint8_t controller, spi_mode_t mode);

/**
 * Get current SPI mode
 * @param controller SPI controller number (0 or 1)
 * @return Current SPI mode, or SPI_MODE_0 on error
 */
spi_mode_t spi_get_mode(uint8_t controller);

/**
 * Set SPI bit order (MSB/LSB first)
 * @param controller SPI controller number (0 or 1)
 * @param bit_order  Bit transmission order
 * @return SPI_OK on success, error code on failure
 */
int spi_set_bit_order(uint8_t controller, spi_bit_order_t bit_order);

/**
 * Set chip select line
 * @param controller SPI controller number (0 or 1)
 * @param cs         Chip select (0, 1, or SPI_CS_NONE)
 * @return SPI_OK on success, error code on failure
 */
int spi_set_cs(uint8_t controller, uint8_t cs);

/**
 * Set chip select polarity
 * @param controller SPI controller number (0 or 1)
 * @param active_high True for active-high, false for active-low
 * @return SPI_OK on success, error code on failure
 */
int spi_set_cs_polarity(uint8_t controller, bool active_high);

/* ============================================================================
 * Public API - Data Transfer
 * ============================================================================ */

/**
 * Perform SPI transfer (full-duplex)
 * Simultaneously transmits and receives data
 * @param controller SPI controller number (0 or 1)
 * @param tx_buf     Transmit buffer (NULL for RX-only)
 * @param rx_buf     Receive buffer (NULL for TX-only)
 * @param len        Transfer length in bytes
 * @return Number of bytes transferred, or negative error code
 */
int spi_transfer(uint8_t controller, const uint8_t *tx_buf, uint8_t *rx_buf, uint32_t len);

/**
 * Perform SPI transfer with detailed control
 * @param controller SPI controller number (0 or 1)
 * @param xfer       Transfer descriptor
 * @return Number of bytes transferred, or negative error code
 */
int spi_transfer_ex(uint8_t controller, const spi_transfer_t *xfer);

/**
 * Transmit data only (TX-only transfer)
 * @param controller SPI controller number (0 or 1)
 * @param tx_buf     Transmit buffer
 * @param len        Number of bytes to transmit
 * @return Number of bytes transmitted, or negative error code
 */
int spi_write(uint8_t controller, const uint8_t *tx_buf, uint32_t len);

/**
 * Receive data only (RX-only transfer)
 * @param controller SPI controller number (0 or 1)
 * @param rx_buf     Receive buffer
 * @param len        Number of bytes to receive
 * @return Number of bytes received, or negative error code
 */
int spi_read(uint8_t controller, uint8_t *rx_buf, uint32_t len);

/**
 * Transfer single byte (8-bit)
 * @param controller SPI controller number (0 or 1)
 * @param tx_byte    Byte to transmit
 * @return Received byte (0-255), or negative error code
 */
int spi_transfer_byte(uint8_t controller, uint8_t tx_byte);

/**
 * Transfer 16-bit word
 * @param controller SPI controller number (0 or 1)
 * @param tx_word    Word to transmit
 * @return Received word (0-65535), or negative error code
 */
int spi_transfer_word(uint8_t controller, uint16_t tx_word);

/* ============================================================================
 * Public API - FIFO Management
 * ============================================================================ */

/**
 * Clear TX FIFO
 * @param controller SPI controller number (0 or 1)
 * @return SPI_OK on success, error code on failure
 */
int spi_clear_tx_fifo(uint8_t controller);

/**
 * Clear RX FIFO
 * @param controller SPI controller number (0 or 1)
 * @return SPI_OK on success, error code on failure
 */
int spi_clear_rx_fifo(uint8_t controller);

/**
 * Check if TX FIFO is empty
 * @param controller SPI controller number (0 or 1)
 * @return true if TX FIFO is empty
 */
bool spi_tx_fifo_empty(uint8_t controller);

/**
 * Check if RX FIFO has data
 * @param controller SPI controller number (0 or 1)
 * @return true if RX FIFO contains data
 */
bool spi_rx_fifo_has_data(uint8_t controller);

/* ============================================================================
 * Public API - Statistics and Diagnostics
 * ============================================================================ */

/**
 * Get SPI subsystem statistics
 * @param controller SPI controller number (0 or 1)
 * @param stats      Pointer to statistics structure to fill
 * @return SPI_OK on success, error code on failure
 */
int spi_get_stats(uint8_t controller, spi_stats_t *stats);

/**
 * Reset SPI statistics counters
 * @param controller SPI controller number (0 or 1)
 */
void spi_reset_stats(uint8_t controller);

/**
 * Validate SPI controller number
 * @param controller Controller number to validate
 * @return true if controller number is valid
 */
bool spi_is_valid_controller(uint8_t controller);

/**
 * Validate chip select number
 * @param cs Chip select to validate
 * @return true if chip select is valid
 */
bool spi_is_valid_cs(uint8_t cs);

/* ============================================================================
 * Public API - Device Management (High-Level Interface)
 * ============================================================================ */

/**
 * Open SPI device with configuration
 * Returns a device handle for subsequent operations
 * @param config Configuration parameters
 * @param device Pointer to device handle to fill
 * @return SPI_OK on success, error code on failure
 */
int spi_open(const spi_config_t *config, spi_device_t *device);

/**
 * Close SPI device
 * @param device Device handle
 * @return SPI_OK on success, error code on failure
 */
int spi_close(spi_device_t *device);

/**
 * Transfer data using device handle
 * @param device Device handle
 * @param tx_buf Transmit buffer (NULL for RX-only)
 * @param rx_buf Receive buffer (NULL for TX-only)
 * @param len    Transfer length in bytes
 * @return Number of bytes transferred, or negative error code
 */
int spi_device_transfer(spi_device_t *device, const uint8_t *tx_buf, uint8_t *rx_buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_SPI_H */
