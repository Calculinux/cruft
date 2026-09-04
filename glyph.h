/* See LICENSE for licence details. */
/* glyph.h: mmap font loader metrics (see tools/mkcruftfont.c) */
#ifndef CRUFT_GLYPH_H
#define CRUFT_GLYPH_H

#include <stdint.h>

enum {
	GLYPH_MAX_W = 8,
	GLYPH_MAX_H = 16,
	/* Sixel cell pixmap buffer: sized for largest supported cell */
	CELL_PIXMAP_W = GLYPH_MAX_W,
	CELL_PIXMAP_H = GLYPH_MAX_H,
};

extern int g_cell_w;
extern int g_cell_h;

#define CELL_WIDTH  (g_cell_w)
#define CELL_HEIGHT (g_cell_h)

struct glyph_t {
	uint32_t code;
	uint8_t width; /* wcwidth: 1 or 2 */
	uint8_t pad[3];
	uint16_t bitmap[GLYPH_MAX_H];
};

int glyph_mmap_init(const char *path);
void glyph_mmap_die(void);
const struct glyph_t *glyph_lookup(uint32_t code);
const struct glyph_t *glyph_get(uint32_t code);

#endif
