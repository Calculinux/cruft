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
#include <unistd.h>

#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"
#include "util.h"
#include "fb/common.h"

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

/* sixel_copy2cell also calls these (scroll/CR not under test here) */
void move_cursor(struct terminal_t *term, int y_offset, int x_offset)
{
	(void)term; (void)y_offset; (void)x_offset;
}
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
	int i, j;

	if (!term->cells)
		return;
	for (i = 0; i < term->lines; i++) {
		if (term->cells[i]) {
			for (j = 0; j < term->cols; j++)
				cell_pixmap_clear(&term->cells[i][j]);
			free(term->cells[i]);
		}
	}
	free(term->cells);
	term->cells = NULL;
	free(term->line_dirty);
	term->line_dirty = NULL;
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

static int alloc_term_grid(struct terminal_t *term, int width, int height)
{
	int i;

	memset(term, 0, sizeof(*term));
	term->width = width;
	term->height = height;
	term->cols = width / CELL_WIDTH;
	term->lines = height / CELL_HEIGHT;
	if (term->cols < 1 || term->lines < 1)
		return -1;
	term->line_dirty = calloc((size_t)term->lines, sizeof(bool));
	term->cells = calloc((size_t)term->lines, sizeof(struct cell_t *));
	if (!term->line_dirty || !term->cells)
		return -1;
	for (i = 0; i < term->lines; i++) {
		term->cells[i] = calloc((size_t)term->cols, sizeof(struct cell_t));
		if (!term->cells[i])
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
	expect(alloc_term_grid(&term, 36, 24) == 0, "G alloc grid");
	sixel_canvas_ensure(&term);
	expect(cell_pixmap_ensure(&term.cells[0][0]) != NULL, "G cell pixmap");
	term.cells[0][0].pixmap[0] = 0x5A;
	term_release_transient(&term);
	expect(term.sixel.pixmap == NULL, "G release frees canvas");
	expect(term.cells[0][0].pixmap && term.cells[0][0].pixmap[0] == 0x5A,
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
	expect(alloc_term_grid(&term, 18, 12) == 0, "J alloc");
	expect(cell_pixmap_ensure(&term.cells[0][0]) != NULL, "J ensure src");
	term.cells[0][0].pixmap[0] = 0xC1;
	term.cells[0][0].width = HALF;
	term.cells[0][0].glyphp = glyph_get(DEFAULT_CHAR);
	copy_cell(&term, 0, 1, 0, 0);
	expect(term.cells[0][1].has_pixmap && term.cells[0][1].pixmap, "J dst has pixmap");
	expect(term.cells[0][1].pixmap != term.cells[0][0].pixmap, "J deep copy");
	expect(term.cells[0][1].pixmap[0] == 0xC1, "J content");
	cell_pixmap_clear(&term.cells[0][0]);
	expect(term.cells[0][1].pixmap[0] == 0xC1, "J src clear leaves dst");
	free_term_cells(&term);

	/* K: sixel_copy2cell must not read past canvas when cols is ceil(width/cw)
	 * (PicoCalc-style 320%6!=0). Force cols=54 while width=320. */
	g_cell_w = 6;
	g_cell_h = 12;
	memset(&term, 0, sizeof(term));
	term.width = 320;
	term.height = 24;
	term.cols = 54; /* ceil(320/6); wider than width/CELL_WIDTH */
	term.lines = 2;
	term.line_dirty = calloc(2, sizeof(bool));
	term.cells = calloc(2, sizeof(struct cell_t *));
	expect(term.line_dirty && term.cells, "K meta");
	term.cells[0] = calloc(54, sizeof(struct cell_t));
	term.cells[1] = calloc(54, sizeof(struct cell_t));
	expect(term.cells[0] && term.cells[1], "K rows");
	expect(sixel_canvas_ensure(&term) == 0, "K canvas");
	canvas_bytes = (size_t)term.width * (size_t)term.height * (size_t)BYTES_PER_PIXEL;
	memset(term.sixel.pixmap, 0xA5, canvas_bytes);
	term.sixel.width = 320;
	term.sixel.height = 12;
	term.sixel.line_length = BYTES_PER_PIXEL * term.width;
	term.cursor.x = 0;
	term.cursor.y = 0;
	sixel_copy2cell(&term, &term.sixel); /* ASAN fails on OOB without row clamp */
	expect(term.cells[0][53].has_pixmap && term.cells[0][53].pixmap,
		"K partial right cell filled");
	lastpx = term.cells[0][53].pixmap;
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
			uint8_t *mfp;

			close(fdf);
			snprintf(mpfont, sizeof(mpfont), "%s.cruftfont", tmfm);
			expect(write_tiny_font6x12(mpfont) == 0, "M write 6x12 font");
			expect(glyph_mmap_init(mpfont) == 1, "M glyph_mmap_init 6x12");
			unlink(mpfont);

			memset(&term, 0, sizeof(term));
			expect(alloc_term_grid(&term, M_W, M_H) == 0, "M alloc 53x26");
			expect(term.cols == 53 && term.lines == 26, "M PicoCalc grid 53x26");

			/* populate cells: row 0 bg=1 (distinct from margin color 0),
			 * other rows bg=0; '?' marker at (0,5) */
			for (int y = 0; y < term.lines; y++) {
				for (int x = 0; x < term.cols; x++) {
					struct cell_t *c = &term.cells[y][x];

					c->glyphp = (y == 0 && x == 5) ? glyph_get(0x3f): glyph_get(0x20);
					c->color_pair.fg = 7;
					c->color_pair.bg = (y == 0) ? 1: 0;
					c->width = HALF;
				}
				term.line_dirty[y] = true;
			}
			term.mode = MODE_AMRIGHT; /* no cursor: keep assertions plain */
			term.palette_modified = false;
			term.cursor.x = term.cursor.y = 0;

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

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	fprintf(stderr, "all selftests passed\n");
	return 0;
}
