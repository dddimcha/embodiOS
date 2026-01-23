/* EMBODIOS Modbus TCP Protocol
 *
 * Industrial automation protocol implementation for SCADA integration.
 * Provides Modbus TCP client and server functionality.
 *
 * Features:
 * - Modbus TCP client (connect, read/write registers)
 * - Modbus TCP server (listen, handle requests)
 * - Standard function codes (0x01-0x17)
 * - Holding/input/coil register access
 * - Multi-register read/write operations
 */

#ifndef EMBODIOS_MODBUS_H
#define EMBODIOS_MODBUS_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Protocol Constants
 * ============================================================================ */

/* Modbus TCP */
#define MODBUS_TCP_PORT         502         /* Default Modbus TCP port */
#define MODBUS_MAX_PDU_SIZE     253         /* Maximum PDU size (ADU - MBAP) */
#define MODBUS_MAX_ADU_SIZE     260         /* Maximum ADU size (MBAP + PDU) */
#define MODBUS_MBAP_SIZE        7           /* MBAP header size */
#define MODBUS_PROTOCOL_ID      0           /* Modbus protocol identifier */

/* Timeouts */
#define MODBUS_TIMEOUT_MS       1000        /* Default timeout (1 second) */
#define MODBUS_CONNECT_TIMEOUT  5000        /* Connection timeout (5 seconds) */

/* Limits */
#define MODBUS_MAX_COILS        2000        /* Max coils per request */
#define MODBUS_MAX_REGISTERS    125         /* Max registers per request */
#define MODBUS_MAX_WRITE_COILS  1968        /* Max coils for write multiple */
#define MODBUS_MAX_WRITE_REGS   123         /* Max registers for write multiple */

/* ============================================================================
 * Function Codes
 * ============================================================================ */

/* Standard Modbus function codes */
#define MODBUS_FC_READ_COILS            0x01    /* Read coils */
#define MODBUS_FC_READ_DISCRETE_INPUTS  0x02    /* Read discrete inputs */
#define MODBUS_FC_READ_HOLDING_REGS     0x03    /* Read holding registers */
#define MODBUS_FC_READ_INPUT_REGS       0x04    /* Read input registers */
#define MODBUS_FC_WRITE_SINGLE_COIL     0x05    /* Write single coil */
#define MODBUS_FC_WRITE_SINGLE_REG      0x06    /* Write single register */
#define MODBUS_FC_WRITE_MULTIPLE_COILS  0x0F    /* Write multiple coils */
#define MODBUS_FC_WRITE_MULTIPLE_REGS   0x10    /* Write multiple registers */
#define MODBUS_FC_READ_WRITE_REGS       0x17    /* Read/write registers */

/* Exception codes */
#define MODBUS_EXCEPTION_OFFSET         0x80    /* Exception response offset */

/* ============================================================================
 * Exception Codes
 * ============================================================================ */

#define MODBUS_EXCEPTION_ILLEGAL_FUNCTION       0x01    /* Illegal function */
#define MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS   0x02    /* Illegal data address */
#define MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE     0x03    /* Illegal data value */
#define MODBUS_EXCEPTION_SLAVE_DEVICE_FAILURE   0x04    /* Slave device failure */
#define MODBUS_EXCEPTION_ACKNOWLEDGE            0x05    /* Acknowledge */
#define MODBUS_EXCEPTION_SLAVE_DEVICE_BUSY      0x06    /* Slave device busy */
#define MODBUS_EXCEPTION_MEMORY_PARITY_ERROR    0x08    /* Memory parity error */
#define MODBUS_EXCEPTION_GATEWAY_PATH           0x0A    /* Gateway path unavailable */
#define MODBUS_EXCEPTION_GATEWAY_TARGET         0x0B    /* Gateway target failed */

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define MODBUS_OK               0       /* Success */
#define MODBUS_ERROR           -1       /* Generic error */
#define MODBUS_TIMEOUT         -2       /* Operation timeout */
#define MODBUS_INVALID_ARG     -3       /* Invalid argument */
#define MODBUS_NOT_CONNECTED   -4       /* Not connected */
#define MODBUS_CONN_FAILED     -5       /* Connection failed */
#define MODBUS_EXCEPTION       -6       /* Modbus exception received */
#define MODBUS_INVALID_CRC     -7       /* Invalid CRC (RTU mode) */
#define MODBUS_INVALID_RESP    -8       /* Invalid response */

/* ============================================================================
 * Protocol Headers
 * ============================================================================ */

/* Modbus Application Protocol (MBAP) header for TCP */
typedef struct modbus_mbap_header {
    uint16_t transaction_id;    /* Transaction identifier */
    uint16_t protocol_id;       /* Protocol identifier (0 for Modbus) */
    uint16_t length;            /* Length of remaining data */
    uint8_t  unit_id;           /* Unit identifier (slave address) */
} __packed modbus_mbap_header_t;

/* Modbus PDU header (function code + data) */
typedef struct modbus_pdu {
    uint8_t function_code;      /* Function code */
    uint8_t data[MODBUS_MAX_PDU_SIZE - 1];  /* PDU data */
} __packed modbus_pdu_t;

/* Modbus ADU (Application Data Unit) = MBAP + PDU */
typedef struct modbus_adu {
    modbus_mbap_header_t mbap;  /* MBAP header */
    modbus_pdu_t pdu;           /* PDU */
} __packed modbus_adu_t;

/* ============================================================================
 * Request/Response Structures
 * ============================================================================ */

/* Read coils/discrete inputs request */
typedef struct modbus_read_bits_req {
    uint8_t  function_code;     /* Function code */
    uint16_t start_addr;        /* Starting address */
    uint16_t quantity;          /* Quantity of coils/inputs */
} __packed modbus_read_bits_req_t;

/* Read coils/discrete inputs response */
typedef struct modbus_read_bits_resp {
    uint8_t  function_code;     /* Function code */
    uint8_t  byte_count;        /* Number of data bytes */
    uint8_t  data[];            /* Coil/input values (packed bits) */
} __packed modbus_read_bits_resp_t;

/* Read holding/input registers request */
typedef struct modbus_read_regs_req {
    uint8_t  function_code;     /* Function code */
    uint16_t start_addr;        /* Starting address */
    uint16_t quantity;          /* Quantity of registers */
} __packed modbus_read_regs_req_t;

/* Read holding/input registers response */
typedef struct modbus_read_regs_resp {
    uint8_t  function_code;     /* Function code */
    uint8_t  byte_count;        /* Number of data bytes */
    uint16_t data[];            /* Register values */
} __packed modbus_read_regs_resp_t;

/* Write single coil request */
typedef struct modbus_write_single_coil_req {
    uint8_t  function_code;     /* Function code */
    uint16_t output_addr;       /* Output address */
    uint16_t output_value;      /* Output value (0x0000 or 0xFF00) */
} __packed modbus_write_single_coil_req_t;

/* Write single register request */
typedef struct modbus_write_single_reg_req {
    uint8_t  function_code;     /* Function code */
    uint16_t reg_addr;          /* Register address */
    uint16_t reg_value;         /* Register value */
} __packed modbus_write_single_reg_req_t;

/* Write multiple coils request */
typedef struct modbus_write_multiple_coils_req {
    uint8_t  function_code;     /* Function code */
    uint16_t start_addr;        /* Starting address */
    uint16_t quantity;          /* Quantity of outputs */
    uint8_t  byte_count;        /* Number of data bytes */
    uint8_t  data[];            /* Output values (packed bits) */
} __packed modbus_write_multiple_coils_req_t;

/* Write multiple registers request */
typedef struct modbus_write_multiple_regs_req {
    uint8_t  function_code;     /* Function code */
    uint16_t start_addr;        /* Starting address */
    uint16_t quantity;          /* Quantity of registers */
    uint8_t  byte_count;        /* Number of data bytes */
    uint16_t data[];            /* Register values */
} __packed modbus_write_multiple_regs_req_t;

/* Write multiple response */
typedef struct modbus_write_multiple_resp {
    uint8_t  function_code;     /* Function code */
    uint16_t start_addr;        /* Starting address */
    uint16_t quantity;          /* Quantity written */
} __packed modbus_write_multiple_resp_t;

/* Exception response */
typedef struct modbus_exception_resp {
    uint8_t  function_code;     /* Function code | 0x80 */
    uint8_t  exception_code;    /* Exception code */
} __packed modbus_exception_resp_t;

/* ============================================================================
 * Modbus Context
 * ============================================================================ */

/* Modbus connection state */
typedef enum {
    MODBUS_STATE_DISCONNECTED = 0,
    MODBUS_STATE_CONNECTING,
    MODBUS_STATE_CONNECTED,
    MODBUS_STATE_ERROR
} modbus_state_t;

/* Modbus mode */
typedef enum {
    MODBUS_MODE_TCP = 0,        /* Modbus TCP */
    MODBUS_MODE_RTU             /* Modbus RTU (serial) */
} modbus_mode_t;

/* Modbus client/server context */
typedef struct modbus_ctx {
    /* Connection info */
    modbus_mode_t mode;         /* Protocol mode (TCP/RTU) */
    modbus_state_t state;       /* Connection state */
    int socket_fd;              /* Socket file descriptor */
    uint32_t remote_ip;         /* Remote IP address (TCP) */
    uint16_t remote_port;       /* Remote port (TCP) */
    uint8_t  unit_id;           /* Unit identifier (slave address) */

    /* Transaction management */
    uint16_t transaction_id;    /* Current transaction ID */
    uint32_t timeout_ms;        /* Timeout in milliseconds */

    /* Buffers */
    uint8_t  tx_buffer[MODBUS_MAX_ADU_SIZE];    /* Transmit buffer */
    uint8_t  rx_buffer[MODBUS_MAX_ADU_SIZE];    /* Receive buffer */
    size_t   rx_length;         /* Received data length */

    /* Server data (if acting as server) */
    uint16_t *holding_regs;     /* Holding registers */
    uint16_t *input_regs;       /* Input registers */
    uint8_t  *coils;            /* Coils */
    uint8_t  *discrete_inputs;  /* Discrete inputs */
    uint16_t num_holding_regs;  /* Number of holding registers */
    uint16_t num_input_regs;    /* Number of input registers */
    uint16_t num_coils;         /* Number of coils */
    uint16_t num_discrete_inputs; /* Number of discrete inputs */
} modbus_ctx_t;

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct modbus_stats {
    uint64_t requests_sent;     /* Requests sent (client) */
    uint64_t responses_received; /* Responses received (client) */
    uint64_t requests_received; /* Requests received (server) */
    uint64_t responses_sent;    /* Responses sent (server) */
    uint64_t exceptions_sent;   /* Exception responses sent */
    uint64_t exceptions_received; /* Exception responses received */
    uint64_t timeouts;          /* Timeout errors */
    uint64_t crc_errors;        /* CRC errors (RTU mode) */
    uint64_t invalid_responses; /* Invalid responses */
    uint64_t bytes_sent;        /* Total bytes sent */
    uint64_t bytes_received;    /* Total bytes received */
} modbus_stats_t;

/* ============================================================================
 * Function Declarations
 * ============================================================================ */

/* Context management */
modbus_ctx_t* modbus_new_tcp(uint32_t ip, uint16_t port, uint8_t unit_id);
void modbus_free(modbus_ctx_t *ctx);
int modbus_set_timeout(modbus_ctx_t *ctx, uint32_t timeout_ms);

/* Connection management */
int modbus_connect(modbus_ctx_t *ctx);
int modbus_disconnect(modbus_ctx_t *ctx);
bool modbus_is_connected(modbus_ctx_t *ctx);

/* Client functions - Read operations */
int modbus_read_coils(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, uint8_t *dest);
int modbus_read_discrete_inputs(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, uint8_t *dest);
int modbus_read_holding_registers(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, uint16_t *dest);
int modbus_read_input_registers(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, uint16_t *dest);

/* Client functions - Write operations */
int modbus_write_coil(modbus_ctx_t *ctx, uint16_t addr, bool value);
int modbus_write_register(modbus_ctx_t *ctx, uint16_t addr, uint16_t value);
int modbus_write_coils(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, const uint8_t *src);
int modbus_write_registers(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, const uint16_t *src);

/* Server functions */
int modbus_server_init(modbus_ctx_t *ctx, uint16_t port);
int modbus_server_start(modbus_ctx_t *ctx);
int modbus_server_stop(modbus_ctx_t *ctx);
int modbus_server_process(modbus_ctx_t *ctx);
int modbus_server_set_data(modbus_ctx_t *ctx, uint16_t *holding_regs, uint16_t num_holding,
                           uint16_t *input_regs, uint16_t num_input,
                           uint8_t *coils, uint16_t num_coils,
                           uint8_t *discrete_inputs, uint16_t num_discrete);

/* Utility functions */
int modbus_get_last_error(modbus_ctx_t *ctx);
const char* modbus_error_string(int error_code);
void modbus_get_stats(modbus_ctx_t *ctx, modbus_stats_t *stats);
void modbus_reset_stats(modbus_ctx_t *ctx);

/* Low-level functions */
int modbus_send_raw(modbus_ctx_t *ctx, const uint8_t *data, size_t length);
int modbus_receive_raw(modbus_ctx_t *ctx, uint8_t *data, size_t max_length);
uint16_t modbus_calc_crc(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_MODBUS_H */
