/* Disk tier of the scrollback (Calculinux/cruft). Holds the rows that
 * fell out of the RAM ring in a session-scoped temp file in /var/tmp
 * (mkstemp, 0600, unlinked on detach): one variable-length record per
 * row, an in-RAM dir of record offsets for O(1) browsing, and lazy
 * compaction of the oldest quarter when the file hits its cap.
 *
 * Cells carry a glyph POINTER, which is not persistable: a row
 * serializes as (fg, bg, attr, width, bitmap) and decodes through an
 * intern table, so cached rows share one glyph block per distinct
 * (width, bitmap) pair. Any unreadable file degrades the tier (the RAM
 * tier keeps serving, the file is dropped) - history is disposable, a
 * crash is not. On-disk layout is little-endian. */
#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SB_MAGIC    0x42534352u
#define SB_VERSION  1u
#define SBDISK_HDRLEN 20u                            /* file header, bytes */
#define SBDISK_CELL   (size_t)(4 + 2 * GLYPH_MAX_H)  /* packed cell, bytes */
#define SBDISK_TMPL   "/var/tmp/cruft-sb.XXXXXX"

struct sb_file_hdr {
	uint32_t magic;
	uint32_t version;
	uint16_t cols, cell_h, cell_w, res;
	uint32_t cap;
};

struct sbrec_hdr {
	uint32_t total;  /* full record length, including this header */
	uint32_t ncols;
};

struct sbdisk_intern_t {
	uint8_t width;               /* glyph_width enum of the owning cell */
	uint16_t bitmap[GLYPH_MAX_H];
	struct glyph_t *g;           /* owned block (survives rehash) */
};

/* --- low-level: full-size transfers; partial I/O is an error --------- */

static int sbd_wr(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0)
			return -1;
		p += (size_t)w;
		n -= (size_t)w;
	}
	return 0;
}

static int sbd_rd(int fd, void *buf, size_t n, uint64_t off)
{
	uint8_t *p = buf;

	while (n) {
		ssize_t r = pread(fd, p, n, (off_t)off);
		if (r <= 0)
			return -1;
		p += (size_t)r;
		off += (uint64_t)r;
		n -= (size_t)r;
	}
	return 0;
}

/* Largest record a row of this width can serialize to (used as the
 * corruption bound when reading lengths back). */
static uint64_t sbd_max_record(const struct terminal_t *term)
{
	return sizeof(struct sbrec_hdr) + (uint64_t)term->cols * SBDISK_CELL
	       + 2 + (uint64_t)term->cols * (6 + cell_pixmap_bytes());
}

static uint64_t sbd_min_record(const struct terminal_t *term)
{
	return sizeof(struct sbrec_hdr) + (uint64_t)term->cols * SBDISK_CELL + 2;
}

/* Width-sized blank row (see sbd_blank); session-scoped, dropped in
 * sbd_teardown. */
static struct cell_t *sbd_blk;

/* --- teardown --------------------------------------------------------- */

static void sbd_teardown(const struct terminal_t *term, struct sbdisk_t *sd, const char *err)
{
	/* Kept across resets (term_init runs reset() after sb_init): the
	 * tier's CAPACITY is configuration; only the acquisitions clear. */
	unsigned cap = sd->cap;

	for (unsigned i = 0; i < SBDISK_CACHE_ROWS; i++)
		if (sd->cache[i].cells) {
			for (int x = 0; x < term->cols; x++)
				cell_pixmap_clear(&sd->cache[i].cells[x]);
			free(sd->cache[i].cells);
			sd->cache[i].cells = NULL;
		}
	for (size_t i = 0; i < sd->glyph_alloc; i++)
		free(sd->glyphs[i].g);
	free(sd->glyphs);
	free(sd->off);
	if (sd->fd >= 0) {
		close(sd->fd);
		if (sd->path[0])
			unlink(sd->path);
	}
	memset(sd, 0, sizeof *sd);
	sd->cap = cap;
	sd->fd = -1;
	free(sbd_blk);
	sbd_blk = NULL;
	if (err)
		fprintf(stderr, "cruft: scrollback-disk: %s\n", err);
}

/* Clean shutdown: same teardown, no message, tier silently drops. */
void sbdisk_detach(struct terminal_t *term)
{
	sbd_teardown(term, &term->sb.disk, NULL);
}

/* Corrupted/unwritable tier: log once, drop the file, keep the RAM -
 * and disarm the tier for the session (capacity cleared). */
static void sbd_degrade(struct terminal_t *term, const char *why)
{
	sbd_teardown(term, &term->sb.disk, why);
	term->sb.disk.cap = 0;
}

/* --- intern table ----------------------------------------------------- */

static uint32_t sbd_key_hash(uint8_t w, const uint16_t *bm)
{
	uint32_t h = 2166136261u;

	for (int i = 0; i < GLYPH_MAX_H; i++) {
		h = (h ^ (uint8_t)(bm[i])) * 16777619u;
		h = (h ^ (uint8_t)(bm[i] >> 8)) * 16777619u;
	}
	return (h ^ w) * 16777619u;
}

static int sbd_intern_grow(struct sbdisk_t *sd, size_t ncap)
{
	struct sbdisk_intern_t *nt;

	nt = (struct sbdisk_intern_t *) calloc(ncap, sizeof *nt);
	if (!nt)
		return -1;
	for (size_t i = 0; i < sd->glyph_alloc; i++) {
		size_t j = sbd_key_hash(sd->glyphs[i].width, sd->glyphs[i].bitmap) & (ncap - 1);

		while (nt[j].g)
			j = (j + 1) & (ncap - 1);
		nt[j] = sd->glyphs[i]; /* the block pointers travel, blocks move never */
	}
	free(sd->glyphs);
	sd->glyphs = nt;
	sd->glyph_alloc = ncap;
	return 0;
}

/* Canonical glyph block for a decoded (width, bitmap); NULL on OOM. */
static struct glyph_t *sbd_intern(struct sbdisk_t *sd, uint8_t w, const uint16_t *bm)
{
	size_t mask, i;

	if (sd->glyph_alloc == 0 || sd->glyph_n * 10 >= sd->glyph_alloc * 7) {
		size_t ncap = sd->glyph_alloc ? sd->glyph_alloc * 2: 16;

		if (sbd_intern_grow(sd, ncap) < 0)
			return NULL;
	}
	mask = sd->glyph_alloc - 1;
	i = sbd_key_hash(w, bm) & mask;
	while (sd->glyphs[i].g && !(sd->glyphs[i].width == w
				    && memcmp(sd->glyphs[i].bitmap, bm, sizeof(sd->glyphs[i].bitmap)) == 0))
		i = (i + 1) & mask;
	if (sd->glyphs[i].g)
		return sd->glyphs[i].g;

	struct glyph_t *g = (struct glyph_t *) calloc(1, sizeof *g);

	if (!g)
		return NULL;
	g->code = 0;
	g->width = (w == WIDE) ? 2: 1;
	memcpy(g->bitmap, bm, sizeof(g->bitmap));
	sd->glyphs[i].width = w;
	memcpy(sd->glyphs[i].bitmap, bm, sizeof(sd->glyphs[i].bitmap));
	sd->glyphs[i].g = g;
	sd->glyph_n++;
	return g;
}

/* --- record codec ------------------------------------------------------ */

/* Pack one row: record header, packed cells, pixmap table. With strip
 * false, a row whose sixel payload outruns the per-record budget
 * returns 1 (almost-all-art rows degrade to blank text on a retry
 * with strip true). 0 on success, -1 on OOM. */
static int sbd_pack(const struct terminal_t *term, const struct cell_t *row,
		    bool strip, uint8_t **out, uint64_t *out_len)
{
	const size_t cell_b = cell_pixmap_bytes();
	uint64_t pix_b = 0, len;
	uint16_t npix = 0;
	uint8_t *buf, *q;
	const uint64_t pix_max = (uint64_t)SB_RECORD_PIXMAP_KB * 1024;

	if (!strip) {
		for (int x = 0; x < term->cols; x++) {
			if (!row[x].has_pixmap || !row[x].pixmap)
				continue;
			if (pix_b + cell_b > pix_max)
				return 1; /* retry stripped */
			pix_b += cell_b;
			npix++;
		}
	}
	len = sizeof(struct sbrec_hdr) + (uint64_t)term->cols * SBDISK_CELL + 2
	      + pix_b + (uint64_t)npix * 6;
	buf = (uint8_t *) malloc(len);
	if (!buf)
		return -1;
	memset(buf, 0, len);
	{
		struct sbrec_hdr rh = { .total = (uint32_t)len,
			.ncols = (uint32_t)term->cols };

		memcpy(buf, &rh, sizeof rh);
	}
	q = buf + sizeof(struct sbrec_hdr);
	for (int x = 0; x < term->cols; x++) {
		const struct cell_t *c = &row[x];

		q[0] = c->color_pair.fg;
		q[1] = c->color_pair.bg;
		q[2] = (uint8_t)c->attribute;
		q[3] = (uint8_t)c->width;
		memcpy(q + 4, c->glyphp->bitmap, sizeof(c->glyphp->bitmap));
		q += SBDISK_CELL;
	}
	memcpy(q, &npix, 2);
	q += 2;
	for (int x = 0; x < term->cols; x++) {
		uint16_t idx = (uint16_t)x;

		if (!row[x].has_pixmap || !row[x].pixmap || !npix)
			continue;
		memcpy(q, &idx, 2);
		memcpy(q + 2, &cell_b, 4);
		q += 6;
		memcpy(q, row[x].pixmap, cell_b);
		q += cell_b;
	}
	*out = buf;
	*out_len = len;
	return 0;
}

/* Blank row handed out when the disk tier cannot serve a line: erased
 * cells, no pixmaps. draw_line indexes term->cols cells, which nothing
 * bounds but framebuffer width / cell width, so the row is built at
 * attach for the real width and dropped with the session on teardown;
 * the fixed static below covers a blank request outside any session.
 * (The OOM-fetch path degrades - dropping the row - before serving
 * the blank, so it lands on the static: fine at real widths, which
 * fit it comfortably.) */
#define SBDISK_BLANK_FALLBACK_COLS 512

static struct cell_t sbd_blk_static[SBDISK_BLANK_FALLBACK_COLS];
static bool sbd_blk_static_ready;

static void sbd_blank_fill(struct cell_t *row, unsigned cols)
{
	for (unsigned x = 0; x < cols; x++) {
		row[x].glyphp = glyph_get(DEFAULT_CHAR);
		row[x].color_pair = (struct color_pair_t) { DEFAULT_FG, DEFAULT_BG };
		row[x].attribute = ATTR_RESET;
		row[x].width = HALF;
		row[x].has_pixmap = false;
		row[x].pixmap = NULL;
	}
}

static const struct cell_t *sbd_blank(const struct terminal_t *term)
{
	if (sbd_blk)
		return sbd_blk;
	if (!sbd_blk_static_ready) {
		sbd_blank_fill(sbd_blk_static, SBDISK_BLANK_FALLBACK_COLS);
		sbd_blk_static_ready = true;
	}
	(void)term;
	return sbd_blk_static;
}

/* --- attach / store / compact ------------------------------------------ */

void sbdisk_attach(struct terminal_t *term)
{
	struct sbdisk_t *sd = &term->sb.disk;
	char tmpl[] = SBDISK_TMPL;
	struct sb_file_hdr hdr;

	if (sd->fd >= 0 || sd->cap == 0)
		return;
	sd->fd = mkstemp(tmpl);
	if (sd->fd < 0) {
		/* No writable /var/tmp: the tier is off for the session
		 * (retrying would re-log on every spilled row). */
		sbd_degrade(term, "cannot open history file");
		return;
	}
	snprintf(sd->path, sizeof(sd->path), "%s", tmpl);
	hdr.magic = SB_MAGIC;
	hdr.version = SB_VERSION;
	hdr.cols = (uint16_t)term->cols;
	hdr.cell_h = (uint16_t)g_cell_h;
	hdr.cell_w = (uint16_t)g_cell_w;
	hdr.res = 0;
	hdr.cap = sd->cap;
	if (sbd_wr(sd->fd, &hdr, SBDISK_HDRLEN) < 0) {
		sbd_degrade(term, "cannot write history header");
		return;
	}
	sd->file_pos = SBDISK_HDRLEN;
	if (sbd_intern_grow(sd, 16) < 0) {
		sbd_degrade(term, "out of memory");
		return;
	}
	sd->off = (uint64_t *) calloc(32, sizeof(uint64_t));
	if (!sd->off) {
		sbd_degrade(term, "out of memory");
		return;
	}
	sd->off_alloc = 32;
	sbd_blk = (struct cell_t *) malloc((size_t)term->cols * sizeof *sbd_blk);
	if (!sbd_blk) {
		sbd_degrade(term, "out of memory");
		return;
	}
	sbd_blank_fill(sbd_blk, term->cols);
	sd->evicted = 0;
	sd->epoch = 0;
}

static int sbd_dir_grow(struct sbdisk_t *sd, size_t need)
{
	size_t ncap = sd->off_alloc ? sd->off_alloc: 16;
	uint64_t *n;

	while (ncap < need)
		ncap *= 2;
	n = (uint64_t *) realloc(sd->off, ncap * sizeof(uint64_t));
	if (!n)
		return -1;
	sd->off = n;
	sd->off_alloc = ncap;
	return 0;
}

/* Rewrite the file keeping the NEWEST three quarters (the oldest
 * quarter is dropped), so appends stay O(1) at the tail. Runs only on
 * the store path when the file is full; the rewrite is a bounded
 * burst (worst case tens of MiB at the 16384-row ceiling, a few MB
 * at everyday depths), amortized over a quarter-cap of output rows. */
static int sbd_compact(struct terminal_t *term)
{
	struct sbdisk_t *sd = &term->sb.disk;
	char tmpl[] = SBDISK_TMPL;
	int nfd;
	uint64_t np;
	struct sb_file_hdr hdr;
	uint32_t drop = sd->cap / 4;

	if (drop < 1)
		drop = 1;
	nfd = mkstemp(tmpl);
	if (nfd < 0) {
		sbd_degrade(term, "history compact failed");
		return -1;
	}
	hdr.magic = SB_MAGIC;
	hdr.version = SB_VERSION;
	hdr.cols = (uint16_t)term->cols;
	hdr.cell_h = (uint16_t)g_cell_h;
	hdr.cell_w = (uint16_t)g_cell_w;
	hdr.res = 0;
	hdr.cap = sd->cap;
	if (sbd_wr(nfd, &hdr, SBDISK_HDRLEN) < 0)
		goto fail_new;
	np = SBDISK_HDRLEN;
	for (uint32_t i = drop; i < sd->count; i++) {
		uint32_t rl;
		uint8_t *rec;

		if (sbd_rd(sd->fd, &rl, 4, sd->off[i]) < 0
		    || rl < sbd_min_record(term) || rl > sbd_max_record(term))
			goto fail_new;
		rec = (uint8_t *) malloc(rl);
		if (!rec)
			goto fail_new;
		if (sbd_rd(sd->fd, rec, rl, sd->off[i]) < 0) {
			free(rec);
			goto fail_new;
		}
		if (sbd_wr(nfd, rec, rl) < 0) {
			free(rec);
			goto fail_new;
		}
		/* The record now lives at a NEW offset in the rewritten
		 * file: rebuild the dir as we copy (a memmove of the old
		 * offsets would keep dead addresses). */
		sd->off[i - drop] = np;
		free(rec);
		np += rl;
	}
	close(sd->fd);
	unlink(sd->path);
	if (rename(tmpl, sd->path) < 0) {
		close(nfd);
		unlink(tmpl);
		sbd_degrade(term, "history compact failed");
		return -1;
	}
	sd->evicted += drop;
	sd->count -= drop;
	sd->epoch++; /* the cache's generations all shifted */
	sd->file_pos = np;
	sd->fd = nfd;
	return 0;

fail_new:
	close(nfd);
	unlink(tmpl);
	sbd_degrade(term, "history file unreadable");
	return -1;
}

int sbdisk_store(struct terminal_t *term, const struct cell_t *row)
{
	struct sbdisk_t *sd = &term->sb.disk;
	uint8_t *buf = NULL;
	uint64_t len = 0;
	int rc;

	if (sd->cap == 0 || sd->fd < 0) {
		sbdisk_attach(term);
		if (sd->fd < 0)
			return -1;
	}
	rc = sbd_pack(term, row, false, &buf, &len);
	if (rc == 1)
		rc = sbd_pack(term, row, true, &buf, &len);
	if (rc < 0) {
		/* Packing failed (allocation): disarm like any other disk
		 * fault instead of retrying on every later eviction. */
		sbd_degrade(term, "out of memory");
		return -1;
	}
	if (sd->count >= sd->cap && sbd_compact(term) < 0) {
		free(buf);
		return -1;
	}
	if (sbd_dir_grow(sd, sd->count + 1) < 0) {
		sbd_degrade(term, "out of memory");
		free(buf);
		return -1;
	}
	sd->off[sd->count] = sd->file_pos;
	if (sbd_wr(sd->fd, buf, (size_t)len) < 0) {
		sbd_degrade(term, "history write failed");
		free(buf);
		return -1;
	}
	free(buf);
	sd->file_pos += len;
	sd->count++;
	return 0;
}

/* --- fetch ------------------------------------------------------------- */

const struct cell_t *sbdisk_fetch(struct terminal_t *term, unsigned k)
{
	struct sbdisk_t *sd = &term->sb.disk;
	const size_t cell_b = cell_pixmap_bytes();

	if (sd->fd < 0 || k >= sd->count)
		return sbd_blank(term);
	{
		uint32_t i = (uint32_t)(sd->count - 1 - k);
		uint64_t gen = sd->evicted + i;

		for (unsigned c = 0; c < SBDISK_CACHE_ROWS; c++)
			if (sd->cache[c].cells
			    && sd->cache[c].gen == gen
			    && sd->cache[c].epoch == sd->epoch)
				return sd->cache[c].cells;

		uint32_t rl = 0;
		uint8_t *rec;
		struct cell_t *cells;

		if (sbd_rd(sd->fd, &rl, 4, sd->off[i]) < 0
		    || rl < sbd_min_record(term) || rl > sbd_max_record(term))
			goto corrupt;
		rec = (uint8_t *) malloc(rl);
		if (!rec)
			goto oom;
		if (sbd_rd(sd->fd, rec, rl, sd->off[i]) < 0) {
			free(rec);
			goto corrupt;
		}
		cells = (struct cell_t *) calloc((size_t)term->cols, sizeof *cells);
		if (!cells) {
			free(rec);
			goto oom;
		}
		{
			const uint8_t *p = rec + 8;
			struct sbrec_hdr rh;
			uint16_t npix;
			const uint8_t *px;
			size_t rem;

			memcpy(&rh, rec, sizeof rh);
			if (rh.ncols != (uint32_t)term->cols) {
				free(cells);
				free(rec);
				goto corrupt;
			}
			for (int x = 0; x < term->cols; x++) {
				struct cell_t *c = &cells[x];
				uint8_t w = p[3];
				uint16_t bm[GLYPH_MAX_H];

				memcpy(bm, p + 4, sizeof bm);
				c->color_pair.fg = p[0];
				c->color_pair.bg = p[1];
				c->attribute = (enum char_attr)p[2];
				c->width = (enum glyph_width)w;
				c->has_pixmap = false;
				c->pixmap = NULL;
				c->glyphp = sbd_intern(sd, w, bm);
				if (!c->glyphp) {
					free(cells);
					free(rec);
					goto oom;
				}
				p += SBDISK_CELL;
			}
			memcpy(&npix, p, 2);
			p += 2;
			rem = rl - (size_t)(sizeof(struct sbrec_hdr)
					    + (size_t)term->cols * SBDISK_CELL + 2);
			px = p;
			if ((uint64_t)npix > (uint64_t)term->cols)
				goto corrupt_cells;
			for (uint16_t j = 0; j < npix; j++) {
				uint16_t idx;
				uint32_t nb;

				if (rem < 6)
					goto corrupt_cells;
				memcpy(&idx, px, 2);
				memcpy(&nb, px + 2, 4);
				if (idx >= (uint16_t)term->cols || nb != (uint32_t)cell_b
				    || rem < 6 + (size_t)nb)
					goto corrupt_cells;
				if (!cell_pixmap_ensure(&cells[idx]))
					goto corrupt_cells;
				memcpy(cells[idx].pixmap, px + 6, cell_b);
				cells[idx].has_pixmap = true;
				px += 6 + (size_t)nb;
				rem -= 6 + (size_t)nb;
			}
		}
		free(rec);
		{
			/* Round-robin cache slot (browsing walks
			 * sequentially, so LRU precision buys nothing). */
			unsigned slot = (unsigned)(sd->cache_tick++) % SBDISK_CACHE_ROWS;
			struct cell_t *victim = sd->cache[slot].cells;

			if (victim) {
				for (int x = 0; x < term->cols; x++)
					cell_pixmap_clear(&victim[x]);
				free(victim);
			}
			sd->cache[slot].cells = cells;
			sd->cache[slot].gen = gen;
			sd->cache[slot].epoch = sd->epoch;
		}
		return cells;
corrupt_cells:
		for (int x = 0; x < term->cols; x++)
			cell_pixmap_clear(&cells[x]);
		free(cells);
		free(rec);
corrupt:
		sbd_degrade(term, "history file unreadable");
oom:
		sbd_degrade(term, "out of memory");
		return sbd_blank(term);
	}
}
