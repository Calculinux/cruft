/*
 * Host self-tests for Calculinux/cruft reduced-footprint helpers.
 * Does not open /dev/fb0.
 */
#define _XOPEN_SOURCE 600
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"
#include "util.h"
#include "fb/common.h"
#include "status.h"
/* Unity headers, mirrored from cruft.c: the W section drives the real
 * parse() so the kitty DCS path is covered end to end. */
#include "terminal.h"
#include "ctrlseq/esc.h"
#include "ctrlseq/csi.h"
#include "ctrlseq/osc.h"
#include "ctrlseq/dcs.h"
#include "parse.h"
const uint32_t color_list[COLORS];

const uint8_t attr_mask[] = {
	0x00, 0x01, 0x00, 0x00,
	0x02, 0x04, 0x00, 0x08,
};
const uint32_t bit_mask[] = {
	0x00,
	0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0xFF,
};

volatile sig_atomic_t vt_active = true;
volatile sig_atomic_t need_redraw = false;
volatile sig_atomic_t child_alive = false;
struct termios termios_orig;

const char *term_name = "cruft-256color";
const char *fb_path = "/dev/fb0";
const char *shell_cmd = "/bin/bash";
const char *sb_default_segments = "all";

/* NOTE: move_cursor / cr / set_cursor / scroll are the REAL functions
 * (terminal.h, included above) — no no-op stubs: the W section asserts
 * kitty placement cursor bookkeeping through them. */
static int failures;

/* M-section fixtures: PicoCalc-style 320x320 framebuffer, 4bpp, truecolor */
#define M_W 320
#define M_H 320

static uint32_t m_px_at(const uint8_t *fp, int x, int y)
{
	const int pos = (y * M_W + x) * 4;

	return (uint32_t) fp[pos] | ((uint32_t) fp[pos + 1] << 8) |
	       ((uint32_t) fp[pos + 2] << 16) | ((uint32_t) fp[pos + 3] << 24);
}

static void m_fill(uint8_t *p, uint32_t v, int npix)
{
	uint8_t b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };

	for (int i = 0; i < npix; i++)
		memcpy(p + i * 4, b, 4);
}

static void expect(int cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		failures++;
	} else {
		fprintf(stderr, "ok: %s\n", msg);
	}
}

/* Nav probes hand the matcher self-contained reads, so nothing may be
 * held over: a nonzero keep would mean a probe buffer now spells an
 * unfinished navigation sequence. */
static int sbnav(struct terminal_t *term, const uint8_t *buf, int n)
{
	unsigned keep = 1;
	int r = sb_consume_nav(term, buf, n, &keep);

	expect(keep == 0, "nav probe holds nothing back");
	return r;
}

static int wall_env_wants_shadow(void)
{
	const char *c = getenv("CRUFT");

	return (c && strstr(c, "wall")) ? 1 : 0;
}

static void write_u32(FILE *fp, uint32_t v)
{
	uint8_t b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };

	fwrite(b, 1, 4, fp);
}

static void write_u16(FILE *fp, uint16_t v)
{
	uint8_t b[2] = { v & 0xff, (v >> 8) & 0xff };

	fwrite(b, 1, 2, fp);
}

/* Build a tiny CRUFTFN1 with space, '?', and U+3000 at 4x8 */
static int write_tiny_font(const char *path)
{
	FILE *fp;
	uint32_t p0, p30, g_space, g_q, g_ideo, cursor, rec = 24;
	int i;

	fp = fopen(path, "wb");
	if (!fp)
		return -1;

	fwrite("CRUFTFN1", 1, 8, fp);
	write_u32(fp, 4);
	write_u32(fp, 8);
	write_u32(fp, rec);
	write_u32(fp, 8);

	cursor = 8 + 16 + 256 * 4;
	p0 = cursor;
	cursor += 256 * 4;
	p30 = cursor;
	cursor += 256 * 4;
	g_space = cursor;
	g_q = g_space + rec;
	g_ideo = g_q + rec;

	for (i = 0; i < 256; i++) {
		uint32_t off = 0;

		if (i == 0)
			off = p0;
		else if (i == 0x30)
			off = p30;
		write_u32(fp, off);
	}

	for (i = 0; i < 256; i++) {
		uint32_t off = 0;

		if (i == 0x20)
			off = g_space;
		else if (i == 0x3f)
			off = g_q;
		write_u32(fp, off);
	}

	for (i = 0; i < 256; i++)
		write_u32(fp, (i == 0) ? g_ideo : 0);

	write_u32(fp, 0x20);
	fputc(1, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 8; i++)
		write_u16(fp, 0);

	write_u32(fp, 0x3f);
	fputc(1, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 8; i++)
		write_u16(fp, (i == 0 || i == 7) ? 0x0f : 0x09);

	write_u32(fp, 0x3000);
	fputc(2, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 8; i++)
		write_u16(fp, 0);

	fclose(fp);
	return 0;
}

static void free_term_cells(struct terminal_t *term)
{
	struct screen_t *scr = active_screen(term);
	int i, j;

	if (!scr->cells)
		return;
	for (i = 0; i < term->lines; i++) {
		if (scr->cells[i]) {
			for (j = 0; j < term->cols; j++)
				cell_pixmap_clear(&scr->cells[i][j]);
			free(scr->cells[i]);
		}
	}
	free(scr->cells);
	scr->cells = NULL;
	free(scr->line_dirty);
	scr->line_dirty = NULL;
}

/* Build a tiny CRUFTFN1 with space, '?' (row0 = bits 0-4) and U+3000 at 6x12.
 * Metrics validated by glyph_mmap_init (32-byte records). */
static int write_tiny_font6x12(const char *path)
{
	FILE *fp;
	uint32_t p0, p30, g_space, g_q, g_ideo, cursor, rec = 32;
	int i;

	fp = fopen(path, "wb");
	if (!fp)
		return -1;

	fwrite("CRUFTFN1", 1, 8, fp);
	write_u32(fp, 6);
	write_u32(fp, 12);
	write_u32(fp, rec);
	write_u32(fp, 8);

	cursor = 8 + 16 + 256 * 4;
	p0 = cursor;
	cursor += 256 * 4;
	p30 = cursor;
	cursor += 256 * 4;
	g_space = cursor;
	g_q = g_space + rec;
	g_ideo = g_q + rec;

	for (i = 0; i < 256; i++) {
		uint32_t off = 0;

		if (i == 0)
			off = p0;
		else if (i == 0x30)
			off = p30;
		write_u32(fp, off);
	}

	for (i = 0; i < 256; i++) {
		uint32_t off = 0;

		if (i == 0x20)
			off = g_space;
		else if (i == 0x3f)
			off = g_q;
		write_u32(fp, off);
	}

	for (i = 0; i < 256; i++)
		write_u32(fp, (i == 0) ? g_ideo : 0);

	write_u32(fp, 0x20);
	fputc(1, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 12; i++)
		write_u16(fp, 0);

	write_u32(fp, 0x3f);
	fputc(1, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 12; i++)
		write_u16(fp, (i == 0) ? 0x1f: 0);

	write_u32(fp, 0x3000);
	fputc(2, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 12; i++)
		write_u16(fp, 0);

	fclose(fp);
	return 0;
}

/* Glyph table for the sb test font: space (blank) + solid 4x8 glyphs
 * (0xffff on every row) so pixel assertions stay trivial. Covers all
 * icons sb_have_icons() checks, the clock, and the %/[] text. */
static const uint32_t sb_cp[] = {
	0x20, '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
	':', '[', ']', '%',
	0x2191, 0x2193,
	0x2581, 0x2582, 0x2584, 0x2586,
	0x258f, 0x258e, 0x258d, 0x258c, 0x258b, 0x258a, 0x2589, 0x2588,
};
#define SB_CP_N (int)(sizeof sb_cp / sizeof sb_cp[0])

static int sb_page_used(uint32_t page)
{
	for (int k = 0; k < SB_CP_N; k++)
		if (sb_cp[k] >> 8 == page)
			return 1;
	return 0;
}

static int sb_pages_before(uint32_t page)
{
	int c = 0;

	for (uint32_t q = 0; q < page; q++)
		if (sb_page_used(q))
			c++;
	return c;
}

/* Build a tiny CRUFTFN1 4x8 status font from sb_cp[] */
static int write_tiny_sb4x8(const char *path)
{
	FILE *fp;
	const size_t hdrsz = 8 + 16 + 256u * 4u;
	const uint32_t rec = 24;
	uint32_t glyphbase, p;
	int npages = 0;

	for (p = 0; p < 256; p++)
		if (sb_page_used(p))
			npages++;
	glyphbase = (uint32_t)hdrsz + (uint32_t)npages * 256 * 4;

	fp = fopen(path, "wb");
	if (!fp)
		return -1;

	fwrite("CRUFTFN1", 1, 8, fp);
	write_u32(fp, 4);
	write_u32(fp, 8);
	write_u32(fp, rec);
	write_u32(fp, 8);

	for (p = 0; p < 256; p++)
		write_u32(fp, sb_page_used(p)
			? (uint32_t)hdrsz + (uint32_t)sb_pages_before(p) * 1024u: 0);

	for (p = 0; p < 256; p++) {
		if (!sb_page_used(p))
			continue;
		for (int i = 0; i < 256; i++) {
			uint32_t off = 0;

			for (int k = 0; k < SB_CP_N; k++)
				if (sb_cp[k] == ((p << 8) | (uint32_t)i))
					off = glyphbase + (uint32_t)k * rec;
			write_u32(fp, off);
		}
	}

	for (int k = 0; k < SB_CP_N; k++) {
		write_u32(fp, sb_cp[k]);
		fputc(1, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
		for (int r = 0; r < 8; r++)
			write_u16(fp, (k == 0) ? 0: 0xffff);
	}

	fclose(fp);
	return 0;
}

static int alloc_term_grid(struct terminal_t *term, int width, int height)
{
	struct screen_t *scr;
	int i;

	memset(term, 0, sizeof(*term));
	term->width = width;
	term->height = height;
	term->cols = width / CELL_WIDTH;
	term->lines = height / CELL_HEIGHT;
	if (term->cols < 1 || term->lines < 1)
		return -1;
	/* Mirror term_init(): primary screen is the active one. */
	term->active = 0;
	scr = &term->scr[0];
	scr->allocated  = true;
	scr->line_dirty = calloc((size_t)term->lines, sizeof(bool));
	scr->cells      = calloc((size_t)term->lines, sizeof(struct cell_t *));
	if (!scr->line_dirty || !scr->cells)
		return -1;
	for (i = 0; i < term->lines; i++) {
		scr->cells[i] = calloc((size_t)term->cols, sizeof(struct cell_t));
		if (!scr->cells[i])
			return -1;
	}
	return 0;
}

/* W-section helpers: drive the real parse() on a 10x2 cell (60x24 px @
 * 6x12) grid with a white background colour (index 7). */

static const char st_b64a[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void st_b64enc(const uint8_t *in, size_t n, char *out)
{
	size_t i = 0, o = 0;

	while (i + 2 < n) {
		uint32_t v = ((uint32_t) in[i] << 16) |
			     ((uint32_t) in[i + 1] << 8) | in[i + 2];

		out[o++] = st_b64a[(v >> 18) & 63];
		out[o++] = st_b64a[(v >> 12) & 63];
		out[o++] = st_b64a[(v >> 6) & 63];
		out[o++] = st_b64a[v & 63];
		i += 3;
	}
	if (i + 1 == n) {
		uint32_t v = (uint32_t) in[i] << 16;

		out[o++] = st_b64a[(v >> 18) & 63];
		out[o++] = st_b64a[(v >> 12) & 63];
		out[o++] = '=';
		out[o++] = '=';
	} else if (i + 2 == n) {
		uint32_t v = ((uint32_t) in[i] << 16) |
			     ((uint32_t) in[i + 1] << 8);

		out[o++] = st_b64a[(v >> 18) & 63];
		out[o++] = st_b64a[(v >> 12) & 63];
		out[o++] = st_b64a[(v >> 6) & 63];
		out[o++] = '=';
	}
	out[o] = '\0';
}

/* Cell accessor for the DCS section (active screen of term). */
static struct cell_t *w_px(struct terminal_t *term, int row, int col)
{
	return &active_screen(term)->cells[row][col];
}

/* Builds "ESC _ G <ctrl> ; <b64> ESC \\", NUL terminated. */
static uint8_t *w_apc(const char *ctrl, const char *b64)
{
	size_t cn = strlen(ctrl), bn = strlen(b64);
	uint8_t *buf = malloc(cn + bn + 8);
	size_t o = 0;

	if (!buf)
		return NULL;
	buf[o++] = 0x1B;
	buf[o++] = '_';
	buf[o++] = 'G';
	memcpy(buf + o, ctrl, cn);
	o += cn;
	buf[o++] = ';';
	memcpy(buf + o, b64, bn);
	o += bn;
	buf[o++] = 0x1B;
	buf[o++] = '\\';
	buf[o] = '\0';
	return buf;
}

/* Drain everything already flushed into the pipe into got. The writer
 * (term.fd) stays open for the session, so read nonblocking and stop at
 * EAGAIN (replies are well under PIPE_BUF, one read each). */
static long w_drain(int fd, char *got, size_t cap)
{
	long off = 0;
	int fl = fcntl(fd, F_GETFL, 0);

	memset(got, 0, cap);
	if (fl >= 0)
		(void) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	while (off < (long) cap - 1) {
		ssize_t rd = read(fd, got + off, (size_t) cap - 1 - (size_t) off);

		if (rd <= 0)
			break;
		off += rd;
	}
	return off;
}

static int u_setup(struct terminal_t *term)
{
	if (alloc_term_grid(term, 60, 24) < 0)
		return -1;
	active_screen(term)->cursor.x = active_screen(term)->cursor.y = 0;
	active_screen(term)->scroll.top = 0;
	active_screen(term)->scroll.bottom = term->lines - 1;
	term->color_pair.fg = 7;
	term->color_pair.bg = 7;
	for (int i = 0; i < COLORS; i++)
		term->virtual_palette[i] = (uint32_t) i * 0x01010101u;
	term->virtual_palette[7] = 0x00FFFFFF; /* white, for alpha maths */
	term->esc.size = ESCSEQ_SIZE;
	term->esc.buf = (char *) calloc(1, (size_t) term->esc.size);
	term->esc.bp = term->esc.buf;
	term->esc.state = STATE_RESET;
	return term->esc.buf ? 0 : -1;
}

static void u_teardown(struct terminal_t *term)
{
	/* mirrors term_die(): the kitty store/xfer outlive the cells */
	kitty_grab_free(term);
	free_term_cells(term);
	free(term->esc.buf);
	term->esc.buf = NULL;
}


int main(void)
{
	struct cell_t a, b;
	struct terminal_t term;
	char fontpath[64];
	char tmpl[] = "/tmp/cruft-st-XXXXXX";
	int fd;
	size_t canvas_bytes;
	uint8_t *lastpx;

	failures = 0;
	g_cell_w = 6;
	g_cell_h = 12;

	/* A: pointer-sized cell */
	expect(sizeof(struct cell_t) <= 64, "A sizeof(cell_t) <= 64");

	/* B: pixmap deep copy / clear */
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	expect(cell_pixmap_ensure(&a) != NULL, "B ensure pixmap");
	a.pixmap[0] = 0xAB;
	expect(cell_pixmap_dup(&b, &a) == 0, "B dup ok");
	expect(b.has_pixmap && b.pixmap && b.pixmap != a.pixmap, "B deep copy distinct");
	expect(b.pixmap[0] == 0xAB, "B deep copy content");
	cell_pixmap_clear(&a);
	expect(!a.has_pixmap && a.pixmap == NULL, "B clear");
	expect(b.pixmap[0] == 0xAB, "B src clear does not free dst");
	cell_pixmap_clear(&b);

	/* C: sixel ensure/free */
	memset(&term, 0, sizeof(term));
	term.width = 32;
	term.height = 24;
	expect(sixel_canvas_ensure(&term) == 0 && term.sixel.pixmap != NULL, "C sixel ensure");
	sixel_canvas_free(&term);
	expect(term.sixel.pixmap == NULL, "C sixel free");

	/* D: drcs ensure/clear */
	expect(drcs_ensure(&term, 0) != NULL, "D drcs ensure");
	expect(term.drcs[0] != NULL, "D drcs slot set");
	drcs_clear_charset(&term, 0);
	expect(term.drcs[0] == NULL, "D drcs clear charset");
	drcs_ensure(&term, 1);
	drcs_ensure(&term, 2);
	drcs_clear_all(&term);
	expect(term.drcs[1] == NULL && term.drcs[2] == NULL, "D drcs clear all");

	/* E: CRUFTFN1 mmap */
	fd = mkstemp(tmpl);
	expect(fd >= 0, "E mkstemp");
	if (fd >= 0) {
		close(fd);
		snprintf(fontpath, sizeof(fontpath), "%s.cruftfont", tmpl);
		unlink(tmpl);
		expect(write_tiny_font(fontpath) == 0, "E write tiny font");
		expect(glyph_mmap_init(fontpath) == 1, "E glyph_mmap_init");
		expect(g_cell_w == 4 && g_cell_h == 8, "E cell metrics 4x8");
		expect(glyph_lookup(0x20) != NULL, "E lookup space");
		expect(glyph_lookup(0x3f) != NULL, "E lookup question");
		expect(glyph_lookup(0x3000) != NULL, "E lookup U+3000");
		expect(glyph_lookup(0x41) == NULL, "E missing glyph");
		glyph_mmap_die();
		unlink(fontpath);
	}

	/* F: wallpaper env helper */
	unsetenv("CRUFT");
	expect(wall_env_wants_shadow() == 0, "F no wall env");
	setenv("CRUFT", "wall", 1);
	expect(wall_env_wants_shadow() == 1, "F CRUFT=wall");
	unsetenv("CRUFT");

	/* G: term_release_transient clears sixel canvas only */
	g_cell_w = 6;
	g_cell_h = 12;
	struct screen_t *gscr;
	expect(alloc_term_grid(&term, 36, 24) == 0, "G alloc grid");
	gscr = active_screen(&term);
	sixel_canvas_ensure(&term);
	expect(cell_pixmap_ensure(&gscr->cells[0][0]) != NULL, "G cell pixmap");
	gscr->cells[0][0].pixmap[0] = 0x5A;
	term_release_transient(&term);
	expect(term.sixel.pixmap == NULL, "G release frees canvas");
	expect(gscr->cells[0][0].pixmap && gscr->cells[0][0].pixmap[0] == 0x5A,
		"G release keeps cell pixmap");
	free_term_cells(&term);

	/* H: PicoCalc 320-wide / 6px cell edge clamp */
	g_cell_w = 6;
	g_cell_h = 12;
	expect(sixel_cell_row_bytes(0, 320) == 6 * 4, "H full first cell");
	expect(sixel_cell_row_bytes(52, 320) == 6 * 4, "H full cell 52");
	expect(sixel_cell_row_bytes(53, 320) == 2 * 4, "H partial last cell (2px)");
	expect(sixel_cell_row_bytes(54, 320) == 0, "H past right edge");
	expect(sixel_cell_row_bytes(-1, 320) == 0, "H negative cell");

	/* I: DECDLD glyph index clamp */
	expect(drcs_clamp_char_index(0) == 0, "I clamp 0");
	expect(drcs_clamp_char_index(95) == 95, "I clamp 95");
	expect(drcs_clamp_char_index(96) == -1, "I reject 96");
	expect(drcs_clamp_char_index(-1) == -1, "I reject -1");
	expect(drcs_clamp_char_index(255) == -1, "I reject 255");

	/* J: copy_cell deep-copies pixmap (ICH/DCH ownership) */
	g_cell_w = 6;
	g_cell_h = 12;
	struct screen_t *jscr;
	expect(alloc_term_grid(&term, 18, 12) == 0, "J alloc");
	jscr = active_screen(&term);
	expect(cell_pixmap_ensure(&jscr->cells[0][0]) != NULL, "J ensure src");
	jscr->cells[0][0].pixmap[0] = 0xC1;
	jscr->cells[0][0].width = HALF;
	jscr->cells[0][0].glyphp = glyph_get(DEFAULT_CHAR);
	copy_cell(&term, 0, 1, 0, 0);
	expect(jscr->cells[0][1].has_pixmap && jscr->cells[0][1].pixmap, "J dst has pixmap");
	expect(jscr->cells[0][1].pixmap != jscr->cells[0][0].pixmap, "J deep copy");
	expect(jscr->cells[0][1].pixmap[0] == 0xC1, "J content");
	cell_pixmap_clear(&jscr->cells[0][0]);
	expect(jscr->cells[0][1].pixmap[0] == 0xC1, "J src clear leaves dst");
	free_term_cells(&term);

	/* K: sixel_copy2cell must not read past canvas when cols is ceil(width/cw)
	 * (PicoCalc-style 320%6!=0). Force cols=54 while width=320. */
	g_cell_w = 6;
	g_cell_h = 12;
	struct screen_t *kscr;
	memset(&term, 0, sizeof(term));
	term.width = 320;
	term.height = 24;
	term.cols = 54; /* ceil(320/6); wider than width/CELL_WIDTH */
	term.lines = 2;
	kscr = active_screen(&term);
	kscr->line_dirty = calloc(2, sizeof(bool));
	kscr->cells = calloc(2, sizeof(struct cell_t *));
	expect(kscr->line_dirty && kscr->cells, "K meta");
	kscr->cells[0] = calloc(54, sizeof(struct cell_t));
	kscr->cells[1] = calloc(54, sizeof(struct cell_t));
	expect(kscr->cells[0] && kscr->cells[1], "K rows");
	expect(sixel_canvas_ensure(&term) == 0, "K canvas");
	canvas_bytes = (size_t)term.width * (size_t)term.height * (size_t)BYTES_PER_PIXEL;
	memset(term.sixel.pixmap, 0xA5, canvas_bytes);
	term.sixel.width = 320;
	term.sixel.height = 12;
	term.sixel.line_length = BYTES_PER_PIXEL * term.width;
	kscr->cursor.x = 0;
	kscr->cursor.y = 0;
	sixel_copy2cell(&term, &term.sixel); /* ASAN fails on OOB without row clamp */
	expect(kscr->cells[0][53].has_pixmap && kscr->cells[0][53].pixmap,
		"K partial right cell filled");
	lastpx = kscr->cells[0][53].pixmap;
	expect(lastpx[0] == 0xA5 && lastpx[4] == 0xA5, "K partial cell got 2px");
	expect(lastpx[2 * 4] == 0, "K bytes past clipped width stay clear");
	sixel_canvas_free(&term);
	free_term_cells(&term);

	/* L: main-loop VT release pattern (flag → free, not in handler) */
	g_cell_w = 6;
	g_cell_h = 12;
	memset(&term, 0, sizeof(term));
	term.width = 16;
	term.height = 16;
	vt_active = false;
	sixel_canvas_ensure(&term);
	if (!vt_active) /* same predicate as cruft.c main loop */
		term_release_transient(&term);
	expect(term.sixel.pixmap == NULL, "L deferred release");
	vt_active = true;

	/* M: centered grid, margin painting, sixel alignment (PicoCalc 320x320).
	 * 6x12 font: cols=53 (grid 318px -> 1px left + 1px right margin),
	 * lines=26 (grid 312px -> 8px bottom margin, rows top-anchored).
	 * '?' glyph row0 = 0x1f (bits 0-4); bit 0 is the rightmost pixel. */
	g_cell_w = 6;
	g_cell_h = 12;
	{
		char tmfm[] = "/tmp/cruft-st-M-XXXXXX";
		char mpfont[64];
		int fdf = mkstemp(tmfm);

		expect(fdf >= 0, "M mkstemp");
		if (fdf >= 0) {
			struct framebuffer_t fbm;
			struct screen_t *mscr;
			uint8_t *mfp;

			close(fdf);
			snprintf(mpfont, sizeof(mpfont), "%s.cruftfont", tmfm);
			expect(write_tiny_font6x12(mpfont) == 0, "M write 6x12 font");
			expect(glyph_mmap_init(mpfont) == 1, "M glyph_mmap_init 6x12");
			unlink(mpfont);

			memset(&term, 0, sizeof(term));
			expect(alloc_term_grid(&term, M_W, M_H) == 0, "M alloc 53x26");
			expect(term.cols == 53 && term.lines == 26, "M PicoCalc grid 53x26");
			mscr = active_screen(&term);

			/* populate cells: row 0 bg=1 (distinct from margin color 0),
			 * other rows bg=0; '?' marker at (0,5) */
			for (int y = 0; y < term.lines; y++) {
				for (int x = 0; x < term.cols; x++) {
					struct cell_t *c = &mscr->cells[y][x];

					c->glyphp = (y == 0 && x == 5) ? glyph_get(0x3f): glyph_get(0x20);
					c->color_pair.fg = 7;
					c->color_pair.bg = (y == 0) ? 1: 0;
					c->width = HALF;
				}
				mscr->line_dirty[y] = true;
			}
			term.mode = MODE_AMRIGHT; /* no cursor: keep assertions plain */
			term.palette_modified = false;
			mscr->cursor.x = mscr->cursor.y = 0;

			memset(&fbm, 0, sizeof(fbm));
			mfp = (uint8_t *) ecalloc(1, (size_t) M_W * M_H * 4);
			expect(mfp != NULL, "M fp alloc");
			if (mfp) {
				m_fill(mfp, 0xEFEFEFEFu, M_W * M_H); /* sentinel: any unpainted pixel fails */
				fbm.fp = mfp;
				fbm.buf = NULL;
				fbm.wall = NULL;
				fbm.info.width = M_W;
				fbm.info.height = M_H;
				fbm.info.line_length = M_W * 4;
				fbm.info.bytes_per_pixel = 4;
				fbm.info.bits_per_pixel = 32;
				fbm.info.type = YAFT_FB_TYPE_PACKED_PIXELS;
				fbm.info.visual = YAFT_FB_VISUAL_TRUECOLOR;
				fbm.info.red.length = 8;   fbm.info.red.offset = 16;
				fbm.info.green.length = 8; fbm.info.green.offset = 8;
				fbm.info.blue.length = 8;  fbm.info.blue.offset = 0;
				for (int i = 0; i < COLORS; i++)
					fbm.real_palette[i] = (uint32_t) i * 0x01010101u;

				/* M1: full-frame check after one refresh()
				 * margin / bottom strip:  palette[0] = 0
				 * row 0 inside grid:      palette[1]
				 * row 0, '?' bits 0-4:    palette[7] at x=36..32, y=0 */
				refresh(&fbm, &term);
				{
					int mism = 0, first_bad = -1;
					for (int y = 0; y < M_H; y++) {
						for (int x = 0; x < M_W; x++) {
							uint32_t exp = 0;

							if (y < term.lines * CELL_HEIGHT && x >= 1 && x < 319) {
								exp = (y < CELL_HEIGHT) ? 0x01010101u: 0;
								if (y == 0 && x >= 32 && x <= 36)
									exp = 0x07070707u;
							}
							if (m_px_at(mfp, x, y) != exp) {
								mism++;
								if (first_bad < 0)
									first_bad = y * M_W + x;
							}
						}
					}
					expect(mism == 0, "M1 full-frame centered + margins painted");
					if (mism)
						fprintf(stderr, "  first mismatch at y=%d x=%d (count=%d)\n",
							first_bad / M_W, first_bad % M_W, mism);
				}

				/* M2: sixel blit shares the text grid origin (ox=1) */
				m_fill(mfp, 0xCDCDCDCDu, M_W * M_H);
				{
					uint8_t *pix = (uint8_t *) ecalloc(1,
						(size_t) CELL_WIDTH * CELL_HEIGHT * BYTES_PER_PIXEL);
					uint32_t want = color2pixel(&fbm.info, 0xA5A5A5A5u);

					expect(pix != NULL, "M2 pix alloc");
					if (pix) {
						m_fill(pix, want, CELL_WIDTH * CELL_HEIGHT);
						draw_sixel(&fbm, &term, 3, 2, pix);
						expect(m_px_at(mfp, 1 + 2 * 6, 3 * 12) == want,
							"M2 sixel first px at grid origin");
						expect(m_px_at(mfp, 1 + 2 * 6 + 5, 3 * 12 + 11) == want,
							"M2 sixel last px");
						expect(m_px_at(mfp, 1 + 2 * 6 - 1, 3 * 12) == 0xCDCDCDCDu,
							"M2 left neighbor untouched");
						expect(m_px_at(mfp, 1 + 2 * 6 + 6, 3 * 12) == 0xCDCDCDCDu,
							"M2 right neighbor untouched");
					}
					free(pix);
				}

				/* M3: right-edge sixel cell clips inside the grid (right margin spared) */
				m_fill(mfp, 0xBB00BB00u, M_W * M_H);
				{
					uint8_t *pix = (uint8_t *) ecalloc(1,
						(size_t) CELL_WIDTH * CELL_HEIGHT * BYTES_PER_PIXEL);
					uint32_t want = color2pixel(&fbm.info, 0xA5A5A5A5u);

					expect(pix != NULL, "M3 pix alloc");
					if (pix) {
						int edge_col = term.cols - 1;
						int edge_x = 1 + edge_col * 6; /* = 313 */

						m_fill(pix, want, CELL_WIDTH * CELL_HEIGHT);
						draw_sixel(&fbm, &term, 0, edge_col, pix);
						expect(m_px_at(mfp, edge_x, 0) == want, "M3 edge cell first px");
						expect(m_px_at(mfp, 318, 11) == want, "M3 edge cell last px (x=318)");
						for (int y = 0; y < CELL_HEIGHT; y++)
							expect(m_px_at(mfp, 319, y) == 0xBB00BB00u, "M3 right margin untouched");
					}
					free(pix);
				}
			}
			free_term_cells(&term);
			free(mfp);
			glyph_mmap_die();
		}
	}

	/* N: status bar font (4x8) loads alongside the main font */
	{
		char tmnf[] = "/tmp/cruft-st-N-XXXXXX";
		char nfont[80], nbad[80];
		int fnf = mkstemp(tmnf);

		expect(fnf >= 0, "N mkstemp");
		if (fnf >= 0) {
			close(fnf);
			snprintf(nfont, sizeof nfont, "%s.sb", tmnf);
			snprintf(nbad, sizeof nbad, "%s.main6x12", tmnf);
			expect(write_tiny_sb4x8(nfont) == 0, "N write sb font");
			expect(write_tiny_font6x12(nbad) == 0, "N write 6x12 font");
			expect(glyph_mmap_init(nbad) == 1, "N main font 6x12");
			expect(glyph_sb_init(nfont) == 1, "N sb font 4x8");
			expect(g_cell_w == 6 && g_cell_h == 12, "N main metrics intact");
			expect(g_sb_cell_w == 4 && g_sb_cell_h == 8, "N sb metrics");
			expect(glyph_lookup(0x20) != NULL, "N main lookup survives sb init");
			expect(glyph_sb_lookup(0x20) != NULL, "N sb space");
			expect(glyph_sb_lookup(0x2588) != NULL, "N sb icon glyph");
			expect(glyph_sb_lookup('A') == NULL, "N sb missing glyph NULL");
			glyph_sb_die();
			expect(g_sb_cell_w == 0 && g_sb_cell_h == 0, "N sb die resets");
			expect(glyph_sb_init("/nonexistent/cruft-sb-selftest.font") == 0,
				"N missing font -> off");
			expect(g_sb_cell_w == 0, "N still off after miss");
			expect(glyph_sb_init(nbad) == 0, "N 6x12 rejected for sb");
			expect(g_sb_cell_w == 0, "N clean after bad metrics");
			expect(glyph_sb_init(nfont) == 1, "N re-init after failure");
			glyph_sb_die();
			glyph_mmap_die();
			unlink(nfont);
			unlink(nbad);
		}
	}

	/* O: status bar pure logic (parser, time, rssi levels, item builder) */
	{
		int unk = 0;
		struct sb_state st;
		struct sb_draw d;
		char t[16], ex[16];
		time_t tt;
		struct tm tmx;

		/* segment parser */
		expect(sb_parse_segments(NULL, &unk) == 0, "O parse NULL");
		expect(sb_parse_segments("", &unk) == 0, "O parse empty");
		expect(sb_parse_segments("0", &unk) == 0, "O parse 0");
		expect(sb_parse_segments("none", &unk) == 0, "O parse none");
		{
			uint8_t all = sb_parse_segments("all", &unk);

			expect(all != 0, "O parse all");
			expect((all & sb_parse_segments("time", &unk)) != 0,
				"O all has time");
			expect(sb_parse_segments("wifi,battery", &unk) ==
				(sb_parse_segments("wifi", &unk) |
				 sb_parse_segments("battery", &unk)),
				"O parse combines");
			expect(sb_parse_segments("TIME, BATTERY", &unk) ==
				(sb_parse_segments("time", &unk) |
				 sb_parse_segments("battery", &unk)),
				"O case/whitespace tolerance");
			{
				uint8_t gw = sb_parse_segments("wifi", &unk);
				uint8_t wg = sb_parse_segments("wifi,garbage",
					&unk);

				expect(wg == gw && unk,
					"O unknown flagged + ignored");
			}
		}

		/* time format at a fixed instant (14:05 local) */
		memset(&tmx, 0, sizeof tmx);
		tmx.tm_year = 100; tmx.tm_mon = 0; tmx.tm_mday = 1;
		tmx.tm_hour = 14; tmx.tm_min = 5; tmx.tm_sec = 0;
		tt = mktime(&tmx);
		sb_fmt_time(tt, 0, t);
		{
			struct tm lm;

			localtime_r(&tt, &lm);
			snprintf(ex, sizeof ex, "%02d:%02d", lm.tm_hour, lm.tm_min);
			expect(strcmp(t, ex) == 0, "O 24h format");
		}
		sb_fmt_time(tt, 1, t);
		{
			struct tm lm;

			localtime_r(&tt, &lm);
			{
				int hh = lm.tm_hour % 12;

				if (!hh)
					hh = 12;
				snprintf(ex, sizeof ex, "%d:%02d%c%c", hh, lm.tm_min,
					lm.tm_hour < 12 ? 'a': 'p', 'm');
			}
			expect(strcmp(t, ex) == 0, "O 12h format");
		}
		{
			/* noon and midnight corners */
			struct tm tmid = tmx, tnoon = tmx;

			tmid.tm_hour = 0; tnoon.tm_hour = 12;
			tmid.tm_min = tnoon.tm_min = 0;
			sb_fmt_time(mktime(&tmid), 1, t);
			expect(strcmp(t, "12:00am") == 0, "O midnight 12:00am");
			sb_fmt_time(mktime(&tnoon), 1, t);
			expect(strcmp(t, "12:00pm") == 0, "O noon 12:00pm");
		}

		/* rssi -> stair level */
		expect(sb_rf_level(-50) == 4, "O rssi -50 -> 4");
		expect(sb_rf_level(-55) == 4, "O rssi -55 -> 4");
		expect(sb_rf_level(-60) == 3, "O rssi -60 -> 3");
		expect(sb_rf_level(-70) == 2, "O rssi -70 -> 2");
		expect(sb_rf_level(-85) == 1, "O rssi -85 -> 1");
		expect(sb_rf_level(-999) == 0, "O rssi n/a -> 0");

		/* /proc/net/wireless Level parsing */
		expect(sb_wireless_level(
			"wlan0: 0000   49.  -61.  -256        0  0  0  0 210 0",
			"wlan0") == -61,
			"O wireless level -61 dBm");
		/* kernel field-format: name right-justified to six chars */
		expect(sb_wireless_level(
			" wlan0: 0000   49.  -61.  -256        0  0  0  0 210 0",
			"wlan0") == -61,
			"O wireless padded row (kernel fmt)");
		expect(sb_wireless_level("wlan1: 0000   49.  -61.  -256",
			"wlan0") == -999,
			"O wireless wrong iface");
		expect(sb_wireless_level("wlan0: 0000   0.   0.   -256",
			"wlan0") == -999,
			"O wireless zero level invalid");
		expect(sb_wireless_level("wlan0: 0000   48.  -62.",
			"wlan0") == -62,
			"O wireless truncated line ok");

		/* battery color thresholds (palette: 9 red, 10 green, 11 yellow) */
		expect(sb_batt_color(100) == 10, "O batt 100 green");
		expect(sb_batt_color(50) == 10, "O batt 50 green");
		expect(sb_batt_color(49) == 11, "O batt 49 yellow");
		expect(sb_batt_color(20) == 11, "O batt 20 yellow");
		expect(sb_batt_color(19) == 9, "O batt 19 red");
		expect(sb_batt_color(0) == 9, "O batt 0 red");

		/* builder: everything present */
		memset(&st, 0, sizeof st);
		st.now = tt; st.wifi = 3; st.battery = 85;
		st.charging = 1;
		sb_build_items(&st, sb_parse_segments("all", &unk), 0, 0, &d);
		expect(d.center.n == 5, "O center 5 glyphs (HH:MM)");
		expect(d.center.it[0].col == 7, "O clock light grey");
		expect(d.left.n == 0, "O left zone unused");
		expect(d.right.n == 16,
			"O right 16 items (stair 4, break 2, battery 10)");
		expect(d.right.it[0].cp == 0x2581 && d.right.it[3].cp == 0x2586,
			"O stair glyphs low..high");
		expect(d.right.it[0].col == 11 && d.right.it[1].col == 11 &&
			d.right.it[2].col == 11 && d.right.it[3].col == 8,
			"O wifi lvl3: 3 lit yellow, 4th dark");
		expect(d.right.it[4].cp == 0x20 && d.right.it[5].cp == 0x20,
			"O 2-space break");
		expect(d.right.it[6].cp == '[' && d.right.it[11].cp == ']',
			"O battery brackets");
		expect(d.right.it[7].cp == 0x2588 && d.right.it[8].cp == 0x2588 &&
			d.right.it[9].cp == 0x2588 && d.right.it[10].cp == 0x258d,
			"O battery 85% cells (full,full,full,3/8)");
		expect(d.right.it[12].cp == '8' && d.right.it[13].cp == '5' &&
			d.right.it[14].cp == '%',
			"O battery 85%");
		expect(d.right.it[15].cp == 0x2191 && d.right.it[15].col == 11,
			"O charging arrow yellow up");
		st.charging = 0;
		sb_build_items(&st, sb_parse_segments("all", &unk), 0, 0, &d);
		expect(d.right.it[15].cp == 0x2193 && d.right.it[15].col == 8,
			"O discharge arrow grey down");
		st.charging = 1; /* restore for the ascii checks below */

		/* builder: 12h + ascii fallback + segment subsets */
		sb_build_items(&st, sb_parse_segments("time", &unk), 1, 1, &d);
		expect(d.center.n == 6 && d.center.it[4].cp == 'p' &&
			d.center.it[5].cp == 'm',
			"O 12h (14:05 -> 2:05pm)");
		expect(d.left.n == 0 && d.right.n == 0,
			"O time-only: other zones empty");
		sb_build_items(&st, sb_parse_segments("wifi,battery", &unk), 0, 1, &d);
		expect(d.left.n == 0, "O ascii: left zone unused");
		expect(d.right.n == 18,
			"O ascii right 'WIFI 3/4  BAT 85%+'");
		expect(d.right.it[7].cp == '4', "O ascii wifi '3/4'");
		expect(d.right.it[8].cp == 0x20 && d.right.it[9].cp == 0x20,
			"O ascii 2-space break");
		expect(d.right.it[17].cp == '+', "O ascii charge '+'");

		/* absent sources hide their segments (clock remains) */
		memset(&st, 0, sizeof st);
		st.now = tt; st.wifi = -1; st.battery = -1;
		sb_build_items(&st, sb_parse_segments("all", &unk), 0, 0, &d);
		expect(d.center.n == 5 && d.left.n == 0 && d.right.n == 0,
			"O absent sources hidden");
	}

	/* P: status bar window title (OSC 0/2 capture + left-zone fit) */
	{
		int unk = 0;
		struct sb_state st;
		struct sb_draw d;
		char title[SB_TITLE_MAX + 1];

		/* segment keyword */
		expect(sb_parse_segments("title", &unk) != 0, "P parse title");
		expect((sb_parse_segments("all", &unk) &
			sb_parse_segments("title", &unk)) != 0,
			"P all has title");
		expect(sb_parse_segments("title,time", &unk) ==
			(sb_parse_segments("title", &unk) |
			 sb_parse_segments("time", &unk)),
			"P parse title combines");

		/* title capture: printable ASCII kept, everything else dropped */
		memset(title, 0, sizeof title);
		expect(sb_title_set(title, "pico@calc: ~/src$ vim main.c") == 1,
			"P set returns changed");
		expect(strcmp(title, "pico@calc: ~/src$ vim main.c") == 0,
			"P ascii kept");
		expect(sb_title_set(title, "pico@calc: ~/src$ vim main.c") == 0,
			"P same content not changed");
		expect(sb_title_set(title, "abc") == 1 && strcmp(title, "abc") == 0,
			"P shorter replaces longer");
		expect(sb_title_set(title, "") == 1 && title[0] == '\0',
			"P empty payload clears");

		expect(sb_title_set(title, "a\tb\nc" "\x01" "d\177e") == 1,
			"P controls");
		expect(strcmp(title, "abcde") == 0, "P controls and DEL dropped");

		expect(sb_title_set(title, "caf" "\xc3\xa9") == 1, "P accent in");
		expect(strcmp(title, "caf") == 0, "P non-ASCII char dropped");
		expect(sb_title_set(title, "a" "\xe6\xbc\xa2" "b" "\xed\xa0\x80" "c")
			== 1, "P mixed in");
		expect(strcmp(title, "abc") == 0, "P CJK + surrogate dropped");

		/* malformed sequences shed bytes, never emit them */
		expect(sb_title_set(title, "xy" "\xc3") == 1, "P dangling lead in");
		expect(strcmp(title, "xy") == 0, "P dangling lead dropped");
		expect(sb_title_set(title, "p" "\x80" "q") == 1, "P lone cont in");
		expect(strcmp(title, "pq") == 0, "P lone continuation dropped");
		expect(sb_title_set(title, "a" "\xc0\xaf" "b") == 1, "P overlong in");
		expect(strcmp(title, "ab") == 0, "P overlong lead dropped");
		expect(sb_title_set(title, "z" "\xf8\x88\x80\x80\x80" "w") == 1,
			"P 5-byte in");
		expect(strcmp(title, "zw") == 0, "P 5-byte sequence dropped");

		/* truncation at the buffer cap, NUL-terminated */
		{
			char long_in[SB_TITLE_MAX + 4];

			memset(long_in, 'x', sizeof long_in);
			long_in[sizeof long_in - 1] = '\0';
			expect(sb_title_set(title, long_in) == 1, "P oversize in");
			expect(strlen(title) == SB_TITLE_MAX, "P truncated at cap");
		}

		/* left-zone fit: one blank cell short of the centered run */
		expect(sb_fit_left(10, 5, 80) == 10, "P fit: short left wins");
		expect(sb_fit_left(50, 5, 80) == 36,
			"P fit: clamped (center 37..41, gap 36)");
		expect(sb_fit_left(10, 5, 10) == 1, "P fit: tight strip");
		expect(sb_fit_left(10, 9, 10) == 0, "P fit: no room");
		expect(sb_fit_left(10, 10, 10) == 0, "P fit: center eats strip");
		expect(sb_fit_left(10, 12, 10) == 0,
			"P fit: center wider than strip");
		expect(sb_fit_left(0, 5, 80) == 0, "P fit: empty left");
		expect(sb_fit_left(5, 0, 4) == 1, "P fit: gap respected");

		/* builder: title goes to the left zone */
		memset(&st, 0, sizeof st);
		st.now = 0;
		st.title = "Hello World";
		sb_build_items(&st, sb_parse_segments("title", &unk), 0, 0, &d);
		expect(d.left.n == 11 && d.left.it[0].cp == 'H' &&
			d.left.it[10].cp == 'd',
			"P left zone holds title");
		expect(d.left.it[0].col == 7, "P title light grey");
		expect(d.center.n == 0 && d.right.n == 0, "P title only");
		sb_build_items(&st, sb_parse_segments("time", &unk), 0, 0, &d);
		expect(d.left.n == 0, "P title hidden when segment off");
		st.title = "";
		sb_build_items(&st, sb_parse_segments("all", &unk), 0, 0, &d);
		expect(d.left.n == 0, "P empty title hidden");
		st.title = NULL;
		sb_build_items(&st, sb_parse_segments("all", &unk), 0, 0, &d);
		expect(d.left.n == 0, "P null title hidden");
	}

	/* Q: status bar + grid shift on the PicoCalc 320x320 (bar on).
	 * 6x12 main: oy=8, lines=(320-8)/12=26; grid rows 8..319, no bottom
	 * strip (8 + 26*12 == 320). Strip: clock center (5 solid glyphs),
	 * right zone = battery only (10 solid glyphs, no leading break),
	 * left unused (no wifi fixtures). */
	{
		char tmqf[] = "/tmp/cruft-st-Q-XXXXXX";
		char qsb[80], qm[80], qbase[96];
		char qnet[120], qpsd[120], qpd[120];
		int fqf = mkstemp(tmqf);

		expect(fqf >= 0, "Q mkstemp");
		if (fqf >= 0) {
			struct framebuffer_t fbq;
			struct screen_t *qscr;
			uint8_t *qfp;
			const char *roots_saved[2];

			close(fqf);
			snprintf(qsb, sizeof qsb, "%s.sb", tmqf);
			snprintf(qm, sizeof qm, "%s.main6x12", tmqf);
			snprintf(qbase, sizeof qbase, "%s.sys", tmqf);
			expect(write_tiny_sb4x8(qsb) == 0, "Q write sb font");
			expect(write_tiny_font6x12(qm) == 0, "Q write 6x12 font");

			/* sysfs fixtures: no wifi (empty net dir), battery 85%
			 * charging */
			{
				const char *dirs[] = { "", "/net", "/ps",
							"/ps/picocalc" };
				const char *fixtures[][2] = {
					{ "/ps/picocalc/capacity", "85\n" },
					{ "/ps/picocalc/type", "Battery\n" },
					{ "/ps/picocalc/status", "Charging\n" },
				};

				for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
					snprintf(qpd, sizeof qpd, "%s%s", qbase, dirs[i]);
					mkdir(qpd, 0755);
				}
				for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0];
				     i++) {
					FILE *fx;

					snprintf(qpd, sizeof qpd, "%s%s", qbase,
						fixtures[i][0]);
					fx = fopen(qpd, "w");
					expect(fx != NULL, "Q fixture fopen");
					if (fx) { /* expect() reports but does not stop */
						fprintf(fx, "%s", fixtures[i][1]);
						fclose(fx);
					}
				}
			}

			roots_saved[0] = sb_net_root;
			roots_saved[1] = sb_ps_root;
			snprintf(qnet, sizeof qnet, "%s/net", qbase);
			sb_net_root = qnet;
			snprintf(qpsd, sizeof qpsd, "%s/ps", qbase);
			sb_ps_root = qpsd;
			statusbar_reset_discovery();

			setenv("CRUFT_SB_FONT", qsb, 1);
			unsetenv("CRUFT_SB"); /* default: all segments */
			statusbar_init();
			expect(statusbar_h() == 8, "Q strip height 8");
			expect(glyph_mmap_init(qm) == 1, "Q main font 6x12");

			memset(&term, 0, sizeof(term));
			expect(alloc_term_grid(&term, M_W, M_H) == 0, "Q alloc grid");
			term.oy = statusbar_h();
			term.lines = (M_H - term.oy) / CELL_HEIGHT; /* as term_init() */
			expect(term.cols == 53 && term.lines == 26,
				"Q grid 53x26 below the bar");
			qscr = active_screen(&term);

			for (int y = 0; y < term.lines; y++) {
				for (int x = 0; x < term.cols; x++) {
					struct cell_t *c = &qscr->cells[y][x];

					c->glyphp = glyph_get(0x20);
					c->color_pair.fg = 7;
					c->color_pair.bg = 1;
					c->width = HALF;
				}
				qscr->line_dirty[y] = true;
			}
			term.mode = MODE_AMRIGHT;
			term.palette_modified = false;
			qscr->cursor.x = qscr->cursor.y = 0;

			memset(&fbq, 0, sizeof(fbq));
			qfp = (uint8_t *) ecalloc(1, (size_t) M_W * M_H * 4);
			expect(qfp != NULL, "Q fp alloc");
			if (qfp) {
				m_fill(qfp, 0xEFEFEFEFu, M_W * M_H);
				fbq.fp = qfp;
				fbq.buf = NULL;
				fbq.wall = NULL;
				fbq.info.width = M_W;
				fbq.info.height = M_H;
				fbq.info.line_length = M_W * 4;
				fbq.info.bytes_per_pixel = 4;
				fbq.info.bits_per_pixel = 32;
				fbq.info.type = YAFT_FB_TYPE_PACKED_PIXELS;
				fbq.info.visual = YAFT_FB_VISUAL_TRUECOLOR;
				fbq.info.red.length = 8;   fbq.info.red.offset = 16;
				fbq.info.green.length = 8; fbq.info.green.offset = 8;
				fbq.info.blue.length = 8;  fbq.info.blue.offset = 0;
				for (int i = 0; i < COLORS; i++)
					fbq.real_palette[i] = (uint32_t) i * 0x01010101u;

				/* Q1: full frame: strip rows (bar) + shifted grid.
				 * All sb test glyphs are solid 4x8 blocks, so every ink
				 * column is a uniform non-black color (per-item palette
				 * index, verified separately in section O); we assert
				 * geometry: which columns carry ink and which stay the
				 * strip background. */
				refresh(&fbq, &term);
				{
					int mism = 0, first_bad = -1;

					for (int y = 0; y < M_H; y++) {
						for (int x = 0; x < M_W; x++) {
							uint32_t px = m_px_at(qfp, x, y);
							int ink;

							if (y < 8) {
								/* clock 150..169; right zone (battery
								 * only) 280..319 */
								ink = (x >= 150 && x <= 169) ||
									(x >= 280 && x <= 319);
								if (ink == (px != 0)) {
									/* correct */
								} else {
									mism++;
									if (first_bad < 0)
										first_bad = y * M_W + x;
								}
							} else {
								/* grid rows: 1px side margins, bg=1 */
								uint32_t exp = (x == 0 || x == 319)
									? 0: 0x01010101u;

								if (px != exp) {
									mism++;
									if (first_bad < 0)
										first_bad = y * M_W + x;
								}
							}
						}
					}
					expect(mism == 0, "Q1 bar + shifted grid pixel-exact");
					if (mism)
						fprintf(stderr,
							"  first mismatch at y=%d x=%d (count=%d)\n",
							first_bad / M_W, first_bad % M_W, mism);
				}

				/* Q2: sixel blit shifted down by oy */
				m_fill(qfp, 0xCDCDCDCDu, M_W * M_H);
				{
					uint8_t *pix = (uint8_t *) ecalloc(1,
						(size_t) CELL_WIDTH * CELL_HEIGHT * BYTES_PER_PIXEL);
					uint32_t want = color2pixel(&fbq.info, 0xA5A5A5A5u);

					expect(pix != NULL, "Q2 pix alloc");
					if (pix) {
						m_fill(pix, want, CELL_WIDTH * CELL_HEIGHT);
						draw_sixel(&fbq, &term, 3, 2, pix);
						expect(m_px_at(qfp, 1 + 2 * 6, 8 + 3 * 12) == want,
							"Q2 sixel at oy + grid origin");
						expect(m_px_at(qfp, 1 + 2 * 6 + 5, 8 + 3 * 12 + 11) == want,
							"Q2 sixel last px");
						expect(m_px_at(qfp, 1 + 2 * 6, 3 * 12) == 0xCDCDCDCDu,
							"Q2 unshifted spot untouched");
					}
					free(pix);
				}

				/* Q3: shadow (wall) mode: draw_line's per-line flush must land
				 * on the oy-shifted band; the pre-fix form copied a band oy
				 * pixels too high, leaving the bottom of each refreshed line
				 * stale on the visible fb. */
				uint8_t *qb = (uint8_t *) ecalloc(1,
					(size_t) M_W * M_H * 4);
				expect(qb != NULL, "Q3 shadow alloc");
				if (qb) {
					m_fill(qfp, 0xCDCDCDCDu, M_W * M_H);
					m_fill(qb, 0xFFFFFFFFu, M_W * M_H);
					fbq.buf = qb;
					for (int y = 0; y < term.lines; y++)
						qscr->line_dirty[y] = false;
					qscr->line_dirty[3] = true;
					refresh(&fbq, &term);
					for (int dy = 0; dy < CELL_HEIGHT; dy++) {
						expect(m_px_at(qfp, 10,
							term.oy + 3 * CELL_HEIGHT + dy) == 0x01010101u,
							"Q3 flush on shifted row");
						/* rows the shifted band does not reach kept their
						 * sentinel; the buggy flush had clobbered them */
						if (dy < term.oy)
							expect(m_px_at(qfp, 10, 3 * CELL_HEIGHT + dy)
								== 0xCDCDCDCDu,
								"Q3 unshifted row untouched");
					}
					fbq.buf = NULL;
					free(qb);
				}
			}
			free_term_cells(&term);
			free(qfp);
			unsetenv("CRUFT_SB_FONT");
			statusbar_die();
			glyph_mmap_die();
			sb_net_root = roots_saved[0];
			sb_ps_root = roots_saved[1];
			{
				const char *gone[] = {
					"/ps/picocalc/capacity", "/ps/picocalc/type",
					"/ps/picocalc/status",
				};
				const char *rdirs[] = {
					"/ps/picocalc", "/ps", "/net", "",
				};

				for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) {
					snprintf(qpd, sizeof qpd, "%s%s", qbase, gone[i]);
					unlink(qpd);
				}
				for (size_t i = 0; i < sizeof rdirs / sizeof rdirs[0]; i++) {
					snprintf(qpd, sizeof qpd, "%s%s", qbase, rdirs[i]);
					rmdir(qpd);
				}
			}
			unlink(qsb);
			unlink(qm);
		}
	}

	/* R: screen split (struct screen_t, prep for DECALT): all screen
	 * state routes through active_screen() to scr[0]; scr[1] stays
	 * reserved and must never be allocated or written. */
	g_cell_w = 6;
	g_cell_h = 12;
	{
		struct screen_t *rscr, *ralt;

		expect(alloc_term_grid(&term, 36, 36) == 0, "R alloc 6x3");
		rscr = active_screen(&term);
		expect(term.active == 0, "R active is primary");
		expect(rscr == &term.scr[0], "R accessor routes to scr[0]");
		expect(rscr->allocated == true, "R primary marked allocated");
		ralt = &term.scr[1];
		expect(ralt->cells == NULL && ralt->line_dirty == NULL &&
			ralt->tabstop == NULL && ralt->allocated == false,
			"R alternate reserved");
		expect(ralt->cursor.x == 0 && ralt->cursor.y == 0 &&
			ralt->scroll.top == 0 && ralt->scroll.bottom == 0,
			"R alternate zeroed");

		/* writes via the public cell API must land in scr[0] */
		expect(cell_pixmap_ensure(&rscr->cells[0][0]) != NULL, "R ensure");
		rscr->cells[0][0].pixmap[0] = 0x77;
		rscr->cells[0][0].width = HALF;
		copy_cell(&term, 1, 2, 0, 0);
		expect(rscr->cells[1][2].has_pixmap && rscr->cells[1][2].pixmap,
			"R copy lands in scr[0]");
		expect(rscr->cells[1][2].pixmap[0] == 0x77, "R copied content");
		erase_cell(&term, 2, 3);
		expect(rscr->line_dirty[2] == true, "R erase dirties scr[0]");
		expect(ralt->cells == NULL, "R alternate still untouched");
		free_term_cells(&term);
	}

	/* S: DECALT alternate screen (CSI ?1049h/l) through the real engine:
	 * lazy alt alloc, wipe on entry, primary freeze, cursor/margin
	 * parking, doubled/mismatched no-ops, RIS discard, re-entry. */
	g_cell_w = 6;
	g_cell_h = 12;
	{
		struct screen_t *p, *a;
		const struct glyph_t *qm = glyph_get(0x3f);

		expect(term_init(&term, 96, 48), "S term_init 16x4");
		expect(term.cols == 16 && term.lines == 4 && term.oy == 0,
			"S grid 16x4 (no status bar)");

		/* seed a primary marker, park cursor + margins */
		set_cell(&term, 0, 1, qm);
		set_cursor(&term, 2, 4);
		active_screen(&term)->scroll.top = 1;

		/* ---- enter */
		enter_alt_screen(&term);
		expect(term.active == 1, "S enter switches to alt");
		expect(active_screen(&term) == &term.scr[1], "S accessor follows active");
		a = &term.scr[1];
		expect(a->cursor.x == 0 && a->cursor.y == 0, "S alt cursor homed");
		expect(a->scroll.top == 0 && a->scroll.bottom == 3, "S alt margin full");
		expect(a->cells[1][1].glyphp == glyph_get(DEFAULT_CHAR), "S alt wiped");
		{
			int all_dirty = 1;

			for (int y = 0; y < 4; y++)
				all_dirty &= a->line_dirty[y];
			expect(all_dirty, "S alt all lines dirty");
		}
		expect(term.alt_saved.cursor.x == 4 && term.alt_saved.cursor.y == 2 &&
			term.alt_saved.scroll.top == 1, "S primary state parked");
		expect(term.scr[0].cells[0][1].glyphp == qm, "S primary frozen (marker)");

		/* writes/scrolls inside the alt screen must not leak into the primary */
		set_cell(&term, 1, 1, qm);
		expect(a->cells[1][1].glyphp == qm, "S alt write lands in scr[1]");
		set_cursor(&term, 3, 15);
		move_cursor(&term, 1, 0); /* bottom edge: scroll up in alt */
		expect(term.scr[0].cells[0][1].glyphp == qm, "S primary intact after alt scroll");
		expect(a->cells[0][1].glyphp == qm, "S alt scroll moved row 1 -> 0");

		/* ---- leave */
		leave_alt_screen(&term);
		expect(term.active == 0, "S leave back to primary");
		p = active_screen(&term);
		expect(p->cursor.x == 4 && p->cursor.y == 2, "S cursor restored");
		expect(p->scroll.top == 1, "S margin restored");
		expect(p->cells[0][1].glyphp == qm, "S primary marker visible again");
		{
			int all_dirty = 1;

			for (int y = 0; y < 4; y++)
				all_dirty &= p->line_dirty[y];
			expect(all_dirty, "S primary forced full repaint");
		}
		expect(a->cells[0][1].glyphp == qm, "S alt content kept between visits");

		/* ---- idempotency */
		enter_alt_screen(&term); /* parks (4,2) again */
		enter_alt_screen(&term); /* doubled: must not re-park the homed cursor */
		leave_alt_screen(&term);
		expect(p->cursor.x == 4 && p->cursor.y == 2,
			"S doubled enter kept parked cursor");
		leave_alt_screen(&term); /* mismatched: no-op */
		expect(term.active == 0, "S doubled leave no-op");

		/* ---- RIS discards the residence and the matrix */
		enter_alt_screen(&term);
		reset(&term);
		expect(term.active == 0, "S RIS forces primary");
		expect(a->cells == NULL && a->allocated == false,
			"S RIS dropped alt matrix");
		expect(p->cells[0][1].glyphp == glyph_get(DEFAULT_CHAR),
			"S RIS wiped primary marker");

		/* re-entry re-allocates lazily; term_die frees both screens */
		enter_alt_screen(&term);
		expect(term.active == 1 && a->cells != NULL, "S re-entry re-allocates");
		leave_alt_screen(&term);
		expect(a->cells != NULL, "S matrix kept between visits");
		/* RIS must drop the resident matrix even while the primary is
		 * active (the kept-between-visits case). */
		reset(&term);
		expect(term.active == 0 && a->cells == NULL,
			"S RIS drops resident matrix on primary");
		term_die(&term);
		expect(term.scr[0].cells == NULL && a->cells == NULL,
			"S term_die freed both screens");
	}

	/* T: scrollback ring (scrollback.c): feeding rules (full-primary
	 * scrolls only), copied-row ownership, eviction order, viewport
	 * mapping, nav keys (native page forms), RIS reset,
	 * pixmap payload cap. */
	g_cell_w = 6;
	g_cell_h = 12;
	{
		struct screen_t *tscr;
		const struct glyph_t *qm = glyph_get(0x3f);
		const uint8_t bare_up[]     = {ESC, '[', 'A'};
		const uint8_t shift_up[]    = {ESC, '[', '1', ';', '2', 'A'};
		const uint8_t plain_pgup[]  = {ESC, '[', '5', '~'};
		const uint8_t plain_pgdwn[] = {ESC, '[', '6', '~'};
		const uint8_t param_pgup[]  = {ESC, '[', '5', ';', '2', '~'};
		const uint8_t ctrl_home[]   = {ESC, '[', '1', ';', '5', 'H'};
		const uint8_t ctrl_end[]    = {ESC, '[', '1', ';', '5', 'F'};
		const uint8_t bare_home[]   = {ESC, '[', '1', '~'};
		const uint8_t mixed[]       = {ESC, '[', '5', '~', 'x'};

		setenv("CRUFT_HISTORY", "4", 1);
		expect(term_init(&term, 96, 48), "T term_init 16x4");
		tscr = active_screen(&term);
		expect(term.sb.cap == 4 && term.sb.count == 0 && term.sb.view == 0,
			"T ring initialised (cap 4)");

		/* one marker per row: the fg colour tags the drain order */
		for (int y = 0; y < term.lines; y++) {
			term.color_pair.fg = (uint8_t) (10 + y);
			set_cell(&term, y, 0, qm);
		}
		term.color_pair.fg = DEFAULT_FG;

		/* draining one row: a full-primary scroll feeds the ring */
		set_cursor(&term, term.lines - 1, 0);
		move_cursor(&term, 1, 0);
		expect(term.sb.count == 1, "T full scroll drains a row");
		expect(tscr->cells[0][0].color_pair.fg == 11, "T live screen scrolled");
		term.sb.view = 1;
		expect(sb_view_row(&term, 0)[0].color_pair.fg == 10,
			"T drained row is the old top");
		expect(sb_view_row(&term, 1) == tscr->cells[0],
			"T view 1: logical row 1 is live row 0");

		/* five more: eviction keeps the youngest four */
		for (int i = 0; i < 5; i++) {
			int y = term.lines - 1;

			term.color_pair.fg = (uint8_t) (20 + i);
			set_cell(&term, y, 0, qm);
			term.color_pair.fg = DEFAULT_FG;
			set_cursor(&term, y, 0);
			move_cursor(&term, 1, 0);
		}
		expect(term.sb.count == 4, "T ring capped at 4");
		term.sb.view = term.sb.count; /* deepest: top line = oldest */
		/* drain order was 10, 11, 12, 13, 20, 21 (each scroll drops the
		 * row sliding off the top, not the row written at the bottom) */
		expect(sb_view_row(&term, 0)[0].color_pair.fg == 12, "T oldest survivor");
		expect(sb_view_row(&term, 1)[0].color_pair.fg == 13, "T middle survivor");
		expect(sb_view_row(&term, 3)[0].color_pair.fg == 21, "T newest survivor");
		term.sb.view = 0;
		expect(sb_view_row(&term, 0) == tscr->cells[0], "T view 0 maps to live");

		/* sub-region and alt-screen scrolls never feed history */
		tscr->scroll.top = 1;
		set_cursor(&term, 1, 0);
		move_cursor(&term, -1, 0);
		expect(term.sb.count == 4, "T sub-region scroll does not drain");
		tscr->scroll.top = 0;
		term.sb.view = 2;
		enter_alt_screen(&term);
		expect(term.sb.view == 0, "T alt entry ends browsing");
		set_cursor(&term, term.lines - 1, 0);
		move_cursor(&term, 1, 0);
		expect(term.sb.count == 4, "T alt scroll does not drain");
		leave_alt_screen(&term);

		/* navigation keys: the native page forms only (Shift+Up /
		 * Shift+Down on the PicoCalc); every other form stays the
		 * program's */
		expect(sbnav(&term, bare_up, 3) == 0, "T bare arrow passes through");
		expect(sbnav(&term, shift_up, (int)sizeof shift_up) == 0
			   && term.sb.view == 0,
			"T shift-modified arrow passes through");
		expect(sbnav(&term, plain_pgup, 4) == 4 && term.sb.view == 2,
			"T prior pages a half screen deeper");
		expect(sbnav(&term, plain_pgdwn, 4) == 4 && term.sb.view == 0,
			"T next pages back to the live tail");
		expect(sbnav(&term, ctrl_home, 6) == 6 && term.sb.view == 4,
			"T ctrl-home jumps to the oldest row");
		expect(sbnav(&term, ctrl_end, 6) == 6 && term.sb.view == 0,
			"T ctrl-end jumps to the live tail");
		{
			/* The 3-byte head of bare Home is an ambiguous
			 * Ctrl+Home prefix and must be held for the next
			 * read; the full 4-byte key passes through (and
			 * snaps the view) exactly as before. */
			unsigned carry;

			term.sb.view = 2;
			expect(sb_consume_nav(&term, bare_home, 3, &carry) == 0
				   && carry == 3 && term.sb.view == 2,
				"T bare-home head held (ctrl-home ambiguity)");
			expect(sbnav(&term, bare_home, (int)sizeof bare_home) == 0
				   && term.sb.view == 0,
				"T full bare home stays the program's");
		}
		term.sb.view = 2;
		expect(sbnav(&term, param_pgup, 6) == 0 && term.sb.view == 0,
			"T parameterised page form passes through and snaps");
		term.sb.view = 3;
		expect(sbnav(&term, mixed, 5) == 4 && term.sb.view == 0,
			"T nav run then foreign byte snaps to live");
		{
			const uint8_t plain = 'x';

			term.sb.view = 2;
			expect(sbnav(&term, &plain, 1) == 0 && term.sb.view == 0,
				"T plain key while browsing snaps to live");
		}

		/* A page key may arrive split across two reads (raw VMIN=1
		 * delivers "ESC", then "[5~"); the unfinished prefix is held
		 * for the next read and the view stays put until the held
		 * bytes prove themselves foreign. cruft.c prepends the held
		 * tail to the next buffer; emulated here. */
		{
			const uint8_t esc_only[] = {ESC};
			const uint8_t pg_rest[]  = {'[', '5', '~'};
			const uint8_t stray[]    = {'A'};
			uint8_t st[16];
			unsigned carry;

			term.sb.view = 2;
			expect(sb_consume_nav(&term, esc_only, 1, &carry) == 0
				   && carry == 1 && term.sb.view == 2,
				"T split nav holds lone ESC, view stays");
			memcpy(st, esc_only, carry);
			memcpy(st + carry, pg_rest, sizeof(pg_rest));
			expect(sb_consume_nav(&term, st,
					      (int)(carry + sizeof(pg_rest)),
					      &carry) == 4
				   && carry == 0 && term.sb.view == 4,
				"T split nav completes to page-up");
			term.sb.view = 2;
			st[0] = ESC;
			memcpy(st + 1, stray, sizeof(stray));
			expect(sb_consume_nav(&term, st, 2, &carry) == 0
				   && carry == 0 && term.sb.view == 0,
				"T broken prefix forwarded, view snaps");
		}

		/* The prefix search stays anchored at the TRUE end of the
		 * input even when foreign bytes precede it: only the
		 * candidate length is capped, the suffix offset is not.
		 * Foreign bytes snap the view (the user typed); the
		 * trailing prefix is STILL held, so a page key completed
		 * in the next read intercepts from the tail. */
		{
			const uint8_t esc_after5[] = { 'a', 'b', 'c', 'd', 'e', ESC };
			const uint8_t pg_tail6[]   = { 'x', 'x', 'x', ESC, '[', '5' };
			const uint8_t foreign6[]   = { 'a', 'b', 'c', 'd', 'e', 'f' };
			const uint8_t pg_rest[]     = { '[', '5', '~' };
			uint8_t st[16];
			unsigned carry;

			term.sb.view = 2;
			expect(sb_consume_nav(&term, esc_after5, 6, &carry) == 0
				   && carry == 1 && term.sb.view == 0,
				"T lone ESC held after 5 foreign bytes");
			/* the held ESC + the split page-up completing it in
			 * the next read must still intercept */
			st[0] = esc_after5[5];
			memcpy(st + 1, pg_rest, sizeof(pg_rest));
			expect(sb_consume_nav(&term, st,
					      (int)(1 + sizeof(pg_rest)),
					      &carry) == 4
				   && carry == 0 && term.sb.view > 0,
				"T held ESC after long input completes to page-up");
			term.sb.view = 2;
			expect(sb_consume_nav(&term, pg_tail6, 6, &carry) == 0
				   && carry == 3 && term.sb.view == 0,
				"T page-up tail held after 3 foreign bytes");
			expect(sb_consume_nav(&term, foreign6, 6, &carry) == 0
				   && carry == 0 && term.sb.view == 0,
				"T all-foreign run forwards, view snaps");
		}
		/* Back to the tail: stored rows leave the live lines clean, so
		 * every line must be re-flagged or the lazy painter would
		 * keep history pixels on screen. */
		for (int y = 0; y < term.lines; y++)
			tscr->line_dirty[y] = false;
		term.sb.view = 3;
		{
			bool all = true;

			expect(sbnav(&term, ctrl_end, 6) == 6 && term.sb.view == 0,
				"T ctrl-end settles at the tail from deep");
			for (int y = 0; y < term.lines; y++)
				all &= tscr->line_dirty[y];
			expect(all, "T ctrl-end re-dirties every line");
		}
		for (int y = 0; y < term.lines; y++)
			tscr->line_dirty[y] = false;
		term.sb.view = 3;
		{
			bool all = true;

			expect(sbnav(&term, mixed, 5) == 4 && term.sb.view == 0,
				"T foreign byte snaps to live");
			for (int y = 0; y < term.lines; y++)
				all &= tscr->line_dirty[y];
			expect(all, "T snap-back re-dirties every line");
		}

		/* RIS resets the ring; an empty ring lets nav pass through */
		reset(&term);
		expect(term.sb.count == 0 && term.sb.view == 0, "T RIS empties ring");
		expect(sbnav(&term, plain_pgup, 4) == 0,
			"T no history: nav passes through");
		/* tear down this generation before the re-init below, which
		 * would otherwise leak the ring, matrices and esc buffer */
		term_die(&term);

		/* CRUFT_HISTORY=0 disables the ring entirely */
		setenv("CRUFT_HISTORY", "0", 1);
		expect(term_init(&term, 96, 48), "T re-init disabled");
		expect(term.sb.cap == 0 && term.sb.rows == NULL, "T CRUFT_HISTORY=0 off");
		set_cursor(&term, term.lines - 1, 0);
		move_cursor(&term, 1, 0);
		expect(term.sb.count == 0, "T disabled ring not fed");
		term_die(&term);

		/* sixel payload cap: newest art survives, oldest degrades */
		setenv("CRUFT_HISTORY", "512", 1);
		expect(term_init(&term, 96, 48), "T cap-512 term_init");
		for (int i = 0; i < 400; i++) {
			int y = term.lines - 1;
			uint8_t *pm = cell_pixmap_ensure(&active_screen(&term)->cells[y][0]);

			/* the cell is re-erased after every scroll, so the pointer
			 * must not be held across iterations */
			if (pm) {
				pm[0] = (uint8_t) i;
				pm[1] = (uint8_t) (i >> 8);
			}
			set_cursor(&term, y, 0);
			move_cursor(&term, 1, 0);
		}
		expect(term.sb.count == 400, "T 400 rows stored");
		expect(term.sb.pixmap_bytes > 0
			&& term.sb.pixmap_bytes <= (size_t) SB_PIXMAP_CAP_KIB * 1024,
			"T pixmap payload capped");
		term.sb.view = term.sb.count;
		expect(sb_view_row(&term, 0)[0].has_pixmap == false, "T oldest art degraded");
		term.sb.view = 4;
		/* the newest stored row carries the marker written 3 scrolls
		 * earlier (the live bottom is not in the ring yet) */
		expect(sb_view_row(&term, 3)[0].has_pixmap
			&& sb_view_row(&term, 3)[0].pixmap[0] == (uint8_t) 396
			&& sb_view_row(&term, 3)[0].pixmap[1] == 1,
			"T newest art kept deep-copied");
		term_die(&term);
		unsetenv("CRUFT_HISTORY");

		/* Bar geometry on 320x320: the first stored row anchors the
		 * grid left (x0 is grid, not margin) and the 2px slider rides
		 * the right edge; track palette 8, thumb palette 7. Direction
		 * is pinned with view == total (the OLDEST row, top of the
		 * document): thumb height 312*26/28 = 289, offset
		 * (312-289)*(2-2)/2 = 0, so the thumb spans y 0..288 (a
		 * view-scaled, not total-view-scaled, offset would put it at
		 * y 23..311 for this state). */		memset(&term, 0, sizeof(term));
		expect(alloc_term_grid(&term, M_W, M_H) == 0, "T bar fixture 53x26");
		{
			struct framebuffer_t fbm;
			uint8_t *mfp = (uint8_t *) ecalloc(1, (size_t) M_W * M_H * 4);

			expect(mfp != NULL, "T bar fp alloc");
			for (int y = 0; y < term.lines; y++)
				for (int x = 0; x < term.cols; x++) {
					struct cell_t *c = &term.scr[0].cells[y][x];

					c->glyphp = glyph_get(0x20);
					c->color_pair.fg = 7;
					c->color_pair.bg = 1;
					c->width = HALF;
				}
			for (int y = 0; y < term.lines; y++)
				term.scr[0].line_dirty[y] = true;
			term.mode = MODE_AMRIGHT;

			/* synthetic non-empty ring (fixture predates sb_init) */
			term.sb.cap = 4;
			term.sb.count = 2;
			term.sb.head = 2;
			term.sb.view = 2; /* == total: browsing the oldest row */
			term.sb.disk.fd = -1; /* mirror term_init(): the zeroed
						* sb must not read as stdin when
						* term_die tears the tier down */			term.sb.rows = (struct cell_t **) ecalloc(4, sizeof(*term.sb.rows));
			term.sb.rows[0] = (struct cell_t *) ecalloc(term.cols, sizeof(struct cell_t));
			term.sb.rows[1] = (struct cell_t *) ecalloc(term.cols, sizeof(struct cell_t));
			expect(term.sb.rows && term.sb.rows[0] && term.sb.rows[1], "T ring synth");
			/* stored rows are populated copies in production */
			for (int r = 0; r < 2; r++)
				for (int x = 0; x < term.cols; x++) {
					struct cell_t *c = &term.sb.rows[r][x];

					c->glyphp = glyph_get(0x20);
					c->color_pair.fg = 7;
					c->color_pair.bg = 1;
					c->width = HALF;
				}

			m_fill(mfp, 0xEFEFEFEFu, M_W * M_H);
			memset(&fbm, 0, sizeof(fbm));
			fbm.fp = mfp;
			fbm.info.width = M_W;
			fbm.info.height = M_H;
			fbm.info.line_length = M_W * 4;
			fbm.info.bytes_per_pixel = 4;
			fbm.info.bits_per_pixel = 32;
			fbm.info.type = YAFT_FB_TYPE_PACKED_PIXELS;
			fbm.info.visual = YAFT_FB_VISUAL_TRUECOLOR;
			fbm.info.red.length = 8;   fbm.info.red.offset = 16;
			fbm.info.green.length = 8; fbm.info.green.offset = 8;
			fbm.info.blue.length = 8;  fbm.info.blue.offset = 0;
			for (int i = 0; i < COLORS; i++)
				fbm.real_palette[i] = (uint32_t) i * 0x01010101u;

			refresh(&fbm, &term);
			expect(m_px_at(mfp, 0, 0) == 0x01010101u, "T grid anchored left");
			expect(m_px_at(mfp, 317, 0) == 0x01010101u, "T grid reaches x317");
			expect(m_px_at(mfp, 318, 5) == 0x07070707u, "T bar thumb pinned to the top (oldest row)");			expect(m_px_at(mfp, 319, 100) == 0x07070707u, "T bar thumb");
			expect(m_px_at(mfp, 318, 305) == 0x08080808u, "T bar track (below thumb)");
			expect(m_px_at(mfp, 318, 315) == 0x00000000u, "T below grid is margin, not bar");
			free(mfp);
		}
		term_die(&term);
	}

	/* U: disk tier (sbdisk.c): spill past the RAM depth, tier boundary
	 * mapping, nav over the combined depth, push pinning, compaction,
	 * pixmap survival on disk, corrupt-file degradation, unlink on
	 * reset and death. */
	g_cell_w = 6;
	g_cell_h = 12;
	{
		struct screen_t *uscr;
		const struct glyph_t *qm = glyph_get(0x3f);
		const uint8_t plain_pgup[]  = {ESC, '[', '5', '~'};
		const uint8_t plain_pgdwn[] = {ESC, '[', '6', '~'};
		const uint8_t ctrl_end[]    = {ESC, '[', '1', ';', '5', 'F'};

		/* total 10, RAM tier 3: the disk tier takes the other 7 */
		setenv("CRUFT_HISTORY", "10", 1);
		setenv("CRUFT_SB_RAM", "3", 1);
		expect(term_init(&term, 96, 48), "U term_init 16x4");
		uscr = active_screen(&term);
		expect(term.sb.cap == 3 && term.sb.disk.cap == 7,
			"U split 3 RAM / 7 disk");
		expect(term.sb.disk.fd < 0 && sb_history_total(&term) == 0,
			"U disk lazy, nothing stored");

		/* three pushes fill the RAM tier; nothing spills yet */
		for (int i = 0; i < 3; i++) {
			int y = term.lines - 1;

			term.color_pair.fg = (uint8_t) (40 + i);
			set_cell(&term, y, 0, qm);
			term.color_pair.fg = DEFAULT_FG;
			set_cursor(&term, y, 0);
			move_cursor(&term, 1, 0);
		}
		expect(term.sb.count == 3 && term.sb.disk.count == 0
			&& term.sb.disk.fd < 0,
			"U RAM tier fills before spill");

		/* seven more: each eviction appends a record to the file */
		for (int i = 3; i < 10; i++) {
			int y = term.lines - 1;

			term.color_pair.fg = (uint8_t) (40 + i);
			set_cell(&term, y, 0, qm);
			term.color_pair.fg = DEFAULT_FG;
			set_cursor(&term, y, 0);
			move_cursor(&term, 1, 0);
		}
		expect(term.sb.count == 3 && term.sb.disk.count == 7,
			"U 10 pushes: 3 RAM + 7 disk");
		expect(term.sb.disk.fd >= 0 && term.sb.disk.path[0],
			"U disk attached after first spill");
		{
			struct stat st;

			expect(stat(term.sb.disk.path, &st) == 0 && st.st_size > 20,
				"U history file on disk");
		}
		/* Drains d1..d10: d_J (J>=4) carries marker 40+J-4, the first
		 * three are unmarked seeds (fg DEFAULT_FG). */
		term.sb.view = 10;
		/* the top window shows the OLDEST rows: line 0 is the top of the
		 * oldest row, line 3 its bottom */
		expect(sb_view_row(&term, 0)[0].color_pair.fg == DEFAULT_FG,
			"U top: oldest row (a seed) at the top");
		expect(sb_view_row(&term, 3)[0].color_pair.fg == 40,
			"U top: window bottom is d4 (disk)");
		/* the RAM/disk seam sits between views 7 and 6 (bottom line) */
		term.sb.view = 7;
		expect(sb_view_row(&term, 3)[0].color_pair.fg == 43,
			"U seam: view 7 bottom from disk");
		term.sb.view = 6;
		expect(sb_view_row(&term, 3)[0].color_pair.fg == 44,
			"U seam: view 6 bottom from RAM");
		/* a disk row is neither a live row nor a ring slot */
		term.sb.view = 10;
		{
			const struct cell_t *r = sb_view_row(&term, 0);
			bool alien = r != uscr->cells[0] && r != uscr->cells[1]
				     && r != uscr->cells[2] && r != uscr->cells[3]
				     && term.sb.rows && r != term.sb.rows[0]
				     && r != term.sb.rows[1] && r != term.sb.rows[2];

			expect(alien, "U disk row is decoded, not borrowed");
		}
		/* nav spans both tiers: page to the live tail, back to the
		 * oldest row (five half-pages each way at total 10) */
		for (int p = 0; p < 5; p++)
			expect(sbnav(&term, plain_pgdwn, 4) == 4, "U pgdn consumed");
		expect(term.sb.view == 0, "U paging reaches the live tail");
		for (int p = 0; p < 5; p++)
			expect(sbnav(&term, plain_pgup, 4) == 4, "U pgup consumed");
		expect(term.sb.view == 10, "U paging reaches the oldest row");

		/* push while browsing MID-history: the window stays put
		 * (view pinned +1), the oldest row is compacted away */
		term.sb.view = 5;
		{
			int y = term.lines - 1;

			term.color_pair.fg = 50;
			set_cell(&term, y, 0, qm);
			term.color_pair.fg = DEFAULT_FG;
			set_cursor(&term, y, 0);
			move_cursor(&term, 1, 0);
		}
		expect(sb_history_total(&term) == 10 && term.sb.view == 6,
			"U push pins the browsed row");
		expect(sb_view_row(&term, 0)[0].color_pair.fg == 42
			&& sb_view_row(&term, 3)[0].color_pair.fg == 45,
			"U window content stayed put");

		/* mid-file damage degrades the tier: blank lines, RAM only,
		 * file dropped, nav still sound */
		{
			int cfd = open(term.sb.disk.path, O_RDWR);

			expect(cfd >= 0 && ftruncate(cfd, 21) == 0 && !close(cfd),
				"U corruption prepared (trunc mid-record)");
		}
		{
			char dead[128];
			const struct cell_t *r;

			memcpy(dead, term.sb.disk.path, sizeof(dead));
			r = sb_view_row(&term, 1); /* not cached: must hit the file */

			expect(r != NULL && r[0].color_pair.fg == DEFAULT_FG,
				"U corrupt file serves blank rows");
			expect(sb_history_total(&term) == 3,
				"U tier degraded: RAM only");
			expect(stat(dead, &(struct stat) { 0 }) != 0,
				"U corrupt file unlinked");
			expect(sbnav(&term, ctrl_end, 6) == 6
				&& term.sb.view == 0,
				"U nav healthy after degrade");
		}
		term_die(&term);

		/* A grid wider than the 512-cell static fallback: a
		 * degraded tier must still serve a blank row spanning
		 * term->cols. Only the session row - which outlives
		 * teardown - can satisfy that; the old static read OOB
		 * (proven by the ASan lane). */
		setenv("CRUFT_HISTORY", "12", 1);
		setenv("CRUFT_SB_RAM", "2", 1);
		expect(term_init(&term, 3200, 96), "U wide term_init");
		expect(term.cols > 512,
			"U wide grid exceeds the static fallback");
		for (int i = 0; i < 12; i++) {
			int y = term.lines - 1;

			set_cursor(&term, y, 0);
			move_cursor(&term, 1, 0);
		}
		expect(term.sb.disk.count > 0, "U wide spill engaged");
		{
			int cfd = open(term.sb.disk.path, O_RDWR);

			expect(cfd >= 0 && ftruncate(cfd, 21) == 0 && !close(cfd),
				"U wide corruption prepared");
		}
		{
			const struct cell_t *r;

			term.sb.view = 12; /* window top = oldest = disk */
			r = sb_view_row(&term, 0);
			expect(r != NULL
				&& r[512].glyphp == glyph_get(DEFAULT_CHAR)
				&& r[512].has_pixmap == false
				&& r[term.cols - 1].glyphp == glyph_get(DEFAULT_CHAR)
				&& r[term.cols - 1].width == HALF,
				"U degraded blank row spans the wide grid");
			expect(sb_history_total(&term) == 2,
				"U wide tier degraded: RAM only");
		}
		term_die(&term);
		unsetenv("CRUFT_HISTORY");
		unsetenv("CRUFT_SB_RAM");
		/* pixmaps survive the trip to disk: total 6, RAM 2, disk 4 */
		setenv("CRUFT_HISTORY", "6", 1);
		setenv("CRUFT_SB_RAM", "2", 1);
		expect(term_init(&term, 96, 48), "U pixmap term_init");
		for (int i = 0; i < 6; i++) {
			int y = term.lines - 1;
			uint8_t *pm = cell_pixmap_ensure(&active_screen(&term)->cells[y][0]);

			/* marker i+1 keeps every stored payload non-zero */
			if (pm) {
				pm[0] = (uint8_t) (i + 1);
				pm[1] = (uint8_t) ((i + 1) >> 8);
			}
			set_cursor(&term, y, 0);
			move_cursor(&term, 1, 0);
		}
		expect(term.sb.count == 2 && term.sb.disk.count == 4,
			"U pixmap split 2 RAM / 4 disk");
		term.sb.view = 6;
		/* the window's bottom line is d4 (disk, marker 1); its top
		 * line is d1, the first seed (art-free) */
		expect(sb_view_row(&term, 3)[0].has_pixmap
			&& sb_view_row(&term, 3)[0].pixmap[0] == 1
			&& sb_view_row(&term, 3)[0].pixmap[1] == 0,
			"U disk row kept its pixmap");
		expect(sb_view_row(&term, 0)[0].has_pixmap == false,
			"U seed row art-free from disk");
		term.sb.view = 4;
		expect(sb_view_row(&term, 3)[0].has_pixmap
			&& sb_view_row(&term, 3)[0].pixmap[0] == 3,
			"U RAM row still direct (regression)");

		/* reset drops the ring AND unlinks the session file */
		{
			char dead[128];

			memcpy(dead, term.sb.disk.path, sizeof(dead));
			reset(&term);
			expect(sb_history_total(&term) == 0 && term.sb.view == 0,
				"U reset empties both tiers");
			expect(stat(dead, &(struct stat) { 0 }) != 0,
				"U reset unlinked the file");
		}
		term_die(&term);
		unsetenv("CRUFT_HISTORY");
		unsetenv("CRUFT_SB_RAM");
	}

	/* V: /etc/cruft.conf loader (conf.c): grammar, validity, and
	 * environment precedence. */
	{
		static const char *vkeys[6] = {
			"CRUFT_FONT", "CRUFT_SB", "CRUFT_SB_FONT",
			"CRUFT_SB_12H", "CRUFT_HISTORY", "CRUFT_SB_RAM",
		};
		char *saved[6];
		char tp[64];

		for (int i = 0; i < 6; i++) {
			const char *v = getenv(vkeys[i]);

			saved[i] = (v == NULL) ? NULL: strdup(v);
			unsetenv(vkeys[i]);
		}
		snprintf(tp, sizeof(tp), "/tmp/cruft-conf-%d", (int) getpid());
		conf_load_file("/nonexistent/cruft.conf-V");
		expect(getenv("CRUFT_HISTORY") == NULL, "V missing file is a no-op");

		FILE *cf;
		char fntp[64];
		char garb[64];

		snprintf(fntp, sizeof(fntp), "/tmp/cruft-conf-%d-f.font",
			 (int) getpid());
		snprintf(garb, sizeof(garb), "/tmp/cruft-conf-%d-g.txt",
			 (int) getpid());
		expect(write_tiny_font(fntp) == 0, "V fixture font written");
		unlink(tp);
		cf = fopen(tp, "w");
		expect(cf != NULL, "V temp path ready");
		if (cf == NULL)
			goto v_cleanup;
		fprintf(cf, "# V well-formed config\n"
		        "CRUFT_FONT=%s\n"
		        "CRUFT_SB=time,battery\n"
		        "CRUFT_SB_FONT=/s.fnt\n"
		        "CRUFT_SB_12H=1\n"
		        "  CRUFT_HISTORY = 8192  \n"
		        "CRUFT_SB_RAM=256\n",
			fntp);
		fclose(cf);
		conf_load_file(tp);
		expect(strcmp(getenv("CRUFT_FONT"), fntp) == 0
		       && strcmp(getenv("CRUFT_SB"), "time,battery") == 0
		       && strcmp(getenv("CRUFT_SB_FONT"), "/s.fnt") == 0
		       && strcmp(getenv("CRUFT_SB_12H"), "1") == 0,
			"V font/SB knobs from file");
		expect(strcmp(getenv("CRUFT_HISTORY"), "8192") == 0
		       && strcmp(getenv("CRUFT_SB_RAM"), "256") == 0,
			"V depth knobs (padding trimmed) from file");

		setenv("CRUFT_HISTORY", "77", 1);
		cf = fopen(tp, "a");
		expect(cf != NULL, "V temp path appendable");
		if (cf == NULL)
			goto v_cleanup;
		fputs("CRUFT_HISTORY=99\n", cf);
		fclose(cf);
		conf_load_file(tp);
		expect(strcmp(getenv("CRUFT_HISTORY"), "77") == 0,
			"V environment beats file");
		unsetenv("CRUFT_HISTORY");
		conf_load_file(tp);
		expect(strcmp(getenv("CRUFT_HISTORY"), "99") == 0,
			"V file value once env is clear");

		/* A blank between segments is exactly what
		 * sb_parse_segments() squeezes into commas: the loader
		 * installs "time, battery" verbatim instead of refusing
		 * it and leaving the default selection in effect. */
		cf = fopen(tp, "a");
		expect(cf != NULL, "V temp path appendable (2)");
		if (cf == NULL)
			goto v_cleanup;
		fputs("CRUFT_SB=time, battery\n", cf);
		fclose(cf);
		unsetenv("CRUFT_SB"); /* the first load installed "time,battery" */
		conf_load_file(tp);
		expect(strcmp(getenv("CRUFT_SB"), "time, battery") == 0,
			"V SB list with a space installs verbatim");

		/* A file-supplied CRUFT_FONT that glyph_mmap_init() would
		 * choke on (missing file, or an existing file that is not
		 * a font) must be refused so the built-in font stands;
		 * the last valid line wins, so the garbage file decides. */
		cf = fopen(garb, "w");
		expect(cf != NULL, "V garbage path writable");
		if (cf == NULL)
			goto v_cleanup;
		fputs("not a font\n", cf);
		fclose(cf);
		cf = fopen(tp, "a");
		expect(cf != NULL, "V temp path appendable (3)");
		if (cf == NULL)
			goto v_cleanup;
		fprintf(cf, "CRUFT_FONT=/nonexistent/v.font\n"
		        "CRUFT_FONT=%s\n", garb);
		fclose(cf);
		unsetenv("CRUFT_FONT");
		conf_load_file(tp);
		expect(getenv("CRUFT_FONT") == NULL,
			"V unusable file font never installed");

		cf = fopen(tp, "w");
		expect(cf != NULL, "V temp path rewritable (junk)");
		if (cf == NULL)
			goto v_cleanup;
		fputs("\n   # only junk below\n"
		      "CRUFT_HISTORY=abc\n"
		      "CRUFT_SB=bad;value\n"
		      "CRUFT_SB_12H=2\n"
		      "CRUFT_SB=\n"
		      "NOT_A_KEY=1\n"
		      "no equals here\n"
		      "CRUFT_FONT\n", cf);
		fclose(cf);
		for (int i = 0; i < 6; i++)
			unsetenv(vkeys[i]);
		conf_load_file(tp);
		for (int i = 0; i < 6; i++)
			expect(getenv(vkeys[i]) == NULL,
				"V invalid or unknown lines never installed");

		/* Overlong physical line (>255 bytes) is discarded whole: a
		 * KEY=VALUE wrapped past the fgets boundary must not surface
		 * at the next read as if it were a new line, and parsing must
		 * resume at the line after it. */
		{
			char pad[256];

			memset(pad, '#', sizeof pad - 1);
			pad[sizeof pad - 1] = '\0';
			cf = fopen(tp, "w");
			expect(cf != NULL, "V temp path rewritable");
			if (cf == NULL)
				goto v_cleanup;
			fprintf(cf, "%sCRUFT_SB_12H=1\nCRUFT_SB_RAM=256\n", pad);
			fclose(cf);
			for (int i = 0; i < 6; i++)
				unsetenv(vkeys[i]);
			conf_load_file(tp);
			expect(getenv("CRUFT_SB_12H") == NULL,
				"V overlong line tail (wrapped key) drained");
			expect(strcmp(getenv("CRUFT_SB_RAM"), "256") == 0,
				"V line after an overlong one still parses");
		}
	v_cleanup:
		unlink(tp);
		unlink(fntp);
		unlink(garb);

		for (int i = 0; i < 6; i++) {
			if (saved[i] != NULL)
				setenv(vkeys[i], saved[i], 1);
			free(saved[i]);
		}
	}

	/* W: kitty graphics transfer (APC _G / DCS +) end to end: probe,
	 * chunked transfer, Unicode-placeholder display, a=p re-place,
	 * delete, direct placement and the probe replies. Own 6x12 font
	 * (the V section tears its mmap down, zeroing the metrics); grid
	 * is 10x2 cells. */
	{
		int rp[2] = { -1, -1 };
		char got[1024];
		char wt[40];
		char wfont[80];
		int wf;

		expect(pipe(rp) == 0, "W pipe");
		snprintf(wt, sizeof wt, "/tmp/cruft-st-W-XXXXXX");
		wf = mkstemp(wt);
		expect(wf >= 0, "W mkstemp");
		if (wf >= 0) {
			close(wf);
			unlink(wt);
			snprintf(wfont, sizeof wfont, "%s.cruftfont", wt);
			expect(write_tiny_font6x12(wfont) == 0, "W write 6x12 font");
			expect(glyph_mmap_init(wfont) == 1, "W glyph init");
			unlink(wfont);
		}
		if (rp[0] >= 0) {
			uint8_t *sq;

			expect(u_setup(&term) == 0, "W base setup");
			term.fd = rp[1];
			term.color_pair.bg = 0; /* blend over black, not u_setup's white */

			/* W1: capability probe (ratatui's exact sequence) */
			sq = w_apc("i=31,s=1,v=1,a=q,t=d,f=24", "AAAA");
			parse(&term, sq, (int) strlen((const char *) sq));
			free(sq);
			expect(w_drain(rp[0], got, sizeof got) > 0, "W1 drained");
			expect(memcmp(got, "\033_Gi=31;OK\033\\",
					strlen("\033_Gi=31;OK\033\\")) == 0,
				"W1 probe reply exact");

			/* W2: DA1 advertises sixel (probe attribute 4 + 6) */
			sq = (uint8_t *) malloc(4);
			if (sq) {
				memcpy(sq, "\033[c", 4);
				parse(&term, sq, 3);
				free(sq);
			}
			expect(w_drain(rp[0], got, sizeof got) > 0, "W2 drained");
			expect(strstr(got, "\033[?62;4;6c") != NULL, "W2 DA1 sixel");

			/* W3: cell size report (font size for the picker) */
			sq = (uint8_t *) malloc(5);
			if (sq) {
				memcpy(sq, "\033[16t", 5);
				parse(&term, sq, 5);
				free(sq);
			}
			expect(w_drain(rp[0], got, sizeof got) > 0, "W3 drained");
			expect(strstr(got, "\033[16;12;6t") != NULL, "W3 cell size");

			/* W4: chunked virtual placement, then placeholder display */
			{
				uint8_t px[12 * 12 * 4];
				char b64[1024], b64a[512], b64b[512];
				int sx, sy;

				for (sy = 0; sy < 12; sy++)
					for (sx = 0; sx < 12; sx++) {
						uint8_t *p = &px[(sy * 12 + sx) * 4];

						if (sx < 6) /* red, opaque */
							p[0] = 255, p[1] = 0, p[2] = 0, p[3] = 255;
						else            /* blue, opaque */
							p[0] = 0, p[1] = 0, p[2] = 255, p[3] = 255;
					}
				st_b64enc(px, sizeof px, b64);
				/* split the base64 at a 3-byte boundary (340 chars) */
				memcpy(b64a, b64, 340);
				b64a[340] = '\0';
				memcpy(b64b, b64 + 340, 768 - 340);
				b64b[768 - 340] = '\0';

				u_teardown(&term); /* release the W base fixture */
				expect(u_setup(&term) == 0, "W4 alloc");
				term.fd = rp[1];
				term.color_pair.bg = 0; /* blend over black, not u_setup's white */

				sq = w_apc("i=1,a=T,U=1,f=32,t=d,s=12,v=12,c=2,r=1,m=1", b64a);
				parse(&term, sq, (int) strlen((const char *) sq));
				free(sq);
				expect(w_px(&term, 0, 0)->has_pixmap == false,
					"W4 stored, not painted yet");

				sq = w_apc("m=0", b64b);
				parse(&term, sq, (int) strlen((const char *) sq));
				free(sq);
				expect(w_drain(rp[0], got, sizeof got) > 0, "W4 drained");
				expect(strstr(got, "\033_Gi=1;OK") != NULL, "W4 OK reply");

				/* q=2 (ratatui-image) silences OK too: an unread reply
				 * would fill the pty input queue and deadlock cruft */
				sq = w_apc("q=2,i=77,a=T,U=1,f=32,t=d,s=4,v=4,c=1,r=1", b64a);
				parse(&term, sq, (int) strlen((const char *) sq));
				free(sq);
				expect(w_drain(rp[0], got, sizeof got) <= 0, "W4 q=2 silent");
				expect(w_px(&term, 0, 0)->has_pixmap == false,
					"W4 still only stored");

				/* placeholder row: anchor (diacritics row=0 col=0
				 * hi=0) + one inherited cell, SGR truecolor fg
				 * carries id 1 (rgb 0,0,1) */
				{
					uint8_t phseq[] = {
						0x1B, '[', '3', '8', ';', '2', ';', '0',
						';', '0', ';', '1', 'm',
						0xF4, 0x8E, 0xBB, 0xAE, /* U+10EEEE */
						0xCC, 0x85, 0xCC, 0x85, 0xCC, 0x85, /* d(0)d(0)d(0) */
						0xF4, 0x8E, 0xBB, 0xAE, /* inherited cell */
						'\n',
						0x1B, '[', '3', '9', 'm',
					};

					parse(&term, phseq, (int) sizeof phseq);
					expect(w_px(&term, 0, 0)->has_pixmap
							&& w_px(&term, 0, 0)->pixmap[2] == 0xFF
							&& w_px(&term, 0, 0)->pixmap[0] == 0x00,
						"W4 left half red");
					expect(w_px(&term, 0, 1)->has_pixmap
							&& w_px(&term, 0, 1)->pixmap[0] == 0xFF
							&& w_px(&term, 0, 1)->pixmap[2] == 0x00,
						"W4 right half blue");
					expect(active_screen(&term)->cursor.x == 2
							&& active_screen(&term)->cursor.y == 1,
						"W4 cursor past the placement");
				}

				/* ratatui-image form: only row+col diacritics (2 marks),
				 * every cell carries them, the row ends in a CSI - the
				 * anchor must still be placed */
				{
					uint8_t phseq2[] = {
						0x1B, '[', '2', ';', '1', 'H',
						0x1B, '[', '3', '8', ';', '2', ';', '0',
						';', '0', ';', '1', 'm',
						0xF4, 0x8E, 0xBB, 0xAE, 0xCC, 0x85, 0xCC, 0x85,
						0xF4, 0x8E, 0xBB, 0xAE, 0xCC, 0x85, 0xCD, 0x8E,
						0x1B, '[', '3', '9', 'm',
					};

					parse(&term, phseq2, (int) sizeof phseq2);
					expect(w_px(&term, 1, 0)->has_pixmap
							&& w_px(&term, 1, 1)->has_pixmap,
						"W4 two-diacritic placeholders paint");
				}

				/* W5: a=p re-places the stored image elsewhere */
				sq = (uint8_t *) malloc(6);
				if (sq) {
					memcpy(sq, "\033[2;4H", 6);
					parse(&term, sq, 6);
					free(sq);
				}
				sq = w_apc("a=p,i=1", "");
				parse(&term, sq, (int) strlen((const char *) sq));
				free(sq);
				expect(w_drain(rp[0], got, sizeof got) > 0, "W5 drained");
				expect(strstr(got, "\033_Gi=1;OK") != NULL, "W5 place OK");
				expect(w_px(&term, 1, 3)->has_pixmap
						&& w_px(&term, 1, 3)->pixmap[2] == 0xFF,
					"W5 replaced left red");
				expect(w_px(&term, 1, 4)->has_pixmap
						&& w_px(&term, 1, 4)->pixmap[0] == 0xFF,
					"W5 replaced right blue");

				/* W8: delete, then the id is gone */
				sq = w_apc("a=d,d=i,i=1", "");
				parse(&term, sq, (int) strlen((const char *) sq));
				free(sq);
				sq = w_apc("a=p,i=1", "");
				parse(&term, sq, (int) strlen((const char *) sq));
				free(sq);
				expect(w_drain(rp[0], got, sizeof got) > 0, "W8 drained");
				expect(strstr(got, "ENOENT") != NULL, "W8 deleted id ENOENT");
				u_teardown(&term);
			}

			/* W6: direct a=T (no virtual placement): paint at the cursor,
			 * cursor ends cols right and lines below (spec placement
			 * epilogue; also pins the multi-row case) */
			{
				uint8_t px[12 * 24 * 3];
				char b64[2048];

				for (size_t i = 0; i < 12 * 24; i++) {
					px[i * 3 + 0] = 255; /* R first: f=24 is RGB */
					px[i * 3 + 1] = 0;
					px[i * 3 + 2] = 0;
				}
				st_b64enc(px, sizeof px, b64);
				expect(u_setup(&term) == 0, "W6 alloc");
				term.fd = rp[1];
				term.color_pair.bg = 0;
				sq = w_apc("i=7,a=T,s=12,v=24,f=24", b64);
				parse(&term, sq, (int) strlen((const char *) sq));
				free(sq);
				expect(w_px(&term, 0, 0)->has_pixmap
						&& w_px(&term, 0, 0)->pixmap[2] == 0xFF
						&& w_px(&term, 1, 1)->has_pixmap
						&& w_px(&term, 1, 1)->pixmap[2] == 0xFF,
					"W6 2x2 cells red");
				expect(active_screen(&term)->cursor.x == 2
						&& active_screen(&term)->cursor.y == 1,
					"W6 cursor right+down (clamped to last row)");

				/* W7: f=32 RGBA alpha blended over the black background */
				{
					uint8_t apx[6 * 12 * 4];
					char ab64[512];

					for (size_t i = 0; i < 6 * 12; i++) {
						apx[i * 4 + 0] = 255;
						apx[i * 4 + 1] = 0;
						apx[i * 4 + 2] = 0;
						apx[i * 4 + 3] = 128;
					}
					st_b64enc(apx, sizeof apx, ab64);
					u_teardown(&term); /* release the W6 fixture */
					expect(u_setup(&term) == 0, "W7 alloc");
					term.fd = rp[1];
					term.color_pair.bg = 0;
					sq = w_apc("i=9,a=T,s=6,v=12,f=32", ab64);
					parse(&term, sq, (int) strlen((const char *) sq));
					free(sq);
					/* 255 * 128/255 + 0 * 127/255 = 128 */
					expect(w_px(&term, 0, 0)->has_pixmap
							&& w_px(&term, 0, 0)->pixmap[2] == 128,
						"W7 alpha blended");
					u_teardown(&term);
				}

				/* W9: an id whose high byte is >= 30 rides a diacritic OUTSIDE
				 * U+0300..U+036F (table index 30 = U+0483); the placeholder
				 * resolver must honour the whole rowcolumn table, not just the
				 * Greek block, or the image never resolves */
				{
					uint8_t gpx[6 * 12 * 4];
					char gb64[512];
					uint8_t ph9[] = {
						0x1B, '[', '3', '8', ';', '2', ';', '0',
						';', '0', ';', '7', 'm',
						0xF4, 0x8E, 0xBB, 0xAE, /* U+10EEEE */
						0xCC, 0x85, 0xCC, 0x85, 0xD2, 0x83, /* d(0)d(0)d(30)=U+0483 */
						'\n',
						0x1B, '[', '3', '9', 'm',
					};

					for (int gi = 0; gi < 6 * 12; gi++) {
						gpx[gi * 4 + 0] = 0;
						gpx[gi * 4 + 1] = 255;
						gpx[gi * 4 + 2] = 0;
						gpx[gi * 4 + 3] = 255;
					}
					st_b64enc(gpx, sizeof gpx, gb64);
					expect(u_setup(&term) == 0, "W9 alloc");
					term.fd = rp[1];
					term.color_pair.bg = 0;
					sq = w_apc("i=503316487,a=T,U=1,f=32,t=d,s=6,v=12,c=1,r=1", gb64);
					parse(&term, sq, (int) strlen((const char *) sq));
					free(sq);
					expect(w_drain(rp[0], got, sizeof got) > 0, "W9 drained");
					expect(strstr(got, "\033_Gi=503316487;OK") != NULL, "W9 OK reply");
					parse(&term, ph9, (int) sizeof ph9);
					expect(w_px(&term, 0, 0)->has_pixmap
							&& w_px(&term, 0, 0)->pixmap[1] == 0xFF
							&& w_px(&term, 0, 0)->pixmap[0] == 0x00
							&& w_px(&term, 0, 0)->pixmap[2] == 0x00,
						"W9 id high byte 30 resolves");
					u_teardown(&term);
				}

				/* W10: every store-release path (single delete,
				 * delete-all, grab reset) must vacate a slot with a
				 * NULL img pointer; the next transfer's kg_store_put
				 * reclaims that slot and must free(NULL), not a
				 * dangling pointer (double-free under ASAN before
				 * the fix) */
				{
					uint8_t ipx[4 * 4 * 4];
					char ib64[512];

					memset(ipx, 0xAB, sizeof ipx);
					st_b64enc(ipx, sizeof ipx, ib64);
					expect(u_setup(&term) == 0, "W10 alloc");
					term.fd = rp[1];
					term.color_pair.bg = 0;

					/* store id 1, then single-delete it */
					sq = w_apc("i=1,a=T,U=1,f=32,t=d,s=4,v=4,c=1,r=1", ib64);
					parse(&term, sq, (int) strlen((const char *) sq));
					free(sq);
					expect(w_drain(rp[0], got, sizeof got) > 0, "W10 drained");
					expect(strstr(got, "\033_Gi=1;OK") != NULL, "W10 stored");
					sq = w_apc("i=1,a=d,d=i", "");
					parse(&term, sq, (int) strlen((const char *) sq));
					free(sq);
					expect(w_drain(rp[0], got, sizeof got) > 0, "W10 del drained");
					expect(strstr(got, "\033_Gi=1;OK") != NULL, "W10 single delete ok");

					/* the reclaimed slot receives the next transfer */
					sq = w_apc("i=2,a=T,U=1,f=32,t=d,s=4,v=4,c=1,r=1", ib64);
					parse(&term, sq, (int) strlen((const char *) sq));
					free(sq);
					expect(w_drain(rp[0], got, sizeof got) > 0, "W10 rec drained");
					expect(strstr(got, "\033_Gi=2;OK") != NULL, "W10 reclaim ok");

					/* delete-all (silent without i=), then reclaim */
					sq = w_apc("a=d", "");
					parse(&term, sq, (int) strlen((const char *) sq));
					free(sq);
					sq = w_apc("i=3,a=T,U=1,f=32,t=d,s=4,v=4,c=1,r=1", ib64);
					parse(&term, sq, (int) strlen((const char *) sq));
					free(sq);
					expect(w_drain(rp[0], got, sizeof got) > 0, "W10 ra drained");
					expect(strstr(got, "\033_Gi=3;OK") != NULL, "W10 reclaim after delete-all ok");

					/* grab reset frees the store; transfer lands clean */
					kitty_grab_reset(&term);
					sq = w_apc("i=4,a=T,U=1,f=32,t=d,s=4,v=4,c=1,r=1", ib64);
					parse(&term, sq, (int) strlen((const char *) sq));
					free(sq);
					expect(w_drain(rp[0], got, sizeof got) > 0, "W10 rs drained");
					expect(strstr(got, "\033_Gi=4;OK") != NULL, "W10 reclaim after grab reset ok");
					u_teardown(&term);
				}
			}
			close(rp[1]);
			rp[1] = -1;
			close(rp[0]);
			rp[0] = -1;
		}
		glyph_mmap_die();
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	fprintf(stderr, "all selftests passed\n");
	return 0;
}
