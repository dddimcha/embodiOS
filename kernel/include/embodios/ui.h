#ifndef EMBODIOS_UI_H
#define EMBODIOS_UI_H

/* EMBODIOS serial-UX module: ANSI colors, banner, boxes, progress bar.
 * Freestanding — built on top of console_printf/console_putchar. */

#include <embodios/types.h>

/* ANSI escape codes (use via ui_c() so they vanish when color is off) */
#define UI_RESET    "\x1b[0m"
#define UI_BOLD     "\x1b[1m"
#define UI_DIM      "\x1b[2m"
#define UI_RED      "\x1b[31m"
#define UI_GREEN    "\x1b[32m"
#define UI_YELLOW   "\x1b[33m"
#define UI_BLUE     "\x1b[34m"
#define UI_MAGENTA  "\x1b[35m"
#define UI_CYAN     "\x1b[36m"
#define UI_WHITE    "\x1b[37m"
#define UI_GRAY     "\x1b[90m"

/* Global color switch (ON by default; `color off` or test mode clears it) */
extern int ui_color_enabled;
void ui_set_color(int enabled);

/* Returns the escape code when color is enabled, "" otherwise */
const char* ui_c(const char* code);

/* Startup banner: block-letter logo + version + motto */
void ui_banner(void);

/* Box helpers (UTF-8 box drawing) */
void ui_hline(int width);                    /* ─ repeated */
void ui_box_top(const char* title);          /* ┌─ title ─...─┐ */
void ui_box_bottom(void);                    /* └─────────────┘ */

/* Progress bar with \r redraw:
 *   Label............ [████████░░░░░░]  62% (65/105)
 * Prints a newline automatically when cur >= total. */
void ui_progress(const char* label, uint32_t cur, uint32_t total);

/* Leveled messages: [OK] / [..] / [!] / [ERR] with colors */
void ui_ok(const char* fmt, ...);
void ui_info(const char* fmt, ...);
void ui_warn(const char* fmt, ...);
void ui_err(const char* fmt, ...);

/* Shell prompt: "embodios> " in bold cyan */
void ui_prompt(void);

/* Compact "model ready" block after a successful load */
void ui_model_ready(uint64_t load_ms);

/* Boot-time block: embedded model info or how-to-load hint */
void ui_boot_model_info(void);

#endif /* EMBODIOS_UI_H */
