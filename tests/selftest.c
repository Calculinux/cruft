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
#include <sys/stat.h>
#include <unistd.h>

#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"
#include "util.h"
#include "fb/common.h"
#include "status.h"
#include "terminal.h"

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

/* cr() lives in ctrlseq/esc.h (not linked here); move_cursor/scroll/etc.
 * are the real terminal.h definitions. */
void cr(struct terminal_t *term)
{
	(void)term;
}

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
	 * mapping, Meta-nav keys, RIS reset, pixmap payload cap. */
	g_cell_w = 6;
	g_cell_h = 12;
	{
		struct screen_t *tscr;
		const struct glyph_t *qm = glyph_get(0x3f);
		const uint8_t bare_up[]   = {ESC, '[', 'A'};
		const uint8_t meta_up[]   = {ESC, ESC, '[', 'A'};
		const uint8_t ss3_down[]  = {ESC, ESC, 'O', 'B'};
		const uint8_t meta_pgup[] = {ESC, ESC, '[', '5', '~'};
		const uint8_t meta_home[] = {ESC, ESC, '[', '7', '~'};
		const uint8_t meta_end[]  = {ESC, ESC, '[', '8', '~'};
		const uint8_t mixed[]     = {ESC, ESC, '[', 'A', 'x'};

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

		/* navigation keys: Meta-prefixed only, leading-run consumption */
		expect(sb_consume_nav(&term, bare_up, 3) == 0, "T bare arrow passes through");
		expect(sb_consume_nav(&term, meta_up, 4) == 4 && term.sb.view == 1,
			"T meta-up browses deeper");
		expect(sb_consume_nav(&term, ss3_down, 4) == 4 && term.sb.view == 0,
			"T SS3 down back to live");
		expect(sb_consume_nav(&term, meta_pgup, 5) == 5 && term.sb.view == 2,
			"T pgup steps a half page");
		expect(sb_consume_nav(&term, meta_home, 5) == 5 && term.sb.view == 4,
			"T home jumps to oldest");
		expect(sb_consume_nav(&term, meta_end, 5) == 5 && term.sb.view == 0,
			"T end jumps to live tail");
		term.sb.view = 3;
		expect(sb_consume_nav(&term, mixed, 5) == 4 && term.sb.view == 0,
			"T nav run then foreign byte snaps to live");
		{
			const uint8_t plain = 'x';

			term.sb.view = 2;
			expect(sb_consume_nav(&term, &plain, 1) == 0 && term.sb.view == 0,
				"T plain key while browsing snaps to live");
		}

		/* RIS resets the ring; an empty ring lets nav pass through */
		reset(&term);
		expect(term.sb.count == 0 && term.sb.view == 0, "T RIS empties ring");
		expect(sb_consume_nav(&term, meta_up, 4) == 0,
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
		 * the right edge; track palette 8, thumb palette 7. With
		 * count 2 / view 1: thumb height 312*26/28 = 289, offset
		 * (312-289)/2 = 11, so thumb spans y 11..299. */
		memset(&term, 0, sizeof(term));
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
			term.sb.view = 1;
			term.sb.rows = (struct cell_t **) ecalloc(4, sizeof(*term.sb.rows));
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
			expect(m_px_at(mfp, 318, 5) == 0x08080808u, "T bar track (above thumb)");
			expect(m_px_at(mfp, 319, 100) == 0x07070707u, "T bar thumb");
			expect(m_px_at(mfp, 318, 305) == 0x08080808u, "T bar track (below thumb)");
			expect(m_px_at(mfp, 318, 315) == 0x00000000u, "T below grid is margin, not bar");
			free(mfp);
		}
		term_die(&term);
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	fprintf(stderr, "all selftests passed\n");
	return 0;
}
