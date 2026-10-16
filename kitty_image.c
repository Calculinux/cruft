/*
 * Kitty graphics protocol (OSC 1337) (Calculinux/cruft).
 *
 * Second image path next to sixel: clients that speak the kitty graphics
 * protocol (kitten icat, catimg, chafa -O kitty) send images as
 *     ESC ] 1337 ; File ; key=value ... ; PAYLOAD (BEL|ST)
 * with PAYLOAD being raw BGR/BGRA pixels, optionally base64-encoded, or
 * PNG (PNG only when built against the system libpng, see makefile).
 *
 * Placements land on the same sparse per-cell pixmaps as sixel
 * (erase_cell + cell_pixmap_ensure + the sixel_copy2cell-style
 * right-edge-clipped copy loop), so scroll/erase/alt-screen/VT-deactivate
 * behaviour is inherited for free. Images are anchored at the cursor and
 * fit (nearest neighbour, aspect preserved by default) to the region from
 * the cursor to the grid edges; they never scroll the screen. Protocol
 * pixel order is BGR(A); cell pixmaps store 0x00RRGGBB like sixel.
 *
 * Image ; operators (chunked upload, query, delete, ...) are not
 * implemented: every one of them is answered with a terse
 *     ESC ] 1337 ; Rep ; i=<id> ; ok=false ; t=<reason> BEL
 * so probing clients degrade to text instead of blocking on a reply that
 * never comes.
 */
#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBPNG
#include <png.h>
#endif

/* move_cursor / set_cursor / cr live in terminal.h (unity from cruft.c). */
void move_cursor(struct terminal_t *term, int y_offset, int x_offset);
void set_cursor(struct terminal_t *term, int y, int x);
void cr(struct terminal_t *term);

/*
 * Decoded-pixel budget (w*h), before fit-scaling. 512x512 covers anything
 * a 320x320-class grid ever needs; bigger payloads are dropped with a note
 * instead of blowing the RAM budget (sixel is inherently bounded by the
 * terminal size; raw payloads need their own gate).
 */
#define KITTY_MAX_PIXELS 262144u
/* Payload gate before base64 expansion: ~8 MB (1 MB RGBA raw -> 1.37 MB b64). */
#define KITTY_MAX_PAYLOAD (8u * 1024u * 1024u)
#define KITTY_TRANSFER_ID_MAX 60
#define KITTY_MAX_FIELDS 16

static const uint8_t png_magic[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

static int b64_val(int c)
{
	if ('A' <= c && c <= 'Z')
		return c - 'A';
	if ('a' <= c && c <= 'z')
		return c - 'a' + 26;
	if ('0' <= c && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

int kitty_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
	size_t real = 0, pad = 0, o = 0, need;
	uint32_t acc = 0;
	unsigned n = 0;

	if (!in || !out || !out_len)
		return -1;
	if (len % 4 == 1)
		return -1;

	for (size_t i = 0; i < len; i++) {
		char c = in[i];

		if (c == '=')
			pad++;
		else if (b64_val(c) < 0)
			return -1;
		else
			real++;
	}

	/* padding, when present, is exact in count and last; absent
	 * padding is tolerated (MIME-style streams omit it) */
	if (real % 4 == 1)
		return -1;
	if (pad != 0 && pad != (4 - (real % 4)) % 4)
		return -1;
	for (size_t i = 0; i < real; i++)
		if (in[i] == '=')
			return -1;

	need = real / 4 * 3 + (real % 4 == 2 ? 1: real % 4 == 3 ? 2: 0);
	if (cap < need)
		return -1;

	for (size_t i = 0; i < real; i++) {
		acc = (acc << 6) | (uint32_t) b64_val(in[i]);
		if (++n == 4) {
			n = 0;
			out[o++] = (uint8_t) (acc >> 16);
			out[o++] = (uint8_t) (acc >> 8);
			out[o++] = (uint8_t) acc;
		}
	}
	if (n == 2)
		out[o++] = (uint8_t) (acc >> 4);
	else if (n == 3) {
		out[o++] = (uint8_t) (acc >> 10);
		out[o++] = (uint8_t) (acc >> 2);
	}

	*out_len = need;
	return 0;
}

/*
 * Recognised key -> its single-letter form, or -1. Single-letter keys
 * stand alone; multi-letter spellings must match a full name, so a
 * base64 payload that merely *begins* with a key letter (e.g. PNGs
 * start with "iV...") is still classified as data.
 */
static int key_identify(const char *k, size_t klen)
{
	if (klen == 1)
		return strchr("fwhabtdimsqs", k[0]) ? tolower((unsigned char) k[0]): -1;
	if (klen >= 2 && klen <= 12) {
		static const char *names[] = {
			"file", "width", "height", "asp", "aspectratio", "aspect",
			"base64", "transparency", "data", "id", "method", "quality",
			"signal",
		};
		char lc[13];

		for (size_t i = 0; i < klen; i++)
			lc[i] = (char) tolower((unsigned char) k[i]);
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (klen == strlen(names[i]) && strncmp(lc, names[i], klen) == 0)
				return (int) names[i][0];
	}
	return -1;
}

/* Decimal digits only (protocol integers); rejects sign/overflow/empties. */
static int parse_uint(const char *v, size_t vlen, unsigned long *out)
{
	unsigned long r = 0;

	if (vlen == 0 || vlen > 9)
		return -1;
	for (size_t i = 0; i < vlen; i++) {
		if (!isdigit((unsigned char) v[i]))
			return -1;
		r = r * 10 + (unsigned long) (v[i] - '0');
	}
	*out = r;
	return 0;
}

/* yes/true/1/y -> 1, no/false/0/n -> 0, case-insensitive. */
static int parse_flag(const char *v, size_t vlen, int *out)
{
	static const struct {
		const char *word;
		int val;
	} tbl[] = {
		{ "yes", 1 }, { "true", 1 }, { "1", 1 }, { "y", 1 },
		{ "no", 0 }, { "false", 0 }, { "0", 0 }, { "n", 0 },
	};
	char lc[8];

	if (vlen == 0 || vlen > 7)
		return -1;
	for (size_t i = 0; i < vlen; i++)
		lc[i] = (char) tolower((unsigned char) v[i]);
	for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
		if (vlen == strlen(tbl[i].word) && strncmp(lc, tbl[i].word, vlen) == 0) {
			*out = tbl[i].val;
			return 0;
		}
	return -1;
}

int kitty_args_parse(const char *payload, struct kitty_args *ka)
{
	const char *fields[KITTY_MAX_FIELDS];
	size_t flen[KITTY_MAX_FIELDS];
	int nf = 0;
	const char *p;

	if (!payload || !ka)
		return -1;
	*ka = (struct kitty_args) {
		.action = NULL, .data = NULL, .data_len = 0,
		.width = -1, .height = -1,
		.preserve_aspect = 1, .base64 = 1, .transparency = 0,
		.id = NULL, .id_len = 0,
		.method = NULL, .method_len = 0,
	};

	/*
	 * Slice on ';' without mutating the caller's buffer (parse_arg's
	 * in-place NUL splitting is what keeps 1337 out of the ordinary
	 * OSC path). Every recognised key - single-letter or full word -
	 * starts with a distinct letter, so the first letter identifies
	 * the key. The LAST field is data unless its key is recognised
	 * (base64 padding puts '=' inside the payload).
	 */
	p = payload;
	while (*p) {
		const char *semi = strchr(p, ';');
		size_t l = semi ? (size_t) (semi - p): strlen(p);

		if (nf >= KITTY_MAX_FIELDS)
			return -1;
		fields[nf] = p;
		flen[nf] = l;
		nf++;
		if (!semi)
			break;
		p = semi + 1;
	}
	if (nf == 0)
		return -1;

	if (flen[0] == 4 && strncmp(fields[0], "File", 4) == 0)
		ka->action = "File";
	else if (flen[0] == 5 && strncmp(fields[0], "Image", 5) == 0)
		ka->action = "Image";
	else
		return -1;

	for (int i = 1; i < nf; i++) {
		const char *f = fields[i];
		const char *eq = (const char *) memchr(f, '=', flen[i]);
		const char *value = NULL;
		size_t vlen = 0;
		int key;

		if (eq) {
			size_t klen = (size_t) (eq - f);

			key = key_identify(f, klen);
			if (i == nf - 1 && key < 0) {
				/* last field, unrecognised key: the payload
				 * (base64 padding may put '=' inside it) */
				ka->data = f;
				ka->data_len = flen[i];
				continue;
			}
			if (key < 0)
				continue; /* forward-compat: unknown key */
			value = eq + 1;
			vlen = flen[i] - klen - 1;
		} else {
			if (i != nf - 1)
				return -1; /* key without '=' only legal as final data */
			ka->data = f;
			ka->data_len = flen[i];
			continue;
		}

		switch (key) {
		case 'w':
			if (parse_uint(value, vlen, (unsigned long *) &ka->width) < 0)
				return -1;
			if (ka->width > 65535)
				return -1;
			break;
		case 'h':
			if (parse_uint(value, vlen, (unsigned long *) &ka->height) < 0)
				return -1;
			if (ka->height > 65535)
				return -1;
			break;
		case 'a':
			if (parse_flag(value, vlen, &ka->preserve_aspect) < 0)
				return -1;
			break;
		case 'b':
			if (parse_flag(value, vlen, &ka->base64) < 0)
				return -1;
			break;
		case 't':
			if (parse_flag(value, vlen, &ka->transparency) < 0)
				return -1;
			break;
		case 'd': /* data= (explicit; the implicit tail still wins if given) */
			ka->data = value;
			ka->data_len = vlen;
			break;
		case 'i':
			ka->id = value;
			ka->id_len = vlen;
			break;
		case 'm':
			ka->method = value;
			ka->method_len = vlen;
			break;
		default:
			/* f(ile name), q(uality), s(ignal): accepted, unused */
			break;
		}
	}

	/* data is mandatory for File; Image operations (query, delete, ...)
	 * carry none. */
	if (strcmp(ka->action, "File") == 0 && (!ka->data || ka->data_len == 0))
		return -1;
	return 0;
}

/* TRUE only when built against libpng: the makefile probe verified both
 * the header and the high-level png_image API symbols, so no further
 * feature-testing is needed (plain libpng 1.6 ships it unconditionally). */
bool kitty_png_available(void)
{
#ifdef HAVE_LIBPNG
	return true;
#else
	return false;
#endif
}

/*
 * Fit a sw x sh source into avail_w x avail_h px: scale down uniformly
 * when preserve_aspect, else 1:1 cropped to the available region. Results
 * stay within [1, avail] when avail >= 1.
 */
static void kitty_fit(int sw, int sh, int avail_w, int avail_h, int preserve_aspect,
		      int *tw, int *th)
{
	double fx = (double) avail_w / (double) sw;
	double fy = (double) avail_h / (double) sh;
	double s = preserve_aspect ? (fx < fy ? fx: fy): 1.0;

	if (s > 1.0)
		s = 1.0; /* never upscale: sixel draws 1:1 too */

	*tw = (int) (sw * s + 0.5);
	*th = (int) (sh * s + 0.5);
	if (*tw > avail_w)
		*tw = avail_w;
	if (*th > avail_h)
		*th = avail_h;
	if (*tw < 1)
		*tw = 1;
	if (*th < 1)
		*th = 1;
}

/*
 * Expand source pixels (BGR[A] or RGB[A] per first_red, sw x sh) to tw x th
 * (nearest neighbour) as 0x00RRGGBB. Semi-transparent pixels are blended over
 * bg when transparent != 0, else rendered opaque (alpha dropped) - kitty's
 * t= semantics, sans wallpaper interaction (cell pixmaps have no alpha).
 */
static void raw_expand(const uint8_t *raw, int sw, int sh, unsigned ch,
		       int first_red, int transparent, uint32_t bg,
		       int tw, int th, uint32_t *out)
{
	for (int y = 0; y < th; y++) {
		int sy = (int) (((long) y * (long) sh) / (long) th);

		for (int x = 0; x < tw; x++) {
			int sx = (int) (((long) x * (long) sw) / (long) tw);
			const uint8_t *p = raw + ((size_t) sy * (size_t) sw + (size_t) sx) * ch;
			uint32_t r, g, b, a;

			if (first_red)
				r = p[0], g = p[1], b = p[2];
			else
				b = p[0], g = p[1], r = p[2];
			a = (ch == 4) ? p[3]: 255;

			if (a < 255 && transparent) {
				r = (r * a + (bg >> 16) * (255 - a)) / 255;
				g = (g * a + (bg >> 8) * (255 - a)) / 255;
				b = (b * a + (bg & 0xFF) * (255 - a)) / 255;
			}
			out[(size_t) y * (size_t) tw + (size_t) x] = (r << 16) | (g << 8) | b;
		}
	}
}

/* Composite an RGBA buffer (what libpng hands us) the same way.
 * Only reachable in a libpng build. */
#ifdef HAVE_LIBPNG
static void rgba_composite(const uint8_t *rgba, int tw, int th, int transparent,
			   uint32_t bg, uint32_t *out)
{
	for (int i = 0; i < tw * th; i++) {
		uint32_t r = rgba[i * 4 + 0];
		uint32_t g = rgba[i * 4 + 1];
		uint32_t b = rgba[i * 4 + 2];
		uint32_t a = rgba[i * 4 + 3];

		if (a < 255 && transparent) {
			r = (r * a + (bg >> 16) * (255 - a)) / 255;
			g = (g * a + (bg >> 8) * (255 - a)) / 255;
			b = (b * a + (bg & 0xFF) * (255 - a)) / 255;
		}
		out[i] = (r << 16) | (g << 8) | b;
	}
}
#endif /* HAVE_LIBPNG */

/*
 * Decode a PNG payload via the libpng high-level API into 0x00RRGGBB
 * (aspect-preserving fit into the region below/right of the cursor).
 * *img_out is malloc'd on success.
 */
static int png_decode(const uint8_t *raw, size_t n, struct terminal_t *term,
		      int transparent, uint32_t bg, int *tw, int *th, uint32_t **img)
{
#ifdef HAVE_LIBPNG
	/* png_image is a typedef of an anonymous struct in libpng */
	png_image pi;
	uint32_t *rgba = NULL;
	uint32_t *scaled = NULL;
	uint32_t *out = NULL;
	int rw, rh, rc, x0, y0, aw, ah;

	memset(&pi, 0, sizeof(pi));
	pi.version = PNG_IMAGE_VERSION;
	rc = png_image_begin_read_from_memory(&pi, raw, n);
	if (rc <= 0) { /* 0 = error (-1 = warned, succeeded) */
		if (rc == 0)
			png_image_free(&pi);
		return -1;
	}
	/* begin_read() overwrites 'format' with the source description:
	 * programme the requested conversion only after it, per the API
	 * docs - setting it before the call is silently discarded. */
	pi.format = PNG_FORMAT_RGBA;

	/* trust the natural size encoded in the PNG over any w/h the
	 * client claims */
	rw = (int) pi.width;
	rh = (int) pi.height;
	if (rw < 1 || rh < 1 ||
	    (unsigned long long) rw * (unsigned long long) rh > KITTY_MAX_PIXELS) {
		fprintf(stderr, "cruft: kitty_image: PNG exceeds %u px budget\n",
			KITTY_MAX_PIXELS);
		png_image_free(&pi);
		return -1;
	}

	x0 = active_screen(term)->cursor.x * CELL_WIDTH;
	y0 = active_screen(term)->cursor.y * CELL_HEIGHT;
	/* Fit region measured in cell-grid pixel space: term->width/height
	 * span the whole framebuffer (incl. the status strip), the cell grid
	 * only extends cols*CELL_WIDTH x lines*CELL_HEIGHT. */
	aw = term->cols * CELL_WIDTH - x0;
	ah = term->lines * CELL_HEIGHT - y0;
	if (aw < 1 || ah < 1) {
		png_image_free(&pi);
		return -1;
	}
	kitty_fit(rw, rh, aw, ah, 1, tw, th); /* aspect kept for rasters */

	rgba = (uint32_t *) malloc((size_t) rw * (size_t) rh * 4);
	if (!rgba) {
		fprintf(stderr, "cruft: kitty_image: alloc failed\n");
		png_image_free(&pi);
		return -1;
	}
	rc = png_image_finish_read(&pi, NULL, rgba, rw * 4, NULL);
	png_image_free(&pi);
	if (rc < 1) {
		free(rgba);
		return -1;
	}

	/* the simplified libpng API decodes at natural size only: shrink
	 * ourselves (nearest neighbour, like the raw path) into the fitted
	 * region - never upscale */
	if (*tw != rw || *th != rh) {
		scaled = (uint32_t *) malloc((size_t) *tw * (size_t) *th * 4);
		if (!scaled) {
			fprintf(stderr, "cruft: kitty_image: alloc failed\n");
			free(rgba);
			return -1;
		}
		for (int y = 0; y < *th; y++)
			for (int x = 0; x < *tw; x++)
				memcpy(scaled + (size_t) y * (size_t) *tw + x,
				       rgba + ((size_t) (y * rh / *th) * (size_t) rw
					       + (size_t) (x * rw / *tw)),
				       4);
		free(rgba);
		rgba = scaled;
	}

	out = (uint32_t *) malloc((size_t) *tw * (size_t) *th * BYTES_PER_PIXEL);
	if (!out) {
		free(rgba);
		fprintf(stderr, "cruft: kitty_image: alloc failed\n");
		return -1;
	}
	rgba_composite((const uint8_t *) rgba, *tw, *th, transparent, bg, out);
	free(rgba);
	*img = out;
	return 0;
#else
	(void) raw; (void) n; (void) term; (void) transparent; (void) bg;
	(void) tw; (void) th; (void) img;
	return -1;
#endif
}

/*
 * Paint a tw x th 0x00RRGGBB image at the cursor, mirroring sixel_copy2cell's
 * ownership dance (erase_cell, lazy cell_pixmap_ensure, right-edge partial
 * cell). Callers pre-clip tw x th to the available region, so no cell lies
 * outside the grid and the screen never scrolls. Reports the actually
 * painted cell extent (clipped) in *p_lines / *p_cols; (0,0) when nothing fit.
 */
static void kitty_paint_core(struct terminal_t *term, const uint32_t *img,
			     int tw, int th, int *p_lines, int *p_cols)
{
	struct screen_t *scr = active_screen(term);
	int x0, y0, cols, lines, y, x;

	cols  = (tw + CELL_WIDTH - 1) / CELL_WIDTH;
	lines = (th + CELL_HEIGHT - 1) / CELL_HEIGHT;

	x0 = scr->cursor.x;
	y0 = scr->cursor.y;
	if (cols + x0 > term->cols)
		cols = term->cols - x0;
	/* belt and braces next to the callers' fit clipping: never paint a
	 * cell outside the grid (term->cells[y] is sized for 'lines' rows) */
	if (lines + y0 > term->lines)
		lines = term->lines - y0;
	if (cols < 1 || lines < 1) {
		*p_lines = 0;
		*p_cols = 0;
		return;
	}

	for (y = 0; y < lines; y++) {
		for (x = 0; x < cols; x++) {
			int tx = x0 + x;
			int ty = y0 + y;
			int sx0 = (tx - x0) * CELL_WIDTH; /* image-local columns */
			int sy0 = (ty - y0) * CELL_HEIGHT; /* image-local rows */
			int wmax = tw - sx0;
			struct cell_t *cellp;
			uint8_t *dst;
			int h;

			if (wmax > CELL_WIDTH)
				wmax = CELL_WIDTH;
			if (wmax < 1)
				continue;

			erase_cell(term, ty, tx);
			cellp = &scr->cells[ty][tx];
			dst = cell_pixmap_ensure(cellp);
			if (!dst)
				continue;
			for (h = 0; h < CELL_HEIGHT; h++) {
				size_t dst_off, src_off;
				int srcy = sy0 + h;

				if (srcy >= th)
					break;
				dst_off = (size_t) h * (size_t) CELL_WIDTH * BYTES_PER_PIXEL;
				src_off = ((size_t) srcy * (size_t) tw + (size_t) sx0)
					   * (size_t) BYTES_PER_PIXEL;
				/* src_off is in bytes; walk it on a byte
				 * pointer (img is uint32_t *). */
				memcpy(dst + dst_off, (const uint8_t *) img + src_off,
				       (size_t) wmax * BYTES_PER_PIXEL);
			}
		}
	}
	*p_lines = lines;
	*p_cols = cols;
}

/*
 * Place at the cursor and park it at column 0 of the first row below the
 * image (the legacy OSC 1337 behaviour the selftests pin down).
 */
static void kitty_place(struct terminal_t *term, const uint32_t *img, int tw, int th)
{
	int lines, cols;

	kitty_paint_core(term, img, tw, th, &lines, &cols);
	if (lines < 1)
		return;
	if (active_screen(term)->cursor.y + lines < term->lines)
		move_cursor(term, lines, 0);
	else
		set_cursor(term, term->lines - 1, 0);
	cr(term);
}

static void kitty_file_render(struct terminal_t *term, const struct kitty_args *ka)
{
	uint8_t *raw = NULL;
	uint32_t *img = NULL;
	uint32_t bg;
	struct screen_t *scr;
	size_t raw_cap, raw_len = 0;
	int is_png, x0, y0, aw, ah, sw, sh, tw = 0, th = 0;

	if (!term)
		return;
	scr = active_screen(term);
	if (!scr->cells)
		return;
	if (ka->data_len > KITTY_MAX_PAYLOAD) {
		fprintf(stderr, "cruft: kitty_image: payload exceeds %u KB budget\n",
			KITTY_MAX_PAYLOAD / 1024u);
		return;
	}

	/* decoded base64 (<= 3/4 * len) or the literal payload alike */
	raw_cap = ka->data_len + (ka->data_len / 4) + 4;
	raw = (uint8_t *) malloc(raw_cap);
	if (!raw) {
		fprintf(stderr, "cruft: kitty_image: alloc failed\n");
		return;
	}
	if (ka->base64) {
		if (kitty_base64_decode(ka->data, ka->data_len, raw, raw_cap, &raw_len) < 0) {
			fprintf(stderr, "cruft: kitty_image: bad base64 payload\n");
			goto out;
		}
	} else {
		memcpy(raw, ka->data, ka->data_len);
		raw_len = ka->data_len;
	}

	is_png = (raw_len >= sizeof(png_magic) &&
		  memcmp(raw, png_magic, sizeof(png_magic)) == 0);
	bg = term->virtual_palette[term->color_pair.bg];
	x0 = scr->cursor.x * CELL_WIDTH;
	y0 = scr->cursor.y * CELL_HEIGHT;
	/* cell-grid pixel space, not the full framebuffer (see png_decode) */
	aw = term->cols * CELL_WIDTH - x0;
	ah = term->lines * CELL_HEIGHT - y0;
	if (aw < 1 || ah < 1)
		goto out; /* cursor outside the grid: nothing to anchor to */

	if (is_png) {
		/* cppcheck sees the HAVE_LIBPNG branch: the note path is
		 * dead there, live in no-libpng builds */
		// cppcheck-suppress knownConditionTrueFalse
		if (!kitty_png_available()) {
			fprintf(stderr,
				"cruft: kitty_image: PNG payload dropped (built without libpng)\n");
			goto out;
		}
		/* png_decode() is a constant -1 in no-libpng builds: same
		 * knownConditionTrueFalse as the availability note above */
		// cppcheck-suppress knownConditionTrueFalse
		if (png_decode(raw, raw_len, term, ka->transparency, bg, &tw, &th, &img) < 0)
			goto out;
		goto place;
	}

	/* raw BGR / BGRA: w/h are mandatory for raw streams */
	sw = (int) ka->width;
	sh = (int) ka->height;
	if (sw < 1 || sh < 1) {
		fprintf(stderr, "cruft: kitty_image: raw payload needs w/h\n");
		goto out;
	}
	if ((unsigned long long) sw * (unsigned long long) sh > KITTY_MAX_PIXELS) {
		fprintf(stderr, "cruft: kitty_image: image exceeds %u px budget\n",
			KITTY_MAX_PIXELS);
		goto out;
	}
	if (raw_len < (size_t) sw * sh * 3) {
		fprintf(stderr, "cruft: kitty_image: payload size does not match w/h\n");
		goto out;
	}

	kitty_fit(sw, sh, aw, ah, ka->preserve_aspect, &tw, &th);
	img = (uint32_t *) malloc((size_t) tw * (size_t) th * BYTES_PER_PIXEL);
	if (!img) {
		fprintf(stderr, "cruft: kitty_image: alloc failed\n");
		goto out;
	}
	raw_expand(raw, sw, sh,
		   (raw_len >= (size_t) sw * sh * 4) ? 4u: 3u,
		   0, ka->transparency, bg, tw, th, img);

place:
	kitty_place(term, img, tw, th);
out:
	free(raw);
	free(img);
}

/*
 * Polite Image ; operator: cruft keeps no registered images and does not
 * assemble chunked uploads, so every Image operation gets ok=false with a
 * reason. Rep grammar: OSC 1337 ; Rep ; i=<id> ; ok=false ; t=<reason> BEL.
 */
/* Length-bounded compare (members of struct kitty_args are slices of the
 * payload, not NUL-terminated). */
static int kv_eq(const char *s, size_t n, const char *lit)
{
	return n == strlen(lit) && memcmp(s, lit, n) == 0;
}

static void kitty_image_reply(struct terminal_t *term, const struct kitty_args *ka)
{
	char buf[256];
	const char *reason;
	int n, idlen;

	if (!ka->method || ka->method_len == 0)
		reason = "invalid_arguments";
	else if (kv_eq(ka->method, ka->method_len, "query")
		 || kv_eq(ka->method, ka->method_len, "delete")
		 || kv_eq(ka->method, ka->method_len, "move")
		 || kv_eq(ka->method, ka->method_len, "resize")
		 || kv_eq(ka->method, ka->method_len, "rotate")
		 || kv_eq(ka->method, ka->method_len, "crop"))
		reason = "not_found";
	else /* new / update / unrecognised: nothing to fulfil either */
		reason = "failed";

	n = snprintf(buf, sizeof(buf), "\033]1337;Rep;");
	idlen = (int) ka->id_len;
	if (idlen > KITTY_TRANSFER_ID_MAX)
		idlen = KITTY_TRANSFER_ID_MAX;
	if (idlen > 0)
		n += snprintf(buf + n, sizeof(buf) - (size_t) n, "i=%.*s;", idlen, ka->id);
	n += snprintf(buf + n, sizeof(buf) - (size_t) n, "ok=false;t=%s", reason);
	buf[n] = BEL;

	/* One small write to the pty master; the standalone TU must not
	 * pull the unity-built util.h (ewrite) into a second object. */
	if (n >= 0 && write(term->fd, buf, (size_t) n + 1) < 0)
		fprintf(stderr, "cruft: kitty_image: Rep reply write failed: %s\n",
			strerror(errno));
}

void kitty_image_dispatch(struct terminal_t *term, const char *payload)
{
	struct kitty_args ka;

	if (!term || !payload || !*payload)
		return;
	if (kitty_args_parse(payload, &ka) < 0)
		return; /* malformed: drop silently, matches DCS behaviour */
	if (strcmp(ka.action, "File") == 0)
		kitty_file_render(term, &ka);
	else
		kitty_image_reply(term, &ka);
}

/* =====================================================================
 * Kitty graphics transfer protocol (APC _G / DCS +)
 *
 * The shipped kitty graphics spec expresses every graphics command as
 *     ESC _ G <control data> ; <base64 payload> ESC \
 * (an Application Program Command with a G marker); the same grammar is
 * also accepted under the DCS-plus spelling ESC + . Control data is a
 * comma-separated list of key=value pairs; the payload is base64 pixel
 * data (or, for f=100, a base64 PNG).
 *
 * Supported surfaces (the rest is answered or dropped politely):
 *   - chunked transfer  m=1 .. m=0   (f=24/32/33/34/35 raw pixels)
 *   - single-shot       a=T (display at cursor) / no-a (same) / a=p
 *   - query action      a=q          (capability probe; always answered)
 *   - virtual placement a=T,U=1 (store for the Unicode placeholder
 *     mechanism, which is what ratatui-image uses to display images)
 *   - delete            a=d          (d=i<i= / bare)
 *   - image numbers     I=<n>        (single-shot, terminal-assigned id)
 *   - f=100 PNG         (libpng builds)
 *
 * Unsupported: zlib (o=z), file/tempfile/SHM transports (t=f/t/s),
 * animations, crop/rescale/zoom operators. Failures are answered with a
 * terse error string so probing clients degrade instead of hanging.
 *
 * Replies travel back in the same envelope: ESC _ G i=<id>;<msg> ESC \.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/* Diacritics that encode a placeholder cell's row / column / id-high:
 * index (0-296) -> code point, per the spec's rowcolumn-diacritics.txt.
 * Only the low index range is ever needed on this grid, but the whole
 * table keeps id-high values (a byte) addressable. */
static const uint32_t kg_diacc[297] = {
    0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F,
    0x0346, 0x034A, 0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357,
    0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
    0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484,
    0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
    0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1,
    0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611,
    0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658,
    0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8,
    0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2,
    0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733,
    0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743,
    0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE,
    0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817, 0x0818, 0x0819,
    0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822,
    0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C,
    0x082D, 0x0951, 0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87,
    0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17, 0x1A75, 0x1A76,
    0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D,
    0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1,
    0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4,
    0x1DC5, 0x1DC6, 0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1,
    0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5, 0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9,
    0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF, 0x1DE0, 0x1DE1,
    0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1,
    0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7,
    0x20E9, 0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2,
    0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA,
    0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2,
    0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA,
    0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D,
    0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5,
    0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9, 0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED,
    0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7,
    0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23,
    0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186, 0x1D187,
    0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243,
    0x1D244
};

static int kg_diac_index(unsigned cp)
{
	for (int i = 0; i < 297; i++)
		if (kg_diacc[i] == cp)
			return i;
	return -1;
}

/* ---------------------------- control data ----------------------- */

struct kg_par {
	long i, I, s, v, f, m, c, r, q;
	unsigned has_i, has_s, has_v, has_m, has_c, has_r, has_I;
	unsigned U;
	int a, t, o, d;
};

static void kg_parse_par(const char *p, size_t n, struct kg_par *k)
{
	size_t off = 0;

	while (off < n) {
		const char *tok;
		size_t tlen, eq, v;
		long num = 0;

		while (off < n && p[off] == ',')
			off++;
		tok  = p + off;
		tlen = 0;
		while (tlen < n - off && tok[tlen] != ',')
			tlen++;
		if (tlen == 0)
			break; /* trailing separator */
		eq  = 0;
		while (eq < tlen && tok[eq] != '=')
			eq++;
		if (eq == 0 || eq + 1 >= tlen) {
			off += tlen; /* malformed or empty token: skip */
			continue;
		}
		for (v = eq + 1; v < tlen; v++) {
			if (!isdigit((unsigned char) tok[v]))
				break;
			num = num * 10 + (tok[v] - '0');
		}
		switch (tok[0]) {
		case 'i': k->i = num; k->has_i = 1; break;
		case 'I': k->I = num; k->has_I = 1; break;
		case 's': k->s = num; k->has_s = 1; break;
		case 'v': k->v = num; k->has_v = 1; break;
		case 'f': k->f = num; break;
		case 'm': k->m = num; k->has_m = 1; break;
		case 'c': k->c = num; k->has_c = 1; break;
		case 'r': k->r = num; k->has_r = 1; break;
		case 'q': k->q = num; break;
		case 'a': k->a = tok[eq + 1]; break;
		case 'U': k->U = num; break;
		case 't': k->t = tok[eq + 1]; break;
		case 'o': k->o = tok[eq + 1]; break;
		case 'd': k->d = tok[eq + 1]; break;
		default: break; /* z, S, O, x, y, N, p, ...: not used here */
		}
		off += tlen;
	}
	if (!k->f)
		k->f = 32; /* spec default */
	if (!k->t)
		k->t = 'd';
}

/* f= -> (channels, first byte red?); 0 = PNG (decoded separately),
 * -1 = not drawable in this build. */
static int kg_format(int f, int *first_red)
{
	switch (f) {
	case 24: case 34: *first_red = 1; return 3; /* RGB  */
	case 32:          *first_red = 1; return 4; /* RGBA */
	case 33:          *first_red = 0; return 4; /* BGRA */
	case 35:          *first_red = 0; return 3; /* BGR  */
	case 100:         *first_red = 1; return 0; /* PNG  */
	default:          return -1;
	}
}

/* ------------------------------ replies -------------------------- */

static void kg_send(struct terminal_t *term, const char *idpart, const char *msg)
{
	char buf[96];
	int n;

	n = snprintf(buf, sizeof buf, "\033_G%s;%s\033\\", idpart, msg);
	if (n > 0 && write(term->fd, buf, (size_t) n) < 0)
		fprintf(stderr, "cruft: kitty_dcs: reply write failed: %s\n",
			strerror(errno));
}

static void kg_reply_id(struct terminal_t *term, unsigned long id, const char *msg)
{
	char idpart[24];

	snprintf(idpart, sizeof idpart, "i=%lu", id);
	kg_send(term, idpart, msg);
}

/* Reply honouring q=: 1 suppresses OK, 2 suppresses errors. */
static void kg_reply_quiet(struct terminal_t *term, const struct kg_par *k,
			   unsigned long id, int ok, const char *err)
{
	if (ok && k->q == 1)
		return;
	if (!ok && k->q == 2)
		return;
	kg_reply_id(term, id, ok ? "OK": err);
}

/* Same suppression rules, keyed on the header chunk's q= (kept in the
 * transfer state for chunked completions). */
static void kg_reply_plain(struct terminal_t *term, unsigned q,
			   unsigned long id, int ok, const char *err)
{
	if (ok && q == 1)
		return;
	if (!ok && q == 2)
		return;
	kg_reply_id(term, id, ok ? "OK": err);
}

/* ------------------------- shared decode buf --------------------- */

static uint8_t *kg_dbuf(size_t need)
{
	static uint8_t *buf;
	static size_t cap = 0;

	if (need > cap) {
		buf  = realloc(buf, need);
		cap = buf ? need: 0;
	}
	return buf;
}

static int kg_b64(const char *in, size_t len, uint8_t *out, size_t cap, size_t *outlen)
{
	return kitty_base64_decode(in, len, out, cap, outlen);
}

/* --------------------------- transfer state ---------------------- */

void kitty_xfer_abort(struct terminal_t *term)
{
	free(term->kgx.raw);
	term->kgx = (struct kitty_xfer) {0};
}

static int kgx_start(struct terminal_t *term, const struct kg_par *k)
{
	struct kitty_xfer *x = &term->kgx;
	int first_red, chan;
	size_t cap;

	kitty_xfer_abort(term);
	if (!k->has_s || !k->has_v || k->s < 1 || k->v < 1)
		return -1;
	if ((unsigned long long) k->s * (unsigned long long) k->v > KITTY_MAX_PIXELS)
		return -1;
	chan = kg_format(k->f, &first_red);
	if (chan < 0 || chan == 0)
		return -1;
	cap = (size_t) k->s * (size_t) k->v * (size_t) chan;
	x->raw = malloc(cap);
	if (!x->raw)
		return -1;
	x->w = (unsigned) k->s;
	x->h = (unsigned) k->v;
	x->chan = (unsigned) chan;
	x->first_red = (unsigned) first_red;
	x->fmt    = (int) k->f;
	x->id     = (uint32_t) k->i;
	x->has_id = k->has_i;
	x->U      = k->U;
	x->q      = k->q;
	x->a      = k->a;
	x->c      = (unsigned) k->c;
	x->has_c  = k->has_c;
	x->r      = (unsigned) k->r;
	x->has_r  = k->has_r;
	x->cap    = cap;
	x->len    = 0;
	x->have   = true;
	x->bad    = false;
	return 0;
}

static int kgx_append(struct terminal_t *term, const char *b64, size_t b64len)
{
	struct kitty_xfer *x = &term->kgx;
	uint8_t *db;
	size_t dl = 0;

	if (!x->have || x->bad || b64len == 0)
		return x->have && !x->bad ? 0: -1;
	db = kg_dbuf(b64len / 4 * 3 + 4);
	if (!db || kg_b64(b64, b64len, db, b64len / 4 * 3 + 4, &dl) < 0)
		goto poison;
	if (x->len + dl > x->cap)
		goto poison;
	memcpy(x->raw + x->len, db, dl);
	x->len += dl;
	return 0;
poison:
	x->bad = true;
	fprintf(stderr, "TRACE append POISON: b64len=%zu len=%zu cap=%zu\n", b64len, x->len, x->cap);
	return -1;
}

/* Native-size 0x00RRGGBB image of a finished transfer (NULL on failure). */
static uint32_t *kgx_build(struct terminal_t *term, uint32_t bg)
{
	struct kitty_xfer *x = &term->kgx;
	uint32_t *img;

	if (!x->have || x->bad || x->len < x->cap)
		return NULL;
	img = malloc(x->cap * BYTES_PER_PIXEL);
	if (!img)
		return NULL;
	raw_expand(x->raw, x->w, x->h, x->chan, x->first_red, 1, bg,
		   x->w, x->h, img);
	return img;
}

/* ----------------------------- image store ----------------------- */

static struct kitty_store *kg_store_find(struct terminal_t *term, uint32_t id)
{
	for (int s = 0; s < KITTY_STORE_SLOTS; s++)
		if (term->kgs[s].used && term->kgs[s].id == id)
			return &term->kgs[s];
	return NULL;
}

/* Steals img ownership. Slot eviction: first free, else slot 0 (oldest). */
static struct kitty_store *kg_store_put(struct terminal_t *term, uint32_t id,
					uint32_t *img, unsigned w, unsigned h)
{
	struct kitty_store *slot = NULL;

	for (int s = 0; s < KITTY_STORE_SLOTS; s++) {
		if (!term->kgs[s].used) {
			slot = &term->kgs[s];
			break;
		}
	}
	if (!slot) {
		slot = &term->kgs[0];
		free(slot->img);
	}
	slot->used = true;
	slot->id   = id;
	slot->img  = img;
	slot->w    = w;
	slot->h    = h;
	slot->vc   = 0;
	slot->vr   = 0;
	return slot;
}

/* ------------------------------- placing -------------------------- */

/*
 * Scale an image into the vc x vr cell box (aspect preserved, never
 * upscaled) and paint it with its top-left cell at (ax, ay). The cursor
 * is restored wherever the caller left it: placeholder painting must not
 * disturb the stream position.
 */
static void kg_place_at(struct terminal_t *term, const struct kitty_store *st,
			int ax, int ay, unsigned vc, unsigned vr)
{
	struct screen_t *scr = active_screen(term);
	int aw, ah, tw, th, lines, cols, cx, cy;

	if (!st->img)
		return;
	if (vc < 1 || vr < 1) {
		aw = (term->cols - ax) * CELL_WIDTH;
		ah = (term->lines - ay) * CELL_HEIGHT;
	} else {
		aw = (int) vc * CELL_WIDTH;
		ah = (int) vr * CELL_HEIGHT;
	}
	if (aw < 1 || ah < 1)
		return;
	kitty_fit(st->w, st->h, aw, ah, 1, &tw, &th);

	cx = scr->cursor.x;
	cy = scr->cursor.y;
	set_cursor(term, ay, ax);
	kitty_paint_core(term, st->img, tw, th, &lines, &cols);
	set_cursor(term, cy, cx);
}

/* a=T without a virtual placement: native size, right-edge truncated per
 * spec, cursor ends cols to the right and lines below the placement. */
static void kg_paint_direct(struct terminal_t *term, const uint32_t *img,
			    int w, int h)
{
	int lines, cols;

	kitty_paint_core(term, img, w, h, &lines, &cols);
	if (lines > 0)
		move_cursor(term, lines, cols);
}

/*
 * Finish an assembled transfer: store it (virtual placement) or paint it,
 * and answer the id if one rode along.
 */
static void kgx_complete(struct terminal_t *term)
{
	struct kitty_xfer *x = &term->kgx;
	uint32_t *img = NULL;

	if (x->have && !x->bad)
		img = kgx_build(term, term->virtual_palette[term->color_pair.bg]);

	if (!img) {
		if (x->has_id)
			kg_reply_plain(term, x->q, x->id, false,
				       "ESHORT:incomplete transfer");
		kitty_xfer_abort(term);
		return;
	}

	if (x->U && x->has_id) {
		/* virtual placement: the pixels wait for the placeholder
		 * stream that carries this id in its foreground color */
		struct kitty_store *st = kg_store_put(term, x->id, img, x->w, x->h);

		st->vc = x->has_c ? x->c: (x->w + CELL_WIDTH - 1) / CELL_WIDTH;
		st->vr = x->has_r ? x->r: (x->h + CELL_HEIGHT - 1) / CELL_HEIGHT;
	} else if (x->a == 'p' && x->has_id) {
		struct kitty_store *st = kg_store_put(term, x->id, img, x->w, x->h);

		st->vc = x->has_c ? x->c: 0;
		st->vr = x->has_r ? x->r: 0;
		kg_place_at(term, st,
			    active_screen(term)->cursor.x,
			    active_screen(term)->cursor.y, st->vc, st->vr);
	} else {
		kg_paint_direct(term, img, x->w, x->h);
		free(img);
	}

	if (x->has_id)
		kg_reply_plain(term, x->q, x->id, true, NULL);
	kitty_xfer_abort(term);
}


/*
 * Entry from dcs_sequence(): body is the control data plus ';', with the
 * base64 payload after it (both spellings share the grammar).
 */
void kitty_dcs_dispatch(struct terminal_t *term, const char *body)
{
	struct kg_par k;
	const char *semi, *payload;
	size_t plen;

	memset(&k, 0, sizeof k);
	semi  = strchr(body, ';');
	payload = semi ? semi + 1: "";
	plen  = semi ? strlen(payload): 0;
	kg_parse_par(body, semi ? (size_t)(semi - body): strlen(body), &k);

	/* Query action: attempt to load, always answer, never store. This
	 * is also the protocol probe (ratatui sends i=31,f=24,s=1,v=1,a=q).
	 * Answer immediately without touching anything else, per spec. */
	if (k.a == 'q') {
		int ok = false;

		if (k.o == 'z')
			ok = false; /* no zlib here */
		else if (kg_format(k.f, &(int) { 0 }) == -1)
			ok = false;
		else if (k.f == 100)
			ok = kitty_png_available();
		else {
			uint8_t *db;
			size_t dl = 0, want;

			want = k.has_s && k.has_v
			       ? (size_t) k.s * (size_t) k.v *
				 (size_t) (kg_format(k.f, &(int) { 0 }))
			       : 0;
			db = kg_dbuf(plen / 4 * 3 + 4);
			ok = want > 0 && db &&
			     kg_b64(payload, plen, db, plen / 4 * 3 + 4, &dl) == 0 &&
			     dl == want;
		}
		if (k.has_i)
			kg_reply_id(term, (unsigned long) k.i, ok ? "OK": "EBADMSG:unsupported query");
		return;
	}

	/* Delete */
	if (k.a == 'd') {
		if (k.d == 'i' && k.has_i) {
			struct kitty_store *st = kg_store_find(term, (uint32_t) k.i);

			if (st) {
				free(st->img);
				st->used = false;
			}
			if (term->kgx.have && term->kgx.id == (uint32_t) k.i)
				kitty_xfer_abort(term);
		} else if (!k.d) {
			for (int s = 0; s < KITTY_STORE_SLOTS; s++) {
				if (!term->kgs[s].used)
					continue;
				free(term->kgs[s].img);
				term->kgs[s].used = false;
			}
			kitty_xfer_abort(term);
		}
		if (k.has_i)
			kg_reply_quiet(term, &k, (unsigned long) k.i, true, "");
		return;
	}

	/* Chunked transfer (m= on every chunk) */
	if (k.has_m) {
		if (k.o == 'z') {
			if (k.has_i)
				kg_reply_quiet(term, &k, (unsigned long) k.i, false,
						"ENOSYS:compression unsupported");
			return;
		}
		if (!k.has_s) { /* continuation chunk */
			if (kgx_append(term, payload, plen) < 0 && k.has_i)
				kg_reply_quiet(term, &k, (unsigned long) k.i, false,
					      "EBADMSG:bad chunk");
			if (k.m == 0)
				kgx_complete(term);
			return;
		}
		if (kgx_start(term, &k) < 0) {
			if (k.has_i)
				kg_reply_quiet(term, &k, (unsigned long) k.i, false,
					      "EINVAL:bad transfer header");
			return;
		}
		if (kgx_append(term, payload, plen) < 0) {
			if (k.has_i)
				kg_reply_quiet(term, &k, (unsigned long) k.i, false,
					      "EBADMSG:bad chunk");
		}
		if (k.m == 0)
			kgx_complete(term);
		return;
	}

	/* a=p without payload: place a previously stored image */
	if (k.a == 'p' && !plen) {
		if (!k.has_i)
			return;
		{
			struct kitty_store *st = kg_store_find(term, (uint32_t) k.i);

			if (st) {
				kg_place_at(term, st,
					    active_screen(term)->cursor.x,
					    active_screen(term)->cursor.y,
					    k.has_c ? (unsigned) k.c: st->vc,
					    k.has_r ? (unsigned) k.r: st->vr);
				kg_reply_quiet(term, &k, (unsigned long) k.i, true, "");
			} else
				kg_reply_quiet(term, &k, (unsigned long) k.i, false,
					      "ENOENT:image not found");
		}
		return;
	}

	/* Single-shot transfer with payload */
	if (!plen)
		return; /* nothing to do, nothing to answer */
	if (k.o == 'z') {
		if (k.has_i)
			kg_reply_quiet(term, &k, (unsigned long) k.i, false,
					"ENOSYS:compression unsupported");
		return;
	}
	if (k.f == 100) {
		uint32_t *img = NULL;
		int tw = 0, th = 0;
		uint8_t *db;
		size_t dl = 0;
		bool ok = false;

		db = kg_dbuf(plen / 4 * 3 + 4);
		if (db && kg_b64(payload, plen, db, plen / 4 * 3 + 4, &dl) == 0)
			ok = png_decode(db, dl, term, 1,
					term->virtual_palette[term->color_pair.bg],
					&tw, &th, &img) == 0;
		if (!ok) {
			if (k.has_i)
				kg_reply_quiet(term, &k, (unsigned long) k.i, false,
					      "EBADF:png payload unreadable");
			return;
		}
		/* png_decode already fits to the region from the cursor;
		 * a virtual placement stores the fitted image */
		if (k.U && k.has_i) {
			struct kitty_store *st = kg_store_put(term, (uint32_t) k.i,
							      img, tw, th);

			st->vc = k.has_c ? (unsigned) k.c: (unsigned)((tw + CELL_WIDTH - 1) / CELL_WIDTH);
			st->vr = k.has_r ? (unsigned) k.r: (unsigned)((th + CELL_HEIGHT - 1) / CELL_HEIGHT);
		} else {
			kg_paint_direct(term, img, tw, th);
			free(img);
		}
		if (k.has_i)
			kg_reply_quiet(term, &k, (unsigned long) k.i, true, "");
		return;
	}
	/* raw pixels, single shot */
	if (kgx_start(term, &k) < 0) {
		if (k.has_i)
			kg_reply_quiet(term, &k, (unsigned long) k.i, false,
				      "EINVAL:bad transfer header");
		return;
	}
	kgx_append(term, payload, plen);
	kgx_complete(term);
}

/* ======================== Unicode placeholders =====================
 *
 * ratatui-image (and the spec's placeholder mechanism) displays an image
 * by printing U+10EEEE cells: the cell's foreground SGR is truecolor and
 * carries the image id (low 24 bits as rgb; the high byte rides on a
 * third diacritic), and diacritics after the placeholder encode the
 * cell's (row, col) within the placement rectangle. The first cell of
 * every row carries the diacritics; the rest inherit them from the cell
 * on the left (zero diacritics of their own).
 */

/* truecolor SGR values park in the reserved dynamic slots (see set_attr)
 * so the id survives unquantized. */
static uint32_t kg_ph_id(struct terminal_t *term, unsigned fg, unsigned hi)
{
	return (hi << 24) | (term->virtual_palette[fg] & 0xFFFFFF);
}

/* Move on as addch() would for a width-1 glyph, without writing the
 * (suppressed) placeholder glyph. Mirrors its fold handling. */
static void kg_ph_advance(struct terminal_t *term)
{
	struct screen_t *scr = active_screen(term);

	if (scr->wrap_occurred && scr->cursor.x == term->cols - 1) {
		set_cursor(term, scr->cursor.y, 0);
		move_cursor(term, 1, 0);
	}
	scr->wrap_occurred = false;
	if (scr->cursor.x + 1 < term->cols)
		scr->cursor.x++;
	else
		scr->wrap_occurred = true;
}

static void kg_ph_row_head(struct terminal_t *term)
{
	struct kitty_ph *ph = &term->kph;
	unsigned row, col, hi, id;

	if (!ph->pend)
		return;
	ph->pend = false;
	if (ph->fg != KITTY_DYN_FG)
		return; /* consumed region cell: no id can be carried */
	row = ph->nd > 0 ? ph->d[0]: 0;
	col = ph->nd > 1 ? ph->d[1]: 0;
	hi  = ph->nd > 2 ? ph->d[2]: 0;
	id  = kg_ph_id(term, ph->fg, hi);

	if (ph->nd >= 2 && row == 0 && col == 0) {
		/* anchor of a (re)placement: paint the whole image */
		struct kitty_store *st = kg_store_find(term, id);

		if (st) {
			kg_place_at(term, st, (int) ph->ax, (int) ph->ay,
				    st->vc, st->vr);
			ph->region.on         = true;
			ph->region.id         = id;
			ph->region.vc         = st->vc;
			ph->region.vr         = st->vr;
			ph->region.rows_done  = 0;
		}
		/* unknown id: no region; the cells still vanish one by
		 * one via the dynamic-fg rule, which is how kitty looks
		 * when asked to place an image it does not have */
		return;
	}
	/* row head of a continuing placement: trust the encoded row */
	if (ph->region.on && ph->region.id == id) {
		ph->region.rows_done = row;
		if (ph->region.rows_done >= ph->region.vr)
			ph->region.on = false;
	}
}

/* addch() front door: swallow the cells the image owns. */
bool kitty_ph_feed(struct terminal_t *term, uint32_t code)
{
	struct kitty_ph *ph = &term->kph;

	if (code == KITTY_PH_CODE) {
		if (!(ph->region.on || term->color_pair.fg == KITTY_DYN_FG))
			return false; /* bare placeholder: tofu it normally */
		ph->ax  = active_screen(term)->cursor.x;
		ph->ay  = active_screen(term)->cursor.y;
		ph->fg  = term->color_pair.fg;
		ph->nd  = 0;
		ph->pend = true;
		kg_ph_advance(term);
		return true;
	}
	if (ph->pend && 0x0300 <= code && code <= 0x036F) {
		int idx = kg_diac_index(code);

		if (idx < 0)
			return false; /* foreign combining mark */
		if (ph->nd < 3)
			ph->d[ph->nd++] = (unsigned) idx;
		if (ph->nd == 3)
			kg_ph_row_head(term);
		return true;
	}
	if (ph->pend) {
		kg_ph_row_head(term); /* group ended without full diacritics */
		return false;
	}
	return false;
}

void kitty_ph_newline(struct terminal_t *term)
{
	struct kitty_ph *ph = &term->kph;

	if (ph->region.on) {
		ph->region.rows_done++;
		if (ph->region.rows_done >= ph->region.vr)
			ph->region.on = false;
	}
}

void kitty_ph_invalidate(struct terminal_t *term)
{
	term->kph.pend     = false;
	term->kph.region.on = false;
}

void kitty_grab_free(struct terminal_t *term)
{
	kitty_xfer_abort(term);
	for (int s = 0; s < KITTY_STORE_SLOTS; s++) {
		if (!term->kgs[s].used)
			continue;
		free(term->kgs[s].img);
		term->kgs[s].used = false;
	}
	term->kph = (struct kitty_ph) {0};
}

void kitty_grab_reset(struct terminal_t *term)
{
	kitty_grab_free(term);
}
