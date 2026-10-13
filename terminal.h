/* See LICENSE for licence details. */
#include "status.h"

int set_cell(struct terminal_t *term, int y, int x, const struct glyph_t *glyphp)
{
	struct cell_t cell, *cellp;
	struct screen_t *scr = active_screen(term);
	uint8_t color_tmp;

	cell.glyphp = glyphp;

	cell.color_pair.fg = (term->attribute & attr_mask[ATTR_BOLD] && term->color_pair.fg <= 7) ?
		term->color_pair.fg + BRIGHT_INC: term->color_pair.fg;
	cell.color_pair.bg = (term->attribute & attr_mask[ATTR_BLINK] && term->color_pair.bg <= 7) ?
		term->color_pair.bg + BRIGHT_INC: term->color_pair.bg;

	if (term->attribute & attr_mask[ATTR_REVERSE]) {
		color_tmp          = cell.color_pair.fg;
		cell.color_pair.fg = cell.color_pair.bg;
		cell.color_pair.bg = color_tmp;
	}

	cell.attribute  = term->attribute;
	cell.width      = glyphp->width;
	cell.has_pixmap = false;
	cell.pixmap     = NULL;

	cellp = &scr->cells[y][x];
	cell_pixmap_clear(cellp);
	*cellp = cell;
	scr->line_dirty[y] = true;

	if (cell.width == WIDE && x + 1 < term->cols) {
		cellp = &scr->cells[y][x + 1];
		cell_pixmap_clear(cellp);
		*cellp = cell;
		cellp->width = NEXT_TO_WIDE;
		return WIDE;
	}

	if (cell.width == HALF /* isolated NEXT_TO_WIDE cell */
		&& x + 1 < term->cols
		&& scr->cells[y][x + 1].width == NEXT_TO_WIDE) {
		erase_cell(term, y, x + 1);
	}
	return HALF;
}

static inline void swap_lines(struct terminal_t *term, int i, int j)
{
	struct cell_t **cells = active_screen(term)->cells;
	struct cell_t *tmp;

	tmp        = cells[i];
	cells[i]   = cells[j];
	cells[j]   = tmp;
}

void scroll(struct terminal_t *term, int from, int to, int offset)
{
	struct screen_t *scr = active_screen(term);
	int abs_offset, scroll_lines;

	if (offset == 0 || from >= to)
		return;

	logging(DEBUG, "scroll from:%d to:%d offset:%d\n", from, to, offset);

	for (int y = from; y <= to; y++)
		scr->line_dirty[y] = true;

	abs_offset = abs(offset);
	scroll_lines = (to - from + 1) - abs_offset;

	if (offset > 0) { /* scroll down */
		for (int y = from; y < from + scroll_lines; y++)
			swap_lines(term, y, y + offset);
		for (int y = (to - offset + 1); y <= to; y++)
			for (int x = 0; x < term->cols; x++)
				erase_cell(term, y, x);
	}
	else {            /* scroll up */
		for (int y = to; y >= from + abs_offset; y--)
			swap_lines(term, y, y - abs_offset);
		for (int y = from; y < from + abs_offset; y++)
			for (int x = 0; x < term->cols; x++)
				erase_cell(term, y, x);
	}
}

/* relative movement: cause scrolling */
void move_cursor(struct terminal_t *term, int y_offset, int x_offset)
{
	struct screen_t *scr = active_screen(term);
	int x, y, top, bottom;

	x = scr->cursor.x + x_offset;
	y = scr->cursor.y + y_offset;

	top    = scr->scroll.top;
	bottom = scr->scroll.bottom;

	if (x < 0) {
		x = 0;
	} else if (x >= term->cols) {
		if (term->mode & MODE_AMRIGHT)
			scr->wrap_occurred = true;
		x = term->cols - 1;
	}
	scr->cursor.x = x;

	y = (y < 0) ? 0:
		(y >= term->lines) ? term->lines - 1: y;

	if (scr->cursor.y == top && y_offset < 0) {
		y = top;
		scroll(term, top, bottom, y_offset);
	} else if (scr->cursor.y == bottom && y_offset > 0) {
		y = bottom;
		scroll(term, top, bottom, y_offset);
	}
	scr->cursor.y = y;
}

/* absolute movement: never scroll */
void set_cursor(struct terminal_t *term, int y, int x)
{
	struct screen_t *scr = active_screen(term);
	int top, bottom;

	if (term->mode & MODE_ORIGIN) {
		top    = scr->scroll.top;
		bottom = scr->scroll.bottom;
		y += scr->scroll.top;
	} else {
		top = 0;
		bottom = term->lines - 1;
	}

	x = (x < 0) ? 0: (x >= term->cols) ? term->cols - 1: x;
	y = (y < top) ? top: (y > bottom) ? bottom: y;

	scr->cursor.x = x;
	scr->cursor.y = y;
	scr->wrap_occurred = false;
}

void addch(struct terminal_t *term, uint32_t code)
{
	struct screen_t *scr = active_screen(term);
	int width;
	const struct glyph_t *glyphp;

	logging(DEBUG, "addch: U+%.4X\n", code);

	width = wcwidth(code);

	if (width <= 0)                                /* zero width: not support combining characters */
		return;
	else if (0x100000 <= code && code <= 0x10FFFD) /* unicode private area: plane 16 (DRCSMMv1) */
		glyphp = drcs_glyph(term, code);
	else if (code >= UCS2_CHARS)                   /* cruft support only UCS2 */
		glyphp = (width == 1) ? glyph_get(SUBSTITUTE_HALF): glyph_get(SUBSTITUTE_WIDE);
	else {
		glyphp = glyph_lookup(code);
		if (glyphp == NULL || glyphp->width != width)
			glyphp = (width == 1) ? glyph_get(SUBSTITUTE_HALF): glyph_get(SUBSTITUTE_WIDE);
	}

	if ((scr->wrap_occurred && scr->cursor.x == term->cols - 1) /* folding */
		|| (glyphp->width == WIDE && scr->cursor.x == term->cols - 1)) {
		set_cursor(term, scr->cursor.y, 0);
		move_cursor(term, 1, 0);
	}
	scr->wrap_occurred = false;

	move_cursor(term, 0, set_cell(term, scr->cursor.y, scr->cursor.x, glyphp));
}

void reset_esc(struct terminal_t *term)
{
	logging(DEBUG, "*esc reset*\n");

	term->esc.bp    = term->esc.buf;
	term->esc.state = STATE_RESET;
}

bool push_esc(struct terminal_t *term, uint8_t ch)
{
	long offset;

	if ((term->esc.bp - term->esc.buf) >= term->esc.size) { /* buffer limit */
		logging(DEBUG, "escape sequence length >= %d, term.esc.buf reallocated\n", term->esc.size);
		offset = term->esc.bp - term->esc.buf;
		term->esc.buf = erealloc(term->esc.buf, term->esc.size * 2);
		term->esc.bp  = term->esc.buf + offset;
		term->esc.size *= 2;
	}

	/* ref: http://www.vt100.net/docs/vt102-ug/appendixd.html */
	*term->esc.bp++ = ch;
	if (term->esc.state == STATE_ESC) {
		/* format:
			ESC  I.......I F
				 ' '  '/'  '0'  '~'
			0x1B 0x20-0x2F 0x30-0x7E
		*/
		if ('0' <= ch && ch <= '~')        /* final char */
			return true;
		else if (SPACE <= ch && ch <= '/') /* intermediate char */
			return false;
	} else if (term->esc.state == STATE_CSI) {
		/* format:
			CSI       P.......P I.......I F
			ESC  '['  '0'  '?'  ' '  '/'  '@'  '~'
			0x1B 0x5B 0x30-0x3F 0x20-0x2F 0x40-0x7E
		*/
		if ('@' <= ch && ch <= '~')
			return true;
		else if (SPACE <= ch && ch <= '?')
			return false;
	} else {
		/* format:
			OSC       I.....I F
			ESC  ']'          BEL  or ESC  '\'
			0x1B 0x5D unknown 0x07 or 0x1B 0x5C
			DCS       I....I  F
			ESC  'P'          BEL  or ESC  '\'
			0x1B 0x50 unknown 0x07 or 0x1B 0x5C
		*/
		if (ch == BEL || (ch == BACKSLASH
			&& (term->esc.bp - term->esc.buf) >= 2 && *(term->esc.bp - 2) == ESC))
			return true;
		else if ((ch == ESC || ch == CR || ch == LF || ch == BS || ch == HT)
			|| (SPACE <= ch && ch <= '~'))
			return false;
	}

	/* invalid sequence */
	reset_esc(term);
	return false;
}

void reset_charset(struct terminal_t *term)
{
	term->charset.code = term->charset.count = term->charset.following_byte = 0;
	term->charset.is_valid = true;
}

void reset(struct terminal_t *term)
{
	struct screen_t *scr = active_screen(term);

	term->mode  = MODE_RESET;
	term->mode |= (MODE_CURSOR | MODE_AMRIGHT);
	scr->wrap_occurred = false;

	scr->scroll.top    = 0;
	scr->scroll.bottom = term->lines - 1;

	scr->cursor.x = scr->cursor.y = 0;

	term->state.mode      = term->mode;
	term->state.cursor    = scr->cursor;
	term->state.attribute = ATTR_RESET;

	term->color_pair.fg = DEFAULT_FG;
	term->color_pair.bg = DEFAULT_BG;

	term->attribute = ATTR_RESET;

	for (int line = 0; line < term->lines; line++) {
		for (int col = 0; col < term->cols; col++) {
			erase_cell(term, line, col);
			if ((col % TABSTOP) == 0)
				scr->tabstop[col] = true;
			else
				scr->tabstop[col] = false;
		}
		scr->line_dirty[line] = true;
	}

	reset_esc(term);
	reset_charset(term);
}

void redraw(struct terminal_t *term)
{
	struct screen_t *scr = active_screen(term);

	for (int i = 0; i < term->lines; i++)
		scr->line_dirty[i] = true;
	statusbar_invalidate(); /* full redraws repaint the status strip too */
}

void term_die(struct terminal_t *term)
{
	int i, j;

	/* Free every screen that is backed by a cell matrix (the alternate
	 * stays NULL until DECALT allocates it lazily). */
	for (i = 0; i < 2; i++) {
		struct screen_t *scr = &term->scr[i];

		if (scr->cells) {
			for (j = 0; j < term->lines; j++) {
				if (scr->cells[j]) {
					for (int k = 0; k < term->cols; k++)
						cell_pixmap_clear(&scr->cells[j][k]);
					free(scr->cells[j]);
				}
			}
			free(scr->cells);
			scr->cells = NULL;
		}
		free(scr->line_dirty);
		scr->line_dirty = NULL;
		free(scr->tabstop);
		scr->tabstop = NULL;
	}

	drcs_clear_all(term);
	sixel_canvas_free(term);

	free(term->esc.buf);
	term->esc.buf = NULL;
}

bool term_init(struct terminal_t *term, int width, int height)
{
	extern const uint32_t color_list[COLORS]; /* global */

	term->width  = width;
	term->height = height;

	/* The optional status bar owns the top strip (statusbar_h() px);
	 * the cell grid fills what remains below it. */
	term->oy    = statusbar_h();
	term->cols  = term->width / CELL_WIDTH;
	term->lines = (term->height - term->oy) / CELL_HEIGHT;

	term->esc.size = ESCSEQ_SIZE;

	logging(DEBUG, "terminal cols:%d lines:%d\n", term->cols, term->lines);

	/* Screens: the primary (scr[0]) is the active one; the alternate
	 * (scr[1]) stays reserved zeroed until DECALT allocates it lazily. */
	term->active = 0;
	term->scr[0] = (struct screen_t) {0};
	term->scr[1] = (struct screen_t) {0};

	/* allocate memory (primary screen only) */
	term->scr[0].allocated  = true;
	term->scr[0].line_dirty = (bool *) ecalloc(term->lines, sizeof(bool));
	term->scr[0].tabstop    = (bool *) ecalloc(term->cols, sizeof(bool));
	term->esc.buf           = (char *) ecalloc(1, term->esc.size);
	term->sixel.pixmap      = NULL; /* lazy: sixel_canvas_ensure on first sixel */

	memset(term->drcs, 0, sizeof(term->drcs));

	term->scr[0].cells = (struct cell_t **) ecalloc(term->lines, sizeof(struct cell_t *));
	if (!term->scr[0].line_dirty || !term->scr[0].tabstop
		|| !term->scr[0].cells || !term->esc.buf) {
		term_die(term);
		return false;
	}
	for (int i = 0; i < term->lines; i++) {
		term->scr[0].cells[i] = (struct cell_t *) ecalloc(term->cols, sizeof(struct cell_t));
		if (!term->scr[0].cells[i]) {
			term_die(term);
			return false;
		}
	}

	/* initialize palette */
	for (int i = 0; i < COLORS; i++)
		term->virtual_palette[i] = color_list[i];
	term->palette_modified = false;

	/* glyphs loaded via glyph_mmap_init() before term_init() */

	/* reset terminal */
	reset(term);

	return true;
}
