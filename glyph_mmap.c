/* Demand-paged cruft font loader (CRUFTFNT1; see tools/mkcruftfont.c). */
#define _XOPEN_SOURCE 600
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

#include "glyph.h"

#define UCS2_CHARS 0x10000
#define CRUFTFONT_MAGIC "CRUFTFNT1"
#define DEFAULT_FONT_PATH "/usr/share/cruft/console.cruftfont"
#define PAGE_SHIFT_EXPECT 8
#define PAGE_COUNT 256
#define HEADER_MIN (8 + 16 + PAGE_COUNT * 4)

int g_cell_w;
int g_cell_h;

struct cruftfont_map {
	uint8_t *map;
	size_t size;
	const uint32_t *page_off;
	uint32_t glyph_bytes;
	uint32_t page_shift;
	struct glyph_t tofu_half;
	struct glyph_t tofu_wide;
};

static struct cruftfont_map gfont;

static void make_tofu(struct glyph_t *g, uint32_t code, uint8_t width)
{
	int cw = g_cell_w;
	int ch = g_cell_h;
	uint16_t edge = (uint16_t)((1u << cw) - 1u);
	uint16_t full = (uint16_t)((1u << (cw * 2)) - 1u);

	memset(g, 0, sizeof(*g));
	g->code = code;
	g->width = width;
	for (int h = 0; h < ch; h++) {
		if (width == 1) {
			if (h == 0 || h == ch - 1)
				g->bitmap[h] = edge;
			else
				g->bitmap[h] = (uint16_t)(1u | (1u << (cw - 1)));
		} else {
			if (h == 0 || h == ch - 1)
				g->bitmap[h] = full;
			else
				g->bitmap[h] = (uint16_t)((1u << (cw * 2 - 1)) | 1u);
		}
	}
}

static uint32_t read_u32_le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int metrics_ok(uint32_t cell_w, uint32_t cell_h, uint32_t glyph_bytes)
{
	/* glyph_bytes = 8 + cell_h * 2 (code/width/pad + uint16 rows) */
	if (cell_w == 4 && cell_h == 8 && glyph_bytes == 24)
		return 1; /* miniwi */
	if (cell_w == 6 && cell_h == 12 && glyph_bytes == 32)
		return 1; /* console default */
	if (cell_w == 8 && cell_h == 16 && glyph_bytes == 40)
		return 1; /* unifont */
	return 0;
}

int glyph_mmap_init(const char *path)
{
	int fd;
	struct stat st;
	const uint8_t *p;
	uint32_t cell_w, cell_h, glyph_bytes, page_shift;

	if (!path || !path[0])
		path = getenv("CRUFT_FONT");
	if (!path || !path[0])
		path = getenv("YAFT_FONT"); /* alias */
	if (!path || !path[0])
		path = DEFAULT_FONT_PATH;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "cruft: open %s: %s\n", path, strerror(errno));
		return 0;
	}
	if (fstat(fd, &st) < 0 || st.st_size < (off_t)HEADER_MIN) {
		fprintf(stderr, "cruft: bad font file %s\n", path);
		close(fd);
		return 0;
	}
	gfont.map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (gfont.map == MAP_FAILED) {
		gfont.map = NULL;
		fprintf(stderr, "cruft: mmap %s failed\n", path);
		return 0;
	}
	gfont.size = (size_t)st.st_size;
	p = gfont.map;
	if (memcmp(p, CRUFTFONT_MAGIC, 8) != 0) {
		fprintf(stderr, "cruft: bad magic in %s\n", path);
		glyph_mmap_die();
		return 0;
	}
	cell_w = read_u32_le(p + 8);
	cell_h = read_u32_le(p + 12);
	glyph_bytes = read_u32_le(p + 16);
	page_shift = read_u32_le(p + 20);
	if (page_shift != PAGE_SHIFT_EXPECT) {
		fprintf(stderr, "cruft: unsupported page_shift %u (want %u)\n",
			page_shift, PAGE_SHIFT_EXPECT);
		glyph_mmap_die();
		return 0;
	}
	if (!metrics_ok(cell_w, cell_h, glyph_bytes)) {
		fprintf(stderr,
			"cruft: unsupported font metrics %ux%u/%u (want 4x8/24, 6x12/32, or 8x16/40)\n",
			cell_w, cell_h, glyph_bytes);
		glyph_mmap_die();
		return 0;
	}
	g_cell_w = (int)cell_w;
	g_cell_h = (int)cell_h;
	gfont.glyph_bytes = glyph_bytes;
	gfont.page_shift = page_shift;
	gfont.page_off = (const uint32_t *)(p + 24);
	make_tofu(&gfont.tofu_half, 0x003F, 1);
	make_tofu(&gfont.tofu_wide, 0x3000, 2);

	if (!glyph_lookup(0x0020) || !glyph_lookup(0x3000) || !glyph_lookup(0x003F)) {
		fprintf(stderr, "cruft: essential glyphs missing from font\n");
		glyph_mmap_die();
		return 0;
	}
	return 1;
}

void glyph_mmap_die(void)
{
	if (gfont.map && gfont.map != MAP_FAILED)
		munmap(gfont.map, gfont.size);
	memset(&gfont, 0, sizeof(gfont));
	g_cell_w = 0;
	g_cell_h = 0;
}

const struct glyph_t *glyph_lookup(uint32_t code)
{
	uint32_t page, idx, page_off, off;
	const uint8_t *sub;

	if (code >= UCS2_CHARS || !gfont.map || !gfont.page_off)
		return NULL;

	page = code >> gfont.page_shift;
	idx = code & ((1u << gfont.page_shift) - 1u);
	if (page >= PAGE_COUNT)
		return NULL;

	page_off = gfont.page_off[page];
	if (page_off == 0 || page_off + PAGE_COUNT * 4 > gfont.size)
		return NULL;

	sub = gfont.map + page_off;
	off = read_u32_le(sub + idx * 4);
	if (off == 0 || off + gfont.glyph_bytes > gfont.size)
		return NULL;

	return (const struct glyph_t *)(gfont.map + off);
}

const struct glyph_t *glyph_get(uint32_t code)
{
	const struct glyph_t *g = glyph_lookup(code);
	int width;

	if (g)
		return g;

	width = wcwidth((wchar_t)code);
	if (width >= 2)
		return &gfont.tofu_wide;
	return &gfont.tofu_half;
}
