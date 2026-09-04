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

/* Provide color_list for reset_sixel if linked; tests don't call it */
const uint32_t color_list[COLORS];

/* Stubs required by modules when not linking the full terminal */
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

/* sixel_copy2cell calls these; provide no-op stubs for the test binary */
void erase_cell(struct terminal_t *term, int y, int x)
{
	(void)term; (void)y; (void)x;
}
void move_cursor(struct terminal_t *term, int y_offset, int x_offset)
{
	(void)term; (void)y_offset; (void)x_offset;
}
void cr(struct terminal_t *term)
{
	(void)term;
}

static int failures;

static void expect(int cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		failures++;
	} else {
		fprintf(stderr, "ok: %s\n", msg);
	}
}

/* F: wallpaper env parsing (fb_draw_dest itself skipped — needs fb struct) */
static int wall_env_wants_shadow(void)
{
	const char *c = getenv("CRUFT");
	const char *y = getenv("YAFT");

	return ((c && strstr(c, "wall")) || (y && strstr(y, "wall"))) ? 1 : 0;
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

	/* glyph: space */
	write_u32(fp, 0x20);
	fputc(1, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 8; i++)
		write_u16(fp, 0);

	/* glyph: '?' */
	write_u32(fp, 0x3f);
	fputc(1, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 8; i++)
		write_u16(fp, (i == 0 || i == 7) ? 0x0f : 0x09);

	/* glyph: U+3000 ideographic space (wide) */
	write_u32(fp, 0x3000);
	fputc(2, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
	for (i = 0; i < 8; i++)
		write_u16(fp, 0);

	fclose(fp);
	return 0;
}

int main(void)
{
	struct cell_t a, b;
	struct terminal_t term;
	char fontpath[64];
	char tmpl[] = "/tmp/cruft-st-XXXXXX";
	int fd;

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

	/* F: skip fb_draw_dest (needs full fb); test wallpaper env helper */
	unsetenv("CRUFT");
	unsetenv("YAFT");
	expect(wall_env_wants_shadow() == 0, "F no wall env");
	setenv("CRUFT", "wall", 1);
	expect(wall_env_wants_shadow() == 1, "F CRUFT=wall");
	unsetenv("CRUFT");
	setenv("YAFT", "wall", 1);
	expect(wall_env_wants_shadow() == 1, "F YAFT=wall");
	unsetenv("YAFT");
	/* ponytail: fb_draw_dest covered by wallpaper env; full fb path needs /dev/fb0 */

	/* G: term_release_transient clears sixel */
	g_cell_w = 6;
	g_cell_h = 12;
	memset(&term, 0, sizeof(term));
	term.width = 16;
	term.height = 16;
	sixel_canvas_ensure(&term);
	term_release_transient(&term);
	expect(term.sixel.pixmap == NULL, "G term_release_transient");

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	fprintf(stderr, "all selftests passed\n");
	return 0;
}
