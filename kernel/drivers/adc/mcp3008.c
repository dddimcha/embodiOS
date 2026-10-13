/* EMBODIOS MCP3008 ADC Driver (over SPI0)
 * WS-DRV (M0, EMG bench loop), branch m0-drv
 * Author: Dmitry Dimcha <dddimcha@gmail.com>
 *
 * MCP3008: 8-channel 10-bit SAR ADC, SPI slave, mode 0, up to ~1.35 MHz
 * at 3.3 V (we run 1 MHz).  Single-ended read = 3-byte transaction
 * (MCP3008 datasheet DS21295, sec. 5/6, figure 6-1):
 *   TX: 0x01 (start), (0x08 | ch) << 4 (SGL/DIFF=1 + channel in D2..D0),
 *       0x00 (dummy clocks out the result)
 *   RX: byte1 bits [1:0] = B9..B8, byte2 = B7..B0
 *   value = ((rx[1] & 0x03) << 8) | rx[2]
 *
 * Also hosts the WS-DRV strong overrides of the contract hooks
 * emg_hw_available()/emg_hw_read() (weak defaults live in WS-DSP's
 * kernel/core/emg/emg.c).  Built strong only when the Makefile defines
 * BOARD_HAS_EMG_HW=1 (pi4/pi5); pi3/virt/x86 keep the weak synthetic
 * source.
 */

#include "embodios/types.h"
#include "embodios/console.h"
#include <embodios/mcp3008.h>
#include <embodios/emg.h>
#include "board.h"

int mcp3008_read(int ch)
{
#if defined(BOARD_PI4) || defined(BOARD_PI5)
    uint8_t tx[3], rx[3];

    if (ch < 0 || ch > 7) {
        return -1;
    }
    tx[0] = 0x01u;
    tx[1] = (uint8_t)((0x08u | (uint8_t)ch) << 4);
    tx[2] = 0x00u;
    if (bcm_spi0_transfer(tx, rx, 3) != 0) {
        return -1;
    }
    return ((int)(rx[1] & 0x03u) << 8) | (int)rx[2];
#else
    (void)ch;
    console_printf("mcp3008: SPI0 unsupported on this board (%s)\n",
                   BOARD_NAME);
    return -1;
#endif
}

#if defined(BOARD_HAS_EMG_HW) && BOARD_HAS_EMG_HW
/* Strong overrides of the weak contract hooks (SPEC-M0 §contract).
 * One-shot lazy init: first use brings up SPI0; on pi4/pi5 the register
 * pokes cannot fail, so hardware presence of the MCP3008 itself is NOT
 * probed (reads on a bare pin return rail/noise counts) — the wiring
 * check is a bench procedure, see docs/emg-bench-wiring.md. */

static int g_emg_spi_state = 0;     /* 0 = not tried, 1 = up, -1 = failed */

int emg_hw_available(void)
{
    if (g_emg_spi_state == 0) {
        g_emg_spi_state = (bcm_spi0_init() == 0) ? 1 : -1;
        if (g_emg_spi_state == 1) {
            console_printf("mcp3008: SPI0 up (CS0, mode 0, 1 MHz) on %s\n",
                           BOARD_NAME);
        }
    }
    return g_emg_spi_state == 1 ? 1 : 0;
}

int emg_hw_read(int ch)
{
    if (g_emg_spi_state == 0) {
        (void)emg_hw_available();
    }
    if (g_emg_spi_state != 1) {
        return -1;
    }
    return mcp3008_read(ch);
}
#endif /* BOARD_HAS_EMG_HW */
