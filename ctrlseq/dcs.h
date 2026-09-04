/* See LICENSE for licence details. */
/* function for dcs sequence */
enum {
	RGBMAX = 255,
	HUEMAX = 360,
	LSMAX  = 100,
};

/*
static inline void split_rgb(uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
	*r = bit_mask[8] & (color >> 16);
	*g = bit_mask[8] & (color >>  8);
	*b = bit_mask[8] & (color >>  0);
}
*/

static inline int sixel_bitmap(struct terminal_t *term, struct sixel_canvas_t *sc, uint8_t bitmap)
{
	int i, offset;
	//uint8_t r, g, b;

	logging(DEBUG, "sixel_bitmap()\nbitmap:%.2X point(%d, %d)\n",
		bitmap, sc->point.x, sc->point.y);

	if (!sc->pixmap)
		return 1;

	if (sc->point.x >= term->width || sc->point.y >= term->height)
		return 1;

	offset = sc->point.x * BYTES_PER_PIXEL + sc->point.y * sc->line_length;

	for (i = 0; i < BITS_PER_SIXEL; i++) {
		if (offset >= BYTES_PER_PIXEL * term->width * term->height)
			break;

		if (bitmap & (0x01 << i)) {
			memcpy(sc->pixmap + offset, &sc->color_table[sc->color_index], BYTES_PER_PIXEL);
			/*
			split_rgb(sc->color_table[sc->color_index], &r, &g, &b);
			*(sc->pixmap + offset + 0) = b;
			*(sc->pixmap + offset + 1) = g;
			*(sc->pixmap + offset + 2) = r;
			*/
		}

		offset += sc->line_length;
	}
	sc->point.x++;

	if (sc->point.x > sc->width)
		sc->width = sc->point.x;

	return 1;
}

static inline int sixel_repeat(struct terminal_t *term, struct sixel_canvas_t *sc, char *buf)
{
	int i, count;
	size_t length;
	char *cp, tmp[BUFSIZE];
	uint8_t bitmap;

	cp = buf + 1; /* skip '!' itself */
	while (isdigit(*cp)) /* skip non sixel bitmap character */
		cp++;

	length = (cp - buf);
	strncpy(tmp, buf + 1, length - 1);
	*(tmp + length - 1) = '\0';

	count = dec2num(tmp);

	logging(DEBUG, "sixel_repeat()\nbuf:%s length:%u\ncount:%d repeat:0x%.2X\n",
		tmp, (unsigned) length, count, *cp);

	if ('?' <= *cp && *cp <= '~') {
		bitmap = bit_mask[BITS_PER_SIXEL] & (*cp - '?');
		for (i = 0; i < count; i++)
			sixel_bitmap(term, sc, bitmap);
	}

	return length + 1;
}

static inline int sixel_attr(struct sixel_canvas_t *sc, char *buf)
{
	char *cp, tmp[BUFSIZE];
	size_t length;
	struct parm_t parm;

	cp = buf + 1;
	while (isdigit(*cp) || *cp == ';') /* skip valid params */
		cp++;

	length = (cp - buf);
	strncpy(tmp, buf + 1, length - 1);
	*(tmp + length - 1) = '\0';

	reset_parm(&parm);
	parse_arg(tmp, &parm, ';', isdigit);

	if (parm.argc >= 4) {
		sc->width  = dec2num(parm.argv[2]);
		sc->height = dec2num(parm.argv[3]);
	}

	logging(DEBUG, "sixel_attr()\nbuf:%s\nwidth:%d height:%d\n",
		tmp, sc->width, sc->height);

	return length;
}

static inline uint32_t hue2rgb(int n1, int n2, int hue)
{
	if (hue < 0)
		hue += HUEMAX;

	if (hue > HUEMAX)
		hue -= HUEMAX;

	if (hue < (HUEMAX / 6))
		return (n1 + (((n2 - n1) * hue + (HUEMAX / 12)) / (HUEMAX / 6)));
	if (hue < (HUEMAX / 2))
		return n2;
	if (hue < ((HUEMAX * 2) / 3))
		return (n1 + (((n2 - n1) * (((HUEMAX * 2) / 3) - hue) + (HUEMAX / 12)) / (HUEMAX / 6)));
	else
		return n1;
}

static inline uint32_t hls2rgb(int hue, int lum, int sat)
{
	uint32_t r, g, b;
	int magic1, magic2;

	if (sat == 0) {
		r = g = b = (lum * RGBMAX) / LSMAX;
	} else {
		if (lum <= (LSMAX / 2) )
			magic2 = (lum * (LSMAX + sat) + (LSMAX / 2)) / LSMAX;
		else
			magic2 = lum + sat - ((lum * sat) + (LSMAX / 2)) / LSMAX;
		magic1 = 2 * lum - magic2;

		r = (hue2rgb(magic1, magic2, hue + (HUEMAX / 3)) * RGBMAX + (LSMAX / 2)) / LSMAX;
		g = (hue2rgb(magic1, magic2, hue) * RGBMAX + (LSMAX / 2)) / LSMAX;
		b = (hue2rgb(magic1, magic2, hue - (HUEMAX / 3)) * RGBMAX + (LSMAX/2)) / LSMAX;
	}
	return (r << 16) + (g << 8) + b;
}

static inline int sixel_color(struct sixel_canvas_t *sc, char *buf)
{
	char *cp, tmp[BUFSIZE];
	int index, type;
	size_t length;
	uint16_t v1, v2, v3, r, g, b;
	uint32_t color;
	struct parm_t parm;

	cp = buf + 1;
	while (isdigit(*cp) || *cp == ';') /* skip valid params */
		cp++;

	length = (cp - buf);
	strncpy(tmp, buf + 1, length - 1); /* skip '#' */
	*(tmp + length - 1) = '\0';

	reset_parm(&parm);
	parse_arg(tmp, &parm, ';', isdigit);

	if (parm.argc < 1)
		return length;

	index = dec2num(parm.argv[0]);
	if (index < 0)
		index = 0;
	else if (index >= COLORS)
		index = COLORS - 1;

	logging(DEBUG, "sixel_color()\nbuf:%s length:%u\nindex:%d\n",
		tmp, (unsigned) length, index);

	if (parm.argc == 1) { /* select color */
		sc->color_index = index;
		return length;
	}

	if (parm.argc != 5)
		return length;

	type  = dec2num(parm.argv[1]);
	v1    = dec2num(parm.argv[2]);
	v2    = dec2num(parm.argv[3]);
	v3    = dec2num(parm.argv[4]);

	if (type == 1) { /* HLS */
		color = hls2rgb(v1, v2, v3);
	} else {
		r = bit_mask[8] & (0xFF * v1 / 100);
		g = bit_mask[8] & (0xFF * v2 / 100);
		b = bit_mask[8] & (0xFF * v3 / 100);
		color = (r << 16) | (g << 8) | b;
	}

	logging(DEBUG, "type:%d v1:%u v2:%u v3:%u color:0x%.8X\n",
		type, v1, v2, v3, color);

	sc->color_table[index] = color;

	return length;
}

static inline int sixel_cr(struct sixel_canvas_t *sc)
{
	logging(DEBUG, "sixel_cr()\n");

	sc->point.x = 0;

	return 1;
}

static inline int sixel_nl(struct sixel_canvas_t *sc)
{
	logging(DEBUG, "sixel_nl()\n");

	/* DECGNL moves active position to left margin
		and down one line of sixels:
		http://odl.sysworks.biz/disk$vaxdocdec963/decw$book/d3qsaaa1.p67.decw$book */
	sc->point.y += BITS_PER_SIXEL;
	sc->point.x = 0;

	if (sc->point.y > sc->height)
		sc->height = sc->point.y;

	return 1;
}

void sixel_parse_data(struct terminal_t *term, struct sixel_canvas_t *sc, char *start_buf)
{
	/*
	DECDLD sixel data
		'$': carriage return
		'-': new line
		'#': color
			# Pc: select color
			# Pc; Pu; Px; Py; Pz
				Pc : color index (0 to 255)
				Pu : color coordinate system
					1: HLS (0 to 360 for Hue, 0 to 100 for others)
					2: RGB (0 to 100 percent) (default)
				Px : Hue        / Red
				Py : Lightness  / Green
				Pz : Saturation / Blue
		'"': attr
			" Pan; Pad; Ph; Pv
				Pan, Pad: defines aspect ratio (Pan / Pad) (ignored)
				Ph, Pv  : defines vertical/horizontal size of the image
		'!': repeat
			! Pn ch
				Pn : repeat count ([0-9]+)
				ch : character to repeat ('?' to '~')
		sixel bitmap:
			range of ? (hex 3F) to ~ (hex 7E)
			? (hex 3F) represents the binary value 00 0000.
			t (hex 74) represents the binary value 11 0101.
			~ (hex 7E) represents the binary value 11 1111.
	*/
	int size = 0;
	char *cp, *end_buf;
	uint8_t bitmap;

	cp = start_buf;
	end_buf = cp + strlen(start_buf);

	while (cp < end_buf) {
		if (*cp == '!') {
			size = sixel_repeat(term, sc, cp);
		} else if (*cp == '"') {
			size = sixel_attr(sc, cp);
		} else if (*cp == '#') {
			size = sixel_color(sc, cp);
		} else if (*cp == '$') {
			size = sixel_cr(sc);
		} else if (*cp == '-') {
			size = sixel_nl(sc);
		} else if ('?' <= *cp && *cp <= '~')  {
			bitmap =  bit_mask[BITS_PER_SIXEL] & (*cp - '?');
			size = sixel_bitmap(term, sc, bitmap);
		} else if (*cp == '\0') { /* end of sixel data */
			break;
		} else {
			size = 1;
		}
		cp += size;
	}

	logging(DEBUG, "sixel_parse_data()\nwidth:%d height:%d\n", sc->width, sc->height);
}

void sixel_parse_header(struct terminal_t *term, char *start_buf)
{
	/*
	sixel format
		DSC P1; P2; P3; q; s...s; ST
	parameters
		DCS: ESC(0x1B) P (0x50) (8bit C1 character not recognized)
		P1 : pixel aspect ratio (force 0, 2:1) (ignored)
		P2 : background mode (ignored)
			0 or 2: 0 stdands for current background color (default)
			1     : 0 stands for remaining current color
		P3 : horizontal grid parameter (ignored)
		q  : final character of sixel sequence
		s  : see parse_sixel_data()
		ST : ESC (0x1B) '\' (0x5C) or BEL (0x07)
	*/
	char *cp;
	struct parm_t parm;

	/* replace final char of sixel header by NUL '\0' */
	cp = strchr(start_buf, 'q');
	*cp = '\0';

	logging(DEBUG, "sixel_parse_header()\nbuf:%s\n", start_buf);

	/* split header by semicolon ';' */
	reset_parm(&parm);
	parse_arg(start_buf, &parm, ';', isdigit);

	/* set canvas parameters (lazy alloc + reset) */
	if (sixel_canvas_ensure(term) < 0)
		return;
	reset_sixel(term, term->color_pair, term->width, term->height);
	sixel_parse_data(term, &term->sixel, cp + 1); /* skip 'q' */
	sixel_copy2cell(term, &term->sixel);
	sixel_canvas_free(term);
}

static inline void decdld_bitmap(struct glyph_t *glyph, uint8_t bitmap, uint8_t row, uint8_t column)
{
	/*
			  MSB        LSB (glyph_t bitmap order, padding at LSB side)
				  -> column
	sixel bit0 ->........
	sixel bit1 ->........
	sixel bit2 ->....@@..
	sixel bit3 ->...@..@.
	sixel bit4 ->...@....
	sixel bit5 ->...@....
				 .@@@@@..
				 ...@....
				|...@....
			row |...@....
				v...@....
				 ...@....
				 ...@....
				 ...@....
				 ........
				 ........
	*/
	int i, height_shift, width_shift;

	logging(DEBUG, "bit pattern:0x%.2X\n", bitmap);

	width_shift = CELL_WIDTH - 1 - column;
	if (width_shift < 0)
		return;

	for (i = 0; i < BITS_PER_SIXEL; i++) {
		if((bitmap >> i) & 0x01) {
			height_shift = row * BITS_PER_SIXEL + i;

			if (height_shift < CELL_HEIGHT) {
				logging(DEBUG, "height_shift:%d width_shift:%d\n", height_shift, width_shift);
				glyph->bitmap[height_shift] |= bit_mask[CELL_WIDTH] & (0x01 << width_shift);
			}
		}
	}
}

static inline void init_glyph(struct glyph_t *glyph)
{
	int i;

	glyph->width = 1; /* drcs glyph must be HALF */
	glyph->code = 0;  /* this value not used: drcs call by DRCSMMv1 */

	for (i = 0; i < CELL_HEIGHT; i++)
		glyph->bitmap[i] = 0;
}

void decdld_parse_data(char *start_buf, int start_char, struct glyph_t *chars)
{
	/*
	DECDLD sixel data
		';': glyph separator
		'/': line feed
		sixel bitmap:
			range of ? (hex 3F) to ~ (hex 7E)
			? (hex 3F) represents the binary value 00 0000.
			t (hex 74) represents the binary value 11 0101.
			~ (hex 7E) represents the binary value 11 1111.
	*/
	char *cp, *end_buf;
	int char_num;
	uint8_t bitmap, row = 0, column = 0;

	/* start_char == 0 means SPACE(0x20); reject out-of-range Pcn */
	if (drcs_clamp_char_index(start_char) < 0)
		return;
	char_num = start_char;

	init_glyph(&chars[char_num]);
	cp      = start_buf;
	end_buf = cp + strlen(cp);

	while (cp < end_buf) {
		if ('?' <= *cp && *cp <= '~') { /* sixel bitmap */
			logging(DEBUG, "char_num(ten):0x%.2X\n", char_num);
			/* remove offset '?' and use only 6bit */
			bitmap = bit_mask[BITS_PER_SIXEL] & (*cp - '?');
			decdld_bitmap(&chars[char_num], bitmap, row, column);
			column++;
		} else if (*cp == ';') {  /* next char */
			row = column = 0;
			if (drcs_clamp_char_index(char_num + 1) < 0)
				break;
			char_num++;
			init_glyph(&chars[char_num]);
		} else if (*cp == '/') {  /* sixel nl+cr */
			row++;
			column = 0;
		} else if (*cp == '\0') { /* end of DECDLD sequence */
			break;
		}
		cp++;
	}
}

void decdld_parse_header(struct terminal_t *term, char *start_buf)
{
	/*
	DECDLD format
		DCS Pfn; Pcn; Pe; Pcmw; Pss; Pt; Pcmh; Pcss; f Dscs Sxbp1 ; Sxbp2 ; .. .; Sxbpn ST
	parameters
		DCS : ESC (0x1B) 'P' (0x50) (DCS(8bit C1 code) is not supported)
		Pfn : fontset (ignored)
		Pcn : start char (0 means SPACE 0x20)
		Pe  : erase mode
				0: clear selectet charset
				1: clear only redefined glyph
				2: clear all drcs charset
		Pcmw: max cellwidth (force CELL_WEDTH defined in glyph.h)
		Pss : screen size (ignored)
		Pt  : defines the glyph as text or full cell or sixel (force full cell mode)
		      (TODO: implement sixel/text mode)
		Pcmh: max cellheight (force CELL_HEIGHT defined in glyph.h)
		Pcss: character set size (force: 96)
				0: 94 glyphs charset
				1: 96 glyphs charset
		f   : '{' (0x7B)
		Dscs: define character set
				Intermediate char: SPACE (0x20) to '/' (0x2F)
				final char       : '0' (0x30) to '~' (0x7E)
									but allow chars between '@' (0x40) and '~' (0x7E) for DRCSMMv1
									(ref: https://github.com/saitoha/drcsterm/blob/master/README.rst)
		Sxbp: see parse_decdld_sixel()
		ST  : ESC (0x1B) '\' (0x5C) or BEL (0x07)
	*/
	char *cp;
	int start_char, erase_mode, charset;
	struct parm_t parm;

	/* replace final char of DECDLD header by NUL '\0' */
	cp = strchr(start_buf, '{');
	*cp = '\0';

	logging(DEBUG, "decdld_parse_header()\nbuf:%s\n", start_buf);

	/* split header by semicolon ';' */
	reset_parm(&parm);
	parse_arg(start_buf, &parm, ';', isdigit);

	if (parm.argc != 8) /* DECDLD header must have 8 params */
		return;

	/* set params */
	start_char = dec2num(parm.argv[1]);
	erase_mode = dec2num(parm.argv[2]);

	/* parse Dscs */
	cp++; /* skip final char (NUL) of DECDLD header */
	while (SPACE <= *cp && *cp <= '/') /* skip intermediate char */
		cp++;

	if (0x40 <= *cp && *cp <= 0x7E) /* final char of Dscs must be between 0x40 to 0x7E (DRCSMMv1) */
		charset = *cp - 0x40;
	else
		charset = 0;

	logging(DEBUG, "charset(ku):0x%.2X start_char:%d erase_mode:%d\n",
		charset, start_char, erase_mode);

	/* reset previous glyph data */
	if (erase_mode < 0 || erase_mode > 2)
		erase_mode = 0;

	if (erase_mode == 2)      /* reset all drcs charset */
		drcs_clear_all(term);
	else if (erase_mode == 0) /* reset selected drcs charset */
		drcs_clear_charset(term, charset);

	{
		struct glyph_t *glyphs = drcs_ensure(term, charset);

		if (!glyphs)
			return;
		decdld_parse_data(cp + 1, start_char, glyphs); /* skip final char */
	}
}
