/* EMBODIOS VideoCore mailbox framebuffer + text console
 * WS-B (pi-periph), v0.9.0 "Galvani"
 * Author: Dmitry Dimcha <dddimcha@gmail.com>
 *
 * Allocates a 1024x768x32 framebuffer through the mailbox property
 * channel (one combined request: set physical/virtual size, depth, pixel
 * order, allocate, get pitch — the firmware requires all FB tags in a
 * single message) and renders a 128x48 cell text console with an 8x16
 * font.  The PL011 serial line stays the authoritative console; this is
 * a best-effort mirror for the HDMI display.
 *
 * Sources:
 *  - https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 *    ("Frame Buffer" section: all tags processed in one operation;
 *    allocate 0x00040001 returns bus base + size; pitch 0x00040008)
 *  - FB bus address -> ARM physical: mask with 0x3FFFFFFF (L2 aliases),
 *    as done by Circle/raspix and documented in the BCM2835 datasheet's
 *    ARM<->bus address mapping (sec. 1.2).
 */

#include <embodios/mailbox_fb.h>
#include <embodios/font8x16.h>
#include <embodios/bcm_mailbox.h>

/* TEMPORARY: shim in lieu of WS-A's kernel/arch/aarch64/board.h.
 * Integrator: replace with the real board.h include and delete
 * kernel/include/arch-aarch64-board-shim/. */
#include "board.h"

#if defined(__aarch64__) && defined(BOARD_HAS_MAILBOX) && BOARD_HAS_MAILBOX

extern void uart_puts(const char *str);   /* boot line straight to serial */

#define FB_PIXEL_ORDER_RGB  1u
#define FB_FG               0x00FFFFFFu   /* white */
#define FB_BG               0x00000000u   /* black */

static volatile uint32_t *fb_base;
static uint32_t fb_pitch;                 /* bytes per line */
static uint32_t fb_size;                  /* bytes */
static int      fb_state;                 /* 0=untried, 1=active, -1=failed */

static uint32_t fb_col, fb_row;

/* ANSI escape stripping state machine */
#define ANSI_NORMAL 0
#define ANSI_ESC    1
#define ANSI_CSI    2
#define ANSI_OSC    3
static int ansi_state;

static void fb_cache_clean(uintptr_t addr, uintptr_t len)
{
    uintptr_t start = addr & ~63UL;
    uintptr_t end = (addr + len + 63UL) & ~63UL;
    for (uintptr_t p = start; p < end; p += 64) {
        __asm__ volatile("dc cvac, %0" :: "r" (p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void fb_print_hex(uint32_t v)
{
    char buf[11];
    const char *hex = "0123456789abcdef";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - i * 4)) & 0xF];
    }
    buf[10] = '\0';
    uart_puts(buf);
}

static void fb_print_dec(uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    if (v == 0) {
        uart_puts("0");
        return;
    }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    uart_puts(&buf[i]);
}

int mailbox_fb_init(void)
{
    if (fb_state != 0) {
        return (fb_state == 1) ? 0 : -1;
    }

    /* Combined property message:
     *   SET_PHYS_WH(8) SET_VIRT_WH(8) SET_DEPTH(4) SET_PIXEL_ORDER(4)
     *   ALLOCATE(4 req / 8 resp) GET_PITCH(4 resp) END
     * Allocate value buffer must be max(req, resp) per tag. */
    volatile uint32_t *b = bcm_mbox_buffer(MBOX_BUFFER_BYTES);
    if (!b) {
        fb_state = -1;
        return -1;
    }

    uint32_t i = 0;
    b[i++] = 0;                    /* [0] total size, filled below */
    b[i++] = 0;                    /* [1] request code */

    const uint32_t phys_idx = i;
    b[i++] = MBOX_TAG_FB_SET_PHYS_WH;  /* [2..] */
    b[i++] = 8;
    b[i++] = 0;
    b[i++] = FBCON_WIDTH;
    b[i++] = FBCON_HEIGHT;

    const uint32_t virt_idx = i;
    b[i++] = MBOX_TAG_FB_SET_VIRT_WH;
    b[i++] = 8;
    b[i++] = 0;
    b[i++] = FBCON_WIDTH;
    b[i++] = FBCON_HEIGHT;

    const uint32_t depth_idx = i;
    b[i++] = MBOX_TAG_FB_SET_DEPTH;
    b[i++] = 4;
    b[i++] = 0;
    b[i++] = FBCON_DEPTH;

    b[i++] = MBOX_TAG_FB_SET_PIXEL_ORDER;
    b[i++] = 4;
    b[i++] = 0;
    b[i++] = FB_PIXEL_ORDER_RGB;

    const uint32_t alloc_idx = i;
    b[i++] = MBOX_TAG_FB_ALLOCATE;
    b[i++] = 8;                    /* response base+size */
    b[i++] = 0;
    b[i++] = 4096;                 /* request: alignment */
    b[i++] = 0;

    const uint32_t pitch_idx = i;
    b[i++] = MBOX_TAG_FB_GET_PITCH;
    b[i++] = 4;
    b[i++] = 0;
    b[i++] = 0;

    b[i++] = MBOX_TAG_END;
    b[0] = i * 4;

    if (bcm_mbox_execute(i * 4) != 0) {
        fb_state = -1;
        return -1;
    }

    /* Validate responses */
    uint32_t width = b[phys_idx + 3];
    uint32_t height = b[phys_idx + 4];
    uint32_t depth = b[depth_idx + 3];
    uint32_t base_bus = b[alloc_idx + 3];
    uint32_t size = b[alloc_idx + 4];
    uint32_t pitch = b[pitch_idx + 3];
    (void)virt_idx;

    if (width != FBCON_WIDTH || height != FBCON_HEIGHT ||
        depth != FBCON_DEPTH || base_bus == 0 || pitch == 0 || size == 0) {
        fb_state = -1;
        return -1;
    }

    fb_base = (volatile uint32_t *)(uintptr_t)(base_bus & 0x3FFFFFFFu);
    fb_pitch = pitch;
    fb_size = size;

    /* Clear to black */
    for (uint32_t y = 0; y < FBCON_HEIGHT; y++) {
        volatile uint32_t *line = (volatile uint32_t *)((volatile const uint8_t *)fb_base + (uintptr_t)y * fb_pitch);
        for (uint32_t x = 0; x < FBCON_WIDTH; x++) {
            line[x] = FB_BG;
        }
    }
    fb_cache_clean((uintptr_t)fb_base, (uintptr_t)fb_pitch * FBCON_HEIGHT);

    fb_col = 0;
    fb_row = 0;
    ansi_state = ANSI_NORMAL;
    fb_state = 1;

    /* Boot line on the serial console (authoritative log) */
    uart_puts("FB: 1024x768 pitch ");
    fb_print_dec(fb_pitch);
    uart_puts(" base ");
    fb_print_hex((uint32_t)(uintptr_t)fb_base);
    uart_puts(" size ");
    fb_print_dec(fb_size);
    uart_puts("\n");
    return 0;
}

int fbcon_active(void)
{
    return fb_state == 1;
}

static void fbcon_draw_cell(uint32_t col, uint32_t row, uint8_t ch)
{
    const uint8_t *glyph = font8x16_glyphs[ch & 0x7F];
    volatile uint8_t *cell = (volatile uint8_t *)fb_base +
                             (uintptr_t)row * FONT8X16_HEIGHT * fb_pitch +
                             (uintptr_t)col * FONT8X16_WIDTH * 4u;
    for (uint32_t y = 0; y < FONT8X16_HEIGHT; y++) {
        volatile uint32_t *px = (volatile uint32_t *)(cell + (uintptr_t)y * fb_pitch);
        uint8_t bits = glyph[y];
        /* font8x8 data: bit 0 = leftmost pixel (verified against the
         * rendered output on QEMU raspi3b) */
        for (uint32_t x = 0; x < FONT8X16_WIDTH; x++) {
            px[x] = (bits & (1u << x)) ? FB_FG : FB_BG;
        }
    }
    fb_cache_clean((uintptr_t)cell, (uintptr_t)FONT8X16_HEIGHT * fb_pitch);
}

static void fbcon_scroll(void)
{
    uintptr_t row_bytes = (uintptr_t)FONT8X16_HEIGHT * fb_pitch;
    volatile uint8_t *dst = (volatile uint8_t *)fb_base;
    volatile const uint8_t *src = dst + row_bytes;
    uintptr_t total = row_bytes * (FBCON_ROWS - 1u);

    for (uintptr_t off = 0; off < total; off++) {
        dst[off] = src[off];
    }
    for (uintptr_t off = total; off < total + row_bytes; off++) {
        dst[off] = 0;
    }
    fb_cache_clean((uintptr_t)fb_base, (uintptr_t)fb_pitch * FBCON_HEIGHT);
}

static void fbcon_newline(void)
{
    fb_col = 0;
    if (++fb_row >= FBCON_ROWS) {
        fbcon_scroll();
        fb_row = FBCON_ROWS - 1u;
    }
}

static void fbcon_emit(char c)
{
    switch (c) {
    case '\n':
        fbcon_newline();
        return;
    case '\r':
        fb_col = 0;
        return;
    case '\t':
        fb_col = (fb_col + 8u) & ~7u;
        if (fb_col >= FBCON_COLS) {
            fbcon_newline();
        }
        return;
    case '\b':
        if (fb_col > 0) {
            fb_col--;
        }
        return;
    default:
        break;
    }
    if ((uint8_t)c < 0x20 || (uint8_t)c == 0x7F) {
        return;                     /* other control chars: drop */
    }
    fbcon_draw_cell(fb_col, fb_row, (uint8_t)c);
    if (++fb_col >= FBCON_COLS) {
        fbcon_newline();
    }
}

void fbcon_putchar(char c)
{
    if (fb_state == 0) {
        /* Lazy one-shot init: the first mirrored character triggers the
         * mailbox allocation.  Keeps boot order independent of console.c. */
        mailbox_fb_init();
    }
    if (fb_state != 1) {
        return;
    }

    /* Strip ANSI escape sequences (ESC [ ... final, ESC ] ... BEL/ESC) */
    switch (ansi_state) {
    case ANSI_NORMAL:
        if (c == 0x1B) {
            ansi_state = ANSI_ESC;
            return;
        }
        break;
    case ANSI_ESC:
        if (c == '[') {
            ansi_state = ANSI_CSI;
        } else if (c == ']') {
            ansi_state = ANSI_OSC;
        } else {
            ansi_state = ANSI_NORMAL;
        }
        return;
    case ANSI_CSI:
        if (c >= 0x40 && c <= 0x7E) {
            ansi_state = ANSI_NORMAL;   /* final byte */
        }
        return;
    case ANSI_OSC:
        if (c == 0x07) {
            ansi_state = ANSI_NORMAL;   /* BEL terminates */
        } else if (c == 0x1B) {
            ansi_state = ANSI_ESC;      /* ST (ESC \) starts */
        }
        return;
    }

    fbcon_emit(c);
}

#else /* virt / non-aarch64 stubs */

int mailbox_fb_init(void) { return -1; }
int fbcon_active(void) { return 0; }
void fbcon_putchar(char c) { (void)c; }

#endif
