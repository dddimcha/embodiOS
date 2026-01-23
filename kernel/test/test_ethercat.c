/* Unit test for EtherCAT Slave Protocol */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/* EtherCAT protocol constants */
#define ECAT_ETHERTYPE          0x88A4
#define ECAT_MAX_FRAME_SIZE     1514
#define ECAT_HEADER_SIZE        2
#define ECAT_DATAGRAM_HEADER    10

/* Commands */
#define ECAT_CMD_NOP            0x00
#define ECAT_CMD_APRD           0x01
#define ECAT_CMD_APWR           0x02
#define ECAT_CMD_APRW           0x03
#define ECAT_CMD_FPRD           0x04
#define ECAT_CMD_FPWR           0x05
#define ECAT_CMD_FPRW           0x06
#define ECAT_CMD_BRD            0x07
#define ECAT_CMD_BWR            0x08
#define ECAT_CMD_BRW            0x09
#define ECAT_CMD_LRD            0x0A
#define ECAT_CMD_LWR            0x0B
#define ECAT_CMD_LRW            0x0C

/* States */
#define ECAT_STATE_INIT         0x01
#define ECAT_STATE_PREOP        0x02
#define ECAT_STATE_BOOT         0x03
#define ECAT_STATE_SAFEOP       0x04
#define ECAT_STATE_OP           0x08
#define ECAT_STATE_ERROR        0x10

/* Flags */
#define ECAT_FLAG_MORE          0x8000
#define ECAT_FLAG_CIRCULATED    0x4000

/* Registers */
#define ECAT_REG_TYPE           0x0000
#define ECAT_REG_STATION_ADDR   0x0010
#define ECAT_REG_AL_CONTROL     0x0120
#define ECAT_REG_AL_STATUS      0x0130
#define ECAT_REG_AL_STATUS_CODE 0x0134

/* Packed attribute */
#define __packed __attribute__((packed))

/* ============================================================================
 * Protocol Structures
 * ============================================================================ */

typedef struct ecat_header {
    uint16_t length_type;
} __packed ecat_header_t;

typedef struct ecat_datagram {
    uint8_t  cmd;
    uint8_t  idx;
    uint32_t addr;
    uint16_t len_flags;
    uint16_t irq;
} __packed ecat_datagram_t;

typedef struct mock_slave {
    uint8_t  al_state;
    uint8_t  requested_state;
    uint16_t station_address;
    uint8_t  registers[256];
    uint32_t frames_processed;
    uint32_t datagrams_processed;
    uint32_t state_transitions;
} mock_slave_t;

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static inline uint16_t ecat_le16_to_cpu(uint16_t val)
{
    return val;  /* x86 is little-endian */
}

static inline uint32_t ecat_le32_to_cpu(uint32_t val)
{
    return val;
}

static inline uint16_t ecat_cpu_to_le16(uint16_t val)
{
    return val;
}

static inline uint32_t ecat_cpu_to_le32(uint32_t val)
{
    return val;
}

uint16_t ecat_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
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

const char *ecat_state_string(uint8_t state)
{
    switch (state & 0x0F) {
        case ECAT_STATE_INIT:   return "INIT";
        case ECAT_STATE_PREOP:  return "PREOP";
        case ECAT_STATE_BOOT:   return "BOOT";
        case ECAT_STATE_SAFEOP: return "SAFEOP";
        case ECAT_STATE_OP:     return "OP";
        default:                return "INVALID";
    }
}

/* ============================================================================
 * Mock Slave Functions
 * ============================================================================ */

mock_slave_t *mock_slave_create(uint16_t station_addr)
{
    mock_slave_t *slave = calloc(1, sizeof(mock_slave_t));
    slave->al_state = ECAT_STATE_INIT;
    slave->requested_state = ECAT_STATE_INIT;
    slave->station_address = station_addr;

    /* Initialize some registers */
    slave->registers[ECAT_REG_TYPE] = 0x04;  /* ESC type */
    *(uint16_t *)&slave->registers[ECAT_REG_STATION_ADDR] =
        ecat_cpu_to_le16(station_addr);

    return slave;
}

void mock_slave_destroy(mock_slave_t *slave)
{
    free(slave);
}

bool is_valid_state_transition(uint8_t from_state, uint8_t to_state)
{
    from_state &= 0x0F;
    to_state &= 0x0F;

    /* INIT can transition to any state */
    if (from_state == ECAT_STATE_INIT) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_PREOP ||
                to_state == ECAT_STATE_BOOT ||
                to_state == ECAT_STATE_SAFEOP ||
                to_state == ECAT_STATE_OP);
    }

    /* PREOP can go to INIT, BOOT, SAFEOP */
    if (from_state == ECAT_STATE_PREOP) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_PREOP ||
                to_state == ECAT_STATE_BOOT ||
                to_state == ECAT_STATE_SAFEOP);
    }

    /* BOOT can only go to INIT */
    if (from_state == ECAT_STATE_BOOT) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_BOOT);
    }

    /* SAFEOP can go to INIT, PREOP, OP */
    if (from_state == ECAT_STATE_SAFEOP) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_PREOP ||
                to_state == ECAT_STATE_SAFEOP ||
                to_state == ECAT_STATE_OP);
    }

    /* OP can go to INIT, SAFEOP */
    if (from_state == ECAT_STATE_OP) {
        return (to_state == ECAT_STATE_INIT ||
                to_state == ECAT_STATE_SAFEOP ||
                to_state == ECAT_STATE_OP);
    }

    return false;
}

int mock_state_transition(mock_slave_t *slave, uint8_t new_state)
{
    if (!is_valid_state_transition(slave->al_state, new_state)) {
        slave->al_state = slave->al_state | ECAT_STATE_ERROR;
        return -1;
    }

    slave->al_state = new_state;
    slave->state_transitions++;

    /* Update AL_STATUS register */
    *(uint16_t *)&slave->registers[ECAT_REG_AL_STATUS] =
        ecat_cpu_to_le16(slave->al_state);

    return 0;
}

/* ============================================================================
 * Test Functions
 * ============================================================================ */

void test_byte_order_conversion()
{
    printf("\n=== Testing Byte Order Conversion ===\n");

    /* Test 16-bit conversion */
    uint16_t val16 = 0x1234;
    uint16_t le16 = ecat_cpu_to_le16(val16);
    uint16_t back16 = ecat_le16_to_cpu(le16);
    assert(back16 == val16);
    printf("16-bit round-trip: 0x%04X -> 0x%04X -> 0x%04X ✓\n",
           val16, le16, back16);

    /* Test 32-bit conversion */
    uint32_t val32 = 0x12345678;
    uint32_t le32 = ecat_cpu_to_le32(val32);
    uint32_t back32 = ecat_le32_to_cpu(le32);
    assert(back32 == val32);
    printf("32-bit round-trip: 0x%08X -> 0x%08X -> 0x%08X ✓\n",
           val32, le32, back32);
}

void test_crc16()
{
    printf("\n=== Testing CRC-16 Calculation ===\n");

    /* Test vector 1: Empty data */
    uint8_t data1[] = {};
    uint16_t crc1 = ecat_crc16(data1, 0);
    printf("CRC-16 of empty data: 0x%04X\n", crc1);
    assert(crc1 == 0xFFFF);

    /* Test vector 2: Simple data */
    uint8_t data2[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc2 = ecat_crc16(data2, sizeof(data2));
    printf("CRC-16 of {0x01,0x02,0x03,0x04}: 0x%04X\n", crc2);
    assert(crc2 != 0);

    /* Test vector 3: Different data produces different CRC */
    uint8_t data3[] = {0x04, 0x03, 0x02, 0x01};
    uint16_t crc3 = ecat_crc16(data3, sizeof(data3));
    printf("CRC-16 of {0x04,0x03,0x02,0x01}: 0x%04X\n", crc3);
    assert(crc3 != crc2);
}

void test_frame_header_encode_decode()
{
    printf("\n=== Testing Frame Header Encode/Decode ===\n");

    uint8_t buffer[ECAT_HEADER_SIZE];
    ecat_header_t *hdr = (ecat_header_t *)buffer;

    /* Encode frame header */
    uint16_t length = 44;  /* Example length */
    uint16_t type = 0x01;  /* Datagram type */
    hdr->length_type = ecat_cpu_to_le16((length & 0x7FF) | (type << 12));

    printf("Encoded header: length=%d, type=0x%X\n", length, type);

    /* Decode frame header */
    uint16_t lt = ecat_le16_to_cpu(hdr->length_type);
    uint16_t dec_length = lt & 0x7FF;
    uint16_t dec_type = (lt >> 12) & 0x0F;

    printf("Decoded header: length=%d, type=0x%X\n", dec_length, dec_type);

    assert(dec_length == length);
    assert(dec_type == type);
}

void test_datagram_encode_decode()
{
    printf("\n=== Testing Datagram Encode/Decode ===\n");

    uint8_t buffer[ECAT_DATAGRAM_HEADER];
    ecat_datagram_t *dg = (ecat_datagram_t *)buffer;

    /* Encode datagram */
    dg->cmd = ECAT_CMD_FPRD;
    dg->idx = 0;
    dg->addr = ecat_cpu_to_le32(0x00010120);  /* Station 1, register 0x0120 */
    dg->len_flags = ecat_cpu_to_le16(4);  /* 4 bytes */
    dg->irq = 0;

    printf("Encoded datagram: cmd=%d, addr=0x%08X, len=%d\n",
           dg->cmd, 0x00010120, 4);

    /* Decode datagram */
    uint8_t cmd = dg->cmd;
    uint32_t addr = ecat_le32_to_cpu(dg->addr);
    uint16_t len_flags = ecat_le16_to_cpu(dg->len_flags);
    uint16_t len = len_flags & 0x7FF;
    bool more = (len_flags & ECAT_FLAG_MORE) != 0;

    printf("Decoded datagram: cmd=%d, addr=0x%08X, len=%d, more=%d\n",
           cmd, addr, len, more);

    assert(cmd == ECAT_CMD_FPRD);
    assert(addr == 0x00010120);
    assert(len == 4);
    assert(!more);
}

void test_datagram_with_flags()
{
    printf("\n=== Testing Datagram Flags ===\n");

    uint8_t buffer[ECAT_DATAGRAM_HEADER];
    ecat_datagram_t *dg = (ecat_datagram_t *)buffer;

    /* Encode datagram with MORE flag */
    dg->cmd = ECAT_CMD_APRD;
    dg->idx = 0;
    dg->addr = ecat_cpu_to_le32(0x0000);
    uint16_t len = 4;
    dg->len_flags = ecat_cpu_to_le16(len | ECAT_FLAG_MORE);
    dg->irq = 0;

    /* Decode flags */
    uint16_t len_flags = ecat_le16_to_cpu(dg->len_flags);
    uint16_t dec_len = len_flags & 0x7FF;
    bool more = (len_flags & ECAT_FLAG_MORE) != 0;
    bool circulated = (len_flags & ECAT_FLAG_CIRCULATED) != 0;

    printf("Datagram flags: len=%d, more=%d, circulated=%d\n",
           dec_len, more, circulated);

    assert(dec_len == 4);
    assert(more);
    assert(!circulated);
}

void test_working_counter()
{
    printf("\n=== Testing Working Counter ===\n");

    /* Simulate a datagram with data and working counter */
    uint8_t buffer[32];
    memset(buffer, 0, sizeof(buffer));

    /* Data length = 4 bytes */
    uint16_t data_len = 4;

    /* Working counter is at offset data_len after data starts */
    uint16_t *wkc = (uint16_t *)&buffer[data_len];

    /* Initialize WKC to 0 */
    *wkc = ecat_cpu_to_le16(0);

    /* Slave processes datagram and increments WKC */
    uint16_t wkc_val = ecat_le16_to_cpu(*wkc);
    wkc_val++;
    *wkc = ecat_cpu_to_le16(wkc_val);

    printf("Working counter after slave response: %d\n", wkc_val);
    assert(wkc_val == 1);

    /* Another slave processes it (broadcast) */
    wkc_val = ecat_le16_to_cpu(*wkc);
    wkc_val++;
    *wkc = ecat_cpu_to_le16(wkc_val);

    printf("Working counter after second slave: %d\n", wkc_val);
    assert(wkc_val == 2);
}

void test_state_machine_init_to_preop()
{
    printf("\n=== Testing State Transition: INIT -> PREOP ===\n");

    mock_slave_t *slave = mock_slave_create(1);
    assert(slave->al_state == ECAT_STATE_INIT);
    printf("Initial state: %s\n", ecat_state_string(slave->al_state));

    /* Transition to PREOP */
    int ret = mock_state_transition(slave, ECAT_STATE_PREOP);
    assert(ret == 0);
    assert(slave->al_state == ECAT_STATE_PREOP);
    printf("After transition: %s ✓\n", ecat_state_string(slave->al_state));

    mock_slave_destroy(slave);
}

void test_state_machine_preop_to_safeop()
{
    printf("\n=== Testing State Transition: PREOP -> SAFEOP ===\n");

    mock_slave_t *slave = mock_slave_create(1);
    mock_state_transition(slave, ECAT_STATE_PREOP);
    printf("Initial state: %s\n", ecat_state_string(slave->al_state));

    /* Transition to SAFEOP */
    int ret = mock_state_transition(slave, ECAT_STATE_SAFEOP);
    assert(ret == 0);
    assert(slave->al_state == ECAT_STATE_SAFEOP);
    printf("After transition: %s ✓\n", ecat_state_string(slave->al_state));

    mock_slave_destroy(slave);
}

void test_state_machine_safeop_to_op()
{
    printf("\n=== Testing State Transition: SAFEOP -> OP ===\n");

    mock_slave_t *slave = mock_slave_create(1);
    mock_state_transition(slave, ECAT_STATE_PREOP);
    mock_state_transition(slave, ECAT_STATE_SAFEOP);
    printf("Initial state: %s\n", ecat_state_string(slave->al_state));

    /* Transition to OP */
    int ret = mock_state_transition(slave, ECAT_STATE_OP);
    assert(ret == 0);
    assert(slave->al_state == ECAT_STATE_OP);
    printf("After transition: %s ✓\n", ecat_state_string(slave->al_state));

    mock_slave_destroy(slave);
}

void test_state_machine_full_sequence()
{
    printf("\n=== Testing Full State Sequence: INIT->PREOP->SAFEOP->OP ===\n");

    mock_slave_t *slave = mock_slave_create(1);

    printf("Starting state: %s\n", ecat_state_string(slave->al_state));
    assert(slave->al_state == ECAT_STATE_INIT);

    /* INIT -> PREOP */
    mock_state_transition(slave, ECAT_STATE_PREOP);
    printf("Transitioned to: %s\n", ecat_state_string(slave->al_state));
    assert(slave->al_state == ECAT_STATE_PREOP);

    /* PREOP -> SAFEOP */
    mock_state_transition(slave, ECAT_STATE_SAFEOP);
    printf("Transitioned to: %s\n", ecat_state_string(slave->al_state));
    assert(slave->al_state == ECAT_STATE_SAFEOP);

    /* SAFEOP -> OP */
    mock_state_transition(slave, ECAT_STATE_OP);
    printf("Transitioned to: %s\n", ecat_state_string(slave->al_state));
    assert(slave->al_state == ECAT_STATE_OP);

    printf("Total transitions: %d ✓\n", slave->state_transitions);
    assert(slave->state_transitions == 3);

    mock_slave_destroy(slave);
}

void test_state_machine_invalid_transition()
{
    printf("\n=== Testing Invalid State Transition ===\n");

    mock_slave_t *slave = mock_slave_create(1);
    mock_state_transition(slave, ECAT_STATE_PREOP);

    /* Try invalid transition: PREOP -> OP (must go through SAFEOP first) */
    printf("Attempting invalid transition: PREOP -> OP\n");
    int ret = mock_state_transition(slave, ECAT_STATE_OP);
    assert(ret == -1);
    printf("Transition rejected ✓\n");

    /* State should have error flag set */
    assert(slave->al_state & ECAT_STATE_ERROR);
    printf("Error flag set: 0x%02X ✓\n", slave->al_state);

    mock_slave_destroy(slave);
}

void test_state_machine_op_to_safeop()
{
    printf("\n=== Testing State Transition: OP -> SAFEOP ===\n");

    mock_slave_t *slave = mock_slave_create(1);
    mock_state_transition(slave, ECAT_STATE_PREOP);
    mock_state_transition(slave, ECAT_STATE_SAFEOP);
    mock_state_transition(slave, ECAT_STATE_OP);
    printf("Initial state: %s\n", ecat_state_string(slave->al_state));

    /* Transition back to SAFEOP (safe shutdown) */
    int ret = mock_state_transition(slave, ECAT_STATE_SAFEOP);
    assert(ret == 0);
    assert(slave->al_state == ECAT_STATE_SAFEOP);
    printf("After transition: %s ✓\n", ecat_state_string(slave->al_state));

    mock_slave_destroy(slave);
}

void test_state_machine_emergency_stop()
{
    printf("\n=== Testing Emergency Stop: OP -> INIT ===\n");

    mock_slave_t *slave = mock_slave_create(1);
    mock_state_transition(slave, ECAT_STATE_PREOP);
    mock_state_transition(slave, ECAT_STATE_SAFEOP);
    mock_state_transition(slave, ECAT_STATE_OP);
    printf("Initial state: %s\n", ecat_state_string(slave->al_state));

    /* Emergency transition to INIT from any state */
    int ret = mock_state_transition(slave, ECAT_STATE_INIT);
    assert(ret == 0);
    assert(slave->al_state == ECAT_STATE_INIT);
    printf("After emergency stop: %s ✓\n", ecat_state_string(slave->al_state));

    mock_slave_destroy(slave);
}

void test_command_types()
{
    printf("\n=== Testing Command Types ===\n");

    const char *cmd_names[] = {
        "NOP", "APRD", "APWR", "APRW",
        "FPRD", "FPWR", "FPRW",
        "BRD", "BWR", "BRW",
        "LRD", "LWR", "LRW"
    };

    uint8_t commands[] = {
        ECAT_CMD_NOP,
        ECAT_CMD_APRD, ECAT_CMD_APWR, ECAT_CMD_APRW,
        ECAT_CMD_FPRD, ECAT_CMD_FPWR, ECAT_CMD_FPRW,
        ECAT_CMD_BRD, ECAT_CMD_BWR, ECAT_CMD_BRW,
        ECAT_CMD_LRD, ECAT_CMD_LWR, ECAT_CMD_LRW
    };

    for (size_t i = 0; i < sizeof(commands); i++) {
        printf("Command 0x%02X: %s\n", commands[i], cmd_names[i]);
        assert(commands[i] == i);
    }
}

void test_register_access()
{
    printf("\n=== Testing Register Access ===\n");

    mock_slave_t *slave = mock_slave_create(0x1001);

    /* Test station address register (check initialization) */
    uint16_t station = ecat_le16_to_cpu(
        *(uint16_t *)&slave->registers[ECAT_REG_STATION_ADDR]);
    assert(station == 0x1001);
    printf("Station address register: 0x%04X ✓\n", station);

    /* Test 8-bit register write/read (use different address to avoid conflict) */
    slave->registers[0x50] = 0xAB;
    assert(slave->registers[0x50] == 0xAB);
    printf("8-bit register [0x50]: 0x%02X ✓\n", slave->registers[0x50]);

    /* Test 16-bit register write/read */
    uint16_t val16 = 0x1234;
    *(uint16_t *)&slave->registers[0x60] = ecat_cpu_to_le16(val16);
    uint16_t read16 = ecat_le16_to_cpu(*(uint16_t *)&slave->registers[0x60]);
    assert(read16 == val16);
    printf("16-bit register [0x60]: 0x%04X ✓\n", read16);

    /* Test 32-bit register write/read */
    uint32_t val32 = 0x12345678;
    *(uint32_t *)&slave->registers[0x70] = ecat_cpu_to_le32(val32);
    uint32_t read32 = ecat_le32_to_cpu(*(uint32_t *)&slave->registers[0x70]);
    assert(read32 == val32);
    printf("32-bit register [0x70]: 0x%08X ✓\n", read32);

    mock_slave_destroy(slave);
}

void test_multiple_datagrams()
{
    printf("\n=== Testing Multiple Datagrams in Frame ===\n");

    /* Simulate frame with 3 datagrams */
    uint8_t buffer[256];
    size_t offset = 0;

    /* Datagram 1: APRD with MORE flag */
    ecat_datagram_t *dg1 = (ecat_datagram_t *)&buffer[offset];
    dg1->cmd = ECAT_CMD_APRD;
    dg1->idx = 0;
    dg1->addr = ecat_cpu_to_le32(0x0000);
    dg1->len_flags = ecat_cpu_to_le16(4 | ECAT_FLAG_MORE);
    dg1->irq = 0;
    offset += ECAT_DATAGRAM_HEADER + 4 + 2;  /* header + data + WKC */

    /* Datagram 2: FPRD with MORE flag */
    ecat_datagram_t *dg2 = (ecat_datagram_t *)&buffer[offset];
    dg2->cmd = ECAT_CMD_FPRD;
    dg2->idx = 1;
    dg2->addr = ecat_cpu_to_le32(0x00010120);
    dg2->len_flags = ecat_cpu_to_le16(2 | ECAT_FLAG_MORE);
    dg2->irq = 0;
    offset += ECAT_DATAGRAM_HEADER + 2 + 2;

    /* Datagram 3: BRD without MORE flag (last) */
    ecat_datagram_t *dg3 = (ecat_datagram_t *)&buffer[offset];
    dg3->cmd = ECAT_CMD_BRD;
    dg3->idx = 2;
    dg3->addr = ecat_cpu_to_le32(0x0000);
    dg3->len_flags = ecat_cpu_to_le16(4);  /* No MORE flag */
    dg3->irq = 0;

    /* Verify datagrams */
    printf("Datagram 1: cmd=%d, idx=%d, more=%d\n",
           dg1->cmd, dg1->idx,
           (ecat_le16_to_cpu(dg1->len_flags) & ECAT_FLAG_MORE) != 0);
    assert((ecat_le16_to_cpu(dg1->len_flags) & ECAT_FLAG_MORE) != 0);

    printf("Datagram 2: cmd=%d, idx=%d, more=%d\n",
           dg2->cmd, dg2->idx,
           (ecat_le16_to_cpu(dg2->len_flags) & ECAT_FLAG_MORE) != 0);
    assert((ecat_le16_to_cpu(dg2->len_flags) & ECAT_FLAG_MORE) != 0);

    printf("Datagram 3: cmd=%d, idx=%d, more=%d\n",
           dg3->cmd, dg3->idx,
           (ecat_le16_to_cpu(dg3->len_flags) & ECAT_FLAG_MORE) != 0);
    assert((ecat_le16_to_cpu(dg3->len_flags) & ECAT_FLAG_MORE) == 0);

    printf("Multiple datagrams processed successfully ✓\n");
}

int main()
{
    printf("=== EMBODIOS EtherCAT Unit Tests ===\n");

    /* Utility tests */
    test_byte_order_conversion();
    test_crc16();
    test_command_types();

    /* Frame parsing tests */
    test_frame_header_encode_decode();
    test_datagram_encode_decode();
    test_datagram_with_flags();
    test_working_counter();
    test_multiple_datagrams();
    test_register_access();

    /* State machine tests */
    test_state_machine_init_to_preop();
    test_state_machine_preop_to_safeop();
    test_state_machine_safeop_to_op();
    test_state_machine_full_sequence();
    test_state_machine_invalid_transition();
    test_state_machine_op_to_safeop();
    test_state_machine_emergency_stop();

    printf("\n=== All EtherCAT tests passed! ===\n");
    return 0;
}
