/* EMBODIOS VideoCore mailbox framebuffer + text console
 * WS-B (pi-periph), v0.9.0 "Galvani"
 *
 * On boards with a mailbox (pi*) mailbox_fb_init() allocates a
 * 1024x768x32 framebuffer via the property channel and fbcon_putchar()
 * renders a mirrored text console (128x48 cells of 8x16).  Serial
 * (PL011) remains the authoritative console; the framebuffer is a
 * best-effort mirror.  On virt (no mailbox) everything is a stub.
 */
#ifndef EMBODIOS_MAILBOX_FB_H
#define EMBODIOS_MAILBOX_FB_H

#include <embodios/types.h>

#define FBCON_WIDTH     1024u
#define FBCON_HEIGHT    768u
#define FBCON_DEPTH     32u
#define FBCON_COLS      (FBCON_WIDTH / 8u)    /* 128 text columns */
#define FBCON_ROWS      (FBCON_HEIGHT / 16u)  /* 48 text rows */

/* Allocate the framebuffer and clear the screen.  Returns 0 on success,
 * -1 when unavailable (no mailbox / firmware rejected the request). */
int  mailbox_fb_init(void);

/* 1 when the framebuffer console is live. */
int  fbcon_active(void);

/* Mirror one character to the framebuffer console (CR/LF/tab handling,
 * scrolling, ANSI escape stripping).  No-op until mailbox_fb_init()
 * succeeded; safe to call at any time. */
void fbcon_putchar(char c);

#endif /* EMBODIOS_MAILBOX_FB_H */
