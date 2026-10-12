/* EMBODIOS BCM2835/2711/2712 VideoCore mailbox property interface
 * WS-B (pi-periph), v0.9.0 "Galvani"
 *
 * Tag IDs verified against the official firmware wiki:
 *   https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 * Register layout verified against:
 *   BCM2835 ARM Peripherals, sec. 1.3 (mailbox) and Ultibo BCM2835 unit
 *   (mailbox0 regs @ PERIPH+0xB880, mailbox1 regs @ PERIPH+0xB8A0).
 *
 * On boards without a mailbox (QEMU virt, BOARD_HAS_MAILBOX=0) every
 * function is a stub returning -1 (0 for bcm_mbox_available).
 */
#ifndef EMBODIOS_BCM_MAILBOX_H
#define EMBODIOS_BCM_MAILBOX_H

#include <embodios/types.h>
#include <stddef.h>

/* Property tags (firmware wiki) */
#define MBOX_TAG_GET_FW_REVISION    0x00000001u
#define MBOX_TAG_GET_BOARD_MODEL    0x00010001u
#define MBOX_TAG_GET_BOARD_REVISION 0x00010002u
#define MBOX_TAG_GET_CLOCK_STATE    0x00030001u
#define MBOX_TAG_SET_CLOCK_STATE    0x00038001u
#define MBOX_TAG_GET_CLOCK_RATE     0x00030002u
#define MBOX_TAG_SET_CLOCK_RATE     0x00038002u
#define MBOX_TAG_FB_ALLOCATE        0x00040001u
#define MBOX_TAG_FB_SET_PHYS_WH     0x00048003u
#define MBOX_TAG_FB_SET_VIRT_WH     0x00048004u
#define MBOX_TAG_FB_SET_DEPTH       0x00048005u
#define MBOX_TAG_FB_SET_PIXEL_ORDER 0x00048006u
#define MBOX_TAG_FB_GET_PITCH       0x00040008u
#define MBOX_TAG_END                0x00000000u

/* Clock IDs (firmware wiki, "Clocks" section) */
#define MBOX_CLK_EMMC               1u
#define MBOX_CLK_UART               2u
#define MBOX_CLK_ARM                3u
#define MBOX_CLK_CORE               4u

/* Property channel: ARM -> VC request channel */
#define MBOX_CHANNEL_PROP           8u

/* Returns 1 when the board has a mailbox (pi*), 0 on virt. */
int  bcm_mbox_available(void);

/* Low-level property call: buf must point to a complete property message
 * (size/request/tags/end) that is 16-byte aligned.  Performs the required
 * D-cache maintenance (dc civac before, dc ivac after).  Returns 0 on
 * success (VC response code 0x80000000), -1 on timeout/error. */
int  bcm_mbox_call(volatile uint32_t *buf, uint32_t buf_bytes);

/* Shared 16-byte-aligned property buffer helpers (static storage).
 * bcm_mbox_buffer() returns the static buffer (MBOX_BUFFER_BYTES max);
 * bcm_mbox_execute() issues the call for the first used_bytes of it. */
#define MBOX_BUFFER_BYTES           1024u
volatile uint32_t *bcm_mbox_buffer(uint32_t required_bytes);
int  bcm_mbox_execute(uint32_t used_bytes);

/* Convenience queries.  Return 0 and fill *out on success, -1 otherwise. */
int  bcm_mbox_get_fw_revision(uint32_t *out);
int  bcm_mbox_get_board_model(uint32_t *out);
int  bcm_mbox_get_clock_rate(uint32_t clock_id, uint32_t *out_hz);
int  bcm_mbox_set_clock_rate(uint32_t clock_id, uint32_t hz,
                             uint32_t *out_actual_hz);

/* UART base clock helpers (clock id MBOX_CLK_UART). */
int  bcm_mbox_get_uart_clock(uint32_t *out_hz);
int  bcm_mbox_set_uart_clock(uint32_t hz, uint32_t *out_actual_hz);

/* Reset the board via the BCM2835 PM watchdog (PM block @ PERIPH+0x100000;
 * there is no mailbox property tag for reset, so "mailbox reset" is
 * implemented with the watchdog the same way the firmware's own reset
 * path does).  Does not return on real hardware.  On boards without the
 * PM block (virt) it is a stub that just returns -1; the caller (WS-A
 * cmd_power fallback) then falls back to wfi. */
int  bcm_mbox_reset(void);

#endif /* EMBODIOS_BCM_MAILBOX_H */
