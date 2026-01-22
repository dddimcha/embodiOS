/* EMBODIOS CAN Bus Driver Implementation
 *
 * Controller Area Network (CAN) driver for industrial and automotive
 * communication. Supports CAN 2.0A (standard) and CAN 2.0B (extended)
 * frame formats.
 */

#include <embodios/can.h>
#include <embodios/pci.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* Debug output (uncomment to enable) */
/* #define CAN_DEBUG 1 */

/* ============================================================================
 * Module State
 * ============================================================================ */

/**
 * CAN device state structure
 * Maintains controller state, configuration, and statistics
 */
typedef struct can_dev {
    pci_device_t *pci_dev;      /* PCI device (if PCI-based controller) */
    uint16_t iobase;            /* I/O base address */

    can_config_t config;        /* Configuration parameters */
    can_state_t state;          /* Controller state */
    can_stats_t stats;          /* Statistics counters */

    /* RX/TX queues */
    can_frame_t rx_queue[CAN_RX_QUEUE_SIZE];
    can_frame_t tx_queue[CAN_TX_QUEUE_SIZE];
    uint16_t rx_head;           /* RX queue head index */
    uint16_t rx_tail;           /* RX queue tail index */
    uint16_t tx_head;           /* TX queue head index */
    uint16_t tx_tail;           /* TX queue tail index */

    /* Filters */
    can_filter_t filters[CAN_MAX_FILTERS];
    uint8_t filter_count;       /* Active filter count */

    bool initialized;           /* Initialization flag */
} can_dev_t;

/* Global CAN device instance */
static can_dev_t g_can = {0};

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Validate CAN frame parameters
 */
static int can_validate_frame(const can_frame_t *frame)
{
    if (!frame) {
        return CAN_ERR_INVALID;
    }

    /* Validate DLC */
    if (frame->dlc > CAN_MAX_DLC) {
        return CAN_ERR_INVALID;
    }

    /* Validate ID based on frame type */
    if (frame->flags & CAN_FLAG_EFF) {
        /* Extended frame - 29-bit ID */
        if (frame->id > CAN_MAX_EXT_ID) {
            return CAN_ERR_INVALID;
        }
    } else {
        /* Standard frame - 11-bit ID */
        if (frame->id > CAN_MAX_STD_ID) {
            return CAN_ERR_INVALID;
        }
    }

    return CAN_OK;
}

/**
 * Check if frame passes filter
 */
static bool can_frame_matches_filter(const can_frame_t *frame, const can_filter_t *filter)
{
    if (!filter->enabled) {
        return true;  /* Disabled filters accept all */
    }

    /* Check extended flag match */
    bool is_extended = (frame->flags & CAN_FLAG_EFF) != 0;
    if (is_extended != filter->extended) {
        return false;
    }

    /* Apply mask and compare */
    uint32_t masked_id = frame->id & filter->mask;
    uint32_t filter_id = filter->id & filter->mask;

    return (masked_id == filter_id);
}

/**
 * Check if frame passes any active filters
 */
static bool can_passes_filters(const can_frame_t *frame)
{
    /* If no filters configured, accept all */
    if (g_can.filter_count == 0) {
        return true;
    }

    /* Check each filter */
    for (int i = 0; i < CAN_MAX_FILTERS; i++) {
        if (can_frame_matches_filter(frame, &g_can.filters[i])) {
            return true;
        }
    }

    return false;
}

/* ============================================================================
 * Initialization and Configuration
 * ============================================================================ */

/**
 * Initialize CAN subsystem
 */
int can_init(const can_config_t *config)
{
    console_printf("[CAN] Initializing CAN bus driver...\n");

    /* Initialize device structure */
    memset(&g_can, 0, sizeof(can_dev_t));

    /* Setup default configuration */
    if (config) {
        g_can.config = *config;
    } else {
        /* Default configuration */
        g_can.config.baud_rate = CAN_BAUD_DEFAULT;
        g_can.config.loopback = false;
        g_can.config.listen_only = false;
        g_can.config.auto_retransmit = true;
    }

    /* Initialize queues */
    g_can.rx_head = 0;
    g_can.rx_tail = 0;
    g_can.tx_head = 0;
    g_can.tx_tail = 0;

    /* Initialize filters */
    g_can.filter_count = 0;
    for (int i = 0; i < CAN_MAX_FILTERS; i++) {
        g_can.filters[i].enabled = false;
    }

    /* Set initial state */
    g_can.state = CAN_STATE_STOPPED;
    g_can.initialized = true;

    console_printf("[CAN] Driver initialized (baud: %d bps)\n", g_can.config.baud_rate);
    console_printf("[CAN] RX queue: %d frames, TX queue: %d frames\n",
                   CAN_RX_QUEUE_SIZE, CAN_TX_QUEUE_SIZE);

    /* Note: Hardware detection via PCI will be added in phase 2 */
    console_printf("[CAN] Waiting for PCI device registration...\n");

    return CAN_OK;
}

/**
 * Shutdown CAN subsystem
 */
void can_shutdown(void)
{
    if (!g_can.initialized) {
        return;
    }

    console_printf("[CAN] Shutting down CAN driver...\n");

    /* Stop controller */
    can_stop();

    /* Clear state */
    g_can.initialized = false;
    g_can.state = CAN_STATE_STOPPED;
}

/**
 * Check if CAN subsystem is initialized
 */
bool can_is_initialized(void)
{
    return g_can.initialized;
}

/**
 * Get current CAN controller state
 */
can_state_t can_get_state(void)
{
    return g_can.state;
}

/* ============================================================================
 * Configuration Functions
 * ============================================================================ */

/**
 * Set CAN baud rate
 */
int can_set_baud_rate(uint32_t baud_rate)
{
    if (!g_can.initialized) {
        return CAN_ERR_NOT_INIT;
    }

    /* Validate baud rate */
    if (baud_rate != CAN_BAUD_125K &&
        baud_rate != CAN_BAUD_250K &&
        baud_rate != CAN_BAUD_500K &&
        baud_rate != CAN_BAUD_1M) {
        console_printf("[CAN] Invalid baud rate: %d\n", baud_rate);
        return CAN_ERR_INVALID;
    }

    /* Store configuration */
    g_can.config.baud_rate = baud_rate;

#ifdef CAN_DEBUG
    console_printf("[CAN] Baud rate set to %d bps\n", baud_rate);
#endif

    /* TODO: Configure hardware timing registers in phase 2 */

    return CAN_OK;
}

/**
 * Get current baud rate
 */
uint32_t can_get_baud_rate(void)
{
    return g_can.config.baud_rate;
}

/**
 * Start CAN controller
 */
int can_start(void)
{
    if (!g_can.initialized) {
        return CAN_ERR_NOT_INIT;
    }

    if (g_can.state == CAN_STATE_RUNNING) {
        return CAN_OK;  /* Already running */
    }

    console_printf("[CAN] Starting CAN controller...\n");

    /* TODO: Enable hardware controller in phase 2 */

    g_can.state = CAN_STATE_RUNNING;

    return CAN_OK;
}

/**
 * Stop CAN controller
 */
int can_stop(void)
{
    if (!g_can.initialized) {
        return CAN_ERR_NOT_INIT;
    }

    if (g_can.state == CAN_STATE_STOPPED) {
        return CAN_OK;  /* Already stopped */
    }

    console_printf("[CAN] Stopping CAN controller...\n");

    /* TODO: Disable hardware controller in phase 2 */

    g_can.state = CAN_STATE_STOPPED;

    return CAN_OK;
}

/* ============================================================================
 * Transmit Functions
 * ============================================================================ */

/**
 * Send a CAN frame (blocking)
 */
int can_send(const can_frame_t *frame, uint32_t timeout_ms)
{
    int ret;

    if (!g_can.initialized) {
        return CAN_ERR_NOT_INIT;
    }

    if (g_can.state != CAN_STATE_RUNNING) {
        return CAN_ERR_INVALID;
    }

    /* Validate frame */
    ret = can_validate_frame(frame);
    if (ret != CAN_OK) {
        return ret;
    }

    /* Check TX queue space */
    uint16_t next_head = (g_can.tx_head + 1) % CAN_TX_QUEUE_SIZE;
    if (next_head == g_can.tx_tail) {
        g_can.stats.tx_dropped++;
        return CAN_ERR_FULL;
    }

    /* Add to TX queue */
    g_can.tx_queue[g_can.tx_head] = *frame;
    g_can.tx_head = next_head;

    /* Update statistics */
    g_can.stats.tx_frames++;
    g_can.stats.tx_bytes += frame->dlc;

#ifdef CAN_DEBUG
    console_printf("[CAN] TX: ID=0x%x DLC=%d\n", frame->id, frame->dlc);
#endif

    /* TODO: Actual hardware transmission in phase 2 */

    return CAN_OK;
}

/**
 * Send a CAN frame (non-blocking)
 */
int can_send_async(const can_frame_t *frame)
{
    return can_send(frame, 0);
}

/* ============================================================================
 * Receive Functions
 * ============================================================================ */

/**
 * Receive a CAN frame (blocking)
 */
int can_receive(can_frame_t *frame, uint32_t timeout_ms)
{
    if (!g_can.initialized) {
        return CAN_ERR_NOT_INIT;
    }

    if (!frame) {
        return CAN_ERR_INVALID;
    }

    if (g_can.state != CAN_STATE_RUNNING) {
        return CAN_ERR_INVALID;
    }

    /* Check if RX queue has data */
    if (g_can.rx_head == g_can.rx_tail) {
        return CAN_ERR_EMPTY;  /* No frames available */
    }

    /* Retrieve frame from queue */
    *frame = g_can.rx_queue[g_can.rx_tail];
    g_can.rx_tail = (g_can.rx_tail + 1) % CAN_RX_QUEUE_SIZE;

#ifdef CAN_DEBUG
    console_printf("[CAN] RX: ID=0x%x DLC=%d\n", frame->id, frame->dlc);
#endif

    return CAN_OK;
}

/**
 * Receive a CAN frame (non-blocking)
 */
int can_receive_async(can_frame_t *frame)
{
    return can_receive(frame, 0);
}

/**
 * Poll for received frames
 */
int can_poll(void)
{
    if (!g_can.initialized) {
        return 0;
    }

    /* TODO: Poll hardware RX buffers in phase 2 */

    return 0;
}

/* ============================================================================
 * Filter Functions
 * ============================================================================ */

/**
 * Set CAN acceptance filter at specific index
 */
int can_set_filter(int filter_index, const can_filter_t *filter)
{
    if (!g_can.initialized) {
        return CAN_ERR_NOT_INIT;
    }

    if (!filter) {
        return CAN_ERR_INVALID;
    }

    if (filter_index < 0 || filter_index >= CAN_MAX_FILTERS) {
        return CAN_ERR_INVALID;
    }

    /* Set filter at specified index */
    g_can.filters[filter_index] = *filter;

    /* If this is a newly enabled filter, increment count */
    if (filter->enabled && !g_can.filters[filter_index].enabled) {
        g_can.filter_count++;
    } else if (!filter->enabled && g_can.filters[filter_index].enabled) {
        g_can.filter_count--;
    }

    g_can.filters[filter_index].enabled = filter->enabled;

#ifdef CAN_DEBUG
    console_printf("[CAN] Filter %d set: ID=0x%x Mask=0x%x %s\n",
                  filter_index, filter->id, filter->mask,
                  filter->enabled ? "enabled" : "disabled");
#endif

    /* TODO: Configure hardware filter in phase 2 */

    return CAN_OK;
}

/**
 * Add CAN acceptance filter
 */
int can_add_filter(const can_filter_t *filter)
{
    if (!g_can.initialized) {
        return CAN_ERR_NOT_INIT;
    }

    if (!filter) {
        return CAN_ERR_INVALID;
    }

    /* Find free filter slot */
    for (int i = 0; i < CAN_MAX_FILTERS; i++) {
        if (!g_can.filters[i].enabled) {
            g_can.filters[i] = *filter;
            g_can.filters[i].enabled = true;
            g_can.filter_count++;

#ifdef CAN_DEBUG
            console_printf("[CAN] Filter %d added: ID=0x%x Mask=0x%x\n",
                          i, filter->id, filter->mask);
#endif

            /* TODO: Configure hardware filter in phase 2 */

            return i;  /* Return filter index */
        }
    }

    return CAN_ERR_FULL;  /* No free filter slots */
}

/**
 * Remove CAN acceptance filter
 */
int can_remove_filter(int filter_index)
{
    if (!g_can.initialized) {
        return CAN_ERR_NOT_INIT;
    }

    if (filter_index < 0 || filter_index >= CAN_MAX_FILTERS) {
        return CAN_ERR_INVALID;
    }

    if (!g_can.filters[filter_index].enabled) {
        return CAN_ERR_INVALID;  /* Filter not active */
    }

    g_can.filters[filter_index].enabled = false;
    g_can.filter_count--;

#ifdef CAN_DEBUG
    console_printf("[CAN] Filter %d removed\n", filter_index);
#endif

    /* TODO: Update hardware filter in phase 2 */

    return CAN_OK;
}

/**
 * Clear all acceptance filters
 */
void can_clear_filters(void)
{
    if (!g_can.initialized) {
        return;
    }

    for (int i = 0; i < CAN_MAX_FILTERS; i++) {
        g_can.filters[i].enabled = false;
    }

    g_can.filter_count = 0;

    console_printf("[CAN] All filters cleared\n");

    /* TODO: Reset hardware filters in phase 2 */
}

/* ============================================================================
 * Statistics and Diagnostics
 * ============================================================================ */

/**
 * Get CAN bus statistics
 */
void can_get_stats(can_stats_t *stats)
{
    if (!stats) {
        return;
    }

    *stats = g_can.stats;
}

/**
 * Reset CAN bus statistics
 */
void can_reset_stats(void)
{
    memset(&g_can.stats, 0, sizeof(can_stats_t));
    console_printf("[CAN] Statistics reset\n");
}

/**
 * Print CAN status and statistics to console
 */
void can_print_info(void)
{
    console_printf("\n=== CAN Bus Status ===\n");
    console_printf("State: ");

    switch (g_can.state) {
        case CAN_STATE_STOPPED:
            console_printf("STOPPED\n");
            break;
        case CAN_STATE_RUNNING:
            console_printf("RUNNING\n");
            break;
        case CAN_STATE_ERROR_ACTIVE:
            console_printf("ERROR_ACTIVE\n");
            break;
        case CAN_STATE_ERROR_PASSIVE:
            console_printf("ERROR_PASSIVE\n");
            break;
        case CAN_STATE_BUS_OFF:
            console_printf("BUS_OFF\n");
            break;
        default:
            console_printf("UNKNOWN\n");
    }

    console_printf("Baud Rate: %d bps\n", g_can.config.baud_rate);
    console_printf("Active Filters: %d/%d\n", g_can.filter_count, CAN_MAX_FILTERS);

    console_printf("\n=== Statistics ===\n");
    console_printf("TX Frames: %llu (%llu bytes)\n",
                   g_can.stats.tx_frames, g_can.stats.tx_bytes);
    console_printf("RX Frames: %llu (%llu bytes)\n",
                   g_can.stats.rx_frames, g_can.stats.rx_bytes);
    console_printf("TX Errors: %llu (dropped: %llu)\n",
                   g_can.stats.tx_errors, g_can.stats.tx_dropped);
    console_printf("RX Errors: %llu (overruns: %llu)\n",
                   g_can.stats.rx_errors, g_can.stats.rx_overrun);
    console_printf("Bus-Off Events: %llu\n", g_can.stats.bus_off);
    console_printf("=====================\n\n");
}

/**
 * Run CAN self-tests
 */
int can_run_tests(void)
{
    console_printf("[CAN] Running self-tests...\n");

    if (!g_can.initialized) {
        console_printf("[CAN] FAIL: Not initialized\n");
        return -1;
    }

    /* Test 1: Frame validation */
    can_frame_t test_frame;
    test_frame.id = 0x123;
    test_frame.dlc = 8;
    test_frame.flags = 0;

    if (can_validate_frame(&test_frame) != CAN_OK) {
        console_printf("[CAN] FAIL: Frame validation\n");
        return -1;
    }

    /* Test 2: Invalid DLC */
    test_frame.dlc = 10;  /* Invalid */
    if (can_validate_frame(&test_frame) == CAN_OK) {
        console_printf("[CAN] FAIL: DLC validation\n");
        return -1;
    }

    console_printf("[CAN] Self-tests PASSED\n");

    return 0;
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Create a standard CAN frame (11-bit ID)
 */
int can_make_std_frame(uint32_t id, const uint8_t *data, uint8_t dlc, can_frame_t *frame)
{
    if (!frame || dlc > CAN_MAX_DLC || id > CAN_MAX_STD_ID) {
        return CAN_ERR_INVALID;
    }

    memset(frame, 0, sizeof(can_frame_t));
    frame->id = id & CAN_STD_ID_MASK;
    frame->dlc = dlc;
    frame->flags = 0;  /* Standard frame */

    if (data && dlc > 0) {
        for (int i = 0; i < dlc; i++) {
            frame->data[i] = data[i];
        }
    }

    return CAN_OK;
}

/**
 * Create an extended CAN frame (29-bit ID)
 */
int can_make_ext_frame(uint32_t id, const uint8_t *data, uint8_t dlc, can_frame_t *frame)
{
    if (!frame || dlc > CAN_MAX_DLC || id > CAN_MAX_EXT_ID) {
        return CAN_ERR_INVALID;
    }

    memset(frame, 0, sizeof(can_frame_t));
    frame->id = id & CAN_EXT_ID_MASK;
    frame->dlc = dlc;
    frame->flags = CAN_FLAG_EFF;  /* Extended frame */

    if (data && dlc > 0) {
        for (int i = 0; i < dlc; i++) {
            frame->data[i] = data[i];
        }
    }

    return CAN_OK;
}

/**
 * Check if frame has extended identifier
 */
bool can_is_extended(const can_frame_t *frame)
{
    if (!frame) {
        return false;
    }

    return (frame->flags & CAN_FLAG_EFF) != 0;
}

/**
 * Check if frame is a Remote Transmission Request
 */
bool can_is_rtr(const can_frame_t *frame)
{
    if (!frame) {
        return false;
    }

    return (frame->flags & CAN_FLAG_RTR) != 0;
}

/**
 * Check if frame is an error frame
 */
bool can_is_error(const can_frame_t *frame)
{
    if (!frame) {
        return false;
    }

    return (frame->flags & CAN_FLAG_ERR) != 0;
}

/* ============================================================================
 * PCI Driver Registration
 * ============================================================================ */

/**
 * PCI probe callback for CAN devices
 * Called when a matching PCI device is discovered
 */
static int can_probe(pci_device_t *dev)
{
    console_printf("[CAN] PCI device detected: vendor=0x%04x device=0x%04x\n",
                   dev->vendor_id, dev->device_id);

    /* TODO: Initialize hardware in phase 2 */

    return CAN_OK;
}

/**
 * PCI driver structure for CAN controllers
 */
static pci_driver_t can_driver = {
    .name = "can",
    .vendor_id = PCI_ANY_ID,
    .device_id = PCI_ANY_ID,
    .class_code = PCI_CLASS_SERIAL,
    .subclass = PCI_ANY_CLASS,
    .probe = can_probe,
    .remove = NULL,
    .next = NULL
};
