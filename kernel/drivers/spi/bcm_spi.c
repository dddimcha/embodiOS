/* EMBODIOS BCM SPI Driver Implementation
 *
 * Serial Peripheral Interface (SPI) driver for BCM2712 (Raspberry Pi 5)
 * and compatible ARM64 platforms. Provides high-speed synchronous serial
 * communication for sensors, displays, and peripheral devices.
 */

#include <embodios/spi.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <arch/aarch64/io.h>

/* Debug output (uncomment to enable) */
/* #define SPI_DEBUG 1 */

/* ============================================================================
 * Module State
 * ============================================================================ */

/**
 * SPI controller state structure
 * Maintains controller state, configuration, and statistics
 */
typedef struct spi_controller {
    volatile void *base;        /* Register base address */
    spi_config_t config;        /* Current configuration */
    spi_state_t state;          /* Controller state */
    spi_stats_t stats;          /* Statistics counters */
    bool initialized;           /* Initialization flag */
} spi_controller_t;

/* Global SPI controller instances */
static spi_controller_t g_spi[SPI_CONTROLLER_COUNT] = {0};

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Calculate clock divider for desired frequency
 */
static uint16_t calculate_clock_divider(uint32_t desired_hz)
{
    uint32_t divider;

    if (desired_hz == 0) {
        return SPI_CLK_MAX_DIVIDER;
    }

    divider = SPI_CORE_CLOCK_HZ / desired_hz;

    /* Ensure even divider */
    if (divider & 1) {
        divider++;
    }

    /* Clamp to valid range */
    if (divider < SPI_CLK_MIN_DIVIDER) {
        divider = SPI_CLK_MIN_DIVIDER;
    } else if (divider > SPI_CLK_MAX_DIVIDER) {
        divider = SPI_CLK_MAX_DIVIDER;
    }

    return (uint16_t)divider;
}

/**
 * Get actual clock frequency for divider
 */
static uint32_t get_actual_clock(uint16_t divider)
{
    if (divider < SPI_CLK_MIN_DIVIDER) {
        divider = SPI_CLK_MIN_DIVIDER;
    }
    return SPI_CORE_CLOCK_HZ / divider;
}

/**
 * Wait for transfer to complete
 */
static int wait_transfer_done(spi_controller_t *ctrl, uint32_t timeout_ms)
{
    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t start_time = 0;  /* TODO: Get actual system time */
    uint32_t elapsed = 0;

    /* Poll DONE bit */
    while (elapsed < timeout_ms) {
        uint32_t cs = mmio_read32(cs_reg);
        if (cs & SPI_CS_DONE) {
            return SPI_OK;
        }
        /* TODO: Add proper delay/yield */
        elapsed++;
    }

    ctrl->stats.timeouts++;
    return SPI_ERR_TIMEOUT;
}

/**
 * Check if TX FIFO can accept data
 */
static bool tx_fifo_ready(spi_controller_t *ctrl)
{
    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs = mmio_read32(cs_reg);
    return (cs & SPI_CS_TXD) != 0;
}

/**
 * Check if RX FIFO has data
 */
static bool rx_fifo_has_data(spi_controller_t *ctrl)
{
    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs = mmio_read32(cs_reg);
    return (cs & SPI_CS_RXD) != 0;
}

/**
 * Clear FIFOs
 */
static void clear_fifos(spi_controller_t *ctrl)
{
    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs = mmio_read32(cs_reg);
    cs |= SPI_CS_CLEAR_TX | SPI_CS_CLEAR_RX;
    mmio_write32(cs_reg, cs);
}

/* ============================================================================
 * Initialization and Configuration
 * ============================================================================ */

/**
 * Initialize SPI subsystem
 */
int spi_init(uint8_t controller)
{
    spi_config_t config = {
        .controller = controller,
        .chip_select = SPI_CS0,
        .clock_hz = SPI_CLOCK_DEFAULT,
        .mode = SPI_MODE_0,
        .bit_order = SPI_BIT_ORDER_MSB_FIRST,
        .cs_polarity = false,
        .use_dma = false,
    };

    return spi_init_config(&config);
}

/**
 * Initialize SPI subsystem with custom configuration
 */
int spi_init_config(const spi_config_t *config)
{
    if (!config) {
        return SPI_ERR_INVALID;
    }

    if (config->controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[config->controller];

    console_printf("[SPI%d] Initializing BCM SPI driver...\n", config->controller);

    /* Initialize device structure */
    memset(ctrl, 0, sizeof(spi_controller_t));

    /* Map register base address */
    if (config->controller == 0) {
        ctrl->base = (volatile void *)BCM2712_SPI0_BASE;
    } else {
        ctrl->base = (volatile void *)BCM2712_SPI1_BASE;
    }

    /* Store configuration */
    ctrl->config = *config;

    /* Clear FIFOs */
    clear_fifos(ctrl);

    /* Configure control register */
    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs = 0;

    /* Set chip select */
    cs |= (config->chip_select & SPI_CS_CS_MASK);

    /* Set SPI mode (CPOL/CPHA) */
    if (config->mode & 0x1) {
        cs |= SPI_CS_CPHA;
    }
    if (config->mode & 0x2) {
        cs |= SPI_CS_CPOL;
    }

    /* Set chip select polarity */
    if (config->cs_polarity) {
        cs |= SPI_CS_CSPOL;
    }

    mmio_write32(cs_reg, cs);

    /* Set clock speed */
    uint16_t divider = calculate_clock_divider(config->clock_hz);
    volatile uint32_t *clk_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CLK);
    mmio_write32(clk_reg, divider);

    /* Update actual clock in config */
    ctrl->config.clock_hz = get_actual_clock(divider);

    /* Initialize statistics */
    ctrl->stats.transfers = 0;
    ctrl->stats.tx_bytes = 0;
    ctrl->stats.rx_bytes = 0;
    ctrl->stats.tx_errors = 0;
    ctrl->stats.rx_errors = 0;
    ctrl->stats.fifo_overruns = 0;
    ctrl->stats.fifo_underruns = 0;
    ctrl->stats.timeouts = 0;
    ctrl->stats.dma_transfers = 0;

    /* Set initial state */
    ctrl->state = SPI_STATE_IDLE;
    ctrl->initialized = true;

    console_printf("[SPI%d] Driver initialized successfully\n", config->controller);
    console_printf("[SPI%d] Base: 0x%llX, Clock: %u Hz, Mode: %d\n",
                   config->controller,
                   (config->controller == 0) ? BCM2712_SPI0_BASE : BCM2712_SPI1_BASE,
                   ctrl->config.clock_hz,
                   config->mode);
    console_printf("[SPI%d] CS: %d, FIFO: %d bytes\n",
                   config->controller,
                   config->chip_select,
                   SPI_FIFO_SIZE);

    return SPI_OK;
}

/**
 * Shutdown SPI subsystem
 */
void spi_shutdown(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return;
    }

    console_printf("[SPI%d] Shutting down SPI driver...\n", controller);
    console_printf("[SPI%d] Statistics: transfers=%llu, tx_bytes=%llu, rx_bytes=%llu\n",
                   controller,
                   ctrl->stats.transfers,
                   ctrl->stats.tx_bytes,
                   ctrl->stats.rx_bytes);
    console_printf("[SPI%d] Errors: tx=%llu, rx=%llu, timeouts=%llu\n",
                   controller,
                   ctrl->stats.tx_errors,
                   ctrl->stats.rx_errors,
                   ctrl->stats.timeouts);

    /* Clear FIFOs */
    clear_fifos(ctrl);

    /* Disable controller */
    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    mmio_write32(cs_reg, 0);

    ctrl->initialized = false;
    ctrl->state = SPI_STATE_DISABLED;

    console_printf("[SPI%d] Driver shutdown complete\n", controller);
}

/**
 * Check if SPI subsystem is initialized
 */
bool spi_is_initialized(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return false;
    }
    return g_spi[controller].initialized;
}

/**
 * Get current SPI controller state
 */
spi_state_t spi_get_state(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_STATE_DISABLED;
    }
    return g_spi[controller].state;
}

/* ============================================================================
 * Configuration Functions
 * ============================================================================ */

/**
 * Set SPI clock frequency
 */
int spi_set_clock(uint8_t controller, uint32_t clock_hz)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    if (clock_hz == 0 || clock_hz > SPI_CLOCK_125MHZ) {
        return SPI_ERR_INVALID_CLOCK;
    }

    uint16_t divider = calculate_clock_divider(clock_hz);
    volatile uint32_t *clk_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CLK);
    mmio_write32(clk_reg, divider);

    /* Update config with actual clock */
    ctrl->config.clock_hz = get_actual_clock(divider);

#ifdef SPI_DEBUG
    console_printf("[SPI%d] Clock set to %u Hz (divider: %u)\n",
                   controller, ctrl->config.clock_hz, divider);
#endif

    return (int)ctrl->config.clock_hz;
}

/**
 * Get current SPI clock frequency
 */
uint32_t spi_get_clock(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return 0;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return 0;
    }

    return ctrl->config.clock_hz;
}

/**
 * Set SPI mode (CPOL/CPHA)
 */
int spi_set_mode(uint8_t controller, spi_mode_t mode)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    if (mode > SPI_MODE_3) {
        return SPI_ERR_INVALID_MODE;
    }

    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs = mmio_read32(cs_reg);

    /* Clear CPOL and CPHA bits */
    cs &= ~(SPI_CS_CPOL | SPI_CS_CPHA);

    /* Set new mode */
    if (mode & 0x1) {
        cs |= SPI_CS_CPHA;
    }
    if (mode & 0x2) {
        cs |= SPI_CS_CPOL;
    }

    mmio_write32(cs_reg, cs);

    ctrl->config.mode = mode;

#ifdef SPI_DEBUG
    console_printf("[SPI%d] Mode set to %d\n", controller, mode);
#endif

    return SPI_OK;
}

/**
 * Get current SPI mode
 */
spi_mode_t spi_get_mode(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_MODE_0;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_MODE_0;
    }

    return ctrl->config.mode;
}

/**
 * Set SPI bit order
 */
int spi_set_bit_order(uint8_t controller, spi_bit_order_t bit_order)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    /* BCM2712 SPI hardware is MSB-first only */
    if (bit_order != SPI_BIT_ORDER_MSB_FIRST) {
        return SPI_ERR_INVALID;
    }

    ctrl->config.bit_order = bit_order;
    return SPI_OK;
}

/**
 * Set chip select line
 */
int spi_set_cs(uint8_t controller, uint8_t cs)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    if (cs != SPI_CS0 && cs != SPI_CS1 && cs != SPI_CS_NONE) {
        return SPI_ERR_INVALID;
    }

    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs_val = mmio_read32(cs_reg);

    /* Clear CS bits */
    cs_val &= ~SPI_CS_CS_MASK;

    /* Set new CS */
    cs_val |= (cs & SPI_CS_CS_MASK);

    mmio_write32(cs_reg, cs_val);

    ctrl->config.chip_select = cs;

#ifdef SPI_DEBUG
    console_printf("[SPI%d] Chip select set to %d\n", controller, cs);
#endif

    return SPI_OK;
}

/**
 * Set chip select polarity
 */
int spi_set_cs_polarity(uint8_t controller, bool active_high)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs = mmio_read32(cs_reg);

    if (active_high) {
        cs |= SPI_CS_CSPOL;
    } else {
        cs &= ~SPI_CS_CSPOL;
    }

    mmio_write32(cs_reg, cs);

    ctrl->config.cs_polarity = active_high;

#ifdef SPI_DEBUG
    console_printf("[SPI%d] CS polarity set to %s\n",
                   controller, active_high ? "active-high" : "active-low");
#endif

    return SPI_OK;
}

/* ============================================================================
 * Data Transfer Functions
 * ============================================================================ */

/**
 * Perform SPI transfer (full-duplex)
 */
int spi_transfer(uint8_t controller, const uint8_t *tx_buf, uint8_t *rx_buf, uint32_t len)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    if (len == 0) {
        return 0;
    }

    if (!tx_buf && !rx_buf) {
        return SPI_ERR_INVALID;
    }

    /* Check if busy */
    if (ctrl->state == SPI_STATE_BUSY) {
        return SPI_ERR_BUSY;
    }

    ctrl->state = SPI_STATE_BUSY;

    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    volatile uint32_t *fifo_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_FIFO);

    /* Clear FIFOs */
    clear_fifos(ctrl);

    /* Set transfer active */
    uint32_t cs = mmio_read32(cs_reg);
    cs |= SPI_CS_TA;
    mmio_write32(cs_reg, cs);

    uint32_t tx_index = 0;
    uint32_t rx_index = 0;

#ifdef SPI_DEBUG
    console_printf("[SPI%d] Transfer: %u bytes\n", controller, len);
#endif

    /* Perform transfer */
    while (rx_index < len) {
        /* Write to TX FIFO if we have data and space available */
        while (tx_index < len && tx_fifo_ready(ctrl)) {
            uint8_t tx_byte = tx_buf ? tx_buf[tx_index] : 0x00;
            mmio_write32(fifo_reg, tx_byte);
            tx_index++;
        }

        /* Read from RX FIFO if data available */
        while (rx_index < tx_index && rx_fifo_has_data(ctrl)) {
            uint32_t rx_byte = mmio_read32(fifo_reg);
            if (rx_buf) {
                rx_buf[rx_index] = (uint8_t)rx_byte;
            }
            rx_index++;
        }
    }

    /* Wait for transfer to complete */
    int ret = wait_transfer_done(ctrl, SPI_TIMEOUT_DEFAULT);
    if (ret != SPI_OK) {
        ctrl->state = SPI_STATE_ERROR;
        ctrl->stats.tx_errors++;
        cs = mmio_read32(cs_reg);
        cs &= ~SPI_CS_TA;
        mmio_write32(cs_reg, cs);
        return ret;
    }

    /* Clear transfer active */
    cs = mmio_read32(cs_reg);
    cs &= ~SPI_CS_TA;
    mmio_write32(cs_reg, cs);

    /* Update statistics */
    ctrl->stats.transfers++;
    if (tx_buf) {
        ctrl->stats.tx_bytes += len;
    }
    if (rx_buf) {
        ctrl->stats.rx_bytes += len;
    }

    ctrl->state = SPI_STATE_IDLE;

    return (int)len;
}

/**
 * Perform SPI transfer with detailed control
 */
int spi_transfer_ex(uint8_t controller, const spi_transfer_t *xfer)
{
    if (!xfer) {
        return SPI_ERR_INVALID;
    }

    /* Handle custom clock speed if specified */
    uint32_t saved_clock = 0;
    if (xfer->speed_hz != 0) {
        saved_clock = spi_get_clock(controller);
        spi_set_clock(controller, xfer->speed_hz);
    }

    /* Perform transfer */
    int ret = spi_transfer(controller, xfer->tx_buf, xfer->rx_buf, xfer->len);

    /* Restore clock if changed */
    if (saved_clock != 0) {
        spi_set_clock(controller, saved_clock);
    }

    /* Handle CS change if requested */
    if (xfer->cs_change && ret > 0) {
        /* Pulse CS by toggling TA bit */
        if (controller < SPI_CONTROLLER_COUNT) {
            spi_controller_t *ctrl = &g_spi[controller];
            volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
            uint32_t cs = mmio_read32(cs_reg);
            cs &= ~SPI_CS_TA;
            mmio_write32(cs_reg, cs);
        }
    }

    /* Handle delay */
    if (xfer->delay_usecs > 0 && ret > 0) {
        /* TODO: Implement microsecond delay */
        for (volatile uint32_t i = 0; i < xfer->delay_usecs * 10; i++);
    }

    return ret;
}

/**
 * Transmit data only
 */
int spi_write(uint8_t controller, const uint8_t *tx_buf, uint32_t len)
{
    return spi_transfer(controller, tx_buf, NULL, len);
}

/**
 * Receive data only
 */
int spi_read(uint8_t controller, uint8_t *rx_buf, uint32_t len)
{
    return spi_transfer(controller, NULL, rx_buf, len);
}

/**
 * Transfer single byte
 */
int spi_transfer_byte(uint8_t controller, uint8_t tx_byte)
{
    uint8_t rx_byte;
    int ret = spi_transfer(controller, &tx_byte, &rx_byte, 1);
    if (ret < 0) {
        return ret;
    }
    return rx_byte;
}

/**
 * Transfer 16-bit word
 */
int spi_transfer_word(uint8_t controller, uint16_t tx_word)
{
    uint8_t tx_buf[2];
    uint8_t rx_buf[2];

    /* MSB first */
    tx_buf[0] = (tx_word >> 8) & 0xFF;
    tx_buf[1] = tx_word & 0xFF;

    int ret = spi_transfer(controller, tx_buf, rx_buf, 2);
    if (ret < 0) {
        return ret;
    }

    return ((uint16_t)rx_buf[0] << 8) | rx_buf[1];
}

/* ============================================================================
 * FIFO Management
 * ============================================================================ */

/**
 * Clear TX FIFO
 */
int spi_clear_tx_fifo(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs = mmio_read32(cs_reg);
    cs |= SPI_CS_CLEAR_TX;
    mmio_write32(cs_reg, cs);

    return SPI_OK;
}

/**
 * Clear RX FIFO
 */
int spi_clear_rx_fifo(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    volatile uint32_t *cs_reg = (volatile uint32_t *)((uintptr_t)ctrl->base + SPI_CS);
    uint32_t cs = mmio_read32(cs_reg);
    cs |= SPI_CS_CLEAR_RX;
    mmio_write32(cs_reg, cs);

    return SPI_OK;
}

/**
 * Check if TX FIFO is empty
 */
bool spi_tx_fifo_empty(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return true;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return true;
    }

    return tx_fifo_ready(ctrl);
}

/**
 * Check if RX FIFO has data
 */
bool spi_rx_fifo_has_data(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return false;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return false;
    }

    return rx_fifo_has_data(ctrl);
}

/* ============================================================================
 * Statistics and Diagnostics
 * ============================================================================ */

/**
 * Get SPI subsystem statistics
 */
int spi_get_stats(uint8_t controller, spi_stats_t *stats)
{
    if (!stats) {
        return SPI_ERR_INVALID;
    }

    if (controller >= SPI_CONTROLLER_COUNT) {
        return SPI_ERR_NO_DEVICE;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    *stats = ctrl->stats;
    return SPI_OK;
}

/**
 * Reset SPI statistics counters
 */
void spi_reset_stats(uint8_t controller)
{
    if (controller >= SPI_CONTROLLER_COUNT) {
        return;
    }

    spi_controller_t *ctrl = &g_spi[controller];

    if (!ctrl->initialized) {
        return;
    }

    ctrl->stats.transfers = 0;
    ctrl->stats.tx_bytes = 0;
    ctrl->stats.rx_bytes = 0;
    ctrl->stats.tx_errors = 0;
    ctrl->stats.rx_errors = 0;
    ctrl->stats.fifo_overruns = 0;
    ctrl->stats.fifo_underruns = 0;
    ctrl->stats.timeouts = 0;
    ctrl->stats.dma_transfers = 0;
}

/**
 * Validate SPI controller number
 */
bool spi_is_valid_controller(uint8_t controller)
{
    return (controller < SPI_CONTROLLER_COUNT);
}

/**
 * Validate chip select number
 */
bool spi_is_valid_cs(uint8_t cs)
{
    return (cs == SPI_CS0 || cs == SPI_CS1 || cs == SPI_CS_NONE);
}

/* ============================================================================
 * Device Management (High-Level Interface)
 * ============================================================================ */

/**
 * Open SPI device with configuration
 */
int spi_open(const spi_config_t *config, spi_device_t *device)
{
    if (!config || !device) {
        return SPI_ERR_INVALID;
    }

    int ret = spi_init_config(config);
    if (ret != SPI_OK) {
        return ret;
    }

    device->controller = config->controller;
    device->chip_select = config->chip_select;
    device->clock_hz = config->clock_hz;
    device->mode = config->mode;
    device->initialized = true;

    return SPI_OK;
}

/**
 * Close SPI device
 */
int spi_close(spi_device_t *device)
{
    if (!device) {
        return SPI_ERR_INVALID;
    }

    if (!device->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    spi_shutdown(device->controller);
    device->initialized = false;

    return SPI_OK;
}

/**
 * Transfer data using device handle
 */
int spi_device_transfer(spi_device_t *device, const uint8_t *tx_buf, uint8_t *rx_buf, uint32_t len)
{
    if (!device) {
        return SPI_ERR_INVALID;
    }

    if (!device->initialized) {
        return SPI_ERR_NOT_INIT;
    }

    return spi_transfer(device->controller, tx_buf, rx_buf, len);
}
