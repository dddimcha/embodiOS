/* EMBODIOS ARM64 Board Configuration (v0.9.0 "Galvani")
 *
 * Single source of truth for per-board memory maps. Selected at build time
 * via -DBOARD_PI3 / -DBOARD_PI4 / -DBOARD_PI5; default is QEMU virt.
 *
 * This header is included by both C and preprocessed assembly (.S) files;
 * everything outside the __ASSEMBLER__ guard must be pure #defines.
 *
 * Address sources (verified v0.9.0, see docs in board.c):
 *   pi3  (BCM2837/BCM2710): Linux bcm2837.dtsi, BCM2835 ARM Peripherals
 *   pi4  (BCM2711):         BCM2711 ARM Peripherals sec.6.5.1 (GIC-400 base
 *                           0xFF840000 low-peripheral), GIC-400 TRM sec.3.2
 *                           (GICD +0x1000, GICC +0x2000)
 *   pi5  (BCM2712+RP1):     Linux bcm2712.dtsi (soc@107c000000, ranges
 *                           0x7c000000->0x107c000000; uart0@7d001000,
 *                           gicv2@107fff9000, mailbox@7c013880)
 *   virt (QEMU):            qemu hw/arm/virt.c memory map
 */
#ifndef EMBODIOS_BOARD_H
#define EMBODIOS_BOARD_H

#if defined(BOARD_PI3)
/* ---- Raspberry Pi 3 (BCM2837, Cortex-A53) ---- */
#define BOARD_NAME          "pi3"
#define PERIPH_BASE         0x3F000000UL
#define UART0_BASE          0x3F201000UL      /* PL011 (serial0 on qemu raspi3b) */
#define GICD_BASE           0x40001000UL      /* GIC-400 @0x40000000 + 0x1000 */
#define GICC_BASE           0x40002000UL      /* GIC-400 @0x40000000 + 0x2000 */
#define MAILBOX_BASE        0x3F00B880UL      /* legacy 0x7E00B880 */
#define BOARD_HAS_MAILBOX   1
#define LINK_BASE           0x80000UL         /* kernel8.img load address */
#define GIC_UART_INTID      89                /* SPI 57 (32+57) PL011 UART0 */

#elif defined(BOARD_PI4)
/* ---- Raspberry Pi 4 (BCM2711, Cortex-A72) ---- */
#define BOARD_NAME          "pi4"
#define PERIPH_BASE         0xFE000000UL      /* low-peripheral mode */
#define UART0_BASE          0xFE201000UL
#define GICD_BASE           0xFF841000UL      /* GIC-400 @0xFF840000 + 0x1000 */
#define GICC_BASE           0xFF842000UL      /* GIC-400 @0xFF840000 + 0x2000 */
#define MAILBOX_BASE        0xFE00B880UL
#define BOARD_HAS_MAILBOX   1
#define LINK_BASE           0x80000UL
#define GIC_UART_INTID      153               /* SPI 121 PL011 UART0 */
/* NOTE: real Pi 4 firmware requires enable_gic=1 in config.txt for the
 * GIC to be usable from bare metal (emitted by scripts/make_sdcard.sh). */

#elif defined(BOARD_PI5)
/* ---- Raspberry Pi 5 (BCM2712 + RP1, Cortex-A76) ---- */
#define BOARD_NAME          "pi5"
#define PERIPH_BASE         0x107C000000UL    /* soc window (RP1), 64MB */
#define UART0_BASE          0x107D001000UL    /* dtsi serial@7d001000 */
#define GICD_BASE           0x107FFF9000UL    /* dtsi gicv2 reg[0] */
#define GICC_BASE           0x107FFFA000UL    /* dtsi gicv2 reg[1] */
#define MAILBOX_BASE        0x107C013880UL    /* dtsi mailbox@7c013880 */
#define BOARD_HAS_MAILBOX   1
#define LINK_BASE           0x80000UL
#define GIC_UART_INTID      153               /* dtsi GIC_SPI 121 -> 32+121 */

#else
/* ---- QEMU virt (default; unchanged v0.8.0 behavior) ---- */
#define BOARD_NAME          "virt"
#define PERIPH_BASE         0x0UL
#define UART0_BASE          0x09000000UL
#define GICD_BASE           0x08000000UL
#define GICC_BASE           0x08010000UL
#define MAILBOX_BASE        0x0UL
#define BOARD_HAS_MAILBOX   0
#define LINK_BASE           0x40000000UL
#define GIC_UART_INTID      33                /* SPI 1 */
#endif

/* Board-is-Pi predicate (link base 0x80000, kernel8.img, firmware boot) */
#if defined(BOARD_PI3) || defined(BOARD_PI4) || defined(BOARD_PI5)
#define BOARD_IS_PI         1
#else
#define BOARD_IS_PI         0
#endif

#ifndef __ASSEMBLER__

#include "embodios/types.h"

/* DTB pointer handed over by the boot loader in x0 (Pi firmware / QEMU
 * -kernel). Saved by boot.S before BSS is cleared; parsing is future work. */
extern uintptr_t g_dtb_ptr;

/* Print a one-line board identity summary to the (already up) UART. */
void board_print_info(void);

#endif /* __ASSEMBLER__ */

#endif /* EMBODIOS_BOARD_H */
