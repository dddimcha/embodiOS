/* EMBODIOS BCM/RP1 SPI0 Driver (polled master)
 * WS-DRV (M0, EMG bench loop), branch m0-drv
 * Author: Dmitry Dimcha <dddimcha@gmail.com>
 *
 * SPI0 master for the MCP3008 ADC: CS0, mode 0 (CPOL=0, CPHA=0), 1 MHz.
 *
 * Boards (gated via board.h):
 *  - pi4 (BCM2711): classic BCM2835-style SPI0 @ PERIPH_BASE + 0x204000,
 *    GPIO 7-11 -> ALT0 (GPFSEL0/1).  Core clock assumed 250 MHz
 *    (BCM2835 ARM Peripherals ch. 10: SCLK = core_clk / CDIV).
 *  - pi5 (BCM2712+RP1): RP1 SPI0 is a Synopsys DesignWare DW-APB-SSI
 *    (NOT the BCM2835 block!) at RP1 bus 0x40050000, i.e. CPU phys
 *    PERIPH_BASE + 0x50000 (Linux rp1.dtsi: spi@50000,
 *    compatible "snps,dw-apb-ssi", ssi_clk = RP1_CLK_SYS ~200 MHz).
 *    GPIO 7-11 -> function a0 (RP1 io_bank0 GPIOx_CTRL FSEL=0):
 *    verified against `pinctrl funcs 0-27` dump (forums.raspberrypi.com
 *    t=358646): GPIO7=SPI0_CE1, 8=SPI0_CE0, 9=SPI0_MISO, 10=SPI0_MOSI,
 *    11=SPI0_SCLK, all function a0.
 *  - pi3/virt: compile-time stub, prints "SPI0 unsupported on this
 *    board" (raspi3b QEMU does not model the SPI block; real Pi3 SPI0
 *    would work with the same register code as pi4 but is out of M0
 *    scope).
 *
 * Sources:
 *  - BCM2835 ARM Peripherals, ch. 10 (SPI) and ch. 6 (GPIO FSEL).
 *  - Raspberry Pi RP1 Peripherals datasheet RP-008370-DS-1, sec. 3.6
 *    (SSI address map: spi0 @ 0x40050000) and sec. 2 (GPIO functions).
 *  - DesignWare DW_apb_ssi register map (CTRLR0/SSIENR/BAUDR/SR/DR
 *    offsets as implemented in Linux drivers/spi/spi-dw-core.c).
 */

#include "embodios/types.h"
#include "embodios/console.h"
#include "board.h"

#include <embodios/mcp3008.h>

#if defined(BOARD_PI4) || defined(BOARD_PI5)

/* ------------------------------------------------------------------ */
/* Common MMIO helpers (pattern from arch/aarch64/uart.c)             */
/* ------------------------------------------------------------------ */
static void spi_mmio_write32(uintptr_t addr, uint32_t value)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    __asm__ volatile("str %w[val], [%[ptr]]"
                     : : [val] "r" (value), [ptr] "r" (ptr) : "memory");
}

static uint32_t spi_mmio_read32(uintptr_t addr)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    uint32_t val;
    __asm__ volatile("ldr %w[val], [%[ptr]]"
                     : [val] "=r" (val) : [ptr] "r" (ptr) : "memory");
    return val;
}
#endif /* BOARD_PI4 || BOARD_PI5 */

/* ================================================================== */
/* Raspberry Pi 4: BCM2711 SPI0 (BCM2835-compatible)                  */
/* ================================================================== */
#if defined(BOARD_PI4)

#define SPI0_BASE       (PERIPH_BASE + 0x204000UL)
#define SPI_CS          (SPI0_BASE + 0x00)  /* Control & Status */
#define SPI_FIFO        (SPI0_BASE + 0x04)  /* TX/RX FIFO */
#define SPI_CLK         (SPI0_BASE + 0x08)  /* Clock divider */

/* CS register bits (BCM2835 ARM Peripherals p. 152) */
#define SPI_CS_CLEAR_TX     (1u << 4)
#define SPI_CS_CLEAR_RX     (1u << 5)
#define SPI_CS_TA           (1u << 7)   /* Transfer Active */
#define SPI_CS_DONE         (1u << 16)
#define SPI_CS_RXD          (1u << 17)  /* RX FIFO has data */
#define SPI_CS_TXD          (1u << 18)  /* TX FIFO accepts data */

/* GPIO block (BCM2835 ch. 6): GPFSELn, 3 bits/pin, ALT0 = 0b100 */
#define GPIO_BASE_P4        (PERIPH_BASE + 0x200000UL)
#define GPFSEL0_P4          (GPIO_BASE_P4 + 0x00)
#define GPFSEL1_P4          (GPIO_BASE_P4 + 0x04)
#define GPIO_FSEL_ALT0_P4   4u

/* SPI core clock: 250 MHz default on BCM2711 (same as BCM2835 family).
 * CDIV = 250 -> 1 MHz SCLK.
 * TODO(hw): verify core clock on hardware (firmware may re-clock; can be
 * confirmed via mailbox clock id 3 = CORE). */
#define SPI_CORE_CLK_HZ_P4  250000000u
#define SPI_SCLK_HZ         1000000u

static void spi0_gpio_init_pi4(void)
{
    /* GPIO7 (CE1), GPIO8 (CE0), GPIO9 (MISO) live in GPFSEL0 bits
     * [23:21], [26:24], [29:27]; GPIO10 (MOSI), GPIO11 (SCLK) live in
     * GPFSEL1 bits [2:0], [5:3]. */
    uint32_t r = spi_mmio_read32(GPFSEL0_P4);
    r &= ~((7u << 21) | (7u << 24) | (7u << 27));
    r |= (GPIO_FSEL_ALT0_P4 << 21) | (GPIO_FSEL_ALT0_P4 << 24) |
         (GPIO_FSEL_ALT0_P4 << 27);
    spi_mmio_write32(GPFSEL0_P4, r);

    r = spi_mmio_read32(GPFSEL1_P4);
    r &= ~((7u << 0) | (7u << 3));
    r |= (GPIO_FSEL_ALT0_P4 << 0) | (GPIO_FSEL_ALT0_P4 << 3);
    spi_mmio_write32(GPFSEL1_P4, r);
}

int bcm_spi0_init(void)
{
    spi0_gpio_init_pi4();

    /* 1 MHz: CDIV = 250 MHz / 1 MHz = 250 (even, in range) */
    spi_mmio_write32(SPI_CLK, SPI_CORE_CLK_HZ_P4 / SPI_SCLK_HZ);

    /* CS0, mode 0 (CPHA=0, CPOL=0), clear FIFOs, idle (TA=0) */
    spi_mmio_write32(SPI_CS, SPI_CS_CLEAR_TX | SPI_CS_CLEAR_RX);
    return 0;
}

int bcm_spi0_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    uint32_t tx_n = 0, rx_n = 0;

    spi_mmio_write32(SPI_CS, SPI_CS_CLEAR_TX | SPI_CS_CLEAR_RX);
    spi_mmio_write32(SPI_CS, SPI_CS_TA);    /* CS0, mode 0, go */

    while (rx_n < len) {
        uint32_t cs = spi_mmio_read32(SPI_CS);
        if (tx_n < len && (cs & SPI_CS_TXD)) {
            spi_mmio_write32(SPI_FIFO, tx[tx_n++]);
        }
        if (cs & SPI_CS_RXD) {
            rx[rx_n++] = (uint8_t)(spi_mmio_read32(SPI_FIFO) & 0xFFu);
        }
    }
    while (!(spi_mmio_read32(SPI_CS) & SPI_CS_DONE)) {
        __asm__ volatile("nop");
    }
    spi_mmio_write32(SPI_CS, 0);            /* TA=0: CS0 deasserts */
    return 0;
}

/* ================================================================== */
/* Raspberry Pi 5: RP1 SPI0 (Synopsys DesignWare DW-APB-SSI master)   */
/* ================================================================== */
#elif defined(BOARD_PI5)

/* RP1 bus 0x40050000 -> CPU phys PERIPH_BASE + 0x50000
 * (same window math as bcm_gpio.c: io_bank0 0x400D0000 -> +0xD0000). */
#define SSI0_BASE       (PERIPH_BASE + 0x050000UL)
#define SSI_CTRLR0      (SSI0_BASE + 0x00)
#define SSI_SSIENR      (SSI0_BASE + 0x08)
#define SSI_SER         (SSI0_BASE + 0x10)  /* Slave Enable: bit0 = CS0 */
#define SSI_BAUDR       (SSI0_BASE + 0x14)  /* SCKDV, even, >= 2 */
#define SSI_TXFLR       (SSI0_BASE + 0x20)
#define SSI_SR          (SSI0_BASE + 0x28)
#define SSI_IMR         (SSI0_BASE + 0x2C)
#define SSI_ICR         (SSI0_BASE + 0x48)
#define SSI_DR          (SSI0_BASE + 0x60)

#define SSI_SR_BUSY     (1u << 0)
#define SSI_SR_TFNF     (1u << 1)           /* TX FIFO not full */
#define SSI_SR_RFNE     (1u << 3)           /* RX FIFO not empty */

/* CTRLR0: DFS_32[4:0] = 7 -> 8-bit frames (DW_apb_ssi v4, 5-bit DFS),
 * FRF[5:4]=0 Motorola SPI, SCPH[6]=0, SCPOL[7]=0 -> mode 0,
 * TMOD[9:8]=0 transmit & receive. */
#define SSI_CTRLR0_DFS8_MODE0   0x07u

/* ssi_clk = RP1 CLK_SYS, ~200 MHz (rp1.dtsi rp1_spi0 clocks =
 * rp1_clocks RP1_CLK_SYS).  BAUDR = 200 -> 1 MHz SCLK.
 * TODO(hw): verify clk_sys rate on hardware (RP1 clocks block @
 * 0x40018000); if clk_sys differs, scale BAUDR accordingly. */
#define RP1_CLK_SYS_HZ      200000000u
#define SPI_SCLK_HZ         1000000u

/* RP1 GPIO (pattern from drivers/gpio/bcm_gpio.c, RP1 datasheet sec. 2):
 * io_bank0 @ RP1 0x400D0000, pads_bank0 @ 0x400F0000. */
#define RP1_IO_BANK0_SPI    (PERIPH_BASE + 0x0D0000UL)
#define RP1_PADS_BANK0_SPI  (PERIPH_BASE + 0x0F0000UL)
#define RP1_GPIO_CTRL_SPI(pin)  (RP1_IO_BANK0_SPI + 8UL * (pin) + 4UL)
#define RP1_PAD_SPI(pin)        (RP1_PADS_BANK0_SPI + 0x04UL + 4UL * (pin))

#define RP1_FSEL_SPI0       0u          /* a0 = SPI0 on GPIO 7-11 */
#define RP1_PAD_IE          (1u << 6)
#define RP1_PAD_DRIVE_4MA   (1u << 4)
#define RP1_PAD_PUE         (1u << 3)
#define RP1_PAD_SCHMITT     (1u << 1)

static void spi0_gpio_init_pi5(void)
{
    static const uint8_t out_pins[] = { 7, 8, 10, 11 }; /* CE1 CE0 MOSI SCLK */

    for (unsigned i = 0; i < sizeof(out_pins); i++) {
        uint8_t p = out_pins[i];
        spi_mmio_write32(RP1_GPIO_CTRL_SPI(p), RP1_FSEL_SPI0);
        spi_mmio_write32(RP1_PAD_SPI(p), RP1_PAD_DRIVE_4MA | RP1_PAD_SCHMITT);
    }
    /* MISO: input enable + pull-up (idle high when no device drives it) */
    spi_mmio_write32(RP1_GPIO_CTRL_SPI(9), RP1_FSEL_SPI0);
    spi_mmio_write32(RP1_PAD_SPI(9), RP1_PAD_IE | RP1_PAD_DRIVE_4MA |
                                     RP1_PAD_PUE | RP1_PAD_SCHMITT);

    /* TODO(hw): RP1 reset/clock gating not touched — Pi5 firmware brings
     * RP1 up with peripherals unreset and clk_sys running (same assumption
     * the v0.9.0 RP1 UART0 path makes).  If SPI0 reads back dead on metal,
     * check resets @ RP1 0x40014000 and clocks @ 0x40018000. */
}

int bcm_spi0_init(void)
{
    spi0_gpio_init_pi5();

    spi_mmio_write32(SSI_SSIENR, 0);            /* disable before config */
    spi_mmio_write32(SSI_CTRLR0, SSI_CTRLR0_DFS8_MODE0);
    spi_mmio_write32(SSI_BAUDR, RP1_CLK_SYS_HZ / SPI_SCLK_HZ); /* 200 */
    spi_mmio_write32(SSI_IMR, 0);               /* polled: no IRQs */
    spi_mmio_write32(SSI_SER, 1u << 0);         /* route CS0 (GPIO8) */
    spi_mmio_write32(SSI_ICR, 0xFFFFFFFFu);     /* clear sticky IRQs */
    spi_mmio_write32(SSI_SSIENR, 1);            /* enable */
    return 0;
}

int bcm_spi0_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    uint32_t tx_n = 0, rx_n = 0;

    while (rx_n < len) {
        uint32_t sr = spi_mmio_read32(SSI_SR);
        /* TX FIFO is 8-deep on RP1 SSI; never overfill */
        if (tx_n < len && (sr & SSI_SR_TFNF) &&
            spi_mmio_read32(SSI_TXFLR) < 8) {
            spi_mmio_write32(SSI_DR, tx[tx_n++]);
        }
        if (sr & SSI_SR_RFNE) {
            rx[rx_n++] = (uint8_t)(spi_mmio_read32(SSI_DR) & 0xFFu);
        }
    }
    while (spi_mmio_read32(SSI_SR) & SSI_SR_BUSY) {
        __asm__ volatile("nop");
    }
    return 0;
}

/* ================================================================== */
/* pi3 / virt / anything else: unsupported                            */
/* ================================================================== */
#else

int bcm_spi0_init(void)
{
    console_printf("SPI0 unsupported on this board (%s)\n", BOARD_NAME);
    return -1;
}

int bcm_spi0_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    (void)tx; (void)rx; (void)len;
    console_printf("SPI0 unsupported on this board (%s)\n", BOARD_NAME);
    return -1;
}

#endif
