/* EMBODIOS GPIO Driver Interface
 *
 * General Purpose Input/Output (GPIO) driver for BCM2712 (Raspberry Pi 5)
 * and compatible ARM64 platforms. Provides digital I/O control for
 * robotics sensors, actuators, and peripheral interfacing.
 *
 * Features:
 * - Digital input/output control
 * - Configurable pull-up/pull-down resistors
 * - Alternative function mapping (SPI, I2C, UART pins)
 * - High-speed GPIO operations via direct register access
 * - 28 user-accessible GPIO pins on Raspberry Pi 5
 */

#ifndef EMBODIOS_GPIO_H
#define EMBODIOS_GPIO_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * BCM2712 GPIO Hardware Constants
 * ============================================================================ */

/* BCM2712 GPIO Base Address (Raspberry Pi 5) */
#define BCM2712_GPIO_BASE       0x107D517C00ULL  /* GPIO register base */
#define BCM2712_PADS_BASE       0x107D51BC00ULL  /* Pad control base */

/* GPIO Bank Configuration */
#define GPIO_PIN_COUNT          54      /* Total GPIO pins */
#define GPIO_USER_PIN_COUNT     28      /* User-accessible pins */
#define GPIO_BANK_SIZE          32      /* Pins per register bank */

/* GPIO Register Count */
#define GPIO_FSEL_REGS          6       /* Function select registers */
#define GPIO_SET_REGS           2       /* Output set registers */
#define GPIO_CLR_REGS           2       /* Output clear registers */
#define GPIO_LEV_REGS           2       /* Pin level registers */

/* ============================================================================
 * BCM2712 GPIO Register Offsets
 * ============================================================================ */

/* Function Select Registers (3 bits per pin, 10 pins per register) */
#define GPIO_FSEL0              0x00    /* GPIO 0-9 function select */
#define GPIO_FSEL1              0x04    /* GPIO 10-19 function select */
#define GPIO_FSEL2              0x08    /* GPIO 20-29 function select */
#define GPIO_FSEL3              0x0C    /* GPIO 30-39 function select */
#define GPIO_FSEL4              0x10    /* GPIO 40-49 function select */
#define GPIO_FSEL5              0x14    /* GPIO 50-53 function select */

/* Output Set Registers (write 1 to set pin high) */
#define GPIO_SET0               0x1C    /* Set GPIO 0-31 */
#define GPIO_SET1               0x20    /* Set GPIO 32-53 */

/* Output Clear Registers (write 1 to set pin low) */
#define GPIO_CLR0               0x28    /* Clear GPIO 0-31 */
#define GPIO_CLR1               0x2C    /* Clear GPIO 32-53 */

/* Pin Level Registers (read current pin state) */
#define GPIO_LEV0               0x34    /* Read GPIO 0-31 */
#define GPIO_LEV1               0x38    /* Read GPIO 32-53 */

/* Event Detect Status Registers */
#define GPIO_EDS0               0x40    /* Event detect GPIO 0-31 */
#define GPIO_EDS1               0x44    /* Event detect GPIO 32-53 */

/* Rising Edge Detect Enable */
#define GPIO_REN0               0x4C    /* Rising edge GPIO 0-31 */
#define GPIO_REN1               0x50    /* Rising edge GPIO 32-53 */

/* Falling Edge Detect Enable */
#define GPIO_FEN0               0x58    /* Falling edge GPIO 0-31 */
#define GPIO_FEN1               0x5C    /* Falling edge GPIO 32-53 */

/* High Detect Enable */
#define GPIO_HEN0               0x64    /* High detect GPIO 0-31 */
#define GPIO_HEN1               0x68    /* High detect GPIO 32-53 */

/* Low Detect Enable */
#define GPIO_LEN0               0x70    /* Low detect GPIO 0-31 */
#define GPIO_LEN1               0x74    /* Low detect GPIO 32-53 */

/* Asynchronous Rising Edge Detect */
#define GPIO_AREN0              0x7C    /* Async rising GPIO 0-31 */
#define GPIO_AREN1              0x80    /* Async rising GPIO 32-53 */

/* Asynchronous Falling Edge Detect */
#define GPIO_AFEN0              0x88    /* Async falling GPIO 0-31 */
#define GPIO_AFEN1              0x8C    /* Async falling GPIO 32-53 */

/* Pull-up/Pull-down Control (BCM2712 uses different mechanism than BCM2711) */
#define GPIO_PUP_PDN_CNTRL0     0xE4    /* Pull control GPIO 0-15 */
#define GPIO_PUP_PDN_CNTRL1     0xE8    /* Pull control GPIO 16-31 */
#define GPIO_PUP_PDN_CNTRL2     0xEC    /* Pull control GPIO 32-47 */
#define GPIO_PUP_PDN_CNTRL3     0xF0    /* Pull control GPIO 48-53 */

/* ============================================================================
 * GPIO Pin Modes
 * ============================================================================ */

/**
 * GPIO function select modes
 * Each pin can be configured for input, output, or alternative functions
 */
typedef enum gpio_mode {
    GPIO_MODE_INPUT  = 0,       /* Digital input */
    GPIO_MODE_OUTPUT = 1,       /* Digital output */
    GPIO_MODE_ALT0   = 4,       /* Alternative function 0 */
    GPIO_MODE_ALT1   = 5,       /* Alternative function 1 */
    GPIO_MODE_ALT2   = 6,       /* Alternative function 2 */
    GPIO_MODE_ALT3   = 7,       /* Alternative function 3 */
    GPIO_MODE_ALT4   = 3,       /* Alternative function 4 */
    GPIO_MODE_ALT5   = 2,       /* Alternative function 5 */
} gpio_mode_t;

/* ============================================================================
 * GPIO Pull-Up/Pull-Down Configuration
 * ============================================================================ */

/**
 * GPIO pull resistor configuration (BCM2712)
 * Controls internal pull-up/pull-down resistors
 */
typedef enum gpio_pull {
    GPIO_PULL_NONE = 0,         /* No pull resistor */
    GPIO_PULL_UP   = 1,         /* Pull-up resistor enabled */
    GPIO_PULL_DOWN = 2,         /* Pull-down resistor enabled */
} gpio_pull_t;

/* ============================================================================
 * GPIO Pin States
 * ============================================================================ */

/**
 * GPIO digital logic levels
 */
typedef enum gpio_value {
    GPIO_LOW  = 0,              /* Logic low (0V) */
    GPIO_HIGH = 1,              /* Logic high (3.3V) */
} gpio_value_t;

/* ============================================================================
 * GPIO Configuration Structure
 * ============================================================================ */

/**
 * GPIO pin configuration
 * Defines mode, pull resistor, and initial state for a pin
 */
typedef struct gpio_config {
    uint8_t      pin;           /* GPIO pin number (0-53) */
    gpio_mode_t  mode;          /* Pin function mode */
    gpio_pull_t  pull;          /* Pull resistor configuration */
    gpio_value_t initial_value; /* Initial output value (if output mode) */
} gpio_config_t;

/* ============================================================================
 * GPIO Statistics
 * ============================================================================ */

/**
 * GPIO subsystem statistics
 */
typedef struct gpio_stats {
    uint64_t reads;             /* Number of gpio_read() calls */
    uint64_t writes;            /* Number of gpio_write() calls */
    uint64_t mode_changes;      /* Number of gpio_set_mode() calls */
    uint64_t errors;            /* Error count */
} gpio_stats_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define GPIO_OK                 0       /* Success */
#define GPIO_ERR_NOT_INIT      -1       /* GPIO subsystem not initialized */
#define GPIO_ERR_INVALID_PIN   -2       /* Invalid pin number */
#define GPIO_ERR_INVALID_MODE  -3       /* Invalid mode parameter */
#define GPIO_ERR_INVALID_PULL  -4       /* Invalid pull parameter */
#define GPIO_ERR_INVALID_VALUE -5       /* Invalid value parameter */
#define GPIO_ERR_HW_FAULT      -6       /* Hardware access fault */
#define GPIO_ERR_BUSY          -7       /* Pin in use by another subsystem */

/* ============================================================================
 * Public API - Initialization
 * ============================================================================ */

/**
 * Initialize GPIO subsystem
 * Maps BCM2712 GPIO registers and prepares for I/O operations
 * @return GPIO_OK on success, error code on failure
 */
int gpio_init(void);

/**
 * Shutdown GPIO subsystem
 * Releases resources and resets all pins to safe state
 */
void gpio_shutdown(void);

/**
 * Check if GPIO subsystem is initialized
 * @return true if initialized and ready
 */
bool gpio_is_initialized(void);

/* ============================================================================
 * Public API - Pin Configuration
 * ============================================================================ */

/**
 * Set GPIO pin mode (input, output, or alternative function)
 * @param pin  GPIO pin number (0-53)
 * @param mode Pin mode (GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_MODE_ALTx)
 * @return GPIO_OK on success, error code on failure
 */
int gpio_set_mode(uint8_t pin, gpio_mode_t mode);

/**
 * Get current GPIO pin mode
 * @param pin  GPIO pin number (0-53)
 * @return Current pin mode, or GPIO_MODE_INPUT on error
 */
gpio_mode_t gpio_get_mode(uint8_t pin);

/**
 * Configure GPIO pin pull-up/pull-down resistor
 * @param pin  GPIO pin number (0-53)
 * @param pull Pull resistor configuration
 * @return GPIO_OK on success, error code on failure
 */
int gpio_set_pull(uint8_t pin, gpio_pull_t pull);

/**
 * Configure GPIO pin with all parameters
 * @param config Pin configuration structure
 * @return GPIO_OK on success, error code on failure
 */
int gpio_configure(const gpio_config_t *config);

/* ============================================================================
 * Public API - Digital I/O
 * ============================================================================ */

/**
 * Write digital value to GPIO output pin
 * @param pin   GPIO pin number (0-53)
 * @param value GPIO_HIGH or GPIO_LOW
 * @return GPIO_OK on success, error code on failure
 */
int gpio_write(uint8_t pin, gpio_value_t value);

/**
 * Read digital value from GPIO input pin
 * @param pin GPIO pin number (0-53)
 * @return GPIO_HIGH, GPIO_LOW, or negative error code
 */
int gpio_read(uint8_t pin);

/**
 * Toggle GPIO output pin (flip between high and low)
 * @param pin GPIO pin number (0-53)
 * @return GPIO_OK on success, error code on failure
 */
int gpio_toggle(uint8_t pin);

/* ============================================================================
 * Public API - Multi-Pin Operations
 * ============================================================================ */

/**
 * Write to multiple GPIO pins simultaneously (bank 0: GPIO 0-31)
 * @param mask Bitmask of pins to modify (1 = modify, 0 = ignore)
 * @param value Bitmask of values (1 = high, 0 = low)
 * @return GPIO_OK on success, error code on failure
 */
int gpio_write_bank0(uint32_t mask, uint32_t value);

/**
 * Write to multiple GPIO pins simultaneously (bank 1: GPIO 32-53)
 * @param mask Bitmask of pins to modify (1 = modify, 0 = ignore)
 * @param value Bitmask of values (1 = high, 0 = low)
 * @return GPIO_OK on success, error code on failure
 */
int gpio_write_bank1(uint32_t mask, uint32_t value);

/**
 * Read all GPIO pins in bank 0 (GPIO 0-31)
 * @return 32-bit value representing pin states, or 0 on error
 */
uint32_t gpio_read_bank0(void);

/**
 * Read all GPIO pins in bank 1 (GPIO 32-53)
 * @return 32-bit value representing pin states (bits 0-21 valid), or 0 on error
 */
uint32_t gpio_read_bank1(void);

/* ============================================================================
 * Public API - Statistics and Diagnostics
 * ============================================================================ */

/**
 * Get GPIO subsystem statistics
 * @param stats Pointer to statistics structure to fill
 * @return GPIO_OK on success, error code on failure
 */
int gpio_get_stats(gpio_stats_t *stats);

/**
 * Reset GPIO statistics counters
 */
void gpio_reset_stats(void);

/**
 * Validate GPIO pin number
 * @param pin Pin number to validate
 * @return true if pin number is valid (0-53)
 */
bool gpio_is_valid_pin(uint8_t pin);

/* ============================================================================
 * Public API - Alternative Function Mapping
 * ============================================================================ */

/**
 * Configure GPIO pins for SPI0 alternative function
 * Maps MISO, MOSI, SCLK, CE0, CE1 to appropriate GPIO pins
 * @return GPIO_OK on success, error code on failure
 */
int gpio_setup_spi0(void);

/**
 * Configure GPIO pins for I2C1 alternative function
 * Maps SDA and SCL to appropriate GPIO pins
 * @return GPIO_OK on success, error code on failure
 */
int gpio_setup_i2c1(void);

/**
 * Configure GPIO pins for UART0 alternative function
 * Maps TX and RX to appropriate GPIO pins
 * @return GPIO_OK on success, error code on failure
 */
int gpio_setup_uart0(void);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_GPIO_H */
