/* EMBODIOS BCM GPIO Driver Implementation
 *
 * General Purpose Input/Output (GPIO) driver for BCM2712 (Raspberry Pi 5)
 * and compatible ARM64 platforms. Provides digital I/O control for
 * robotics sensors, actuators, and peripheral interfacing.
 */

#include <embodios/gpio.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <arch/aarch64/io.h>

/* Debug output (uncomment to enable) */
/* #define GPIO_DEBUG 1 */

/* ============================================================================
 * Module State
 * ============================================================================ */

/**
 * GPIO device state structure
 * Maintains controller state and statistics
 */
typedef struct gpio_dev {
    volatile void *gpio_base;   /* GPIO register base address */
    volatile void *pads_base;   /* Pad control register base */
    gpio_stats_t stats;         /* Statistics counters */
    bool initialized;           /* Initialization flag */
} gpio_dev_t;

/* Global GPIO device instance */
static gpio_dev_t g_gpio = {0};

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Validate GPIO pin number
 */
static inline bool validate_pin(uint8_t pin)
{
    return (pin < GPIO_PIN_COUNT);
}

/**
 * Get function select register address for pin
 */
static inline volatile uint32_t *get_fsel_reg(uint8_t pin)
{
    uint32_t reg_index = pin / 10;
    uint32_t offset = GPIO_FSEL0 + (reg_index * 4);
    return (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + offset);
}

/**
 * Get bit offset within function select register for pin
 */
static inline uint32_t get_fsel_shift(uint8_t pin)
{
    return (pin % 10) * 3;  /* 3 bits per pin */
}

/**
 * Get SET register address for pin
 */
static inline volatile uint32_t *get_set_reg(uint8_t pin)
{
    uint32_t offset = (pin < 32) ? GPIO_SET0 : GPIO_SET1;
    return (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + offset);
}

/**
 * Get CLR register address for pin
 */
static inline volatile uint32_t *get_clr_reg(uint8_t pin)
{
    uint32_t offset = (pin < 32) ? GPIO_CLR0 : GPIO_CLR1;
    return (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + offset);
}

/**
 * Get LEV register address for pin
 */
static inline volatile uint32_t *get_lev_reg(uint8_t pin)
{
    uint32_t offset = (pin < 32) ? GPIO_LEV0 : GPIO_LEV1;
    return (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + offset);
}

/**
 * Get bit mask for pin within its register
 */
static inline uint32_t get_pin_mask(uint8_t pin)
{
    return (1U << (pin % 32));
}

/**
 * Get pull-up/pull-down control register for pin
 */
static inline volatile uint32_t *get_pull_reg(uint8_t pin)
{
    uint32_t reg_index = pin / 16;
    uint32_t offset = GPIO_PUP_PDN_CNTRL0 + (reg_index * 4);
    return (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + offset);
}

/**
 * Get bit offset within pull control register for pin
 */
static inline uint32_t get_pull_shift(uint8_t pin)
{
    return (pin % 16) * 2;  /* 2 bits per pin */
}

/* ============================================================================
 * Initialization and Configuration
 * ============================================================================ */

/**
 * Initialize GPIO subsystem
 */
int gpio_init(void)
{
    console_printf("[GPIO] Initializing BCM2712 GPIO driver...\n");

    /* Initialize device structure */
    memset(&g_gpio, 0, sizeof(gpio_dev_t));

    /* Map GPIO register base address */
    g_gpio.gpio_base = (volatile void *)BCM2712_GPIO_BASE;
    g_gpio.pads_base = (volatile void *)BCM2712_PADS_BASE;

    /* Initialize statistics */
    g_gpio.stats.reads = 0;
    g_gpio.stats.writes = 0;
    g_gpio.stats.mode_changes = 0;
    g_gpio.stats.errors = 0;

    g_gpio.initialized = true;

    console_printf("[GPIO] Driver initialized successfully\n");
    console_printf("[GPIO] GPIO base: 0x%llX, Pins: %d\n",
                   BCM2712_GPIO_BASE, GPIO_PIN_COUNT);
    console_printf("[GPIO] User-accessible pins: %d\n", GPIO_USER_PIN_COUNT);

    return GPIO_OK;
}

/**
 * Shutdown GPIO subsystem
 */
void gpio_shutdown(void)
{
    if (!g_gpio.initialized) {
        return;
    }

    console_printf("[GPIO] Shutting down GPIO driver...\n");
    console_printf("[GPIO] Statistics: reads=%llu, writes=%llu, mode_changes=%llu, errors=%llu\n",
                   g_gpio.stats.reads, g_gpio.stats.writes,
                   g_gpio.stats.mode_changes, g_gpio.stats.errors);

    /* Reset all pins to input mode (safe state) */
    for (uint8_t pin = 0; pin < GPIO_PIN_COUNT; pin++) {
        gpio_set_mode(pin, GPIO_MODE_INPUT);
    }

    g_gpio.initialized = false;
    console_printf("[GPIO] Driver shutdown complete\n");
}

/**
 * Check if GPIO subsystem is initialized
 */
bool gpio_is_initialized(void)
{
    return g_gpio.initialized;
}

/* ============================================================================
 * Pin Configuration
 * ============================================================================ */

/**
 * Set GPIO pin mode
 */
int gpio_set_mode(uint8_t pin, gpio_mode_t mode)
{
    if (!g_gpio.initialized) {
        g_gpio.stats.errors++;
        return GPIO_ERR_NOT_INIT;
    }

    if (!validate_pin(pin)) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_PIN;
    }

    if (mode > 7) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_MODE;
    }

    volatile uint32_t *fsel_reg = get_fsel_reg(pin);
    uint32_t shift = get_fsel_shift(pin);

    /* Read current value */
    uint32_t value = mmio_read32(fsel_reg);

    /* Clear the 3 bits for this pin */
    value &= ~(7U << shift);

    /* Set new mode */
    value |= ((uint32_t)mode << shift);

    /* Write back */
    mmio_write32(fsel_reg, value);

    g_gpio.stats.mode_changes++;

#ifdef GPIO_DEBUG
    console_printf("[GPIO] Pin %d set to mode %d\n", pin, mode);
#endif

    return GPIO_OK;
}

/**
 * Get current GPIO pin mode
 */
gpio_mode_t gpio_get_mode(uint8_t pin)
{
    if (!g_gpio.initialized || !validate_pin(pin)) {
        g_gpio.stats.errors++;
        return GPIO_MODE_INPUT;
    }

    volatile uint32_t *fsel_reg = get_fsel_reg(pin);
    uint32_t shift = get_fsel_shift(pin);

    uint32_t value = mmio_read32(fsel_reg);
    return (gpio_mode_t)((value >> shift) & 0x7);
}

/**
 * Configure GPIO pin pull-up/pull-down resistor
 */
int gpio_set_pull(uint8_t pin, gpio_pull_t pull)
{
    if (!g_gpio.initialized) {
        g_gpio.stats.errors++;
        return GPIO_ERR_NOT_INIT;
    }

    if (!validate_pin(pin)) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_PIN;
    }

    if (pull > GPIO_PULL_DOWN) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_PULL;
    }

    volatile uint32_t *pull_reg = get_pull_reg(pin);
    uint32_t shift = get_pull_shift(pin);

    /* Read current value */
    uint32_t value = mmio_read32(pull_reg);

    /* Clear the 2 bits for this pin */
    value &= ~(3U << shift);

    /* Set new pull configuration */
    value |= ((uint32_t)pull << shift);

    /* Write back */
    mmio_write32(pull_reg, value);

#ifdef GPIO_DEBUG
    console_printf("[GPIO] Pin %d pull set to %d\n", pin, pull);
#endif

    return GPIO_OK;
}

/**
 * Configure GPIO pin with all parameters
 */
int gpio_configure(const gpio_config_t *config)
{
    int ret;

    if (!config) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_PIN;
    }

    /* Set pull resistor first */
    ret = gpio_set_pull(config->pin, config->pull);
    if (ret != GPIO_OK) {
        return ret;
    }

    /* Set pin mode */
    ret = gpio_set_mode(config->pin, config->mode);
    if (ret != GPIO_OK) {
        return ret;
    }

    /* If output mode, set initial value */
    if (config->mode == GPIO_MODE_OUTPUT) {
        ret = gpio_write(config->pin, config->initial_value);
        if (ret != GPIO_OK) {
            return ret;
        }
    }

    return GPIO_OK;
}

/* ============================================================================
 * Digital I/O Operations
 * ============================================================================ */

/**
 * Write digital value to GPIO output pin
 */
int gpio_write(uint8_t pin, gpio_value_t value)
{
    if (!g_gpio.initialized) {
        g_gpio.stats.errors++;
        return GPIO_ERR_NOT_INIT;
    }

    if (!validate_pin(pin)) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_PIN;
    }

    if (value != GPIO_LOW && value != GPIO_HIGH) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_VALUE;
    }

    uint32_t mask = get_pin_mask(pin);

    if (value == GPIO_HIGH) {
        /* Set pin high */
        volatile uint32_t *set_reg = get_set_reg(pin);
        mmio_write32(set_reg, mask);
    } else {
        /* Set pin low */
        volatile uint32_t *clr_reg = get_clr_reg(pin);
        mmio_write32(clr_reg, mask);
    }

    g_gpio.stats.writes++;

#ifdef GPIO_DEBUG
    console_printf("[GPIO] Pin %d written to %d\n", pin, value);
#endif

    return GPIO_OK;
}

/**
 * Read digital value from GPIO input pin
 */
int gpio_read(uint8_t pin)
{
    if (!g_gpio.initialized) {
        g_gpio.stats.errors++;
        return GPIO_ERR_NOT_INIT;
    }

    if (!validate_pin(pin)) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_PIN;
    }

    volatile uint32_t *lev_reg = get_lev_reg(pin);
    uint32_t mask = get_pin_mask(pin);

    uint32_t value = mmio_read32(lev_reg);
    g_gpio.stats.reads++;

    return (value & mask) ? GPIO_HIGH : GPIO_LOW;
}

/**
 * Toggle GPIO output pin
 */
int gpio_toggle(uint8_t pin)
{
    int current_value = gpio_read(pin);
    if (current_value < 0) {
        return current_value;  /* Return error */
    }

    gpio_value_t new_value = (current_value == GPIO_HIGH) ? GPIO_LOW : GPIO_HIGH;
    return gpio_write(pin, new_value);
}

/* ============================================================================
 * Multi-Pin Operations
 * ============================================================================ */

/**
 * Write to multiple GPIO pins simultaneously (bank 0: GPIO 0-31)
 */
int gpio_write_bank0(uint32_t mask, uint32_t value)
{
    if (!g_gpio.initialized) {
        g_gpio.stats.errors++;
        return GPIO_ERR_NOT_INIT;
    }

    volatile uint32_t *set_reg = (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + GPIO_SET0);
    volatile uint32_t *clr_reg = (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + GPIO_CLR0);

    /* Set high pins */
    mmio_write32(set_reg, mask & value);

    /* Clear low pins */
    mmio_write32(clr_reg, mask & ~value);

    g_gpio.stats.writes++;

    return GPIO_OK;
}

/**
 * Write to multiple GPIO pins simultaneously (bank 1: GPIO 32-53)
 */
int gpio_write_bank1(uint32_t mask, uint32_t value)
{
    if (!g_gpio.initialized) {
        g_gpio.stats.errors++;
        return GPIO_ERR_NOT_INIT;
    }

    /* Ensure we only use valid bits (0-21 for pins 32-53) */
    mask &= 0x003FFFFF;
    value &= 0x003FFFFF;

    volatile uint32_t *set_reg = (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + GPIO_SET1);
    volatile uint32_t *clr_reg = (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + GPIO_CLR1);

    /* Set high pins */
    mmio_write32(set_reg, mask & value);

    /* Clear low pins */
    mmio_write32(clr_reg, mask & ~value);

    g_gpio.stats.writes++;

    return GPIO_OK;
}

/**
 * Read all GPIO pins in bank 0 (GPIO 0-31)
 */
uint32_t gpio_read_bank0(void)
{
    if (!g_gpio.initialized) {
        g_gpio.stats.errors++;
        return 0;
    }

    volatile uint32_t *lev_reg = (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + GPIO_LEV0);
    g_gpio.stats.reads++;

    return mmio_read32(lev_reg);
}

/**
 * Read all GPIO pins in bank 1 (GPIO 32-53)
 */
uint32_t gpio_read_bank1(void)
{
    if (!g_gpio.initialized) {
        g_gpio.stats.errors++;
        return 0;
    }

    volatile uint32_t *lev_reg = (volatile uint32_t *)((uintptr_t)g_gpio.gpio_base + GPIO_LEV1);
    g_gpio.stats.reads++;

    return mmio_read32(lev_reg) & 0x003FFFFF;  /* Only bits 0-21 valid */
}

/* ============================================================================
 * Statistics and Diagnostics
 * ============================================================================ */

/**
 * Get GPIO subsystem statistics
 */
int gpio_get_stats(gpio_stats_t *stats)
{
    if (!stats) {
        g_gpio.stats.errors++;
        return GPIO_ERR_INVALID_PIN;
    }

    if (!g_gpio.initialized) {
        return GPIO_ERR_NOT_INIT;
    }

    *stats = g_gpio.stats;
    return GPIO_OK;
}

/**
 * Reset GPIO statistics counters
 */
void gpio_reset_stats(void)
{
    g_gpio.stats.reads = 0;
    g_gpio.stats.writes = 0;
    g_gpio.stats.mode_changes = 0;
    g_gpio.stats.errors = 0;
}

/**
 * Validate GPIO pin number
 */
bool gpio_is_valid_pin(uint8_t pin)
{
    return validate_pin(pin);
}

/* ============================================================================
 * Alternative Function Mapping
 * ============================================================================ */

/**
 * Configure GPIO pins for SPI0 alternative function
 * BCM2712: MISO=GPIO9, MOSI=GPIO10, SCLK=GPIO11, CE0=GPIO8, CE1=GPIO7
 */
int gpio_setup_spi0(void)
{
    int ret;

    console_printf("[GPIO] Configuring SPI0 pins...\n");

    /* SPI0 pins use ALT0 function */
    ret = gpio_set_mode(7, GPIO_MODE_ALT0);   /* CE1 */
    if (ret != GPIO_OK) return ret;

    ret = gpio_set_mode(8, GPIO_MODE_ALT0);   /* CE0 */
    if (ret != GPIO_OK) return ret;

    ret = gpio_set_mode(9, GPIO_MODE_ALT0);   /* MISO */
    if (ret != GPIO_OK) return ret;

    ret = gpio_set_mode(10, GPIO_MODE_ALT0);  /* MOSI */
    if (ret != GPIO_OK) return ret;

    ret = gpio_set_mode(11, GPIO_MODE_ALT0);  /* SCLK */
    if (ret != GPIO_OK) return ret;

    console_printf("[GPIO] SPI0 configured (GPIO 7-11)\n");
    return GPIO_OK;
}

/**
 * Configure GPIO pins for I2C1 alternative function
 * BCM2712: SDA=GPIO2, SCL=GPIO3
 */
int gpio_setup_i2c1(void)
{
    int ret;

    console_printf("[GPIO] Configuring I2C1 pins...\n");

    /* I2C1 pins use ALT0 function */
    ret = gpio_set_mode(2, GPIO_MODE_ALT0);   /* SDA */
    if (ret != GPIO_OK) return ret;

    ret = gpio_set_mode(3, GPIO_MODE_ALT0);   /* SCL */
    if (ret != GPIO_OK) return ret;

    console_printf("[GPIO] I2C1 configured (GPIO 2-3)\n");
    return GPIO_OK;
}

/**
 * Configure GPIO pins for UART0 alternative function
 * BCM2712: TX=GPIO14, RX=GPIO15
 */
int gpio_setup_uart0(void)
{
    int ret;

    console_printf("[GPIO] Configuring UART0 pins...\n");

    /* UART0 pins use ALT0 function */
    ret = gpio_set_mode(14, GPIO_MODE_ALT0);  /* TX */
    if (ret != GPIO_OK) return ret;

    ret = gpio_set_mode(15, GPIO_MODE_ALT0);  /* RX */
    if (ret != GPIO_OK) return ret;

    console_printf("[GPIO] UART0 configured (GPIO 14-15)\n");
    return GPIO_OK;
}
