/* EMBODIOS Modbus TCP Protocol Implementation
 *
 * Industrial automation protocol for SCADA integration.
 */

#include <embodios/modbus.h>
#include <embodios/tcpip.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* ============================================================================
 * Global State
 * ============================================================================ */

static modbus_stats_t global_stats;
static bool modbus_initialized = false;

/* ============================================================================
 * Byte Order Conversion (Big-endian for Modbus)
 * ============================================================================ */

static inline uint16_t modbus_htons(uint16_t val)
{
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

static inline uint16_t modbus_ntohs(uint16_t val)
{
    return modbus_htons(val);
}

/* ============================================================================
 * CRC-16 (for Modbus RTU)
 * ============================================================================ */

uint16_t modbus_calc_crc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < length; i++) {
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

/* ============================================================================
 * PDU Encoding Functions
 * ============================================================================ */

static int encode_read_bits(uint8_t *pdu, size_t max_len, uint8_t function_code,
                           uint16_t start_addr, uint16_t quantity)
{
    if (max_len < 5) return MODBUS_ERROR;

    modbus_read_bits_req_t *req = (modbus_read_bits_req_t *)pdu;
    req->function_code = function_code;
    req->start_addr = modbus_htons(start_addr);
    req->quantity = modbus_htons(quantity);

    return 5;  /* Function code (1) + start_addr (2) + quantity (2) */
}

static int encode_read_regs(uint8_t *pdu, size_t max_len, uint8_t function_code,
                           uint16_t start_addr, uint16_t quantity)
{
    if (max_len < 5) return MODBUS_ERROR;

    modbus_read_regs_req_t *req = (modbus_read_regs_req_t *)pdu;
    req->function_code = function_code;
    req->start_addr = modbus_htons(start_addr);
    req->quantity = modbus_htons(quantity);

    return 5;  /* Function code (1) + start_addr (2) + quantity (2) */
}

static int encode_write_single_coil(uint8_t *pdu, size_t max_len,
                                   uint16_t addr, bool value)
{
    if (max_len < 5) return MODBUS_ERROR;

    modbus_write_single_coil_req_t *req = (modbus_write_single_coil_req_t *)pdu;
    req->function_code = MODBUS_FC_WRITE_SINGLE_COIL;
    req->output_addr = modbus_htons(addr);
    req->output_value = modbus_htons(value ? 0xFF00 : 0x0000);

    return 5;  /* Function code (1) + addr (2) + value (2) */
}

static int encode_write_single_reg(uint8_t *pdu, size_t max_len,
                                  uint16_t addr, uint16_t value)
{
    if (max_len < 5) return MODBUS_ERROR;

    modbus_write_single_reg_req_t *req = (modbus_write_single_reg_req_t *)pdu;
    req->function_code = MODBUS_FC_WRITE_SINGLE_REG;
    req->reg_addr = modbus_htons(addr);
    req->reg_value = modbus_htons(value);

    return 5;  /* Function code (1) + addr (2) + value (2) */
}

static int encode_write_multiple_coils(uint8_t *pdu, size_t max_len,
                                      uint16_t start_addr, uint16_t quantity,
                                      const uint8_t *values)
{
    size_t byte_count = (quantity + 7) / 8;
    size_t total_len = 6 + byte_count;

    if (max_len < total_len) return MODBUS_ERROR;
    if (quantity > MODBUS_MAX_WRITE_COILS) return MODBUS_INVALID_ARG;

    modbus_write_multiple_coils_req_t *req = (modbus_write_multiple_coils_req_t *)pdu;
    req->function_code = MODBUS_FC_WRITE_MULTIPLE_COILS;
    req->start_addr = modbus_htons(start_addr);
    req->quantity = modbus_htons(quantity);
    req->byte_count = (uint8_t)byte_count;

    memcpy(req->data, values, byte_count);

    return (int)total_len;
}

static int encode_write_multiple_regs(uint8_t *pdu, size_t max_len,
                                     uint16_t start_addr, uint16_t quantity,
                                     const uint16_t *values)
{
    size_t byte_count = quantity * 2;
    size_t total_len = 6 + byte_count;

    if (max_len < total_len) return MODBUS_ERROR;
    if (quantity > MODBUS_MAX_WRITE_REGS) return MODBUS_INVALID_ARG;

    modbus_write_multiple_regs_req_t *req = (modbus_write_multiple_regs_req_t *)pdu;
    req->function_code = MODBUS_FC_WRITE_MULTIPLE_REGS;
    req->start_addr = modbus_htons(start_addr);
    req->quantity = modbus_htons(quantity);
    req->byte_count = (uint8_t)byte_count;

    /* Convert register values to big-endian */
    for (uint16_t i = 0; i < quantity; i++) {
        req->data[i] = modbus_htons(values[i]);
    }

    return (int)total_len;
}

static int encode_exception(uint8_t *pdu, size_t max_len,
                           uint8_t function_code, uint8_t exception_code)
{
    if (max_len < 2) return MODBUS_ERROR;

    modbus_exception_resp_t *resp = (modbus_exception_resp_t *)pdu;
    resp->function_code = function_code | MODBUS_EXCEPTION_OFFSET;
    resp->exception_code = exception_code;

    return 2;
}

/* ============================================================================
 * PDU Decoding Functions
 * ============================================================================ */

static int decode_read_bits_response(const uint8_t *pdu, size_t pdu_len,
                                    uint8_t *dest, uint16_t expected_quantity)
{
    if (pdu_len < 2) return MODBUS_INVALID_RESP;

    const modbus_read_bits_resp_t *resp = (const modbus_read_bits_resp_t *)pdu;

    /* Check if this is an exception response */
    if (resp->function_code & MODBUS_EXCEPTION_OFFSET) {
        return MODBUS_EXCEPTION;
    }

    size_t expected_bytes = (expected_quantity + 7) / 8;
    if (resp->byte_count != expected_bytes) {
        return MODBUS_INVALID_RESP;
    }

    if (pdu_len < 2 + resp->byte_count) {
        return MODBUS_INVALID_RESP;
    }

    memcpy(dest, resp->data, resp->byte_count);
    return MODBUS_OK;
}

static int decode_read_regs_response(const uint8_t *pdu, size_t pdu_len,
                                    uint16_t *dest, uint16_t expected_quantity)
{
    if (pdu_len < 2) return MODBUS_INVALID_RESP;

    const modbus_read_regs_resp_t *resp = (const modbus_read_regs_resp_t *)pdu;

    /* Check if this is an exception response */
    if (resp->function_code & MODBUS_EXCEPTION_OFFSET) {
        return MODBUS_EXCEPTION;
    }

    size_t expected_bytes = expected_quantity * 2;
    if (resp->byte_count != expected_bytes) {
        return MODBUS_INVALID_RESP;
    }

    if (pdu_len < 2 + resp->byte_count) {
        return MODBUS_INVALID_RESP;
    }

    /* Convert from big-endian to host byte order */
    for (uint16_t i = 0; i < expected_quantity; i++) {
        dest[i] = modbus_ntohs(resp->data[i]);
    }

    return MODBUS_OK;
}

static int decode_write_response(const uint8_t *pdu, size_t pdu_len,
                                uint16_t *addr_out, uint16_t *quantity_out)
{
    if (pdu_len < 5) return MODBUS_INVALID_RESP;

    /* Check if this is an exception response */
    if (pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        return MODBUS_EXCEPTION;
    }

    const modbus_write_multiple_resp_t *resp = (const modbus_write_multiple_resp_t *)pdu;

    if (addr_out) *addr_out = modbus_ntohs(resp->start_addr);
    if (quantity_out) *quantity_out = modbus_ntohs(resp->quantity);

    return MODBUS_OK;
}

static uint8_t decode_exception_code(const uint8_t *pdu, size_t pdu_len)
{
    if (pdu_len < 2) return 0;

    const modbus_exception_resp_t *resp = (const modbus_exception_resp_t *)pdu;

    if (resp->function_code & MODBUS_EXCEPTION_OFFSET) {
        return resp->exception_code;
    }

    return 0;
}

/* ============================================================================
 * MBAP Header Functions
 * ============================================================================ */

static int encode_mbap_header(uint8_t *buffer, size_t max_len,
                             uint16_t transaction_id, uint8_t unit_id,
                             uint16_t pdu_length)
{
    if (max_len < MODBUS_MBAP_SIZE) return MODBUS_ERROR;

    modbus_mbap_header_t *mbap = (modbus_mbap_header_t *)buffer;
    mbap->transaction_id = modbus_htons(transaction_id);
    mbap->protocol_id = modbus_htons(MODBUS_PROTOCOL_ID);
    mbap->length = modbus_htons(pdu_length + 1);  /* +1 for unit_id */
    mbap->unit_id = unit_id;

    return MODBUS_MBAP_SIZE;
}

static int decode_mbap_header(const uint8_t *buffer, size_t buf_len,
                             uint16_t *transaction_id, uint8_t *unit_id,
                             uint16_t *pdu_length)
{
    if (buf_len < MODBUS_MBAP_SIZE) return MODBUS_INVALID_RESP;

    const modbus_mbap_header_t *mbap = (const modbus_mbap_header_t *)buffer;

    if (transaction_id) *transaction_id = modbus_ntohs(mbap->transaction_id);
    if (unit_id) *unit_id = mbap->unit_id;
    if (pdu_length) {
        uint16_t len = modbus_ntohs(mbap->length);
        *pdu_length = len > 0 ? len - 1 : 0;  /* -1 for unit_id */
    }

    /* Verify protocol ID */
    if (modbus_ntohs(mbap->protocol_id) != MODBUS_PROTOCOL_ID) {
        return MODBUS_INVALID_RESP;
    }

    return MODBUS_OK;
}

/* ============================================================================
 * Context Management
 * ============================================================================ */

modbus_ctx_t* modbus_new_tcp(uint32_t ip, uint16_t port, uint8_t unit_id)
{
    modbus_ctx_t *ctx = (modbus_ctx_t *)kzalloc(sizeof(modbus_ctx_t));
    if (!ctx) return NULL;

    ctx->mode = MODBUS_MODE_TCP;
    ctx->state = MODBUS_STATE_DISCONNECTED;
    ctx->socket_fd = -1;
    ctx->remote_ip = ip;
    ctx->remote_port = port ? port : MODBUS_TCP_PORT;
    ctx->unit_id = unit_id;
    ctx->transaction_id = 1;
    ctx->timeout_ms = MODBUS_TIMEOUT_MS;

    if (!modbus_initialized) {
        memset(&global_stats, 0, sizeof(global_stats));
        modbus_initialized = true;
    }

    return ctx;
}

void modbus_free(modbus_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->state == MODBUS_STATE_CONNECTED) {
        modbus_disconnect(ctx);
    }

    kfree(ctx);
}

int modbus_set_timeout(modbus_ctx_t *ctx, uint32_t timeout_ms)
{
    if (!ctx) return MODBUS_INVALID_ARG;

    ctx->timeout_ms = timeout_ms;
    return MODBUS_OK;
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static int modbus_send_request(modbus_ctx_t *ctx, const uint8_t *pdu, size_t pdu_len)
{
    if (!ctx || !pdu) return MODBUS_INVALID_ARG;

    /* Build MBAP header */
    int mbap_len = encode_mbap_header(ctx->tx_buffer, sizeof(ctx->tx_buffer),
                                     ctx->transaction_id, ctx->unit_id, pdu_len);
    if (mbap_len < 0) return mbap_len;

    /* Copy PDU after MBAP header */
    if (MODBUS_MBAP_SIZE + pdu_len > sizeof(ctx->tx_buffer)) {
        return MODBUS_ERROR;
    }
    memcpy(ctx->tx_buffer + MODBUS_MBAP_SIZE, pdu, pdu_len);

    /* Send complete message */
    size_t total_len = MODBUS_MBAP_SIZE + pdu_len;
    int ret = modbus_send_raw(ctx, ctx->tx_buffer, total_len);
    if (ret < 0) return ret;

    /* Increment transaction ID for next request */
    ctx->transaction_id++;
    if (ctx->transaction_id == 0) ctx->transaction_id = 1;

    global_stats.requests_sent++;
    return MODBUS_OK;
}

static int modbus_receive_response(modbus_ctx_t *ctx, uint8_t **pdu_out, size_t *pdu_len_out)
{
    if (!ctx || !pdu_out || !pdu_len_out) return MODBUS_INVALID_ARG;

    /* Receive MBAP header first */
    int ret = modbus_receive_raw(ctx, ctx->rx_buffer, MODBUS_MBAP_SIZE);
    if (ret < MODBUS_MBAP_SIZE) {
        return MODBUS_TIMEOUT;
    }

    /* Decode MBAP header */
    uint16_t transaction_id;
    uint8_t unit_id;
    uint16_t pdu_length;
    ret = decode_mbap_header(ctx->rx_buffer, MODBUS_MBAP_SIZE,
                            &transaction_id, &unit_id, &pdu_length);
    if (ret != MODBUS_OK) {
        return MODBUS_INVALID_RESP;
    }

    /* Verify transaction ID matches */
    if (transaction_id != ctx->transaction_id - 1) {
        return MODBUS_INVALID_RESP;
    }

    /* Receive PDU */
    if (pdu_length > MODBUS_MAX_PDU_SIZE) {
        return MODBUS_INVALID_RESP;
    }

    ret = modbus_receive_raw(ctx, ctx->rx_buffer + MODBUS_MBAP_SIZE, pdu_length);
    if (ret < (int)pdu_length) {
        return MODBUS_TIMEOUT;
    }

    /* Return pointer to PDU and its length */
    *pdu_out = ctx->rx_buffer + MODBUS_MBAP_SIZE;
    *pdu_len_out = pdu_length;

    global_stats.responses_received++;
    return MODBUS_OK;
}

/* ============================================================================
 * Connection Management
 * ============================================================================ */

int modbus_connect(modbus_ctx_t *ctx)
{
    if (!ctx) return MODBUS_INVALID_ARG;
    if (ctx->state == MODBUS_STATE_CONNECTED) return MODBUS_OK;
    if (ctx->mode != MODBUS_MODE_TCP) return MODBUS_ERROR;

    /* Create TCP socket */
    ctx->socket_fd = socket_create(SOCK_STREAM, IP_PROTO_TCP);
    if (ctx->socket_fd < 0) {
        ctx->state = MODBUS_STATE_ERROR;
        return MODBUS_CONN_FAILED;
    }

    /* Connect to remote server */
    ctx->state = MODBUS_STATE_CONNECTING;
    int ret = socket_connect(ctx->socket_fd, ctx->remote_ip, ctx->remote_port);
    if (ret != NET_OK) {
        socket_close(ctx->socket_fd);
        ctx->socket_fd = -1;
        ctx->state = MODBUS_STATE_ERROR;
        return MODBUS_CONN_FAILED;
    }

    ctx->state = MODBUS_STATE_CONNECTED;
    return MODBUS_OK;
}

int modbus_disconnect(modbus_ctx_t *ctx)
{
    if (!ctx) return MODBUS_INVALID_ARG;

    if (ctx->socket_fd >= 0) {
        socket_close(ctx->socket_fd);
        ctx->socket_fd = -1;
    }

    ctx->state = MODBUS_STATE_DISCONNECTED;
    return MODBUS_OK;
}

bool modbus_is_connected(modbus_ctx_t *ctx)
{
    if (!ctx) return false;

    return ctx->state == MODBUS_STATE_CONNECTED;
}

/* ============================================================================
 * Client Functions
 * ============================================================================ */

int modbus_read_coils(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, uint8_t *dest)
{
    if (!ctx || !dest) return MODBUS_INVALID_ARG;
    if (count > MODBUS_MAX_COILS) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    /* Build request PDU */
    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int pdu_len = encode_read_bits(pdu, sizeof(pdu), MODBUS_FC_READ_COILS, addr, count);
    if (pdu_len < 0) return pdu_len;

    /* Send request */
    int ret = modbus_send_request(ctx, pdu, pdu_len);
    if (ret != MODBUS_OK) return ret;

    /* Receive response */
    uint8_t *resp_pdu;
    size_t resp_len;
    ret = modbus_receive_response(ctx, &resp_pdu, &resp_len);
    if (ret != MODBUS_OK) return ret;

    /* Check for exception */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_received++;
        return MODBUS_EXCEPTION;
    }

    /* Decode response */
    ret = decode_read_bits_response(resp_pdu, resp_len, dest, count);
    return ret;
}

int modbus_read_discrete_inputs(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, uint8_t *dest)
{
    if (!ctx || !dest) return MODBUS_INVALID_ARG;
    if (count > MODBUS_MAX_COILS) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    /* Build request PDU */
    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int pdu_len = encode_read_bits(pdu, sizeof(pdu), MODBUS_FC_READ_DISCRETE_INPUTS, addr, count);
    if (pdu_len < 0) return pdu_len;

    /* Send request */
    int ret = modbus_send_request(ctx, pdu, pdu_len);
    if (ret != MODBUS_OK) return ret;

    /* Receive response */
    uint8_t *resp_pdu;
    size_t resp_len;
    ret = modbus_receive_response(ctx, &resp_pdu, &resp_len);
    if (ret != MODBUS_OK) return ret;

    /* Check for exception */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_received++;
        return MODBUS_EXCEPTION;
    }

    /* Decode response */
    ret = decode_read_bits_response(resp_pdu, resp_len, dest, count);
    return ret;
}

int modbus_read_holding_registers(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, uint16_t *dest)
{
    if (!ctx || !dest) return MODBUS_INVALID_ARG;
    if (count > MODBUS_MAX_REGISTERS) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    /* Build request PDU */
    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int pdu_len = encode_read_regs(pdu, sizeof(pdu), MODBUS_FC_READ_HOLDING_REGS, addr, count);
    if (pdu_len < 0) return pdu_len;

    /* Send request */
    int ret = modbus_send_request(ctx, pdu, pdu_len);
    if (ret != MODBUS_OK) return ret;

    /* Receive response */
    uint8_t *resp_pdu;
    size_t resp_len;
    ret = modbus_receive_response(ctx, &resp_pdu, &resp_len);
    if (ret != MODBUS_OK) return ret;

    /* Check for exception */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_received++;
        return MODBUS_EXCEPTION;
    }

    /* Decode response */
    ret = decode_read_regs_response(resp_pdu, resp_len, dest, count);
    return ret;
}

int modbus_read_input_registers(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, uint16_t *dest)
{
    if (!ctx || !dest) return MODBUS_INVALID_ARG;
    if (count > MODBUS_MAX_REGISTERS) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    /* Build request PDU */
    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int pdu_len = encode_read_regs(pdu, sizeof(pdu), MODBUS_FC_READ_INPUT_REGS, addr, count);
    if (pdu_len < 0) return pdu_len;

    /* Send request */
    int ret = modbus_send_request(ctx, pdu, pdu_len);
    if (ret != MODBUS_OK) return ret;

    /* Receive response */
    uint8_t *resp_pdu;
    size_t resp_len;
    ret = modbus_receive_response(ctx, &resp_pdu, &resp_len);
    if (ret != MODBUS_OK) return ret;

    /* Check for exception */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_received++;
        return MODBUS_EXCEPTION;
    }

    /* Decode response */
    ret = decode_read_regs_response(resp_pdu, resp_len, dest, count);
    return ret;
}

int modbus_write_coil(modbus_ctx_t *ctx, uint16_t addr, bool value)
{
    if (!ctx) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    /* Build request PDU */
    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int pdu_len = encode_write_single_coil(pdu, sizeof(pdu), addr, value);
    if (pdu_len < 0) return pdu_len;

    /* Send request */
    int ret = modbus_send_request(ctx, pdu, pdu_len);
    if (ret != MODBUS_OK) return ret;

    /* Receive response */
    uint8_t *resp_pdu;
    size_t resp_len;
    ret = modbus_receive_response(ctx, &resp_pdu, &resp_len);
    if (ret != MODBUS_OK) return ret;

    /* Check for exception */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_received++;
        return MODBUS_EXCEPTION;
    }

    /* Verify response matches request */
    if (resp_len < 5) return MODBUS_INVALID_RESP;

    return MODBUS_OK;
}

int modbus_write_register(modbus_ctx_t *ctx, uint16_t addr, uint16_t value)
{
    if (!ctx) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    /* Build request PDU */
    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int pdu_len = encode_write_single_reg(pdu, sizeof(pdu), addr, value);
    if (pdu_len < 0) return pdu_len;

    /* Send request */
    int ret = modbus_send_request(ctx, pdu, pdu_len);
    if (ret != MODBUS_OK) return ret;

    /* Receive response */
    uint8_t *resp_pdu;
    size_t resp_len;
    ret = modbus_receive_response(ctx, &resp_pdu, &resp_len);
    if (ret != MODBUS_OK) return ret;

    /* Check for exception */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_received++;
        return MODBUS_EXCEPTION;
    }

    /* Verify response matches request */
    if (resp_len < 5) return MODBUS_INVALID_RESP;

    return MODBUS_OK;
}

int modbus_write_coils(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, const uint8_t *src)
{
    if (!ctx || !src) return MODBUS_INVALID_ARG;
    if (count > MODBUS_MAX_WRITE_COILS) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    /* Build request PDU */
    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int pdu_len = encode_write_multiple_coils(pdu, sizeof(pdu), addr, count, src);
    if (pdu_len < 0) return pdu_len;

    /* Send request */
    int ret = modbus_send_request(ctx, pdu, pdu_len);
    if (ret != MODBUS_OK) return ret;

    /* Receive response */
    uint8_t *resp_pdu;
    size_t resp_len;
    ret = modbus_receive_response(ctx, &resp_pdu, &resp_len);
    if (ret != MODBUS_OK) return ret;

    /* Check for exception */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_received++;
        return MODBUS_EXCEPTION;
    }

    /* Decode response to verify */
    uint16_t resp_addr, resp_count;
    ret = decode_write_response(resp_pdu, resp_len, &resp_addr, &resp_count);
    if (ret != MODBUS_OK) return ret;
    if (resp_addr != addr || resp_count != count) {
        return MODBUS_INVALID_RESP;
    }

    return MODBUS_OK;
}

int modbus_write_registers(modbus_ctx_t *ctx, uint16_t addr, uint16_t count, const uint16_t *src)
{
    if (!ctx || !src) return MODBUS_INVALID_ARG;
    if (count > MODBUS_MAX_WRITE_REGS) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    /* Build request PDU */
    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int pdu_len = encode_write_multiple_regs(pdu, sizeof(pdu), addr, count, src);
    if (pdu_len < 0) return pdu_len;

    /* Send request */
    int ret = modbus_send_request(ctx, pdu, pdu_len);
    if (ret != MODBUS_OK) return ret;

    /* Receive response */
    uint8_t *resp_pdu;
    size_t resp_len;
    ret = modbus_receive_response(ctx, &resp_pdu, &resp_len);
    if (ret != MODBUS_OK) return ret;

    /* Check for exception */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_received++;
        return MODBUS_EXCEPTION;
    }

    /* Decode response to verify */
    uint16_t resp_addr, resp_count;
    ret = decode_write_response(resp_pdu, resp_len, &resp_addr, &resp_count);
    if (ret != MODBUS_OK) return ret;
    if (resp_addr != addr || resp_count != count) {
        return MODBUS_INVALID_RESP;
    }

    return MODBUS_OK;
}

/* ============================================================================
 * Server Request Handlers
 * ============================================================================ */

static int handle_read_coils(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                             uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 5) return MODBUS_INVALID_RESP;

    const modbus_read_bits_req_t *req = (const modbus_read_bits_req_t *)req_pdu;
    uint16_t start_addr = modbus_ntohs(req->start_addr);
    uint16_t quantity = modbus_ntohs(req->quantity);

    /* Validate request */
    if (quantity == 0 || quantity > MODBUS_MAX_COILS) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (!ctx->coils || start_addr + quantity > ctx->num_coils) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    }

    /* Build response */
    modbus_read_bits_resp_t *resp = (modbus_read_bits_resp_t *)resp_pdu;
    size_t byte_count = (quantity + 7) / 8;

    if (max_resp_len < 2 + byte_count) return MODBUS_ERROR;

    resp->function_code = req->function_code;
    resp->byte_count = (uint8_t)byte_count;

    /* Pack coil values into bytes */
    memset(resp->data, 0, byte_count);
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t coil_idx = start_addr + i;
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;

        if (ctx->coils[coil_idx]) {
            resp->data[byte_idx] |= (1 << bit_idx);
        }
    }

    return 2 + byte_count;
}

static int handle_read_discrete_inputs(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                                       uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 5) return MODBUS_INVALID_RESP;

    const modbus_read_bits_req_t *req = (const modbus_read_bits_req_t *)req_pdu;
    uint16_t start_addr = modbus_ntohs(req->start_addr);
    uint16_t quantity = modbus_ntohs(req->quantity);

    /* Validate request */
    if (quantity == 0 || quantity > MODBUS_MAX_COILS) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (!ctx->discrete_inputs || start_addr + quantity > ctx->num_discrete_inputs) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    }

    /* Build response */
    modbus_read_bits_resp_t *resp = (modbus_read_bits_resp_t *)resp_pdu;
    size_t byte_count = (quantity + 7) / 8;

    if (max_resp_len < 2 + byte_count) return MODBUS_ERROR;

    resp->function_code = req->function_code;
    resp->byte_count = (uint8_t)byte_count;

    /* Pack input values into bytes */
    memset(resp->data, 0, byte_count);
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t input_idx = start_addr + i;
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;

        if (ctx->discrete_inputs[input_idx]) {
            resp->data[byte_idx] |= (1 << bit_idx);
        }
    }

    return 2 + byte_count;
}

static int handle_read_holding_registers(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                                         uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 5) return MODBUS_INVALID_RESP;

    const modbus_read_regs_req_t *req = (const modbus_read_regs_req_t *)req_pdu;
    uint16_t start_addr = modbus_ntohs(req->start_addr);
    uint16_t quantity = modbus_ntohs(req->quantity);

    /* Validate request */
    if (quantity == 0 || quantity > MODBUS_MAX_REGISTERS) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (!ctx->holding_regs || start_addr + quantity > ctx->num_holding_regs) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    }

    /* Build response */
    modbus_read_regs_resp_t *resp = (modbus_read_regs_resp_t *)resp_pdu;
    size_t byte_count = quantity * 2;

    if (max_resp_len < 2 + byte_count) return MODBUS_ERROR;

    resp->function_code = req->function_code;
    resp->byte_count = (uint8_t)byte_count;

    /* Copy register values and convert to big-endian */
    for (uint16_t i = 0; i < quantity; i++) {
        resp->data[i] = modbus_htons(ctx->holding_regs[start_addr + i]);
    }

    return 2 + byte_count;
}

static int handle_read_input_registers(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                                       uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 5) return MODBUS_INVALID_RESP;

    const modbus_read_regs_req_t *req = (const modbus_read_regs_req_t *)req_pdu;
    uint16_t start_addr = modbus_ntohs(req->start_addr);
    uint16_t quantity = modbus_ntohs(req->quantity);

    /* Validate request */
    if (quantity == 0 || quantity > MODBUS_MAX_REGISTERS) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (!ctx->input_regs || start_addr + quantity > ctx->num_input_regs) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    }

    /* Build response */
    modbus_read_regs_resp_t *resp = (modbus_read_regs_resp_t *)resp_pdu;
    size_t byte_count = quantity * 2;

    if (max_resp_len < 2 + byte_count) return MODBUS_ERROR;

    resp->function_code = req->function_code;
    resp->byte_count = (uint8_t)byte_count;

    /* Copy register values and convert to big-endian */
    for (uint16_t i = 0; i < quantity; i++) {
        resp->data[i] = modbus_htons(ctx->input_regs[start_addr + i]);
    }

    return 2 + byte_count;
}

static int handle_write_single_coil(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                                    uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 5) return MODBUS_INVALID_RESP;

    const modbus_write_single_coil_req_t *req = (const modbus_write_single_coil_req_t *)req_pdu;
    uint16_t addr = modbus_ntohs(req->output_addr);
    uint16_t value = modbus_ntohs(req->output_value);

    /* Validate request */
    if (value != 0x0000 && value != 0xFF00) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (!ctx->coils || addr >= ctx->num_coils) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    }

    /* Write coil value */
    ctx->coils[addr] = (value == 0xFF00) ? 1 : 0;

    /* Echo request as response */
    if (max_resp_len < 5) return MODBUS_ERROR;
    memcpy(resp_pdu, req_pdu, 5);

    return 5;
}

static int handle_write_single_register(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                                        uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 5) return MODBUS_INVALID_RESP;

    const modbus_write_single_reg_req_t *req = (const modbus_write_single_reg_req_t *)req_pdu;
    uint16_t addr = modbus_ntohs(req->reg_addr);
    uint16_t value = modbus_ntohs(req->reg_value);

    /* Validate request */
    if (!ctx->holding_regs || addr >= ctx->num_holding_regs) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    }

    /* Write register value */
    ctx->holding_regs[addr] = value;

    /* Echo request as response */
    if (max_resp_len < 5) return MODBUS_ERROR;
    memcpy(resp_pdu, req_pdu, 5);

    return 5;
}

static int handle_write_multiple_coils(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                                       uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 6) return MODBUS_INVALID_RESP;

    const modbus_write_multiple_coils_req_t *req = (const modbus_write_multiple_coils_req_t *)req_pdu;
    uint16_t start_addr = modbus_ntohs(req->start_addr);
    uint16_t quantity = modbus_ntohs(req->quantity);
    uint8_t byte_count = req->byte_count;

    /* Validate request */
    if (quantity == 0 || quantity > MODBUS_MAX_WRITE_COILS) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (byte_count != (quantity + 7) / 8) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (req_len < 6 + byte_count) return MODBUS_INVALID_RESP;

    if (!ctx->coils || start_addr + quantity > ctx->num_coils) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    }

    /* Write coil values */
    for (uint16_t i = 0; i < quantity; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;
        ctx->coils[start_addr + i] = (req->data[byte_idx] & (1 << bit_idx)) ? 1 : 0;
    }

    /* Build response */
    if (max_resp_len < 5) return MODBUS_ERROR;

    modbus_write_multiple_resp_t *resp = (modbus_write_multiple_resp_t *)resp_pdu;
    resp->function_code = req->function_code;
    resp->start_addr = req->start_addr;
    resp->quantity = req->quantity;

    return 5;
}

static int handle_write_multiple_registers(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                                           uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 6) return MODBUS_INVALID_RESP;

    const modbus_write_multiple_regs_req_t *req = (const modbus_write_multiple_regs_req_t *)req_pdu;
    uint16_t start_addr = modbus_ntohs(req->start_addr);
    uint16_t quantity = modbus_ntohs(req->quantity);
    uint8_t byte_count = req->byte_count;

    /* Validate request */
    if (quantity == 0 || quantity > MODBUS_MAX_WRITE_REGS) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (byte_count != quantity * 2) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE);
    }

    if (req_len < 6 + byte_count) return MODBUS_INVALID_RESP;

    if (!ctx->holding_regs || start_addr + quantity > ctx->num_holding_regs) {
        return encode_exception(resp_pdu, max_resp_len, req->function_code,
                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    }

    /* Write register values */
    for (uint16_t i = 0; i < quantity; i++) {
        ctx->holding_regs[start_addr + i] = modbus_ntohs(req->data[i]);
    }

    /* Build response */
    if (max_resp_len < 5) return MODBUS_ERROR;

    modbus_write_multiple_resp_t *resp = (modbus_write_multiple_resp_t *)resp_pdu;
    resp->function_code = req->function_code;
    resp->start_addr = req->start_addr;
    resp->quantity = req->quantity;

    return 5;
}

static int handle_request(modbus_ctx_t *ctx, const uint8_t *req_pdu, size_t req_len,
                         uint8_t *resp_pdu, size_t max_resp_len)
{
    if (req_len < 1) return MODBUS_INVALID_RESP;

    uint8_t function_code = req_pdu[0];

    /* Dispatch to appropriate handler */
    switch (function_code) {
        case MODBUS_FC_READ_COILS:
            return handle_read_coils(ctx, req_pdu, req_len, resp_pdu, max_resp_len);

        case MODBUS_FC_READ_DISCRETE_INPUTS:
            return handle_read_discrete_inputs(ctx, req_pdu, req_len, resp_pdu, max_resp_len);

        case MODBUS_FC_READ_HOLDING_REGS:
            return handle_read_holding_registers(ctx, req_pdu, req_len, resp_pdu, max_resp_len);

        case MODBUS_FC_READ_INPUT_REGS:
            return handle_read_input_registers(ctx, req_pdu, req_len, resp_pdu, max_resp_len);

        case MODBUS_FC_WRITE_SINGLE_COIL:
            return handle_write_single_coil(ctx, req_pdu, req_len, resp_pdu, max_resp_len);

        case MODBUS_FC_WRITE_SINGLE_REG:
            return handle_write_single_register(ctx, req_pdu, req_len, resp_pdu, max_resp_len);

        case MODBUS_FC_WRITE_MULTIPLE_COILS:
            return handle_write_multiple_coils(ctx, req_pdu, req_len, resp_pdu, max_resp_len);

        case MODBUS_FC_WRITE_MULTIPLE_REGS:
            return handle_write_multiple_registers(ctx, req_pdu, req_len, resp_pdu, max_resp_len);

        default:
            /* Unsupported function code */
            return encode_exception(resp_pdu, max_resp_len, function_code,
                                   MODBUS_EXCEPTION_ILLEGAL_FUNCTION);
    }
}

/* ============================================================================
 * Server Functions
 * ============================================================================ */

int modbus_server_init(modbus_ctx_t *ctx, uint16_t port)
{
    if (!ctx) return MODBUS_INVALID_ARG;

    /* Set up server mode */
    ctx->mode = MODBUS_MODE_TCP;
    ctx->remote_port = port ? port : MODBUS_TCP_PORT;
    ctx->state = MODBUS_STATE_DISCONNECTED;

    /* Create TCP socket */
    ctx->socket_fd = socket_create(SOCK_STREAM, IP_PROTO_TCP);
    if (ctx->socket_fd < 0) {
        return MODBUS_ERROR;
    }

    /* Bind to port */
    int ret = socket_bind(ctx->socket_fd, 0, ctx->remote_port);
    if (ret != NET_OK) {
        socket_close(ctx->socket_fd);
        ctx->socket_fd = -1;
        return MODBUS_ERROR;
    }

    return MODBUS_OK;
}

int modbus_server_start(modbus_ctx_t *ctx)
{
    if (!ctx) return MODBUS_INVALID_ARG;
    if (ctx->socket_fd < 0) return MODBUS_ERROR;

    /* Start listening for connections */
    int ret = socket_listen(ctx->socket_fd, 1);
    if (ret != NET_OK) {
        return MODBUS_ERROR;
    }

    ctx->state = MODBUS_STATE_CONNECTED;
    return MODBUS_OK;
}

int modbus_server_stop(modbus_ctx_t *ctx)
{
    if (!ctx) return MODBUS_INVALID_ARG;

    if (ctx->socket_fd >= 0) {
        socket_close(ctx->socket_fd);
        ctx->socket_fd = -1;
    }

    ctx->state = MODBUS_STATE_DISCONNECTED;
    return MODBUS_OK;
}

int modbus_server_process(modbus_ctx_t *ctx)
{
    if (!ctx) return MODBUS_INVALID_ARG;
    if (ctx->state != MODBUS_STATE_CONNECTED) return MODBUS_NOT_CONNECTED;

    /* Try to receive MBAP header */
    int ret = socket_recv(ctx->socket_fd, ctx->rx_buffer, MODBUS_MBAP_SIZE);
    if (ret < MODBUS_MBAP_SIZE) {
        /* No data available or incomplete header */
        return MODBUS_OK;
    }

    /* Decode MBAP header */
    uint16_t transaction_id;
    uint8_t unit_id;
    uint16_t pdu_length;
    ret = decode_mbap_header(ctx->rx_buffer, MODBUS_MBAP_SIZE,
                            &transaction_id, &unit_id, &pdu_length);
    if (ret != MODBUS_OK) {
        global_stats.invalid_responses++;
        return MODBUS_INVALID_RESP;
    }

    /* Validate PDU length */
    if (pdu_length > MODBUS_MAX_PDU_SIZE) {
        global_stats.invalid_responses++;
        return MODBUS_INVALID_RESP;
    }

    /* Receive PDU */
    ret = socket_recv(ctx->socket_fd, ctx->rx_buffer + MODBUS_MBAP_SIZE, pdu_length);
    if (ret < (int)pdu_length) {
        global_stats.timeouts++;
        return MODBUS_TIMEOUT;
    }

    global_stats.requests_received++;

    /* Process request and generate response */
    uint8_t resp_pdu[MODBUS_MAX_PDU_SIZE];
    int resp_pdu_len = handle_request(ctx, ctx->rx_buffer + MODBUS_MBAP_SIZE, pdu_length,
                                      resp_pdu, sizeof(resp_pdu));
    if (resp_pdu_len < 0) {
        return resp_pdu_len;
    }

    /* Check if this is an exception response */
    if (resp_pdu[0] & MODBUS_EXCEPTION_OFFSET) {
        global_stats.exceptions_sent++;
    }

    /* Build MBAP header for response */
    int mbap_len = encode_mbap_header(ctx->tx_buffer, sizeof(ctx->tx_buffer),
                                     transaction_id, unit_id, resp_pdu_len);
    if (mbap_len < 0) return mbap_len;

    /* Copy response PDU */
    if (MODBUS_MBAP_SIZE + (size_t)resp_pdu_len > sizeof(ctx->tx_buffer)) {
        return MODBUS_ERROR;
    }
    memcpy(ctx->tx_buffer + MODBUS_MBAP_SIZE, resp_pdu, resp_pdu_len);

    /* Send response */
    size_t total_len = MODBUS_MBAP_SIZE + resp_pdu_len;
    ret = socket_send(ctx->socket_fd, ctx->tx_buffer, total_len);
    if (ret < 0) {
        return MODBUS_ERROR;
    }

    global_stats.responses_sent++;
    global_stats.bytes_sent += total_len;

    return MODBUS_OK;
}

int modbus_server_set_data(modbus_ctx_t *ctx, uint16_t *holding_regs, uint16_t num_holding,
                           uint16_t *input_regs, uint16_t num_input,
                           uint8_t *coils, uint16_t num_coils,
                           uint8_t *discrete_inputs, uint16_t num_discrete)
{
    if (!ctx) return MODBUS_INVALID_ARG;

    ctx->holding_regs = holding_regs;
    ctx->num_holding_regs = num_holding;
    ctx->input_regs = input_regs;
    ctx->num_input_regs = num_input;
    ctx->coils = coils;
    ctx->num_coils = num_coils;
    ctx->discrete_inputs = discrete_inputs;
    ctx->num_discrete_inputs = num_discrete;

    return MODBUS_OK;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

int modbus_get_last_error(modbus_ctx_t *ctx)
{
    if (!ctx) return MODBUS_INVALID_ARG;

    /* TODO: Add error tracking to context in future subtasks */
    return MODBUS_ERROR;
}

const char* modbus_error_string(int error_code)
{
    switch (error_code) {
        case MODBUS_OK:              return "Success";
        case MODBUS_ERROR:           return "Generic error";
        case MODBUS_TIMEOUT:         return "Operation timeout";
        case MODBUS_INVALID_ARG:     return "Invalid argument";
        case MODBUS_NOT_CONNECTED:   return "Not connected";
        case MODBUS_CONN_FAILED:     return "Connection failed";
        case MODBUS_EXCEPTION:       return "Modbus exception received";
        case MODBUS_INVALID_CRC:     return "Invalid CRC";
        case MODBUS_INVALID_RESP:    return "Invalid response";
        default:                     return "Unknown error";
    }
}

void modbus_get_stats(modbus_ctx_t *ctx, modbus_stats_t *stats)
{
    if (!stats) return;

    /* Return global stats for now */
    memcpy(stats, &global_stats, sizeof(modbus_stats_t));
}

void modbus_reset_stats(modbus_ctx_t *ctx)
{
    memset(&global_stats, 0, sizeof(global_stats));
}

/* ============================================================================
 * Low-level Functions
 * ============================================================================ */

int modbus_send_raw(modbus_ctx_t *ctx, const uint8_t *data, size_t length)
{
    if (!ctx || !data) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    int ret = socket_send(ctx->socket_fd, data, length);
    if (ret < 0) {
        return MODBUS_ERROR;
    }

    global_stats.bytes_sent += length;
    return ret;
}

int modbus_receive_raw(modbus_ctx_t *ctx, uint8_t *data, size_t max_length)
{
    if (!ctx || !data) return MODBUS_INVALID_ARG;
    if (!modbus_is_connected(ctx)) return MODBUS_NOT_CONNECTED;

    int ret = socket_recv(ctx->socket_fd, data, max_length);
    if (ret < 0) {
        return MODBUS_ERROR;
    }

    global_stats.bytes_received += ret;
    return ret;
}
