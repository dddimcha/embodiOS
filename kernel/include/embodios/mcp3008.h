/* EMBODIOS BCM SPI0 + MCP3008 ADC interface (WS-DRV, M0)
 * Author: Dmitry Dimcha <dddimcha@gmail.com>
 *
 * bcm_spi0_*: board-gated SPI0 master (kernel/drivers/adc/bcm_spi.c).
 * mcp3008_*:  10-bit ADC over SPI0 (kernel/drivers/adc/mcp3008.c).
 *
 * Only meaningful on boards with real SPI0 (pi4/pi5); pi3/virt builds
 * get stubs that return -1.
 */
#ifndef EMBODIOS_MCP3008_H
#define EMBODIOS_MCP3008_H

#include <stdint.h>

/* SPI0 master: init (GPIO mux + 1 MHz mode 0 CS0) and full-duplex
 * polled transfer. Return 0 on success, -1 if unsupported on board. */
int bcm_spi0_init(void);
int bcm_spi0_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len);

/* MCP3008 single-ended read of channel 0..7.
 * Returns 0..1023, or <0 on error (bad channel / SPI unsupported). */
int mcp3008_read(int ch);

#endif /* EMBODIOS_MCP3008_H */
