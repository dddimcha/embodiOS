/* EMBODIOS Feetech STS3215 TTL-bus servo driver (actuator backend)
 * WS-DRV (M0, EMG bench loop), branch m0-drv
 * Author: Dmitry Dimcha <dddimcha@gmail.com>
 *
 * Feetech SMS/STS serial-bus protocol, TTL half-duplex single-wire bus,
 * 8N1, 115200 baud for M0 (servo factory default is 1000000; set 115200
 * once with the FD/URT-1 tool or a bus adapter — see
 * docs/emg-bench-wiring.md).
 *
 * Packet format (instruction packet):
 *   0xFF 0xFF  ID  LEN  INST  PARAM1..PARAMn  CHKSUM
 *   LEN = nparams + 2 (INST + CHKSUM counted)
 *   CHKSUM = ~(ID + LEN + INST + PARAM1 + ... + PARAMn) & 0xFF
 * Reply packet:
 *   0xFF 0xFF  ID  LEN  ERR  PARAM1..PARAMn  CHKSUM
 * Protocol source (cited per SPEC):
 *   Feetech "Serial Bus Smart Servo Communication Protocol User Manual"
 *   (doc 191218), Waveshare mirror:
 *   https://files.waveshare.com/upload/2/27/Communication_Protocol_User_Manual-EN%28191218-0923%29.pdf
 *   STS3215 control table (addr 40 torque enable, 41 acceleration,
 *   42 goal pos (2B), 46 goal speed (2B), 48 max torque (2B),
 *   56 present pos (2B), 60 present load (2B)):
 *   https://www.kevsrobots.com/resources/how_it_works/bus-servos.html
 *   and the Feetech SCServo SDK (gitee.com/ftservo/SCServoSDK).
 *
 * Half-duplex wiring (docs/emg-bench-wiring.md): Pi TXD -> 1k resistor
 * -> servo DATA; Pi RXD -> servo DATA directly; common GND.  Because RX
 * sits on the bus, the UART hears its own transmitted bytes: every
 * transmit is followed by discarding that many echo bytes.
 *
 * Aux UART (console UART0 stays free for the shell):
 *  - pi4 (BCM2711): UART2 (PL011) @ PERIPH_BASE + 0x201400,
 *    GPIO0 (pin 27) = TXD2, GPIO1 (pin 28) = RXD2, both ALT4
 *    (BCM2711 ARM Peripherals ch. 11; dtoverlay=uart2 uses the same).
 *  - pi5 (RP1): UART1 (PL011) @ RP1 bus 0x40034000 ->
 *    PERIPH_BASE + 0x34000, GPIO0/1 (pins 27/28) function a2
 *    (`pinctrl funcs 0-1`: a2 = TXD1/RXD1; dtoverlay=uart1-pi5).
 *    NOTE: GPIO0/1 double as the HAT EEPROM I2C (ID_SD/ID_SC) — using
 *    them for the servo bus means no HAT ID EEPROM on the bench rig.
 *  - pi3/virt/x86: no strong overrides here; WS-DSP weak stubs stand.
 *
 * The actuator_* strong overrides below are compiled only when the
 * Makefile defines BOARD_HAS_EMG_HW=1 (pi4/pi5).
 */

#include "embodios/types.h"
#include "embodios/console.h"
#include "board.h"

#if defined(BOARD_HAS_EMG_HW) && BOARD_HAS_EMG_HW

#include <embodios/actuator.h>
#if defined(BOARD_PI4)
#include <embodios/bcm_mailbox.h>
#endif

/* ------------------------------------------------------------------ */
/* Aux UART (PL011), polled                                            */
/* ------------------------------------------------------------------ */
#if defined(BOARD_PI4)
#define AUX_UART_BASE   (PERIPH_BASE + 0x201400UL)      /* UART2 */
#else /* BOARD_PI5 */
#define AUX_UART_BASE   (PERIPH_BASE + 0x034000UL)      /* RP1 UART1 */
#endif

#define AUX_DR          (AUX_UART_BASE + 0x00)
#define AUX_FR          (AUX_UART_BASE + 0x18)
#define AUX_IBRD        (AUX_UART_BASE + 0x24)
#define AUX_FBRD        (AUX_UART_BASE + 0x28)
#define AUX_LCRH        (AUX_UART_BASE + 0x2C)
#define AUX_CR          (AUX_UART_BASE + 0x30)
#define AUX_IMSC        (AUX_UART_BASE + 0x38)
#define AUX_ICR         (AUX_UART_BASE + 0x44)

#define AUX_FR_TXFF     (1u << 5)
#define AUX_FR_RXFE     (1u << 4)
#define AUX_FR_BUSY     (1u << 3)
#define AUX_CR_UARTEN   (1u << 0)
#define AUX_CR_TXE      (1u << 8)
#define AUX_CR_RXE      (1u << 9)
#define AUX_LCRH_FEN    (1u << 4)
#define AUX_LCRH_WLEN8  (3u << 5)

#define AUX_BAUD        115200u

/* Default PL011 clock 48 MHz.  pi4: confirmed via mailbox (clock id 2).
 * pi5: RP1 clk_uart — bare-metal threads report 48 MHz as the firmware
 * default for RP1 UARTs (forums.raspberrypi.com t=361321).
 * TODO(hw): verify RP1 clk_uart rate on hardware; wrong clock = wrong
 * baud = servo never answers. */
#define AUX_UART_CLK_HZ 48000000u

/* RX timeout: 100k nops is far above one byte time at 115200 (87 us);
 * servo turnaround after a request is typically < 1 ms. */
#define AUX_RX_TIMEOUT  100000u

static void aux_mmio_write32(uintptr_t addr, uint32_t value)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    __asm__ volatile("str %w[val], [%[ptr]]"
                     : : [val] "r" (value), [ptr] "r" (ptr) : "memory");
}

static uint32_t aux_mmio_read32(uintptr_t addr)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    uint32_t val;
    __asm__ volatile("ldr %w[val], [%[ptr]]"
                     : [val] "=r" (val) : [ptr] "r" (ptr) : "memory");
    return val;
}

#if defined(BOARD_PI4)
/* BCM2711 GPIO: GPIO0/1 -> ALT4 (TXD2/RXD2), pulls via the 2711-style
 * GPIO_PUP_PDN_CNTRL registers (GPPUD is legacy-2835 only). */
#define FT_GPIO_BASE        (PERIPH_BASE + 0x200000UL)
#define FT_GPFSEL0          (FT_GPIO_BASE + 0x00)
#define FT_PUP_PDN_CNTRL0   (FT_GPIO_BASE + 0xE4)
#define GPIO_FSEL_ALT4      3u

static void aux_gpio_init(void)
{
    uint32_t r = aux_mmio_read32(FT_GPFSEL0);
    r &= ~((7u << 0) | (7u << 3));          /* GPIO0, GPIO1 fields */
    r |= (GPIO_FSEL_ALT4 << 0) | (GPIO_FSEL_ALT4 << 3);
    aux_mmio_write32(FT_GPFSEL0, r);

    /* 2 bits/pin: 00 = no pull, 01 = pull-up.  TX: none, RX: pull-up. */
    r = aux_mmio_read32(FT_PUP_PDN_CNTRL0);
    r &= ~((3u << 0) | (3u << 2));
    r |= (1u << 2);                         /* GPIO1 (RXD2) pull-up */
    aux_mmio_write32(FT_PUP_PDN_CNTRL0, r);
}
#else /* BOARD_PI5 */
/* RP1 io_bank0/pads (pattern from drivers/gpio/bcm_gpio.c).
 * GPIO0/1 -> a2 = UART1 TXD1/RXD1. */
#define FT_IO_BANK0         (PERIPH_BASE + 0x0D0000UL)
#define FT_PADS_BANK0       (PERIPH_BASE + 0x0F0000UL)
#define FT_GPIO_CTRL(pin)   (FT_IO_BANK0 + 8UL * (pin) + 4UL)
#define FT_PAD(pin)         (FT_PADS_BANK0 + 0x04UL + 4UL * (pin))
#define RP1_FSEL_UART1      2u              /* a2 = TXD1/RXD1 */
#define RP1_PAD_IE          (1u << 6)
#define RP1_PAD_DRIVE_4MA   (1u << 4)
#define RP1_PAD_PUE         (1u << 3)
#define RP1_PAD_SCHMITT     (1u << 1)

static void aux_gpio_init(void)
{
    aux_mmio_write32(FT_GPIO_CTRL(0), RP1_FSEL_UART1);  /* TXD1, pin 27 */
    aux_mmio_write32(FT_GPIO_CTRL(1), RP1_FSEL_UART1);  /* RXD1, pin 28 */
    aux_mmio_write32(FT_PAD(0), RP1_PAD_DRIVE_4MA | RP1_PAD_SCHMITT);
    aux_mmio_write32(FT_PAD(1), RP1_PAD_IE | RP1_PAD_DRIVE_4MA |
                              RP1_PAD_PUE | RP1_PAD_SCHMITT);
    /* TODO(hw): RP1 resets/clocks assumed firmware-default (as in the
     * v0.9.0 RP1 UART0 path); if UART1 is dead on metal, check resets @
     * RP1 0x40014000 and clocks @ 0x40018000. */
}
#endif

static void aux_uart_init(void)
{
    uint32_t clk_hz = AUX_UART_CLK_HZ;
    uint32_t div16, ibrd, fbrd;

    aux_mmio_write32(AUX_CR, 0);
    aux_gpio_init();

#if defined(BOARD_PI4)
    uint32_t mbox_clk = 0;
    if (bcm_mbox_get_uart_clock(&mbox_clk) == 0 && mbox_clk != 0) {
        clk_hz = mbox_clk;
    }
#endif

    div16 = 16u * AUX_BAUD;
    ibrd = clk_hz / div16;
    fbrd = ((clk_hz % div16) * 64u + div16 / 2u) / div16;
    if (ibrd == 0) {
        ibrd = 1;
        fbrd = 0;
    }
    aux_mmio_write32(AUX_IBRD, ibrd);
    aux_mmio_write32(AUX_FBRD, fbrd);
    aux_mmio_write32(AUX_LCRH, AUX_LCRH_WLEN8 | AUX_LCRH_FEN);
    aux_mmio_write32(AUX_ICR, 0x7FFu);
    aux_mmio_write32(AUX_IMSC, 0);
    aux_mmio_write32(AUX_CR, AUX_CR_UARTEN | AUX_CR_TXE | AUX_CR_RXE);

    /* Drain stale bytes (e.g. line noise after pin mux switch) */
    for (int i = 0; i < 64 && !(aux_mmio_read32(AUX_FR) & AUX_FR_RXFE); i++) {
        (void)aux_mmio_read32(AUX_DR);
    }
}

static void aux_putc(uint8_t b)
{
    while (aux_mmio_read32(AUX_FR) & AUX_FR_TXFF) {
        __asm__ volatile("nop");
    }
    aux_mmio_write32(AUX_DR, b);
}

/* Returns byte 0..255 or -1 on timeout */
static int aux_getc_timeout(void)
{
    for (uint32_t i = 0; i < AUX_RX_TIMEOUT; i++) {
        if (!(aux_mmio_read32(AUX_FR) & AUX_FR_RXFE)) {
            return (int)(aux_mmio_read32(AUX_DR) & 0xFFu);
        }
        __asm__ volatile("nop");
    }
    return -1;
}

static void aux_flush_tx(void)
{
    while (aux_mmio_read32(AUX_FR) & AUX_FR_BUSY) {
        __asm__ volatile("nop");
    }
}

/* ------------------------------------------------------------------ */
/* Feetech SMS/STS protocol                                            */
/* ------------------------------------------------------------------ */
#define FT_INST_PING    0x01u
#define FT_INST_READ    0x02u
#define FT_INST_WRITE   0x03u

#define FT_SERVO_ID     1u

/* STS3215 control table addresses */
#define FT_ADDR_TORQUE_ENABLE   40u     /* 1B: 0 off, 1 on */
#define FT_ADDR_ACCELERATION    41u     /* 1B: 0..254 */
#define FT_ADDR_GOAL_POSITION   42u     /* 2B LE: 0..4095 */
#define FT_ADDR_GOAL_SPEED      46u     /* 2B LE */
#define FT_ADDR_MAX_TORQUE      48u     /* 2B LE: 0..1000 */
#define FT_ADDR_PRESENT_POS     56u     /* 2B LE */
#define FT_ADDR_PRESENT_LOAD    60u     /* 2B LE: bit10 = direction */

/* M0 safety limits (SPEC-M0 §WS-DRV) */
#define FT_GOAL_MIN         100     /* stay clear of mechanical ends */
#define FT_GOAL_MAX         900
#define FT_SLEW_PER_TICK    40      /* counts per 1 kHz tick */
#define FT_MIN_DELTA        4       /* skip tiny commands */
#define FT_GOAL_SPEED       500     /* conservative cap */
#define FT_MAX_TORQUE_LIMIT 500     /* ~50% of full scale */
#define FT_ACCEL            50

/* Send one instruction packet, discard the RX echo of our own bytes. */
static void ft_send(uint8_t id, uint8_t inst, const uint8_t *params, int nparams)
{
    uint8_t len = (uint8_t)(nparams + 2);
    uint8_t sum = (uint8_t)(id + len + inst);
    int sent = 0;

    aux_putc(0xFFu); aux_putc(0xFFu); sent += 2;
    aux_putc(id); aux_putc(len); aux_putc(inst); sent += 3;
    for (int i = 0; i < nparams; i++) {
        aux_putc(params[i]);
        sum = (uint8_t)(sum + params[i]);
        sent++;
    }
    aux_putc((uint8_t)(~sum));
    sent++;

    aux_flush_tx();
    /* Half-duplex echo discard (RX hears our own TX on the bus) */
    for (int i = 0; i < sent; i++) {
        if (aux_getc_timeout() < 0) {
            break;      /* don't wedge the caller if echo never arrives */
        }
    }
}

/* Receive one reply packet.  Returns total byte count (>=5) or -1. */
static int ft_recv(uint8_t *buf, int maxlen)
{
    int b0 = aux_getc_timeout();
    int b1, len, total, sum;
    if (b0 < 0) {
        return -1;
    }
    b1 = aux_getc_timeout();
    if (b1 < 0) {
        return -1;
    }
    if (b0 != 0xFF || b1 != 0xFF) {
        return -1;      /* garbage on the bus */
    }
    buf[0] = 0xFF; buf[1] = 0xFF;
    for (int i = 2; i < 4; i++) {
        int b = aux_getc_timeout();
        if (b < 0) {
            return -1;
        }
        buf[i] = (uint8_t)b;
    }
    len = buf[3];                       /* LEN = ERR + params + checksum */
    total = 4 + len;
    if (total > maxlen) {
        return -1;
    }
    for (int i = 4; i < total; i++) {
        int b = aux_getc_timeout();
        if (b < 0) {
            return -1;
        }
        buf[i] = (uint8_t)b;
    }
    sum = buf[2] + buf[3];              /* ID + LEN */
    for (int i = 4; i < total - 1; i++) {
        sum += buf[i];
    }
    if ((uint8_t)(~sum) != buf[total - 1]) {
        return -1;                      /* checksum mismatch */
    }
    return total;
}

static int ft_ping(uint8_t id)
{
    uint8_t buf[16];
    ft_send(id, FT_INST_PING, 0, 0);
    int n = ft_recv(buf, (int)sizeof(buf));
    if (n != 6 || buf[2] != id || buf[3] != 2) {
        return -1;
    }
    return (int)buf[4];                 /* ERR byte, 0 = ok */
}

static int ft_write(uint8_t id, uint8_t addr, const uint8_t *data, int ndata)
{
    uint8_t params[8];
    uint8_t buf[16];
    if (ndata < 1 || ndata > 6) {
        return -1;
    }
    params[0] = addr;
    for (int i = 0; i < ndata; i++) {
        params[1 + i] = data[i];
    }
    ft_send(id, FT_INST_WRITE, params, 1 + ndata);
    int n = ft_recv(buf, (int)sizeof(buf));
    if (n != 6 || buf[2] != id) {
        return -1;
    }
    return (int)buf[4];                 /* ERR byte */
}

/* Read ndata bytes starting at addr into out.  Returns 0 or -1. */
static int ft_read(uint8_t id, uint8_t addr, uint8_t ndata, uint8_t *out)
{
    uint8_t params[2] = { addr, ndata };
    uint8_t buf[16];
    if (ndata > 8) {
        return -1;
    }
    ft_send(id, FT_INST_READ, params, 2);
    int n = ft_recv(buf, (int)sizeof(buf));
    if (n != 6 + (int)ndata || buf[2] != id) {
        return -1;
    }
    if (buf[4] != 0) {
        return -1;                      /* servo error flags */
    }
    for (int i = 0; i < (int)ndata; i++) {
        out[i] = buf[5 + i];
    }
    return 0;
}

static int ft_torque_enable(int on)
{
    uint8_t v = on ? 1u : 0u;
    return ft_write(FT_SERVO_ID, FT_ADDR_TORQUE_ENABLE, &v, 1);
}

/* Goal position + speed in one 6-byte write at addr 42
 * (position 2B LE, time 2B LE = 0, speed 2B LE). */
static int ft_goal(uint16_t pos, uint16_t speed)
{
    uint8_t data[6];
    data[0] = (uint8_t)(pos & 0xFFu);
    data[1] = (uint8_t)(pos >> 8);
    data[2] = 0; data[3] = 0;
    data[4] = (uint8_t)(speed & 0xFFu);
    data[5] = (uint8_t)(speed >> 8);
    return ft_write(FT_SERVO_ID, FT_ADDR_GOAL_POSITION, data, 6);
}

/* Present position (addr 56), <0 on error.  Exposed for M1+ telemetry. */
int feetech_read_position(void)
{
    uint8_t d[2];
    if (ft_read(FT_SERVO_ID, FT_ADDR_PRESENT_POS, 2, d) != 0) {
        return -1;
    }
    return (int)d[0] | ((int)d[1] << 8);
}

/* Present load (addr 60, bit10 = direction), <0 on error. */
int feetech_read_load(void)
{
    uint8_t d[2];
    if (ft_read(FT_SERVO_ID, FT_ADDR_PRESENT_LOAD, 2, d) != 0) {
        return -1;
    }
    return (int)d[0] | ((int)d[1] << 8);
}

/* ------------------------------------------------------------------ */
/* Contract overrides (strong symbols; weak stubs live in WS-DSP)      */
/* ------------------------------------------------------------------ */
static int g_uart_up = 0;
static int g_servo_up = 0;
static int g_estop = 0;
static int g_cur_goal = -1;     /* last commanded goal, -1 = unknown */

int actuator_init(void)
{
    if (!g_uart_up) {
        aux_uart_init();
        g_uart_up = 1;
    }
    g_estop = 0;                        /* m0demo start re-arms */

    if (ft_ping(FT_SERVO_ID) != 0) {
        g_servo_up = 0;
        console_printf("feetech: servo id %u not answering on %s aux UART\n",
                       (unsigned)FT_SERVO_ID, BOARD_NAME);
        return -1;
    }

    /* Sane limits: torque ~50%, acceleration modest (SPEC-M0 §WS-DRV) */
    uint8_t tq[2] = { (uint8_t)(FT_MAX_TORQUE_LIMIT & 0xFF),
                      (uint8_t)(FT_MAX_TORQUE_LIMIT >> 8) };
    (void)ft_write(FT_SERVO_ID, FT_ADDR_MAX_TORQUE, tq, 2);
    uint8_t acc = FT_ACCEL;
    (void)ft_write(FT_SERVO_ID, FT_ADDR_ACCELERATION, &acc, 1);

    /* Seed slew state from the real position; hold where we are */
    int pos = feetech_read_position();
    if (pos >= 0) {
        g_cur_goal = pos < FT_GOAL_MIN ? FT_GOAL_MIN
                   : pos > FT_GOAL_MAX ? FT_GOAL_MAX : pos;
    } else {
        g_cur_goal = (FT_GOAL_MIN + FT_GOAL_MAX) / 2;
    }
    (void)ft_goal((uint16_t)g_cur_goal, FT_GOAL_SPEED);

    if (ft_torque_enable(1) != 0) {
        g_servo_up = 0;
        return -1;
    }
    g_servo_up = 1;
    console_printf("feetech: STS3215 id %u online, torque capped at %u\n",
                   (unsigned)FT_SERVO_ID, (unsigned)FT_MAX_TORQUE_LIMIT);
    return 0;
}

int actuator_set(float level)
{
    if (g_estop || !g_servo_up) {
        return -1;
    }
    if (level < 0.0f) {
        level = 0.0f;
    }
    if (level > 1.0f) {
        level = 1.0f;
    }
    int target = FT_GOAL_MIN +
                 (int)(level * (float)(FT_GOAL_MAX - FT_GOAL_MIN) + 0.5f);
    if (g_cur_goal < 0) {
        g_cur_goal = target;
    }
    int diff = target - g_cur_goal;
    if (diff > FT_SLEW_PER_TICK) {
        diff = FT_SLEW_PER_TICK;
    } else if (diff < -FT_SLEW_PER_TICK) {
        diff = -FT_SLEW_PER_TICK;
    }
    if (diff >= -FT_MIN_DELTA && diff <= FT_MIN_DELTA) {
        return 0;                       /* tiny change: skip bus traffic */
    }
    int goal = g_cur_goal + diff;
    if (ft_goal((uint16_t)goal, FT_GOAL_SPEED) != 0) {
        return -1;
    }
    g_cur_goal = goal;
    return 0;
}

int actuator_estop(void)
{
    g_estop = 1;                        /* latch: re-arm via actuator_init */
    if (!g_uart_up) {
        return 0;
    }
    (void)ft_torque_enable(0);          /* best effort, must never block */
    return 0;
}

const char *actuator_name(void)
{
    return "feetech-sts3215";
}

#endif /* BOARD_HAS_EMG_HW */
