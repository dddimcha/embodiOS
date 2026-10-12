/* EMBODIOS ARM64 Board Configuration (v0.9.0 "Galvani")
 *
 * Runtime side of board.h: the DTB hand-over global and a boot banner
 * line naming the selected board and its key base addresses.
 *
 * Address provenance (verified against primary sources for v0.9.0):
 *   pi3:  Linux arch/arm64/boot/dts/broadcom/bcm2837.dtsi (GIC-400 @
 *         0x40000000, GICD/GICC at GIC-400 TRM offsets +0x1000/+0x2000),
 *         BCM2835 ARM Peripherals (legacy bus 0x7E... -> ARM 0x3F...).
 *   pi4:  BCM2711 ARM Peripherals sec.6.5.1 (GIC-400 base 0xFF840000 in
 *         low-peripheral mode) + ARM GIC-400 TRM sec.3.2 (GICD +0x1000,
 *         GICC +0x2000). NOTE: SPEC draft listed 0xFF840000/0xFF850000;
 *         corrected to 0xFF841000/0xFF842000.
 *   pi5:  Linux arch/arm64/boot/dts/broadcom/bcm2712.dtsi:
 *         soc@107c000000 (ranges 0x7c000000 -> 0x107c000000),
 *         uart0 serial@7d001000, gicv2 interrupt-controller@107fff9000
 *         (reg 0x107fff9000/0x107fffa000), mailbox@7c013880. NOTE: SPEC
 *         draft listed mailbox 0x107C01B880; corrected to 0x107C013880.
 *   virt: qemu-system-aarch64 -M virt map (UART 0x09000000, GICv2
 *         0x08000000/0x08010000).
 */

#include "board.h"
#include "embodios/types.h"

/* Saved by boot.S before BSS clear. Must live in .data (NOT .bss) so the
 * value survives the early BSS wipe; the section attribute keeps it out of
 * .bss despite the zero initializer. */
__attribute__((section(".data")))
uintptr_t g_dtb_ptr = 0;

extern void uart_puts(const char* str);
extern void uart_putchar(char c);

static void print_hex64(uint64_t v)
{
    static const char hexd[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        uart_putchar(hexd[(v >> i) & 0xF]);
}

static void print_hex32(uint32_t v)
{
    static const char hexd[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        uart_putchar(hexd[(v >> i) & 0xF]);
}

void board_print_info(void)
{
    uart_puts("Board: ");
    uart_puts(BOARD_NAME);
    uart_puts(" (PERIPH=");
#if defined(BOARD_PI5)
    print_hex64(PERIPH_BASE);
#else
    print_hex32((uint32_t)PERIPH_BASE);
#endif
    uart_puts(" UART0=");
#if defined(BOARD_PI5)
    print_hex64(UART0_BASE);
#else
    print_hex32((uint32_t)UART0_BASE);
#endif
    uart_puts(" GIC=");
#if defined(BOARD_PI5)
    print_hex64(GICD_BASE);
#else
    print_hex32((uint32_t)GICD_BASE);
#endif
    uart_puts(" LINK=");
#if defined(BOARD_PI5) || defined(BOARD_PI4) || defined(BOARD_PI3)
    print_hex32((uint32_t)LINK_BASE);
#else
    print_hex32((uint32_t)LINK_BASE);
#endif
    uart_puts(")\n");
    uart_puts("DTB:   ");
    print_hex64((uint64_t)g_dtb_ptr);
    uart_puts("\n");
}
