/* EMBODIOS BCM2835/2711/2712 VideoCore mailbox property interface
 * WS-B (pi-periph), v0.9.0 "Galvani"
 * Author: Dmitry Dimcha <dddimcha@gmail.com>
 *
 * ARM<->VC mailbox: ARM writes the 28-bit (16-byte aligned) buffer address
 * ORed with channel 8 to mailbox1, VC processes the property tags in place,
 * ARM polls mailbox0 for the response.
 *
 * Cache maintenance: the property buffer lives in normal cacheable RAM and
 * the VC does not snoop the ARM L1, so the buffer is cleaned+invalidated
 * (dc civac) before the doorbell and invalidated (dc ivac) after the
 * response arrives.  Maintenance is done on 64-byte cache lines.
 *
 * Sources:
 *  - https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 *    (message format, tag IDs, channel 8, 16-byte alignment)
 *  - BCM2835 ARM Peripherals datasheet, ch. 1.3 (mailbox registers,
 *    status bits: FULL=bit31, EMPTY=bit30) — same offsets on bcm2836/7/2711.
 *  - Ultibo BCM2835 unit: mailbox0 @ PERIPH+0xB880, mailbox1 @ PERIPH+0xB8A0.
 *  - PM watchdog: BCM2835 ARM Peripherals ch. 12/PM (PM_RSTC @ +0x10001C,
 *    PM_WDOG @ +0x100024, password 0x5A000000).
 */

#include <embodios/bcm_mailbox.h>

/* TEMPORARY: shim in lieu of WS-A's kernel/arch/aarch64/board.h.
 * Integrator: replace with the real board.h include and delete
 * kernel/include/arch-aarch64-board-shim/. */
#include "board.h"

#if defined(__aarch64__) && defined(BOARD_HAS_MAILBOX) && BOARD_HAS_MAILBOX

/* Mailbox register block (bcm2835 layout, offsets from MAILBOX_BASE).
 * mailbox0: ARM reads responses; mailbox1: ARM writes requests. */
#define MBOX0_READ      (MAILBOX_BASE + 0x00)
#define MBOX0_STATUS    (MAILBOX_BASE + 0x18)
#define MBOX1_WRITE     (MAILBOX_BASE + 0x20)
#define MBOX1_STATUS    (MAILBOX_BASE + 0x38)

#define MBOX_STATUS_FULL    (1u << 31)
#define MBOX_STATUS_EMPTY   (1u << 30)

#define MBOX_REQUEST        0x00000000u
#define MBOX_RESP_SUCCESS   0x80000000u

/* PM watchdog (reset) registers */
#define PM_BASE         (PERIPH_BASE + 0x100000UL)
#define PM_RSTC         (PM_BASE + 0x1C)
#define PM_RSTS         (PM_BASE + 0x20)
#define PM_WDOG         (PM_BASE + 0x24)
#define PM_PASSWD       (0x5A000000u)
#define PM_RSTC_WRCFG_FULL_RESET  (0x00000020u)

#define MBOX_TIMEOUT    10000000u   /* poll iterations (~hundreds of ms) */

static volatile uint32_t mbox_read32(uintptr_t addr)
{
    uint32_t val;
    __asm__ volatile("ldr %w[v], [%[a]]" : [v] "=r" (val) : [a] "r" (addr) : "memory");
    return val;
}

static void mbox_write32(uintptr_t addr, uint32_t val)
{
    __asm__ volatile("str %w[v], [%[a]]" :: [v] "r" (val), [a] "r" (addr) : "memory");
}

/* Clean+invalidate D-cache by virtual address to point of coherency over
 * [addr, addr+len), rounded out to 64-byte lines. */
static void mbox_cache_civac(uintptr_t addr, uintptr_t len)
{
    uintptr_t start = addr & ~63UL;
    uintptr_t end = (addr + len + 63UL) & ~63UL;
    for (uintptr_t p = start; p < end; p += 64) {
        __asm__ volatile("dc civac, %0" :: "r" (p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Invalidate (discard) D-cache lines over [addr, addr+len).  Only safe when
 * the lines carry no dirty ARM-side data — true for the property buffer
 * right after the VC wrote the response. */
static void mbox_cache_ivac(uintptr_t addr, uintptr_t len)
{
    uintptr_t start = addr & ~63UL;
    uintptr_t end = (addr + len + 63UL) & ~63UL;
    __asm__ volatile("dsb sy" ::: "memory");
    for (uintptr_t p = start; p < end; p += 64) {
        __asm__ volatile("dc ivac, %0" :: "r" (p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/* Static property buffer, 16-byte aligned as required by the interface. */
static volatile uint32_t mbox_buf[MBOX_BUFFER_BYTES / 4] __attribute__((aligned(16)));

int bcm_mbox_available(void)
{
    return 1;
}

volatile uint32_t *bcm_mbox_buffer(uint32_t required_bytes)
{
    if (required_bytes > MBOX_BUFFER_BYTES) {
        return (volatile uint32_t *)0;
    }
    return mbox_buf;
}

int bcm_mbox_call(volatile uint32_t *buf, uint32_t buf_bytes)
{
    uintptr_t addr = (uintptr_t)buf;
    uint32_t resp;

    if ((addr & 0xF) != 0 || buf_bytes == 0) {
        return -1;
    }

    /* Make the request visible to the VC */
    mbox_cache_civac(addr, buf_bytes);

    /* Wait for space in mailbox1, then post buffer address | channel */
    uint32_t timeout = MBOX_TIMEOUT;
    while (mbox_read32(MBOX1_STATUS) & MBOX_STATUS_FULL) {
        if (--timeout == 0) {
            return -1;
        }
    }
    mbox_write32(MBOX1_WRITE, (uint32_t)(addr & 0xFFFFFFF0u) | MBOX_CHANNEL_PROP);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Wait for the VC response on mailbox0 */
    timeout = MBOX_TIMEOUT;
    for (;;) {
        while (mbox_read32(MBOX0_STATUS) & MBOX_STATUS_EMPTY) {
            if (--timeout == 0) {
                return -1;
            }
        }
        resp = mbox_read32(MBOX0_READ);
        if ((resp & 0xFu) == MBOX_CHANNEL_PROP &&
            (resp & 0xFFFFFFF0u) == (uint32_t)(addr & 0xFFFFFFF0u)) {
            break;
        }
        if (--timeout == 0) {
            return -1;
        }
    }

    /* Drop stale cache lines, then inspect the response code */
    mbox_cache_ivac(addr, buf_bytes);
    return (buf[1] == MBOX_RESP_SUCCESS) ? 0 : -1;
}

int bcm_mbox_execute(uint32_t used_bytes)
{
    if (used_bytes > MBOX_BUFFER_BYTES) {
        return -1;
    }
    /* Property message must cover header+tags+end; size field is in buf[0]
     * and is authoritative for the VC, we only bound the cache ops. */
    return bcm_mbox_call(mbox_buf, MBOX_BUFFER_BYTES);
}

/* Build a minimal one-tag request into the static buffer.
 * req_words: number of u32 request payload words (0..2 supported here). */
static volatile uint32_t *mbox_make_simple(uint32_t tag, const uint32_t *req,
                                           uint32_t req_words, uint32_t resp_words,
                                           uint32_t *out_used)
{
    volatile uint32_t *b = bcm_mbox_buffer(MBOX_BUFFER_BYTES);
    if (!b) {
        return (volatile uint32_t *)0;
    }
    uint32_t payload = resp_words > req_words ? resp_words : req_words;
    uint32_t i;

    b[0] = 8 + 12 + payload * 4 + 4;   /* total size */
    b[1] = MBOX_REQUEST;
    b[2] = tag;
    b[3] = payload * 4;                /* value buffer size */
    b[4] = req_words * 4;              /* request: b31 clear = request */
    for (i = 0; i < payload; i++) {
        b[5 + i] = (i < req_words) ? req[i] : 0;
    }
    b[5 + payload] = MBOX_TAG_END;
    *out_used = 8 + 12 + payload * 4 + 4;
    return b;
}

int bcm_mbox_get_fw_revision(uint32_t *out)
{
    uint32_t used;
    volatile uint32_t *b = mbox_make_simple(MBOX_TAG_GET_FW_REVISION, 0, 0, 1, &used);
    if (!b || bcm_mbox_execute(used) != 0 || (b[4] & 0x80000000u) == 0) {
        return -1;
    }
    *out = b[5];
    return 0;
}

int bcm_mbox_get_board_model(uint32_t *out)
{
    uint32_t used;
    volatile uint32_t *b = mbox_make_simple(MBOX_TAG_GET_BOARD_MODEL, 0, 0, 1, &used);
    if (!b || bcm_mbox_execute(used) != 0 || (b[4] & 0x80000000u) == 0) {
        return -1;
    }
    *out = b[5];
    return 0;
}

int bcm_mbox_get_clock_rate(uint32_t clock_id, uint32_t *out_hz)
{
    uint32_t used;
    uint32_t req[1] = { clock_id };
    volatile uint32_t *b = mbox_make_simple(MBOX_TAG_GET_CLOCK_RATE, req, 1, 2, &used);
    if (!b || bcm_mbox_execute(used) != 0 || (b[4] & 0x80000000u) == 0) {
        return -1;
    }
    if (b[5] != clock_id) {
        return -1;
    }
    *out_hz = b[6];
    return 0;
}

int bcm_mbox_set_clock_rate(uint32_t clock_id, uint32_t hz, uint32_t *out_actual_hz)
{
    uint32_t used;
    uint32_t req[3] = { clock_id, hz, 0 /* skip setting turbo */ };
    volatile uint32_t *b = mbox_make_simple(MBOX_TAG_SET_CLOCK_RATE, req, 3, 3, &used);
    if (!b || bcm_mbox_execute(used) != 0 || (b[4] & 0x80000000u) == 0) {
        return -1;
    }
    if (out_actual_hz) {
        *out_actual_hz = b[6];
    }
    return 0;
}

int bcm_mbox_get_uart_clock(uint32_t *out_hz)
{
    return bcm_mbox_get_clock_rate(MBOX_CLK_UART, out_hz);
}

int bcm_mbox_set_uart_clock(uint32_t hz, uint32_t *out_actual_hz)
{
    return bcm_mbox_set_clock_rate(MBOX_CLK_UART, hz, out_actual_hz);
}

int bcm_mbox_reset(void)
{
    /* No property tag exists for reset; use the PM watchdog exactly like
     * the firmware does: full reset config, ~10us watchdog timeout. */
    uint32_t rsts = mbox_read32(PM_RSTS);
    rsts = PM_PASSWD | (rsts & 0xFFFFFCFu);   /* clear partition, keep bits */
    mbox_write32(PM_RSTS, rsts);
    mbox_write32(PM_WDOG, PM_PASSWD | 10);
    mbox_write32(PM_RSTC, PM_PASSWD | PM_RSTC_WRCFG_FULL_RESET);
    /* Should not return; if the watchdog is not emulated (QEMU), spin. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}

#else /* !aarch64 or BOARD_HAS_MAILBOX == 0 — virt stubs */

int bcm_mbox_available(void) { return 0; }
volatile uint32_t *bcm_mbox_buffer(uint32_t required_bytes)
{
    (void)required_bytes;
    return (volatile uint32_t *)0;
}
int bcm_mbox_call(volatile uint32_t *buf, uint32_t buf_bytes)
{
    (void)buf; (void)buf_bytes;
    return -1;
}
int bcm_mbox_execute(uint32_t used_bytes)
{
    (void)used_bytes;
    return -1;
}
int bcm_mbox_get_fw_revision(uint32_t *out) { (void)out; return -1; }
int bcm_mbox_get_board_model(uint32_t *out) { (void)out; return -1; }
int bcm_mbox_get_clock_rate(uint32_t clock_id, uint32_t *out_hz)
{
    (void)clock_id; (void)out_hz;
    return -1;
}
int bcm_mbox_set_clock_rate(uint32_t clock_id, uint32_t hz, uint32_t *out_actual_hz)
{
    (void)clock_id; (void)hz; (void)out_actual_hz;
    return -1;
}
int bcm_mbox_get_uart_clock(uint32_t *out_hz) { (void)out_hz; return -1; }
int bcm_mbox_set_uart_clock(uint32_t hz, uint32_t *out_actual_hz)
{
    (void)hz; (void)out_actual_hz;
    return -1;
}
int bcm_mbox_reset(void) { return -1; }

#endif
