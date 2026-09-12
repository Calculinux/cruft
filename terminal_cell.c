/* Cell erase/copy with sparse sixel pixmap ownership (Calculinux/cruft). */
#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"

void erase_cell(struct terminal_t *term, int y, int x)
{
	struct cell_t *cellp;

	cellp = &term->cells[y][x];
	cell_pixmap_clear(cellp);
	cellp->glyphp     = glyph_get(DEFAULT_CHAR);
	cellp->color_pair = term->color_pair; /* bce */
	cellp->attribute  = ATTR_RESET;
	cellp->width      = HALF;

	term->line_dirty[y] = true;
}

void copy_cell(struct terminal_t *term, int dst_y, int dst_x, int src_y, int src_x)
{
	struct cell_t *dst;
	const struct cell_t *src;

	dst = &term->cells[dst_y][dst_x];
	src = &term->cells[src_y][src_x];

	if (src->width == NEXT_TO_WIDE) {
		return;
	} else if (src->width == WIDE && dst_x == (term->cols - 1)) {
		erase_cell(term, dst_y, dst_x);
	} else {
		cell_pixmap_clear(dst);
		*dst = *src;
		dst->pixmap = NULL;
		dst->has_pixmap = false;
		cell_pixmap_dup(dst, src);
		if (src->width == WIDE) {
			cell_pixmap_clear(dst + 1);
			*(dst + 1) = *src;
			(dst + 1)->width = NEXT_TO_WIDE;
			(dst + 1)->pixmap = NULL;
			(dst + 1)->has_pixmap = false;
			cell_pixmap_dup(dst + 1, src);
		}
		term->line_dirty[dst_y] = true;
	}
}
