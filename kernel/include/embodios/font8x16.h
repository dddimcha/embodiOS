/* EMBODIOS 8x16 console font
 * WS-B (pi-periph), v0.9.0 "Galvani"
 */
#ifndef EMBODIOS_FONT8X16_H
#define EMBODIOS_FONT8X16_H

#include <embodios/types.h>

#define FONT8X16_WIDTH        8u
#define FONT8X16_HEIGHT       16u
#define FONT8X16_NUM_GLYPHS   128u

/* font8x16_glyphs[c][row] — bit 0 = leftmost pixel */
extern const uint8_t font8x16_glyphs[FONT8X16_NUM_GLYPHS][FONT8X16_HEIGHT];

#endif /* EMBODIOS_FONT8X16_H */
