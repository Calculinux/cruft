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
 * Expand source pixels (BGR or BGRA, sw x sh) to tw x th (nearest
 * neighbour) as 0x00RRGGBB. Semi-transparent pixels are blended over bg
 * when transparent != 0, else rendered opaque (alpha dropped) - kitty's
 * t= semantics, sans wallpaper interaction (cell pixmaps have no alpha).
 */
static void raw_expand(const uint8_t *raw, int sw, int sh, unsigned ch,
		       int transparent, uint32_t bg,
		       int tw, int th, uint32_t *out)
{
	for (int y = 0; y < th; y++) {
		int sy = (int) (((long) y * (long) sh) / (long) th);

		for (int x = 0; x < tw; x++) {
			int sx = (int) (((long) x * (long) sw) / (long) tw);
			const uint8_t *p = raw + ((size_t) sy * (size_t) sw + (size_t) sx) * ch;
			uint32_t r, g, b, a;

			b = p[0];
			g = p[1];
			r = p[2];
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

	x0 = term->cursor.x * CELL_WIDTH;
	y0 = term->cursor.y * CELL_HEIGHT;
	aw = term->width - x0;
	ah = term->height - y0;
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
 * outside the grid and the screen never scrolls; the cursor ends at column
 * 0 of the first row below the image.
 */
static void kitty_place(struct terminal_t *term, const uint32_t *img, int tw, int th)
{
	struct screen_t *scr = active_screen(term);
	int x0, y0, cols, lines, y, x;

	cols  = (tw + CELL_WIDTH - 1) / CELL_WIDTH;
	lines = (th + CELL_HEIGHT - 1) / CELL_HEIGHT;

	x0 = scr->cursor.x;
	y0 = scr->cursor.y;
	if (cols + x0 > term->cols)
		cols = term->cols - x0;
	if (cols < 1 || lines < 1)
		goto done;

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
done:
	if (y0 + lines < term->lines)
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
	aw = term->width - x0;
	ah = term->height - y0;
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
		   ka->transparency, bg, tw, th, img);

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
	if (n >= 0)
		(void) write(term->fd, buf, (size_t) n + 1);
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
