/* Lazy DRCS charset tables (Calculinux/cruft). */
#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"

#include <stdlib.h>
#include <stdio.h>

/* -1 if idx is outside the 96-glyph DRCS charset; else idx. */
int drcs_clamp_char_index(int idx)
{
	if (idx < 0 || idx >= GLYPHS_PER_CHARSET)
		return -1;
	return idx;
}

struct glyph_t *drcs_ensure(struct terminal_t *term, int charset)
{
	if (!term || charset < 0 || charset >= DRCS_CHARSETS)
		return NULL;

	if (!term->drcs[charset]) {
		term->drcs[charset] = (struct glyph_t *)calloc(
			GLYPHS_PER_CHARSET, sizeof(struct glyph_t));
		if (!term->drcs[charset])
			fprintf(stderr, "cruft: drcs_ensure: alloc charset %d failed\n", charset);
	}
	return term->drcs[charset];
}

void drcs_clear_charset(struct terminal_t *term, int charset)
{
	if (!term || charset < 0 || charset >= DRCS_CHARSETS)
		return;
	free(term->drcs[charset]);
	term->drcs[charset] = NULL;
}

void drcs_clear_all(struct terminal_t *term)
{
	int i;

	if (!term)
		return;
	for (i = 0; i < DRCS_CHARSETS; i++)
		drcs_clear_charset(term, i);
}

const struct glyph_t *drcs_glyph(struct terminal_t *term, uint32_t code)
{
	/* DRCSMMv1
		ESC ( SP <\xXX> <\xYY> ESC ( B
		<===> U+10XXYY ( 0x40 <= 0xXX <=0x7E, 0x20 <= 0xYY <= 0x7F )
	*/
	int row, cell, charset;

	row  = (0xFF00 & code) >> 8;
	cell = 0xFF & code;

	if ((0x40 <= row && row <= 0x7E) && (0x20 <= cell && cell <= 0x7F)) {
		charset = row - 0x40;
		if (!term->drcs[charset])
			return glyph_get(SUBSTITUTE_HALF);
		return &term->drcs[charset][cell - 0x20];
	}
	return glyph_get(SUBSTITUTE_HALF);
}
