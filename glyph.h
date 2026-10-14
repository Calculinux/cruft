/* See LICENSE for licence details. */
/* glyph.h: mmap font loader metrics (see tools/mkcruftfont.c) */
#ifndef CRUFT_GLYPH_H
#define CRUFT_GLYPH_H

#include <stddef.h>
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

/* Status bar font metrics (4x8 line of the strip) */
extern int g_sb_cell_w;
extern int g_sb_cell_h;

struct glyph_t {
	uint32_t code;
	uint8_t width; /* wcwidth: 1 or 2 */
	uint8_t pad[3];
	uint16_t bitmap[GLYPH_MAX_H];
};

/* Loaded CRUFTFN1 blob (glyph_mmap.c); one instance per font. */
struct cruftfont_map {
	uint8_t *map;
	size_t size;
	const uint8_t *page_dir; /* LE uint32[PAGE_COUNT] at map+24 */
	uint32_t glyph_bytes;
	uint32_t page_shift;
	uint32_t cell_w;
	uint32_t cell_h;
	struct glyph_t tofu_half;
	struct glyph_t tofu_wide;
};

/* Terminal font (CRUFT_FONT, default /usr/share/cruft/console.cruftfont). */
int glyph_mmap_init(const char *path);
void glyph_mmap_die(void);
const struct glyph_t *glyph_lookup(uint32_t code);
const struct glyph_t *glyph_get(uint32_t code);

/* Core loader: validates magic/structure into *m; caller checks metrics.
 * quiet: no diagnostics (status font fails softly). 1 on success. */
int cruftfont_map_init(struct cruftfont_map *m, const char *path, int quiet);

/* Status bar font (4x8 only). 1 on success, 0 = bar disabled (quiet) or
 * unsuitable blob (diagnostics printed). glyph_sb_lookup() returns NULL for
 * missing glyphs (no tofu) so chrome renders nothing rather than a box. */
int glyph_sb_init(const char *path);
void glyph_sb_die(void);
const struct glyph_t *glyph_sb_lookup(uint32_t code);

#endif
