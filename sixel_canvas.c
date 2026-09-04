/* Lazy sixel canvas + cell blit (Calculinux/cruft). */
#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* erase_cell / move_cursor / cr live in terminal.h / esc.h (unity from cruft.c). */
void erase_cell(struct terminal_t *term, int y, int x);
void move_cursor(struct terminal_t *term, int y_offset, int x_offset);
void cr(struct terminal_t *term);

static inline int my_ceil_local(int val, int div)
{
	if (div == 0)
		return 0;
	return (val + div - 1) / div;
}

int sixel_canvas_ensure(struct terminal_t *term)
{
	size_t n;

	if (!term)
		return -1;
	if (term->sixel.pixmap)
		return 0;

	n = (size_t)term->width * (size_t)term->height * (size_t)BYTES_PER_PIXEL;
	term->sixel.pixmap = (uint8_t *)calloc(1, n);
	if (!term->sixel.pixmap) {
		fprintf(stderr, "cruft: sixel_canvas_ensure: alloc failed\n");
		return -1;
	}
	return 0;
}

void sixel_canvas_free(struct terminal_t *term)
{
	if (!term)
		return;
	free(term->sixel.pixmap);
	term->sixel.pixmap = NULL;
}

void term_release_transient(struct terminal_t *term)
{
	if (!term)
		return;
	sixel_canvas_free(term);
}

void reset_sixel(struct terminal_t *term, struct color_pair_t color_pair, int width, int height)
{
	struct sixel_canvas_t *sc;
	int i;

	if (!term || sixel_canvas_ensure(term) < 0)
		return;

	sc = &term->sixel;
	if (!sc->pixmap)
		return;

	memset(sc->pixmap, 0, (size_t)BYTES_PER_PIXEL * (size_t)width * (size_t)height);

	sc->width   = 1;
	sc->height  = 6;
	sc->point.x = 0;
	sc->point.y = 0;
	sc->line_length = BYTES_PER_PIXEL * width;
	sc->color_index = 0;

	/* 0 - 15: VT340 default color map */
	sc->color_table[0] = 0x000000; sc->color_table[8]  = 0x424242;
	sc->color_table[1] = 0x3333CC; sc->color_table[9]  = 0x545499;
	sc->color_table[2] = 0xCC2121; sc->color_table[10] = 0x994242;
	sc->color_table[3] = 0x33CC33; sc->color_table[11] = 0x549954;
	sc->color_table[4] = 0xCC33CC; sc->color_table[12] = 0x995499;
	sc->color_table[5] = 0x33CCCC; sc->color_table[13] = 0x549999;
	sc->color_table[6] = 0xCCCC33; sc->color_table[14] = 0x999954;
	sc->color_table[7] = 0x878787; sc->color_table[15] = 0xCCCCCC;

	sc->color_table[0] = color_list[color_pair.fg];

	for (i = 16; i < COLORS; i++)
		sc->color_table[i] = color_list[i];
}

void sixel_copy2cell(struct terminal_t *term, struct sixel_canvas_t *sc)
{
	int y, x, h, cols, lines;
	int src_offset, dst_offset;
	struct cell_t *cellp;
	uint8_t *dst;

	if (!term || !sc || !sc->pixmap)
		return;

	if (sc->height > term->height)
		sc->height = term->height;

	cols  = my_ceil_local(sc->width, CELL_WIDTH);
	lines = my_ceil_local(sc->height, CELL_HEIGHT);

	if (cols + term->cursor.x > term->cols)
		cols -= (cols + term->cursor.x - term->cols);

	for (y = 0; y < lines; y++) {
		for (x = 0; x < cols; x++) {
			erase_cell(term, term->cursor.y, term->cursor.x + x);
			cellp = &term->cells[term->cursor.y][term->cursor.x + x];
			dst = cell_pixmap_ensure(cellp);
			if (!dst)
				continue;
			for (h = 0; h < CELL_HEIGHT; h++) {
				src_offset = (y * CELL_HEIGHT + h) * sc->line_length
					+ (CELL_WIDTH * x) * BYTES_PER_PIXEL;
				dst_offset = h * CELL_WIDTH * BYTES_PER_PIXEL;
				if (src_offset >= BYTES_PER_PIXEL * term->width * term->height)
					break;
				memcpy(dst + dst_offset, sc->pixmap + src_offset,
					(size_t)CELL_WIDTH * BYTES_PER_PIXEL);
			}
		}
		move_cursor(term, 1, 0);
	}
	cr(term);
}
