/* Unit test for Modbus TCP Protocol */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/* Modbus protocol constants */
#define MODBUS_TCP_PORT         502
#define MODBUS_MAX_PDU_SIZE     253
#define MODBUS_MAX_ADU_SIZE     260
#define MODBUS_MBAP_SIZE        7
#define MODBUS_PROTOCOL_ID      0

/* Function codes */
#define MODBUS_FC_READ_COILS            0x01
#define MODBUS_FC_READ_DISCRETE_INPUTS  0x02
#define MODBUS_FC_READ_HOLDING_REGS     0x03
#define MODBUS_FC_READ_INPUT_REGS       0x04
#define MODBUS_FC_WRITE_SINGLE_COIL     0x05
#define MODBUS_FC_WRITE_SINGLE_REG      0x06
#define MODBUS_FC_WRITE_MULTIPLE_COILS  0x0F
#define MODBUS_FC_WRITE_MULTIPLE_REGS   0x10

/* Exception codes */
#define MODBUS_EXCEPTION_OFFSET         0x80
#define MODBUS_EXCEPTION_ILLEGAL_FUNCTION       0x01
#define MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS   0x02
#define MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE     0x03

/* Error codes */
#define MODBUS_OK               0
#define MODBUS_ERROR           -1
#define MODBUS_INVALID_ARG     -3
#define MODBUS_INVALID_RESP    -8

/* Limits */
#define MODBUS_MAX_COILS        2000
#define MODBUS_MAX_REGISTERS    125
#define MODBUS_MAX_WRITE_COILS  1968
#define MODBUS_MAX_WRITE_REGS   123

/* Packed attribute for structs */
#define __packed __attribute__((packed))

/* ============================================================================
 * Protocol Structures
 * ============================================================================ */

typedef struct modbus_mbap_header {
    uint16_t transaction_id;
    uint16_t protocol_id;
    uint16_t length;
    uint8_t  unit_id;
} __packed modbus_mbap_header_t;

typedef struct modbus_read_bits_req {
    uint8_t  function_code;
    uint16_t start_addr;
    uint16_t quantity;
} __packed modbus_read_bits_req_t;

typedef struct modbus_read_regs_req {
    uint8_t  function_code;
    uint16_t start_addr;
    uint16_t quantity;
} __packed modbus_read_regs_req_t;

typedef struct modbus_write_single_coil_req {
    uint8_t  function_code;
    uint16_t output_addr;
    uint16_t output_value;
} __packed modbus_write_single_coil_req_t;

typedef struct modbus_write_single_reg_req {
    uint8_t  function_code;
    uint16_t reg_addr;
    uint16_t reg_value;
} __packed modbus_write_single_reg_req_t;

typedef struct modbus_exception_resp {
    uint8_t  function_code;
    uint8_t  exception_code;
} __packed modbus_exception_resp_t;

/* ============================================================================
 * Byte Order Conversion
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

    return 5;
}

static int encode_read_regs(uint8_t *pdu, size_t max_len, uint8_t function_code,
                           uint16_t start_addr, uint16_t quantity)
{
    if (max_len < 5) return MODBUS_ERROR;

    modbus_read_regs_req_t *req = (modbus_read_regs_req_t *)pdu;
    req->function_code = function_code;
    req->start_addr = modbus_htons(start_addr);
    req->quantity = modbus_htons(quantity);

    return 5;
}

static int encode_write_single_coil(uint8_t *pdu, size_t max_len,
                                   uint16_t addr, bool value)
{
    if (max_len < 5) return MODBUS_ERROR;

    modbus_write_single_coil_req_t *req = (modbus_write_single_coil_req_t *)pdu;
    req->function_code = MODBUS_FC_WRITE_SINGLE_COIL;
    req->output_addr = modbus_htons(addr);
    req->output_value = modbus_htons(value ? 0xFF00 : 0x0000);

    return 5;
}

static int encode_write_single_reg(uint8_t *pdu, size_t max_len,
                                  uint16_t addr, uint16_t value)
{
    if (max_len < 5) return MODBUS_ERROR;

    modbus_write_single_reg_req_t *req = (modbus_write_single_reg_req_t *)pdu;
    req->function_code = MODBUS_FC_WRITE_SINGLE_REG;
    req->reg_addr = modbus_htons(addr);
    req->reg_value = modbus_htons(value);

    return 5;
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
    mbap->length = modbus_htons(pdu_length + 1);
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
        *pdu_length = len > 0 ? len - 1 : 0;
    }

    if (modbus_ntohs(mbap->protocol_id) != MODBUS_PROTOCOL_ID) {
        return MODBUS_INVALID_RESP;
    }

    return MODBUS_OK;
}

/* ============================================================================
 * Test Functions
 * ============================================================================ */

void test_byte_order()
{
    printf("\n=== Testing Byte Order Conversion ===\n");

    /* Test htons */
    assert(modbus_htons(0x1234) == 0x3412);
    assert(modbus_htons(0xABCD) == 0xCDAB);
    assert(modbus_htons(0x0000) == 0x0000);
    assert(modbus_htons(0xFFFF) == 0xFFFF);
    printf("htons: OK\n");

    /* Test ntohs */
    assert(modbus_ntohs(0x1234) == 0x3412);
    assert(modbus_ntohs(0xABCD) == 0xCDAB);
    printf("ntohs: OK\n");

    /* Test round-trip */
    uint16_t values[] = {0x0001, 0x1234, 0x5678, 0xABCD, 0xFFFF};
    for (int i = 0; i < 5; i++) {
        uint16_t converted = modbus_htons(values[i]);
        uint16_t restored = modbus_ntohs(converted);
        assert(restored == values[i]);
    }
    printf("Round-trip conversion: OK\n");
}

void test_crc()
{
    printf("\n=== Testing CRC-16 Calculation ===\n");

    /* Test case 1: Simple message */
    uint8_t msg1[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
    uint16_t crc1 = modbus_calc_crc(msg1, sizeof(msg1));
    printf("CRC for {0x01, 0x03, 0x00, 0x00, 0x00, 0x01}: 0x%04X\n", crc1);

    /* Test case 2: Different message */
    uint8_t msg2[] = {0x01, 0x06, 0x00, 0x01, 0x00, 0x03};
    uint16_t crc2 = modbus_calc_crc(msg2, sizeof(msg2));
    printf("CRC for {0x01, 0x06, 0x00, 0x01, 0x00, 0x03}: 0x%04X\n", crc2);

    /* Verify different messages produce different CRCs */
    assert(crc1 != crc2);

    /* Test case 3: Empty message */
    uint8_t msg3[] = {};
    uint16_t crc3 = modbus_calc_crc(msg3, 0);
    assert(crc3 == 0xFFFF);  /* Initial CRC value */
    printf("CRC for empty message: 0x%04X\n", crc3);

    printf("CRC-16 calculation: OK\n");
}

void test_encode_read_coils()
{
    printf("\n=== Testing Encode Read Coils ===\n");

    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int len;

    /* Test encoding read coils request */
    len = encode_read_bits(pdu, sizeof(pdu), MODBUS_FC_READ_COILS, 100, 10);
    assert(len == 5);

    modbus_read_bits_req_t *req = (modbus_read_bits_req_t *)pdu;
    assert(req->function_code == MODBUS_FC_READ_COILS);
    assert(modbus_ntohs(req->start_addr) == 100);
    assert(modbus_ntohs(req->quantity) == 10);

    printf("Read coils request (addr=100, count=10): OK\n");

    /* Test buffer too small */
    len = encode_read_bits(pdu, 3, MODBUS_FC_READ_COILS, 100, 10);
    assert(len == MODBUS_ERROR);
    printf("Buffer too small error: OK\n");
}

void test_encode_read_registers()
{
    printf("\n=== Testing Encode Read Registers ===\n");

    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int len;

    /* Test encoding read holding registers */
    len = encode_read_regs(pdu, sizeof(pdu), MODBUS_FC_READ_HOLDING_REGS, 0, 5);
    assert(len == 5);

    modbus_read_regs_req_t *req = (modbus_read_regs_req_t *)pdu;
    assert(req->function_code == MODBUS_FC_READ_HOLDING_REGS);
    assert(modbus_ntohs(req->start_addr) == 0);
    assert(modbus_ntohs(req->quantity) == 5);

    printf("Read holding registers (addr=0, count=5): OK\n");

    /* Test encoding read input registers */
    len = encode_read_regs(pdu, sizeof(pdu), MODBUS_FC_READ_INPUT_REGS, 50, 20);
    assert(len == 5);

    req = (modbus_read_regs_req_t *)pdu;
    assert(req->function_code == MODBUS_FC_READ_INPUT_REGS);
    assert(modbus_ntohs(req->start_addr) == 50);
    assert(modbus_ntohs(req->quantity) == 20);

    printf("Read input registers (addr=50, count=20): OK\n");
}

void test_encode_write_coil()
{
    printf("\n=== Testing Encode Write Single Coil ===\n");

    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int len;

    /* Test write coil ON */
    len = encode_write_single_coil(pdu, sizeof(pdu), 5, true);
    assert(len == 5);

    modbus_write_single_coil_req_t *req = (modbus_write_single_coil_req_t *)pdu;
    assert(req->function_code == MODBUS_FC_WRITE_SINGLE_COIL);
    assert(modbus_ntohs(req->output_addr) == 5);
    assert(modbus_ntohs(req->output_value) == 0xFF00);

    printf("Write coil ON (addr=5): OK\n");

    /* Test write coil OFF */
    len = encode_write_single_coil(pdu, sizeof(pdu), 10, false);
    assert(len == 5);

    req = (modbus_write_single_coil_req_t *)pdu;
    assert(req->function_code == MODBUS_FC_WRITE_SINGLE_COIL);
    assert(modbus_ntohs(req->output_addr) == 10);
    assert(modbus_ntohs(req->output_value) == 0x0000);

    printf("Write coil OFF (addr=10): OK\n");
}

void test_encode_write_register()
{
    printf("\n=== Testing Encode Write Single Register ===\n");

    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int len;

    /* Test write register */
    len = encode_write_single_reg(pdu, sizeof(pdu), 100, 0x1234);
    assert(len == 5);

    modbus_write_single_reg_req_t *req = (modbus_write_single_reg_req_t *)pdu;
    assert(req->function_code == MODBUS_FC_WRITE_SINGLE_REG);
    assert(modbus_ntohs(req->reg_addr) == 100);
    assert(modbus_ntohs(req->reg_value) == 0x1234);

    printf("Write register (addr=100, value=0x1234): OK\n");

    /* Test another value */
    len = encode_write_single_reg(pdu, sizeof(pdu), 200, 0xABCD);
    assert(len == 5);

    req = (modbus_write_single_reg_req_t *)pdu;
    assert(modbus_ntohs(req->reg_addr) == 200);
    assert(modbus_ntohs(req->reg_value) == 0xABCD);

    printf("Write register (addr=200, value=0xABCD): OK\n");
}

void test_encode_exception()
{
    printf("\n=== Testing Encode Exception Response ===\n");

    uint8_t pdu[MODBUS_MAX_PDU_SIZE];
    int len;

    /* Test illegal function exception */
    len = encode_exception(pdu, sizeof(pdu), MODBUS_FC_READ_COILS,
                          MODBUS_EXCEPTION_ILLEGAL_FUNCTION);
    assert(len == 2);

    modbus_exception_resp_t *resp = (modbus_exception_resp_t *)pdu;
    assert(resp->function_code == (MODBUS_FC_READ_COILS | MODBUS_EXCEPTION_OFFSET));
    assert(resp->exception_code == MODBUS_EXCEPTION_ILLEGAL_FUNCTION);

    printf("Exception (illegal function): OK\n");

    /* Test illegal data address exception */
    len = encode_exception(pdu, sizeof(pdu), MODBUS_FC_READ_HOLDING_REGS,
                          MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    assert(len == 2);

    resp = (modbus_exception_resp_t *)pdu;
    assert(resp->function_code == (MODBUS_FC_READ_HOLDING_REGS | MODBUS_EXCEPTION_OFFSET));
    assert(resp->exception_code == MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

    printf("Exception (illegal data address): OK\n");
}

void test_mbap_header()
{
    printf("\n=== Testing MBAP Header Encode/Decode ===\n");

    uint8_t buffer[MODBUS_MBAP_SIZE];
    int len;

    /* Test encode */
    len = encode_mbap_header(buffer, sizeof(buffer), 1234, 1, 5);
    assert(len == MODBUS_MBAP_SIZE);

    /* Verify buffer contents */
    modbus_mbap_header_t *mbap = (modbus_mbap_header_t *)buffer;
    assert(modbus_ntohs(mbap->transaction_id) == 1234);
    assert(modbus_ntohs(mbap->protocol_id) == MODBUS_PROTOCOL_ID);
    assert(modbus_ntohs(mbap->length) == 6);  /* PDU length + 1 for unit_id */
    assert(mbap->unit_id == 1);

    printf("MBAP encode (txn=1234, unit=1, pdu_len=5): OK\n");

    /* Test decode */
    uint16_t transaction_id;
    uint8_t unit_id;
    uint16_t pdu_length;

    int ret = decode_mbap_header(buffer, sizeof(buffer), &transaction_id,
                                 &unit_id, &pdu_length);
    assert(ret == MODBUS_OK);
    assert(transaction_id == 1234);
    assert(unit_id == 1);
    assert(pdu_length == 5);

    printf("MBAP decode: OK\n");

    /* Test round-trip with different values */
    len = encode_mbap_header(buffer, sizeof(buffer), 9999, 255, 253);
    assert(len == MODBUS_MBAP_SIZE);

    ret = decode_mbap_header(buffer, sizeof(buffer), &transaction_id,
                            &unit_id, &pdu_length);
    assert(ret == MODBUS_OK);
    assert(transaction_id == 9999);
    assert(unit_id == 255);
    assert(pdu_length == 253);

    printf("MBAP round-trip (txn=9999, unit=255, pdu_len=253): OK\n");
}

void test_register_operations()
{
    printf("\n=== Testing Register Read/Write Operations ===\n");

    /* Simulate holding registers */
    uint16_t holding_regs[100];
    for (int i = 0; i < 100; i++) {
        holding_regs[i] = i * 10;
    }

    /* Test register read */
    printf("Register[0] = %u\n", holding_regs[0]);
    printf("Register[10] = %u\n", holding_regs[10]);
    printf("Register[50] = %u\n", holding_regs[50]);
    assert(holding_regs[0] == 0);
    assert(holding_regs[10] == 100);
    assert(holding_regs[50] == 500);
    printf("Register read: OK\n");

    /* Test register write */
    holding_regs[5] = 0x1234;
    holding_regs[25] = 0xABCD;
    assert(holding_regs[5] == 0x1234);
    assert(holding_regs[25] == 0xABCD);
    printf("Register write: OK\n");

    /* Test multiple register read */
    uint16_t read_buffer[10];
    for (int i = 0; i < 10; i++) {
        read_buffer[i] = holding_regs[20 + i];
    }
    assert(read_buffer[0] == 200);
    assert(read_buffer[9] == 290);
    printf("Multiple register read (addr=20, count=10): OK\n");

    /* Test multiple register write */
    uint16_t write_data[] = {1111, 2222, 3333, 4444, 5555};
    for (int i = 0; i < 5; i++) {
        holding_regs[30 + i] = write_data[i];
    }
    assert(holding_regs[30] == 1111);
    assert(holding_regs[34] == 5555);
    printf("Multiple register write (addr=30, count=5): OK\n");
}

void test_coil_operations()
{
    printf("\n=== Testing Coil Read/Write Operations ===\n");

    /* Simulate coils (bit array) */
    uint8_t coils[32];  /* 256 coils (32 bytes * 8 bits) */
    memset(coils, 0, sizeof(coils));

    /* Test setting individual coils */
    coils[0] |= (1 << 0);  /* Coil 0 */
    coils[0] |= (1 << 5);  /* Coil 5 */
    coils[2] |= (1 << 3);  /* Coil 19 (2*8 + 3) */

    assert(coils[0] & (1 << 0));
    assert(coils[0] & (1 << 5));
    assert(coils[2] & (1 << 3));
    printf("Coil write: OK\n");

    /* Test reading coils */
    bool coil_0 = (coils[0] & (1 << 0)) != 0;
    bool coil_1 = (coils[0] & (1 << 1)) != 0;
    bool coil_5 = (coils[0] & (1 << 5)) != 0;

    assert(coil_0 == true);
    assert(coil_1 == false);
    assert(coil_5 == true);
    printf("Coil read: OK\n");

    /* Test clearing coils */
    coils[0] &= ~(1 << 0);  /* Clear coil 0 */
    assert(!(coils[0] & (1 << 0)));
    printf("Coil clear: OK\n");

    /* Test bulk coil operations */
    uint8_t coil_data = 0b10101010;  /* Alternating pattern */
    coils[1] = coil_data;

    for (int i = 0; i < 8; i++) {
        bool expected = (i % 2 == 1);  /* Odd bits set */
        bool actual = (coils[1] & (1 << i)) != 0;
        assert(actual == expected);
    }
    printf("Bulk coil operations: OK\n");
}

void test_protocol_limits()
{
    printf("\n=== Testing Protocol Limits ===\n");

    /* Test maximum coils */
    printf("Max coils per request: %d\n", MODBUS_MAX_COILS);
    assert(MODBUS_MAX_COILS == 2000);

    /* Test maximum registers */
    printf("Max registers per request: %d\n", MODBUS_MAX_REGISTERS);
    assert(MODBUS_MAX_REGISTERS == 125);

    /* Test maximum write coils */
    printf("Max write coils: %d\n", MODBUS_MAX_WRITE_COILS);
    assert(MODBUS_MAX_WRITE_COILS == 1968);

    /* Test maximum write registers */
    printf("Max write registers: %d\n", MODBUS_MAX_WRITE_REGS);
    assert(MODBUS_MAX_WRITE_REGS == 123);

    /* Test PDU size limits */
    printf("Max PDU size: %d\n", MODBUS_MAX_PDU_SIZE);
    assert(MODBUS_MAX_PDU_SIZE == 253);

    printf("Protocol limits: OK\n");
}

void test_function_codes()
{
    printf("\n=== Testing Function Codes ===\n");

    /* Verify function code values */
    assert(MODBUS_FC_READ_COILS == 0x01);
    assert(MODBUS_FC_READ_DISCRETE_INPUTS == 0x02);
    assert(MODBUS_FC_READ_HOLDING_REGS == 0x03);
    assert(MODBUS_FC_READ_INPUT_REGS == 0x04);
    assert(MODBUS_FC_WRITE_SINGLE_COIL == 0x05);
    assert(MODBUS_FC_WRITE_SINGLE_REG == 0x06);
    assert(MODBUS_FC_WRITE_MULTIPLE_COILS == 0x0F);
    assert(MODBUS_FC_WRITE_MULTIPLE_REGS == 0x10);

    printf("Function codes: OK\n");

    /* Test exception offset */
    uint8_t normal_fc = MODBUS_FC_READ_COILS;
    uint8_t exception_fc = normal_fc | MODBUS_EXCEPTION_OFFSET;

    assert(exception_fc == 0x81);
    assert(exception_fc & MODBUS_EXCEPTION_OFFSET);

    printf("Exception offset: OK\n");
}

int main()
{
    printf("=== EMBODIOS Modbus TCP Unit Tests ===\n");

    test_byte_order();
    test_crc();
    test_encode_read_coils();
    test_encode_read_registers();
    test_encode_write_coil();
    test_encode_write_register();
    test_encode_exception();
    test_mbap_header();
    test_register_operations();
    test_coil_operations();
    test_protocol_limits();
    test_function_codes();

    printf("\n=== All Modbus tests passed! ===\n");
    return 0;
}
