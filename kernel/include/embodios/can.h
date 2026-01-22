/* EMBODIOS CAN Bus Driver Interface
 *
 * Controller Area Network (CAN) driver for industrial and automotive
 * communication. Supports CAN 2.0A (standard) and CAN 2.0B (extended)
 * frame formats for real-time sensor/actuator integration.
 *
 * Features:
 * - CAN 2.0A (11-bit identifier) and CAN 2.0B (29-bit identifier)
 * - Configurable baud rates (125k, 250k, 500k, 1M)
 * - Message filtering by CAN ID
 * - USB-CAN adapter support
 * - Error detection and statistics
 */

#ifndef EMBODIOS_CAN_H
#define EMBODIOS_CAN_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CAN Protocol Constants
 * ============================================================================ */

/* CAN Frame Types */
#define CAN_2_0A                0       /* Standard 11-bit identifier */
#define CAN_2_0B                1       /* Extended 29-bit identifier */

/* CAN Identifier Limits */
#define CAN_STD_ID_MASK         0x7FF   /* 11-bit standard ID mask */
#define CAN_EXT_ID_MASK         0x1FFFFFFF  /* 29-bit extended ID mask */
#define CAN_MAX_STD_ID          0x7FF   /* Maximum standard ID */
#define CAN_MAX_EXT_ID          0x1FFFFFFF  /* Maximum extended ID */

/* CAN Data Lengths */
#define CAN_MAX_DLC             8       /* Maximum data length code */
#define CAN_MAX_DATA_BYTES      8       /* Maximum data bytes per frame */

/* CAN Frame Flags */
#define CAN_FLAG_EFF            0x80000000  /* Extended Frame Format */
#define CAN_FLAG_RTR            0x40000000  /* Remote Transmission Request */
#define CAN_FLAG_ERR            0x20000000  /* Error frame flag */

/* ============================================================================
 * CAN Baud Rates
 * ============================================================================ */

#define CAN_BAUD_125K           125000  /* 125 kbit/s */
#define CAN_BAUD_250K           250000  /* 250 kbit/s */
#define CAN_BAUD_500K           500000  /* 500 kbit/s */
#define CAN_BAUD_1M             1000000 /* 1 Mbit/s */

/* Default baud rate */
#define CAN_BAUD_DEFAULT        CAN_BAUD_500K

/* ============================================================================
 * CAN Buffer Sizes
 * ============================================================================ */

#define CAN_RX_QUEUE_SIZE       64      /* Receive queue depth */
#define CAN_TX_QUEUE_SIZE       32      /* Transmit queue depth */
#define CAN_MAX_FILTERS         16      /* Maximum acceptance filters */

/* ============================================================================
 * CAN Frame Structure
 * ============================================================================ */

/**
 * CAN Frame (CAN 2.0A/2.0B compatible)
 * Supports both standard (11-bit) and extended (29-bit) identifiers
 */
typedef struct can_frame {
    uint32_t id;            /* CAN identifier (11-bit or 29-bit) */
    uint8_t  dlc;           /* Data length code (0-8) */
    uint8_t  flags;         /* Frame flags (EFF, RTR, ERR) */
    uint8_t  reserved[2];   /* Reserved for alignment */
    uint8_t  data[CAN_MAX_DATA_BYTES];  /* Frame payload data */
} __packed can_frame_t;

/* ============================================================================
 * CAN Filter Structure
 * ============================================================================ */

/**
 * CAN acceptance filter
 * Filters incoming messages by ID and mask
 */
typedef struct can_filter {
    uint32_t id;            /* CAN ID to match */
    uint32_t mask;          /* Mask for ID matching (1=must match, 0=don't care) */
    bool extended;          /* True for 29-bit ID, false for 11-bit */
    bool enabled;           /* Filter active flag */
} can_filter_t;

/* ============================================================================
 * CAN Device Configuration
 * ============================================================================ */

/**
 * CAN bus configuration parameters
 */
typedef struct can_config {
    uint32_t baud_rate;     /* Baud rate (125k, 250k, 500k, 1M) */
    bool loopback;          /* Loopback mode for testing */
    bool listen_only;       /* Listen-only mode (no ACK transmission) */
    bool auto_retransmit;   /* Automatic retransmission on error */
} can_config_t;

/* ============================================================================
 * CAN Device Statistics
 * ============================================================================ */

/**
 * CAN bus statistics and error counters
 */
typedef struct can_stats {
    uint64_t rx_frames;     /* Frames received */
    uint64_t tx_frames;     /* Frames transmitted */
    uint64_t rx_bytes;      /* Bytes received */
    uint64_t tx_bytes;      /* Bytes transmitted */
    uint64_t rx_errors;     /* Receive errors */
    uint64_t tx_errors;     /* Transmit errors */
    uint64_t bus_off;       /* Bus-off events */
    uint64_t error_warning; /* Error warning threshold exceeded */
    uint64_t rx_overrun;    /* Receive buffer overruns */
    uint64_t tx_dropped;    /* Dropped transmit frames */
} can_stats_t;

/* ============================================================================
 * CAN Device State
 * ============================================================================ */

/**
 * CAN controller state
 */
typedef enum can_state {
    CAN_STATE_STOPPED = 0,  /* Controller stopped */
    CAN_STATE_RUNNING,      /* Normal operation */
    CAN_STATE_ERROR_ACTIVE, /* Error active state */
    CAN_STATE_ERROR_PASSIVE,/* Error passive state */
    CAN_STATE_BUS_OFF,      /* Bus-off state */
} can_state_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define CAN_OK                  0       /* Success */
#define CAN_ERR_NOT_INIT       -1       /* Not initialized */
#define CAN_ERR_NOMEM          -2       /* Out of memory */
#define CAN_ERR_INVALID        -3       /* Invalid parameter */
#define CAN_ERR_TIMEOUT        -4       /* Operation timeout */
#define CAN_ERR_BUSY           -5       /* Device busy */
#define CAN_ERR_NO_DEVICE      -6       /* No CAN device found */
#define CAN_ERR_IO             -7       /* I/O error */
#define CAN_ERR_BUS_OFF        -8       /* Bus-off state */
#define CAN_ERR_FULL           -9       /* Queue full */
#define CAN_ERR_EMPTY          -10      /* Queue empty */

/* ============================================================================
 * Public API - Initialization
 * ============================================================================ */

/**
 * Initialize CAN subsystem
 * Scans for CAN devices and initializes the first one found
 * @param config    Configuration parameters (NULL for defaults)
 * @return CAN_OK on success, error code on failure
 */
int can_init(const can_config_t *config);

/**
 * Shutdown CAN subsystem
 * Stops the controller and releases resources
 */
void can_shutdown(void);

/**
 * Check if CAN subsystem is initialized
 * @return true if initialized and ready
 */
bool can_is_initialized(void);

/**
 * Get current CAN controller state
 * @return Current controller state
 */
can_state_t can_get_state(void);

/* ============================================================================
 * Public API - Configuration
 * ============================================================================ */

/**
 * Set CAN baud rate
 * @param baud_rate Baud rate in bits/sec (125k, 250k, 500k, 1M)
 * @return CAN_OK on success, error code on failure
 */
int can_set_baud_rate(uint32_t baud_rate);

/**
 * Get current baud rate
 * @return Current baud rate in bits/sec
 */
uint32_t can_get_baud_rate(void);

/**
 * Start CAN controller
 * Begins normal operation
 * @return CAN_OK on success, error code on failure
 */
int can_start(void);

/**
 * Stop CAN controller
 * Halts all transmission and reception
 * @return CAN_OK on success, error code on failure
 */
int can_stop(void);

/* ============================================================================
 * Public API - Frame Transmission
 * ============================================================================ */

/**
 * Send a CAN frame (blocking)
 * @param frame     Frame to transmit
 * @param timeout_ms Timeout in milliseconds (0 = no wait)
 * @return CAN_OK on success, error code on failure
 */
int can_send(const can_frame_t *frame, uint32_t timeout_ms);

/**
 * Send a CAN frame (non-blocking)
 * @param frame Frame to transmit
 * @return CAN_OK if queued, error code on failure
 */
int can_send_async(const can_frame_t *frame);

/* ============================================================================
 * Public API - Frame Reception
 * ============================================================================ */

/**
 * Receive a CAN frame (blocking)
 * @param frame     Buffer to store received frame
 * @param timeout_ms Timeout in milliseconds (0 = no wait)
 * @return CAN_OK on success, CAN_ERR_TIMEOUT if no frame, error code on failure
 */
int can_receive(can_frame_t *frame, uint32_t timeout_ms);

/**
 * Receive a CAN frame (non-blocking)
 * @param frame Buffer to store received frame
 * @return CAN_OK on success, CAN_ERR_EMPTY if no frame, error code on failure
 */
int can_receive_async(can_frame_t *frame);

/**
 * Poll for received frames
 * Call this periodically to process incoming frames
 * @return Number of frames processed
 */
int can_poll(void);

/* ============================================================================
 * Public API - Filtering
 * ============================================================================ */

/**
 * Add CAN acceptance filter
 * @param filter    Filter configuration
 * @return Filter index on success, negative error code on failure
 */
int can_add_filter(const can_filter_t *filter);

/**
 * Remove CAN acceptance filter
 * @param filter_index Filter index returned by can_add_filter()
 * @return CAN_OK on success, error code on failure
 */
int can_remove_filter(int filter_index);

/**
 * Clear all acceptance filters
 * Removes all configured filters (accepts all frames)
 */
void can_clear_filters(void);

/* ============================================================================
 * Public API - Statistics and Diagnostics
 * ============================================================================ */

/**
 * Get CAN bus statistics
 * @param stats Output structure for statistics
 */
void can_get_stats(can_stats_t *stats);

/**
 * Reset CAN bus statistics
 * Clears all error counters and statistics
 */
void can_reset_stats(void);

/**
 * Print CAN status and statistics to console
 */
void can_print_info(void);

/**
 * Run CAN self-tests
 * @return 0 on success, -1 on failure
 */
int can_run_tests(void);

/* ============================================================================
 * Public API - Helper Functions
 * ============================================================================ */

/**
 * Create a standard CAN frame (11-bit ID)
 * @param id        CAN identifier (0-0x7FF)
 * @param data      Payload data
 * @param dlc       Data length code (0-8)
 * @param frame     Output frame structure
 * @return CAN_OK on success, error code on failure
 */
int can_make_std_frame(uint32_t id, const uint8_t *data, uint8_t dlc, can_frame_t *frame);

/**
 * Create an extended CAN frame (29-bit ID)
 * @param id        CAN identifier (0-0x1FFFFFFF)
 * @param data      Payload data
 * @param dlc       Data length code (0-8)
 * @param frame     Output frame structure
 * @return CAN_OK on success, error code on failure
 */
int can_make_ext_frame(uint32_t id, const uint8_t *data, uint8_t dlc, can_frame_t *frame);

/**
 * Check if frame has extended identifier
 * @param frame Frame to check
 * @return true if extended (29-bit), false if standard (11-bit)
 */
bool can_is_extended(const can_frame_t *frame);

/**
 * Check if frame is a Remote Transmission Request
 * @param frame Frame to check
 * @return true if RTR frame
 */
bool can_is_rtr(const can_frame_t *frame);

/**
 * Check if frame is an error frame
 * @param frame Frame to check
 * @return true if error frame
 */
bool can_is_error(const can_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_CAN_H */
