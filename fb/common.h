/* See LICENSE for licence details. */
/* common framebuffer types (fb/fb.h), then the fb backend functions.
 * The backend functions below are definitions, not prototypes: exactly
 * one TU (cruft.c) may include this header for the main binary. */
#include "fb.h"

/* os dependent typedef/include (Calculinux/cruft: Linux only) */
#include "linux.h"

/* Status bar (status.c, status.h) hooks. Declared here rather than
 * including status.h: fb/ is a subdir of the quote-include path and the
 * build does not add -I. (the Yocto recipe overrides CFLAGS wholesale).
 * Keep in sync with status.h. Consumers include cruft.h first, so
 * struct terminal_t is already defined. */
int statusbar_h(void);
void statusbar_render(struct framebuffer_t *fb, struct terminal_t *term);
void statusbar_invalidate(void);

/* common framebuffer functions */
uint8_t *load_wallpaper(uint8_t *fp, long screen_size)
{
	uint8_t *ptr;

	if ((ptr = (uint8_t *) ecalloc(1, screen_size)) == NULL) {
		logging(ERROR, "couldn't allocate wallpaper buffer\n");
		return NULL;
	}
	memcpy(ptr, fp, screen_size);

	return ptr;
}

void cmap_die(cmap_t *cmap)
{
	if (cmap) {
		free(cmap->red);
		free(cmap->green);
		free(cmap->blue);
		//free(cmap->transp);
		free(cmap);
	}
}

cmap_t *cmap_create(int colors)
{
	cmap_t *cmap;

	if ((cmap = (cmap_t *) ecalloc(1, sizeof(cmap_t))) == NULL) {
		logging(ERROR, "couldn't allocate cmap buffer\n");
		return NULL;
	}

	/* init os specific */
	alloc_cmap(cmap, colors);

	if (!cmap->red || !cmap->green || !cmap->blue) {
		logging(ERROR, "couldn't allocate red/green/blue buffer of cmap\n");
		cmap_die(cmap);
		return NULL;
	}
	return cmap;
}

bool cmap_update(int fd, cmap_t *cmap)
{
	if (cmap) {
		if (put_cmap(fd, cmap)) {
			logging(ERROR, "put_cmap failed\n");
			return false;
		}
	}
	return true;
}

bool cmap_save(int fd, cmap_t *cmap)
{
	if (get_cmap(fd, cmap)) {
		logging(WARN, "get_cmap failed\n");
		return false;
	}
	return true;
}

bool cmap_init(int fd, struct fb_info_t *info, cmap_t *cmap, int colors, int length)
{
	uint16_t r, g, b, r_index, g_index, b_index;

	for (int i = 0; i < colors; i++) {
		if (info->visual == YAFT_FB_VISUAL_DIRECTCOLOR) {
			r_index = (i >> (length - info->red.length))   & bit_mask[info->red.length];
			g_index = (i >> (length - info->green.length)) & bit_mask[info->green.length];
			b_index = (i >> (length - info->blue.length))  & bit_mask[info->blue.length];

			/* XXX: maybe only upper order byte is used */
			r = r_index << (CMAP_COLOR_LENGTH - info->red.length);
			g = g_index << (CMAP_COLOR_LENGTH - info->green.length);
			b = b_index << (CMAP_COLOR_LENGTH - info->blue.length);

			*(cmap->red   + r_index) = r;
			*(cmap->green + g_index) = g;
			*(cmap->blue  + b_index) = b;
		} else { /* YAFT_FB_VISUAL_PSEUDOCOLOR */
			r_index = (i >> info->red.offset)   & bit_mask[info->red.length];
			g_index = (i >> info->green.offset) & bit_mask[info->green.length];
			b_index = (i >> info->blue.offset)  & bit_mask[info->blue.length];

			r = r_index << (CMAP_COLOR_LENGTH - info->red.length);
			g = g_index << (CMAP_COLOR_LENGTH - info->green.length);
			b = b_index << (CMAP_COLOR_LENGTH - info->blue.length);

			*(cmap->red   + i) = r;
			*(cmap->green + i) = g;
			*(cmap->blue  + i) = b;
		}
	}

	if (!cmap_update(fd, cmap))
		return false;

	return true;
}

bool init_truecolor(struct fb_info_t *info, cmap_t **cmap, cmap_t **cmap_orig)
{
	switch(info->bits_per_pixel) {
	case 15: /* XXX: 15 bpp is not tested */
	case 16:
	case 24:
	case 32:
		break;
	default:
		logging(ERROR, "truecolor %d bpp not supported\n", info->bits_per_pixel);
		return false;
	}
	/* we don't use cmap in truecolor */
	*cmap = *cmap_orig = NULL;

	return true;
}

bool init_indexcolor(int fd, struct fb_info_t *info, cmap_t **cmap, cmap_t **cmap_orig)
{
	int colors, max_length;

	if (info->visual == YAFT_FB_VISUAL_DIRECTCOLOR) {
		switch(info->bits_per_pixel) {
		case 15: /* XXX: 15 bpp is not tested */
		case 16:
		case 24: /* XXX: 24 bpp is not tested */
		case 32:
			break;
		default:
			logging(ERROR, "directcolor %d bpp not supported\n", info->bits_per_pixel);
			return false;
		}
		logging(DEBUG, "red.length:%d gree.length:%d blue.length:%d\n",
			info->red.length, info->green.length, info->blue.length);

		max_length = (info->red.length > info->green.length) ? info->red.length: info->green.length;
		max_length = (max_length > info->blue.length) ? max_length: info->blue.length;
	} else { /* YAFT_FB_VISUAL_PSEUDOCOLOR */
		switch(info->bits_per_pixel) {
		case 8:
			break;
		default:
			logging(ERROR, "pseudocolor %d bpp not supported\n", info->bits_per_pixel);
			return false;
		}

		/* in pseudo color, we use fixed palette (red/green: 3bit, blue: 2bit).
			this palette is not compatible with xterm 256 colors,
			but we can convert 24bit color into palette number easily. */
		info->red.length   = 3;
		info->green.length = 3;
		info->blue.length  = 2;

		info->red.offset   = 5;
		info->green.offset = 2;
		info->blue.offset  = 0;

		/* XXX: not 3 but 8, each color has 256 colors palette */
		max_length = 8;
	}

	colors = 1 << max_length;
	logging(DEBUG, "colors:%d max_length:%d\n", colors, max_length);

	*cmap      = cmap_create(colors);
	*cmap_orig = cmap_create(colors);

	if (!(*cmap) || !(*cmap_orig))
		goto cmap_init_err;

	if (!cmap_save(fd, *cmap_orig)) {
		logging(WARN, "couldn't save original cmap\n");
		cmap_die(*cmap_orig);
		*cmap_orig = NULL;
	}

	if (!cmap_init(fd, info, *cmap, colors, max_length))
		goto cmap_init_err;

	return true;

cmap_init_err:
	cmap_die(*cmap);
	cmap_die(*cmap_orig);
	return false;
}

void fb_print_info(struct fb_info_t *info)
{
	const char *type_str[] = {
		[YAFT_FB_TYPE_PACKED_PIXELS] = "YAFT_FB_TYPE_PACKED_PIXELS",
		[YAFT_FB_TYPE_PLANES]        = "YAFT_FB_TYPE_PLANES",
		[YAFT_FB_TYPE_UNKNOWN]       = "YAFT_FB_TYPE_UNKNOWN",
	};

	const char *visual_str[] = {
		[YAFT_FB_VISUAL_TRUECOLOR]   = "YAFT_FB_VISUAL_TRUECOLOR",
		[YAFT_FB_VISUAL_DIRECTCOLOR] = "YAFT_FB_VISUAL_DIRECTCOLOR",
		[YAFT_FB_VISUAL_PSEUDOCOLOR] = "YAFT_FB_VISUAL_PSEUDOCOLOR",
		[YAFT_FB_VISUAL_UNKNOWN]     = "YAFT_FB_VISUAL_UNKNOWN",
	};

	logging(DEBUG, "framebuffer info:\n");
	logging(DEBUG, "\tred(off:%d len:%d) green(off:%d len:%d) blue(off:%d len:%d)\n",
		info->red.offset, info->red.length, info->green.offset, info->green.length, info->blue.offset, info->blue.length);
	logging(DEBUG, "\tresolution %dx%d\n", info->width, info->height);
	logging(DEBUG, "\tscreen size:%ld line length:%d\n", info->screen_size, info->line_length);
	logging(DEBUG, "\tbits_per_pixel:%d bytes_per_pixel:%d\n", info->bits_per_pixel, info->bytes_per_pixel);
	logging(DEBUG, "\ttype:%s\n", type_str[info->type]);
	logging(DEBUG, "\tvisual:%s\n", visual_str[info->visual]);
}

bool fb_init(struct framebuffer_t *fb)
{
	extern const uint32_t color_list[COLORS]; /* defined in color.h */
	extern const char *fb_path;               /* defined in conf.h */
	const char *path;
	char *env;
	int want_wall = 0;

	/* open framebuffer device: check FRAMEBUFFER env at first */
	path = ((env = getenv("FRAMEBUFFER")) == NULL) ? fb_path: env;
	if ((fb->fd = eopen(path, O_RDWR)) < 0)
		return false;

	/* os dependent initialize */
	if (!set_fbinfo(fb->fd, &fb->info))
		goto set_fbinfo_failed;

	if (VERBOSE)
		fb_print_info(&fb->info);

	/* wallpaper / shadow buffer only when CRUFT env contains "wall" */
	if ((env = getenv("CRUFT")) && strstr(env, "wall"))
		want_wall = 1;

	/* allocate memory */
	fb->fp   = (uint8_t *) emmap(0, fb->info.screen_size,
				PROT_WRITE | PROT_READ, MAP_SHARED, fb->fd, 0);
	fb->buf  = want_wall ? (uint8_t *) ecalloc(1, fb->info.screen_size) : NULL;
	fb->wall = want_wall ? load_wallpaper(fb->fp, fb->info.screen_size) : NULL;

	/* error check */
	if (fb->fp == MAP_FAILED || (want_wall && !fb->buf))
		goto allocate_failed;

	if (fb->info.type != YAFT_FB_TYPE_PACKED_PIXELS) {
		/* TODO: support planes type */
		logging(ERROR, "unsupport framebuffer type\n");
		goto fb_init_failed;
	}

	if (fb->info.visual == YAFT_FB_VISUAL_TRUECOLOR) {
		if (!init_truecolor(&fb->info, &fb->cmap, &fb->cmap_orig))
			goto fb_init_failed;
	} else if (fb->info.visual == YAFT_FB_VISUAL_DIRECTCOLOR
		|| fb->info.visual == YAFT_FB_VISUAL_PSEUDOCOLOR) {
		if (!init_indexcolor(fb->fd, &fb->info, &fb->cmap, &fb->cmap_orig))
			goto fb_init_failed;
	} else {
		/* TODO: support mono visual */
		logging(ERROR, "unsupport framebuffer visual\n");
		goto fb_init_failed;
	}

	/* init color palette */
	for (int i = 0; i < COLORS; i++)
		fb->real_palette[i] = color2pixel(&fb->info, color_list[i]);

	return true;

fb_init_failed:
allocate_failed:
	free(fb->buf);
	free(fb->wall);
	if (fb->fp != MAP_FAILED)
		emunmap(fb->fp, fb->info.screen_size);
set_fbinfo_failed:
	eclose(fb->fd);
	return false;
}

void fb_die(struct framebuffer_t *fb)
{
	cmap_die(fb->cmap);
	if (fb->cmap_orig) {
		put_cmap(fb->fd, fb->cmap_orig);
		cmap_die(fb->cmap_orig);
	}
	free(fb->wall);
	free(fb->buf);
	emunmap(fb->fp, fb->info.screen_size);
	eclose(fb->fd);
	//fb_release(fb->fd, &fb->info); /* os specific */
}

/*
 * Grid is centered horizontally so the leftover (width - cols*CELL_WIDTH)
 * pixels are split between the left and right edges. Vertically the grid
 * sits below the status bar strip: rows are anchored at term->oy (0 when
 * the bar is off, one status-font line when it is on). The same origins
 * must be applied to text (draw_line) and sixel blits (draw_sixel) or
 * sixel content lands offset from the adjacent glyphs.
 */
static inline int grid_origin_x(const struct terminal_t *term)
{
	return (term->width - term->cols * CELL_WIDTH) / 2;
}

/*
 * Paint the pixels the cell grid never covers: left/right edge strips
 * (width - cols*CELL_WIDTH, split around grid_origin_x), the bottom
 * strip (height - oy - lines*CELL_HEIGHT), and the top strip (0..oy) —
 * except when the status bar owns the top strip (statusbar_h() > 0):
 * it paints that region itself, so leave it to the bar. Contents:
 * wallpaper when present, else the default background. Upstream yaft
 * hid this by always flushing a zeroed full-screen shadow; direct-draw
 * mode has no shadow, so the strips must be written explicitly or stale
 * framebuffer contents (kernel boot log, previous VT) survive in them.
 */
static void fb_paint_margin(struct framebuffer_t *fb, struct terminal_t *term)
{
	int ox, oy, grid_w, grid_h, x, y;
	int rem_left, rem_right;
	uint32_t bgpix = fb->real_palette[DEFAULT_BG];
	uint8_t *dest = fb_draw_dest(fb);
	int bpp = fb->info.bytes_per_pixel;
	int ll = fb->info.line_length;

	ox = grid_origin_x(term);
	oy = term->oy;
	grid_w = term->cols * CELL_WIDTH;
	grid_h = term->lines * CELL_HEIGHT;
	rem_left = ox;
	rem_right = term->width - ox - grid_w;
	/* Nothing to paint: no side strips and the grid reaches the bottom.
	 * (A top strip only exists when the bar is on, and the bar paints it
	 * itself, so it is not a reason to run the loop.) */
	if (rem_left <= 0 && rem_right <= 0 && grid_h >= term->height - oy)
		return;

	for (y = 0; y < term->height; y++) {
		int bot_strip = (y >= oy + grid_h);
		int x_end;

		if (y < oy && statusbar_h() > 0)
			continue; /* status bar owns the top strip */
		if (bot_strip)
			x_end = term->width;
		else {
			/* left strip only; right strip handled per-x below */
			x_end = ox;
		}
		for (x = 0; x < x_end; x++) {
			int pos = y * ll + x * bpp;
			if (fb->wall)
				memcpy(dest + pos, fb->wall + pos, bpp);
			else
				memcpy(dest + pos, &bgpix, bpp);
		}
		for (x = ox + grid_w; x < term->width; x++) {
			int pos = y * ll + x * bpp;
			if (fb->wall)
				memcpy(dest + pos, fb->wall + pos, bpp);
			else
				memcpy(dest + pos, &bgpix, bpp);
		}
	}

	/* Shadow mode: per-line blits only flush the grid rows, so copy the
	 * uncovered strips to the display ourselves. The side margins in
	 * particular ride along with dirty-row blits only; a refresh() with
	 * no dirty rows (e.g. cursor hidden) would otherwise leave stale
	 * pixels in the visible margins after contamination. Rows above the
	 * status strip (y < oy) are owned by the bar, which flushes them
	 * itself. */
	if (fb->buf) {
		for (y = oy; y < term->height; y++) {
			if (rem_left > 0)
				memcpy(fb->fp + (long)y * ll, fb->buf + (long)y * ll,
					(long)rem_left * bpp);
			if (rem_right > 0)
				memcpy(fb->fp + (long)y * ll + (long)(ox + grid_w) * bpp,
					fb->buf + (long)y * ll + (long)(ox + grid_w) * bpp,
					(long)rem_right * bpp);
		}
		if (grid_h < term->height - oy)
			memcpy(fb->fp + (long)(oy + grid_h) * ll, fb->buf + (long)(oy + grid_h) * ll,
				(long)(term->height - oy - grid_h) * ll);
	}
}

static inline void draw_sixel(struct framebuffer_t *fb, struct terminal_t *term, int line, int col, uint8_t *pixmap)
{
	int h, w, src_offset, dst_offset;
	uint32_t pixel, color = 0;
	uint8_t *dest = fb_draw_dest(fb);
	int ox = grid_origin_x(term);
	/* Clip the right-edge cell to the drawable area (valid cols never
	 * exceed it, this is defensive). */
	int avail = fb->info.width - ox - col * CELL_WIDTH;
	if (avail > CELL_WIDTH)
		avail = CELL_WIDTH;

	for (h = 0; h < CELL_HEIGHT; h++) {
		for (w = 0; w < avail; w++) {
			src_offset = BYTES_PER_PIXEL * (h * CELL_WIDTH + w);
			memcpy(&color, pixmap + src_offset, BYTES_PER_PIXEL);

			dst_offset = (term->oy + line * CELL_HEIGHT + h) * fb->info.line_length
				+ (ox + col * CELL_WIDTH + w) * fb->info.bytes_per_pixel;
			pixel = color2pixel(&fb->info, color);
			memcpy(dest + dst_offset, &pixel, fb->info.bytes_per_pixel);
		}
	}
}

static inline void draw_line(struct framebuffer_t *fb, struct terminal_t *term, int line)
{
	int pos, size, bit_base, ox;
	int col, w, h;
	uint32_t pixel;
	struct color_pair_t color_pair;
	struct cell_t *cellp;
	uint8_t *dest = fb_draw_dest(fb);

	ox = grid_origin_x(term);
	for (col = term->cols - 1; col >= 0; col--) {
		/* target cell */
		cellp = &term->cells[line][col];

		/* draw sixel pixmap */
		if (cellp->has_pixmap && cellp->pixmap) {
			draw_sixel(fb, term, line, col, cellp->pixmap);
			continue;
		}

		/* copy current color_pair (maybe changed) */
		color_pair = cellp->color_pair;

		/*
		 * .cruftfont rows are packed into exactly cell_w [*2 if wide]
		 * bits with LSB = rightmost pixel (see mkcruftfont). Upstream
		 * BDF mkfont left-padded to a byte boundary; that padding must
		 * not be applied here or 6px glyphs shift ~half a cell right.
		 */
		bit_base = (cellp->width == WIDE) ? CELL_WIDTH : 0;

		/* check cursor position */
		if ((term->mode & MODE_CURSOR && line == term->cursor.y)
			&& (col == term->cursor.x
			|| (cellp->width == WIDE && (col + 1) == term->cursor.x)
			|| (cellp->width == NEXT_TO_WIDE && (col - 1) == term->cursor.x))) {
			color_pair.fg = DEFAULT_BG;
			color_pair.bg = (!vt_active && BACKGROUND_DRAW) ? PASSIVE_CURSOR_COLOR: ACTIVE_CURSOR_COLOR;
		}

		for (h = 0; h < CELL_HEIGHT; h++) {
			/* if UNDERLINE attribute on, swap bg/fg */
			if ((h == (CELL_HEIGHT - 1)) && (cellp->attribute & attr_mask[ATTR_UNDERLINE]))
				color_pair.bg = color_pair.fg;

			for (w = 0; w < CELL_WIDTH; w++) {
				/* w counts from the cell's right edge (bit 0 = LSB =
				 * rightmost pixel); x grows left-to-right as w shrinks.
				 * (The old right-anchored form width-1-margin_right-w
				 * baked in the full leftover on the left; centered grids
				 * must use the explicit ox + col*CELL_WIDTH form.) */
				pos = (ox + col * CELL_WIDTH + (CELL_WIDTH - 1 - w)) * fb->info.bytes_per_pixel
					+ (term->oy + line * CELL_HEIGHT + h) * fb->info.line_length;

				/* set color palette */
				if (cellp->glyphp->bitmap[h] & (0x01 << (bit_base + w)))
					pixel = fb->real_palette[color_pair.fg];
				else if (fb->wall && color_pair.bg == DEFAULT_BG) /* wallpaper */
					memcpy(&pixel, fb->wall + pos, fb->info.bytes_per_pixel);
				else
					pixel = fb->real_palette[color_pair.bg];

				/* update draw destination (shadow or direct fb) */
				memcpy(dest + pos, &pixel, fb->info.bytes_per_pixel);
			}
		}
	}

	/* actual display update (bit blit) — skip when drawing direct to fp */
	if (fb->buf) {
		pos = (line * CELL_HEIGHT) * fb->info.line_length;
		size = CELL_HEIGHT * fb->info.line_length;
		memcpy(fb->fp + pos, fb->buf + pos, size);
	}

	/* TODO: page flip
		if fb_fix_screeninfo.ypanstep > 0, we can use hardware panning.
		set fb_fix_screeninfo.{yres_virtual,yoffset} and call ioctl(FBIOPAN_DISPLAY)
		but drivers  of recent hardware (inteldrmfb, nouveaufb, radeonfb) don't support...
		(maybe we can use this by using libdrm) */
	/* TODO: vertical synchronizing */

	term->line_dirty[line] = ((term->mode & MODE_CURSOR) && term->cursor.y == line) ? true: false;
}

void refresh(struct framebuffer_t *fb, struct terminal_t *term)
{
	if (term->palette_modified) {
		term->palette_modified = false;
		for (int i = 0; i < COLORS; i++)
			fb->real_palette[i] = color2pixel(&fb->info, term->virtual_palette[i]);
		statusbar_invalidate(); /* strip colors come from the palette */
	}

	if (term->mode & MODE_CURSOR)
		term->line_dirty[term->cursor.y] = true;

	/* Edge strips the grid never covers (left/right/bottom margins; the
	 * top strip is the status bar's when it is on). */
	fb_paint_margin(fb, term);

	for (int line = 0; line < term->lines; line++) {
		if (term->line_dirty[line]) {
			draw_line(fb, term, line);
		}
	}

	/* Status bar in the top strip (no-op when disabled) */
	statusbar_render(fb, term);
}
