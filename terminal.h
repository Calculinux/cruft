/* See LICENSE for licence details. */
int set_cell(struct terminal_t *term, int y, int x, const struct glyph_t *glyphp)
{
	struct cell_t cell, *cellp;
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

	cellp = &term->cells[y][x];
	cell_pixmap_clear(cellp);
	*cellp = cell;
	term->line_dirty[y] = true;

	if (cell.width == WIDE && x + 1 < term->cols) {
		cellp = &term->cells[y][x + 1];
		cell_pixmap_clear(cellp);
		*cellp = cell;
		cellp->width = NEXT_TO_WIDE;
		return WIDE;
	}

	if (cell.width == HALF /* isolated NEXT_TO_WIDE cell */
		&& x + 1 < term->cols
		&& term->cells[y][x + 1].width == NEXT_TO_WIDE) {
		erase_cell(term, y, x + 1);
	}
	return HALF;
}

static inline void swap_lines(struct terminal_t *term, int i, int j)
{
	struct cell_t *tmp;

	tmp            = term->cells[i];
	term->cells[i] = term->cells[j];
	term->cells[j] = tmp;
}

void scroll(struct terminal_t *term, int from, int to, int offset)
{
	int abs_offset, scroll_lines;

	if (offset == 0 || from >= to)
		return;

	logging(DEBUG, "scroll from:%d to:%d offset:%d\n", from, to, offset);

	for (int y = from; y <= to; y++)
		term->line_dirty[y] = true;

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
	int x, y, top, bottom;

	x = term->cursor.x + x_offset;
	y = term->cursor.y + y_offset;

	top    = term->scroll.top;
	bottom = term->scroll.bottom;

	if (x < 0) {
		x = 0;
	} else if (x >= term->cols) {
		if (term->mode & MODE_AMRIGHT)
			term->wrap_occurred = true;
		x = term->cols - 1;
	}
	term->cursor.x = x;

	y = (y < 0) ? 0:
		(y >= term->lines) ? term->lines - 1: y;

	if (term->cursor.y == top && y_offset < 0) {
		y = top;
		scroll(term, top, bottom, y_offset);
	} else if (term->cursor.y == bottom && y_offset > 0) {
		y = bottom;
		scroll(term, top, bottom, y_offset);
	}
	term->cursor.y = y;
}

/* absolute movement: never scroll */
void set_cursor(struct terminal_t *term, int y, int x)
{
	int top, bottom;

	if (term->mode & MODE_ORIGIN) {
		top    = term->scroll.top;
		bottom = term->scroll.bottom;
		y += term->scroll.top;
	} else {
		top = 0;
		bottom = term->lines - 1;
	}

	x = (x < 0) ? 0: (x >= term->cols) ? term->cols - 1: x;
	y = (y < top) ? top: (y > bottom) ? bottom: y;

	term->cursor.x = x;
	term->cursor.y = y;
	term->wrap_occurred = false;
}

void addch(struct terminal_t *term, uint32_t code)
{
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

	if ((term->wrap_occurred && term->cursor.x == term->cols - 1) /* folding */
		|| (glyphp->width == WIDE && term->cursor.x == term->cols - 1)) {
		set_cursor(term, term->cursor.y, 0);
		move_cursor(term, 1, 0);
	}
	term->wrap_occurred = false;

	move_cursor(term, 0, set_cell(term, term->cursor.y, term->cursor.x, glyphp));
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
	term->mode  = MODE_RESET;
	term->mode |= (MODE_CURSOR | MODE_AMRIGHT);
	term->wrap_occurred = false;

	term->scroll.top    = 0;
	term->scroll.bottom = term->lines - 1;

	term->cursor.x = term->cursor.y = 0;

	term->state.mode      = term->mode;
	term->state.cursor    = term->cursor;
	term->state.attribute = ATTR_RESET;

	term->color_pair.fg = DEFAULT_FG;
	term->color_pair.bg = DEFAULT_BG;

	term->attribute = ATTR_RESET;

	for (int line = 0; line < term->lines; line++) {
		for (int col = 0; col < term->cols; col++) {
			erase_cell(term, line, col);
			if ((col % TABSTOP) == 0)
				term->tabstop[col] = true;
			else
				term->tabstop[col] = false;
		}
		term->line_dirty[line] = true;
	}

	reset_esc(term);
	reset_charset(term);
}

void redraw(struct terminal_t *term)
{
	for (int i = 0; i < term->lines; i++)
		term->line_dirty[i] = true;
}

void term_die(struct terminal_t *term)
{
	int i, j;

	if (term->cells) {
		for (i = 0; i < term->lines; i++) {
			if (term->cells[i]) {
				for (j = 0; j < term->cols; j++)
					cell_pixmap_clear(&term->cells[i][j]);
				free(term->cells[i]);
			}
		}
		free(term->cells);
		term->cells = NULL;
	}

	drcs_clear_all(term);
	sixel_canvas_free(term);

	free(term->line_dirty);
	free(term->tabstop);
	free(term->esc.buf);
	term->line_dirty = NULL;
	term->tabstop = NULL;
	term->esc.buf = NULL;
}

bool term_init(struct terminal_t *term, int width, int height)
{
	extern const uint32_t color_list[COLORS]; /* global */

	term->width  = width;
	term->height = height;

	term->cols  = term->width / CELL_WIDTH;
	term->lines = term->height / CELL_HEIGHT;

	term->esc.size = ESCSEQ_SIZE;

	logging(DEBUG, "terminal cols:%d lines:%d\n", term->cols, term->lines);

	/* allocate memory */
	term->line_dirty   = (bool *) ecalloc(term->lines, sizeof(bool));
	term->tabstop      = (bool *) ecalloc(term->cols, sizeof(bool));
	term->esc.buf      = (char *) ecalloc(1, term->esc.size);
	term->sixel.pixmap = NULL; /* lazy: sixel_canvas_ensure on first sixel */

	memset(term->drcs, 0, sizeof(term->drcs));

	term->cells        = (struct cell_t **) ecalloc(term->lines, sizeof(struct cell_t *));
	if (!term->line_dirty || !term->tabstop || !term->cells || !term->esc.buf) {
		term_die(term);
		return false;
	}
	for (int i = 0; i < term->lines; i++) {
		term->cells[i] = (struct cell_t *) ecalloc(term->cols, sizeof(struct cell_t));
		if (!term->cells[i]) {
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
