/* Sparse per-cell sixel pixmap helpers (Calculinux/cruft). */
#define CRUFT_NO_GLOBALS
#include "cruft.h"

#include <stdlib.h>
#include <string.h>

size_t cell_pixmap_bytes(void)
{
	return (size_t)BYTES_PER_PIXEL * (size_t)CELL_WIDTH * (size_t)CELL_HEIGHT;
}

void cell_pixmap_clear(struct cell_t *c)
{
	if (!c)
		return;
	free(c->pixmap);
	c->pixmap = NULL;
	c->has_pixmap = false;
}

uint8_t *cell_pixmap_ensure(struct cell_t *c)
{
	size_t n;

	if (!c)
		return NULL;
	if (c->pixmap) {
		c->has_pixmap = true;
		return c->pixmap;
	}
	n = cell_pixmap_bytes();
	c->pixmap = (uint8_t *)calloc(1, n);
	if (!c->pixmap) {
		c->has_pixmap = false;
		return NULL;
	}
	c->has_pixmap = true;
	return c->pixmap;
}

int cell_pixmap_dup(struct cell_t *dst, const struct cell_t *src)
{
	size_t n;
	uint8_t *p;

	if (!dst || !src)
		return -1;

	cell_pixmap_clear(dst);

	if (!src->has_pixmap || !src->pixmap)
		return 0;

	n = cell_pixmap_bytes();
	p = (uint8_t *)calloc(1, n);
	if (!p)
		return -1;
	memcpy(p, src->pixmap, n);
	dst->pixmap = p;
	dst->has_pixmap = true;
	return 0;
}
