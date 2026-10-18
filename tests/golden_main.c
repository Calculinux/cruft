/*
 * Golden-pixel harness (L2): feeds scripted VT streams through the real
 * parse()/redraw()/refresh() path into a fabricated 320x320 4bpp
 * truecolor frame (the M/Q selftest fixture convention) and compares
 * the resulting WHOLE frame — plus the exact bytes the product replied
 * on the pty — against committed golden files.
 *
 * Unlike the in-tree selftests (spot checks), this locks the complete
 * pixel map, so any regression in font raster, palette mapping, cell
 * placement, sixel/kitty compositing, margin painting or cursor drawing
 * fails in seconds on a plain host: no /dev/fb0, no pty device, no QEMU.
 *
 * Files per scenario <name>, relative to the repo root (= the working
 * directory under `make test`; CRUFT_GOLDEN_ROOT overrides the root):
 *   tests/goldens/<name>.frame   raw 320x320x4 frame (little endian,
 *                                blue in the low byte; layout
 *                                red=8@16 green=8@8 blue=8@0)
 *   tests/goldens/<name>.reply   exact pty reply bytes, in order
 *
 * Usage:
 *   ./tests/golden                    compare; non-zero exit on drift
 *   UPDATE_GOLDENS=1 ./tests/golden   rewrite .frame/.reply from this build
 *
 * The frame is sentinel-filled before refresh() and full coverage is
 * asserted afterwards: an unpainted pixel is a hard failure, so a
 * golden can never silently bake in an undrawn region.
 *
 * The 6x12 console font is taken verbatim from tests/assets/
 * (sha256 a50ba932...6d7b2711, the console-font package output — the
 * exact bytes the device installs), so glyph pixels are byte-stable
 * across hosts and CI machines alike.
 */
#define _XOPEN_SOURCE 600
#include <stdbool.h>
#include <stdint.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cruft.h"
#include "conf.h"
#include "util.h"
#include "fb/common.h"
#include "status.h"
/* Unity headers, mirrored from cruft.c: the real parse() and the real
 * refresh() are what under test. */
#include "terminal.h"
#include "ctrlseq/esc.h"
#include "ctrlseq/csi.h"
#include "ctrlseq/osc.h"
#include "ctrlseq/dcs.h"
#include "parse.h"

/* NOTE: no CRUFT_NO_GLOBALS here — this TU wants the REAL color_list
 * from color.h and the runtime globals from cruft.h (both internal
 * linkage, per-TU copies), so real_palette[] matches the device.
 * vt_active is forced on in main() (the product sets it from the
 * VT-switch signals). */

#define GW      320
#define GH      320
#define GBPP    4
#define GSZ     ((size_t) GW * GH * GBPP)
#define SENTINEL 0xFEFEFEFEu
/* Odd chunk size: pty delivery is never chunk-aligned; parse() must
 * behave identically across arbitrary splits. */
#define CHUNK   333
#define REPLY_SZ 8192

static int failures;

static void fail(const char *msg)
{
	failures++;
	fprintf(stderr, "FAIL: %s\n", msg);
}

/* ------------------------------------------------------------------ */
/* stream building                                                    */
/* ------------------------------------------------------------------ */

/* 256 KiB: the frozen live-app trace (kitty-applive.trace: a
 * ~24 KB base64 payload plus the crossterm buffer traffic around
 * it) must sit beside the hand-built scenes in the same arena. */
#define MAXSTREAM (1u << 18)
static uint8_t g_stream[MAXSTREAM];
static size_t g_len;

static void put(const void *p, size_t n)
{
	if (g_len + n > sizeof g_stream) {
		fprintf(stderr, "internal: stream overflow (%zu+%zu)\n", g_len, n);
		exit(2);
	}
	memcpy(g_stream + g_len, p, n);
	g_len += n;
}

static void putlit(const char *s)
{
	put(s, strlen(s));
}

/* Test-local base64 (mirror of the selftest helper). */
static const char st_b64a[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void st_b64enc(const uint8_t *in, size_t n, char *out, size_t outsz)
{
	size_t o = 0;

	/* hard guard: an undersized out[] must never overflow the stack */
	if (outsz < 4 * (n / 3) + 5) {
		fprintf(stderr, "internal: st_b64enc needs %zu bytes, got %zu\n",
			4 * (n / 3) + 5, outsz);
		exit(2);
	}
	for (size_t i = 0; i + 2 < n; i += 3) {
		uint32_t v = (uint32_t) in[i] << 16 |
				   (uint32_t) in[i + 1] << 8 | in[i + 2];

		out[o++] = st_b64a[(v >> 18) & 63];
		out[o++] = st_b64a[(v >> 12) & 63];
		out[o++] = st_b64a[(v >> 6) & 63];
		out[o++] = st_b64a[v & 63];
	}
	if (n % 3 == 1) {
		uint32_t v = (uint32_t) in[n - 1] << 16;

		out[o++] = st_b64a[(v >> 18) & 63];
		out[o++] = st_b64a[(v >> 12) & 63];
		out[o++] = '=';
		out[o++] = '=';
	} else if (n % 3 == 2) {
		uint32_t v = (uint32_t) in[n - 2] << 16 |
				   (uint32_t) in[n - 1] << 8;

		out[o++] = st_b64a[(v >> 18) & 63];
		out[o++] = st_b64a[(v >> 12) & 63];
		out[o++] = st_b64a[(v >> 6) & 63];
		out[o++] = '=';
	}
	out[o] = '\0';
}

/* Kitty graphics request: ESC _ G ctrl [ ';' payload ] ST — the byte
 * form the W selftests validate against the product. */
static void kg(const char *ctrl, const char *payload)
{
	putlit("\033_G");
	putlit(ctrl);
	if (payload[0]) {
		putlit(";");
		putlit(payload);
	}
	putlit("\033\\");
}

/* ------------------------------------------------------------------ */
/* scenarios                                                          */
/* ------------------------------------------------------------------ */

/* Sixel quadrants: a 24x24 (2x2 cell) square, one RGB colour per
 * quadrant, drawn through DCS q at the grid origin, with text lines
 * below. Cursor stays visible: its final cell is part of the golden.
 *
 * Governance label: (a) app traffic — the stimulus is the classic DEC
 * bitmap sixel form (percent-RGB '#Pc;2;R%;G%;B%' definitions, '!Pn'
 * repeats, '-' newlines) as ratified by the outside world (DEC sixel /
 * ECMA-48 grammar; the sixel.rs emitter of ratatui-image speaks the
 * same dialect; cross-checked against vtdn.dev's sixel page), so the
 * decoded quadrant pixels must track it.
 *
 * Dialect notes (the product implements the DEC bitmap form, see
 * sixel_parse_data): colours are defined with #Pc;2;R%;G%;B% (percent
 * RGB), selected with #Pc; one data char '?'.. '~' is a 6-pixel column
 * ('~' = solid); '-' is the sixel newline (y += 6). There is no
 * S/R solid-repeat vocabulary — unknown verbs are silently consumed,
 * which is how a malformed stream degrades rather than misbehaving. */
static void build_sixel_quadrants(void)
{
	g_len = 0;
	putlit("\033[H");
	putlit("SIXEL QUADRANTS\r\n");

	putlit("\033Pq");
	/* colour table: 0 red, 1 green, 2 blue, 3 white (percent RGB) */
	putlit("#0;2;100;0;0");
	putlit("#1;2;0;100;0");
	putlit("#2;2;0;0;100");
	putlit("#3;2;100;100;100");
	/* 4 sixel rows x 6 px = 24 px tall; 2 chars x 6 px = 12 px per
	 * half, so each row is: left colour, right colour, newline. */
	putlit("#0!12~#1!12~-");	/* top:    red   | green   */
	putlit("#0!12~#1!12~-");
	putlit("#2!12~#3!12~-");	/* bottom: blue  | white   */
	putlit("#2!12~#3!12~-");
	putlit("\033\\");

	/* copy2cell lands the 2x2 cells at (0,1) and homes the cursor to
	 * row 3; walk to known lines, text must not overlap the square. */
	putlit("\033[6;1H");
	putlit("TEXT BELOW ART\r\n");
	putlit("\033[8;1H");
	putlit("CURSOR AT END?");
}

/* Kitty composite: the Phase-2 headline. Mirrors a negotiating TUI
 * app: the ratatui probe batch (DA1 + kitty query + cell-size), then
 * BOTH product store modes —
 *   i=7: t=d store-only at (2,4), re-placed with a=p to (4,10);
 *   i=8: direct render at (20,2); a=p afterwards replies ENOENT
 *        (a rendered image drops its store — behaviour this golden
 *        locks as intentional, not a latent bug),
 * a text write over the vacated original spot, and a coloured footer.
 * Raw payloads here use f=24 = 3 bytes/px RGB in spec order
 * ([R,G,B]; f=32 is RGBA) — the byte order kg_format() implements and
 * the kitty spec mandates (kg_format case 24: first_red = 1). Cursor
 * hidden throughout, app style.
 *
 * Governance labels: (a) app traffic — the probe batch is ratatui-image's
 * verbatim negotiation (DA1 ESC[c, kitty query i=31 a=q t=d f=24, cell
 * size CSI 16t) and the single-chunk m=0 transfer is spec grammar, so
 * those replies and pixels must track the outside world; (b) product
 * extension — the i=7 store/re-place dance and the "direct render drops
 * its store, a=p answers ENOENT" behaviour are product-internal surfaces
 * kept for engine coverage and labelled as such, not app-traffic truth. */
static void build_kitty_composite(void)
{
#define RB_RAW (12 * 24 * 3)		/* 12w x 24h RGB = 2 cells squared */
#define GR_RAW (6 * 12 * 3)		/* 6w x 12h RGB = 1 cell, solid green */
	uint8_t rb[RB_RAW];
	uint8_t gr[GR_RAW];
	/* base64 output: 4 chars per 3 raw bytes, up to 4 pad/group tail,
	 * plus NUL. Sizes derive from the raw size (a guessed constant
	 * once came out 561 bytes short, smacked the stack and corrupted
	 * the stream mid-payload, masquerading as a product ESHORT).
	 * st_b64enc() refuses to run short at runtime. */
#define B64_OUTSZ(n) (4 * ((n) / 3) + 5)
	char b64rb[B64_OUTSZ(RB_RAW)];
	char b64gr[B64_OUTSZ(GR_RAW)];

#undef B64_OUTSZ

	for (int y = 0; y < 24; y++)
		for (int x = 0; x < 12; x++) {
			uint8_t *q = &rb[(size_t) y * 12 * 3 + x * 3];

			q[0] = x < 6 ? 0: 255;	/* R: right half red   */
			q[1] = 0;			/* G                   */
			q[2] = x < 6 ? 255: 0;	/* B: left half blue   */
		}
	st_b64enc(rb, sizeof rb, b64rb, sizeof b64rb);
	for (size_t i = 0; i < sizeof gr; i += 3) {
		gr[i + 0] = 0;
		gr[i + 1] = 255;
		gr[i + 2] = 0;
	}
	st_b64enc(gr, sizeof gr, b64gr, sizeof b64gr);

	g_len = 0;
	putlit("\033[H");
	putlit("\033[2J");
	putlit("\033[?25l");		/* hide cursor */

	/* the ratatui-image probe batch (W1/W2/W3 ground truth) */
	putlit("\033[c");
	kg("i=31,s=1,v=1,a=q,t=d,f=24", "AAAA");
	putlit("\033[16t");

	/* stored transfer (U=1 t=d: kept, not painted yet; m=0 marks the
	 * final chunk — a single chunk carries its own completion) */
	putlit("\033[2;4H");
	putlit("IMG ");
	kg("i=7,a=T,U=1,t=d,f=24,s=12,v=24,m=0", b64rb);

	/* re-place at row 4 col 10, then overwrite the original spot */
	putlit("\033[4;10H");
	kg("a=p,i=7", "");
	putlit("\033[2;4H");
	putlit("ORIG SPOT NOW TEXT");

	/* direct render at row 20 col 2, then prove the store is gone */
	putlit("\033[20;2H");
	kg("i=8,a=T,s=6,v=12,f=24", b64gr);
	kg("a=p,i=8", "");

	/* coloured footer on the last grid line */
	putlit("\033[26;1H");
	putlit("\033[32m");
	putlit("COMPOSITE OK");
	putlit("\033[39m");
}

/* Halfblocks / braille / UTF-8: the Unicode fallback path — the glyphs
 * a terminal emitter renders when no graphics protocol is available
 * (U+2580/2584/2588/2591/2592/2593 block elements, U+2810/283F/2880/28FC
 * braille) — plus SGR states, 2- and 3-byte UTF-8, and a row filled to
 * the last column so the cursor parks at the edge. SGR 22 (lift bold)
 * is exercised deliberately: it used to leak (finding N5, now fixed in
 * set_attr), so this scene locks the corrected behaviour.
 *
 * Governance label: (a) app traffic — the block/braille repertoire is
 * exactly what ratatui-image's halfblocks backend emits as its TUI
 * fallback, and the SGR states mirror crossterm's style resets, so the
 * pixels track an outside emitter, not a product opinion. */
static void build_halfblocks_utf8(void)
{
	g_len = 0;
	putlit("\033[H");
	putlit("\033[2J");
	putlit("HALFBLKS ");
	putlit("\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88");	/* upper half, full   */
	putlit("\xe2\x96\x91\xe2\x96\x92\xe2\x96\x93");	/* shade thirds       */
	putlit("\xe2\x97\x8b\xe2\x97\x8f");		/* small circle, full  */
	putlit("\r\nBRAILLE  ");
	putlit("\xe2\xa0\x90\xe2\xa0\xbf");		/* U+2810 (dot 5),
							U+283F (all six) */
	putlit("\xe2\xa2\x80");			/* U+2880 (dot 8,
							tall right)    */
	putlit("\xe2\xa3\xbc");			/* U+28FC (dots 3-8)  */
	putlit("\r\n\033[1mBOLD\033[22m ");
	putlit("\033[4mUNDER\033[24m ");
	putlit("\033[31mRED\033[39m ");
	putlit("\033[7mREV\033[27m");
	putlit("\r\nUTF8  \xc3\xa9\xc3\xa0\xc3\xbc\xc3\x9f"); /* e-acute,
								e-grave,
								u-diaeresis,
								eszett (U+00DF) */
	putlit("\xe2\x82\xac\xc2\xa5");		/* euro (3B), yen (2B)   */
	putlit("\r\n");
	{
		static const char abc[] = "abcdefghijklmnopqrstuvwxyz0123456789";

		for (int i = 0; i < 53; i++)
			put(&abc[i % (int) (sizeof abc - 1)], 1);
	}
}

/* ------------------------------------------------------------------ */
/* frame + run                                                        */
/* ------------------------------------------------------------------ */

static uint8_t g_frame[GSZ];
static char g_reply[REPLY_SZ];
static size_t g_repllen;

static void g_make_fb(struct framebuffer_t *fb)
{
	memset(fb, 0, sizeof *fb);
	fb->fp = g_frame;
	fb->buf = NULL;
	fb->wall = NULL;
	fb->fd = -1;
	fb->info.width = GW;
	fb->info.height = GH;
	fb->info.line_length = GW * GBPP;
	fb->info.screen_size = (long) GSZ;
	fb->info.bytes_per_pixel = GBPP;
	fb->info.bits_per_pixel = 32;
	fb->info.type = YAFT_FB_TYPE_PACKED_PIXELS;
	fb->info.visual = YAFT_FB_VISUAL_TRUECOLOR;
	fb->info.red.length = 8;   fb->info.red.offset = 16;
	fb->info.green.length = 8; fb->info.green.offset = 8;
	fb->info.blue.length = 8;  fb->info.blue.offset = 0;
	/* fb_init()'s own palette loop */
	for (int i = 0; i < COLORS; i++)
		fb->real_palette[i] = color2pixel(&fb->info, color_list[i]);
}

static uint32_t px_at(int x, int y)
{
	const int pos = (y * GW + x) * GBPP;

	return (uint32_t) g_frame[pos] | ((uint32_t) g_frame[pos + 1] << 8) |
	       ((uint32_t) g_frame[pos + 2] << 16) | ((uint32_t) g_frame[pos + 3] << 24);
}

static long reply_drain(int fd)
{
	long off = 0;
	int fl = fcntl(fd, F_GETFL, 0);

	if (fl >= 0)
		(void) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	while (off < (long) sizeof g_reply - 1) {
		ssize_t rd = read(fd, g_reply + off,
				  (size_t) sizeof g_reply - 1 - (size_t) off);

		if (rd <= 0)
			break;
		off += rd;
	}
	return off;
}

struct scen {
	const char *name;
	/* Exactly one of the two: an inline builder, or a frozen
	 * external stimulus file tests/goldens/<name><ext> (the L2b
	 * tier, see the note above the table). */
	void (*build)(void);
	const char *ext;
};

/* L2b tier 1 (canonical, dependency-free): the last two scenarios
 * do not build the stimulus inline — they replay a FROZEN byte
 * stream committed next to the goldens, captured once, live, by
 * tests/apptest (tier 2) against a real emitter. The replay below
 * feeds it through the same parse()/refresh() path, so the frozen
 * frame and reply are re-derivable on any host with no app, no
 * network and no rust toolchain.
 *
 * kitty-applive.trace — verbatim stdout of ratatui-image v12.0.0-rc.2
 * (a08dc3c, the asset pinned in tests/l2b/asset/): its verbatim
 * probe batch, the negotiated KGP win, and a 100x60 RGBA image
 * (tests/l2b/asset/img/apptest-100x60.png, deterministic
 * gen_image.py) that the app scales to fit a 40x20-cell rect and
 * transfers as raw f=32 in 4096-base64-char chunks, U=1 virtual
 * placement with U+10EEEE placeholder glyphs, plus the crossterm
 * buffer writes around it.
 * Governance: (a) app traffic — the whole stimulus is what a real
 * peer emits, so frame and reply must track the outside world.
 *
 * sixel-icy-overlay.dcs — raw icy_sixel encoder output (generator
 * pinned in tests/l2b/sixgen/; REPORT.md sec. 3.3 audits the
 * dialect against the product verb table) for a 20x20 five-colour
 * image with overlapping layers: multi-entry percent palette,
 * full-band '$' layer overlays and '!' runs — shapes the hand-built
 * scenes never reach. Preceded here by ESC H (app-style anchor; the
 * DCS lands at the cursor, DEC style).
 * Governance: (a) app traffic — the emitter is the outside encoder.
 */
static const struct scen scens[] = {
	{ "sixel-quadrants", build_sixel_quadrants, NULL },
	{ "kitty-composite", build_kitty_composite, NULL },
	{ "halfblocks-utf8", build_halfblocks_utf8, NULL },
	{ "kitty-applive", NULL, ".trace" },
	{ "sixel-icy-overlay", NULL, ".dcs" },
};

static const char *g_root = ".";

static void g_paths(const struct scen *s, char *frame, size_t fsz,
		    char *reply, size_t rsz)
{
	snprintf(frame, fsz, "%s/tests/goldens/%s.frame", g_root, s->name);
	snprintf(reply, rsz, "%s/tests/goldens/%s.reply", g_root, s->name);
}

static int cmp_dump(const char *what, const uint8_t *have, size_t hsz,
		    const uint8_t *want, size_t wsz,
		    const char *name)
{
	size_t n = hsz < wsz ? hsz: wsz;
	size_t shown = 0;

	if (hsz != wsz) {
		fprintf(stderr, "  [%s] %s: size %zu != golden %zu\n",
			name, what, hsz, wsz);
		return 1;
	}
	for (size_t i = 0; i < n; i++) {
		if (have[i] != want[i] && shown < 8) {
			fprintf(stderr, "  [%s] %s: diff @%zu have=0x%02x want=0x%02x (px y=%zu x=%zu)\n",
				name, what, i, have[i], want[i],
				i / (size_t) GBPP / (size_t) GW,
				(i / (size_t) GBPP) % (size_t) GW);
			shown++;
		}
	}
	return 1;
}

/* L2b external stimulus: replay a frozen byte stream from
 * tests/goldens/<name><ext>. The .dcs variant gets an app-style
 * ESC H anchor in front (the DCS image lands at the cursor, DEC
 * style). put() enforces MAXSTREAM; an oversized file is a hard
 * abort, which is what we want for a frozen reference artefact. */
static int load_external(const struct scen *s)
{
	char path[512];
	static uint8_t buf[8192];
	FILE *f;
	size_t n;

	g_len = 0;
	snprintf(path, sizeof path, "%s/tests/goldens/%s%s",
	         g_root, s->name, s->ext);
	if (s->ext[1] == 'd')		/* .dcs: home first, then the DCS */
		putlit("\033[H");
	f = fopen(path, "rb");
	if (!f) {
		fail(s->name);
		fprintf(stderr, "  [%s] cannot open %s\n", s->name, path);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		put(buf, n);
	if (ferror(f)) {
		fail(s->name);
		fprintf(stderr, "  [%s] read error on %s\n", s->name, path);
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int g_run_one(const struct scen *s, int update)
{
	struct framebuffer_t fb;
	struct terminal_t term;
	char pf[512], pr[512];
	int rp[2] = { -1, -1 };
	int scene_fail = 0;
	unsigned unpainted = 0;
	size_t first = 0;

	if (s->ext) {
		if (load_external(s) != 0)
			return 1;
	} else {
		s->build();
	}
	if (getenv("DEBUG_STREAM")) {
		fprintf(stderr, "---- stream %s (%zu bytes) ----\n", s->name, g_len);
		for (size_t i = 0; i < g_len; i += 16) {
			char asc[17];
			size_t n = (g_len - i < 16) ? g_len - i: 16;

			fprintf(stderr, "%04zx ", i);
			for (size_t j = 0; j < n; j++) {
				fprintf(stderr, "%02x", g_stream[i + j]);
				asc[j] = (g_stream[i + j] >= 0x20
					  && g_stream[i + j] < 0x7f)
					 ? (char) g_stream[i + j]: '.';
			}
			asc[n] = '\0';
			fprintf(stderr, " |%s|\n", asc);
		}
		fprintf(stderr, "---- end stream ----\n");
	}
	memset(g_frame, 0xEF, sizeof g_frame);	/* 0xFEFEFEFE pixel-wise */
	g_make_fb(&fb);
	memset(&term, 0, sizeof term);
	if (!term_init(&term, GW, GH)) {
		fail(s->name);
		return 1;
	}

	if (pipe(rp) == 0) {
		size_t off;

		term.fd = rp[1];
		/* feed in CHUNK bites: incremental parse must agree */
		for (off = 0; off < g_len; ) {
			size_t n = g_len - off < CHUNK ? g_len - off: CHUNK;

			parse(&term, g_stream + off, (int) n);
			off += n;
		}
		g_repllen = (size_t) reply_drain(rp[0]);
	} else {
		g_repllen = 0;
	}

	/* Normalise nothing: whatever the cursor/viewport state is now
	 * is what a real session has; lock it. */
	redraw(&term);
	refresh(&fb, &term);
	term_die(&term);
	if (rp[1] >= 0)
		close(rp[1]);
	if (rp[0] >= 0)
		close(rp[0]);

	/* full-coverage guard: refresh() must have written every pixel */
	for (size_t y = 0; y < (size_t) GH; y++)
		for (size_t x = 0; x < (size_t) GW; x++)
			if (px_at(x, y) == SENTINEL) {
				if (unpainted++ == 0)
					first = y * (size_t) GW + x;
			}
	if (unpainted) {
		fprintf(stderr, "  [%s] UNPAINTED PIXELS: %u (first y=%zu x=%zu)\n",
			s->name, unpainted, first / GW, first % GW);
		scene_fail = 1;
	}

	g_paths(s, pf, sizeof pf, pr, sizeof pr);

	if (update) {
		FILE *f = fopen(pf, "wb");
		FILE *r = fopen(pr, "wb");

		if (!f || !r ||
		    fwrite(g_frame, 1, GSZ, f) != GSZ ||
		    fwrite(g_reply, 1, g_repllen, r) != g_repllen) {
			fprintf(stderr, "  [%s] cannot write goldens\n", s->name);
			scene_fail = 1;
		} else {
			printf("updated %s (%lu reply bytes)\n", pf,
			       (unsigned long) g_repllen);
		}
		fclose(f);
		fclose(r);
	} else {
		static uint8_t gframe[GSZ];
		static uint8_t greply[REPLY_SZ];
		FILE *f = fopen(pf, "rb");
		FILE *r = fopen(pr, "rb");
		size_t frsz, rsrz;

		if (!f || !r) {
			fprintf(stderr, "  [%s] golden missing (%s); "
			    "run UPDATE_GOLDENS=1 tests/golden\n", s->name, pf);
			scene_fail = 1;
		} else {
			frsz = fread(gframe, 1, GSZ, f);
			rsrz = fread(greply, 1, sizeof greply, r);
			if (frsz != GSZ || rsrz != g_repllen) {
				fprintf(stderr, "  [%s] golden read %zu/%zu "
				    "(reply %zu vs %zu)\n",
					s->name, frsz, GSZ, rsrz, g_repllen);
				scene_fail = 1;
			} else {
				if (memcmp(g_frame, gframe, GSZ) != 0) {
					cmp_dump("frame", g_frame, GSZ,
						 gframe, GSZ, s->name);
					scene_fail = 1;
				}
				if (memcmp(g_reply, greply, g_repllen) != 0) {
					fprintf(stderr, "  [%s] reply: %.*s\n"
					    "            golden: %.*s\n",
						s->name,
						(int) (g_repllen < 40 ? g_repllen: 40), g_reply,
						(int) (g_repllen < 40 ? g_repllen: 40), greply);
					scene_fail = 1;
				}
				if (!scene_fail)
					printf("%-18s ok (frame %zu B, reply %zu B)\n",
					       s->name, GSZ, g_repllen);
			}
			fclose(f);
			fclose(r);
		}
	}

	if (scene_fail) {
		fail(s->name);
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	char font[512] = "tests/assets/console-6x12.cruftfont";
	const char *root;
	int update = getenv("UPDATE_GOLDENS") != NULL;
	int bad_scens = 0;

	(void) argc;
	(void) argv;

	/* cruft.c main() does this before parse(): the cell path checks
	 * glyph width with wcwidth(), which is locale-dependent — without a
	 * UTF-8 locale every multi-byte glyph degrades to U+003F. */
	if (setlocale(LC_ALL, "") == NULL)
		fprintf(stderr, "golden: WARNING setlocale failed (multi-byte "
			"glyphs will misbehave)\n");
	vt_active = true;

	if ((root = getenv("CRUFT_GOLDEN_ROOT")) != NULL && root[0]) {
		g_root = root;
		snprintf(font, sizeof font,
			 "%s/tests/assets/console-6x12.cruftfont", root);
	}

	if (glyph_mmap_init(font) != 1) {
		fprintf(stderr, "golden: cannot init font %s\n", font);
		return 2;
	}

	for (size_t i = 0; i < sizeof scens / sizeof *scens; i++)
		if (g_run_one(&scens[i], update) != 0)
			bad_scens++;

	glyph_mmap_die();
	fprintf(stderr, "%sgolden: %d/%zu scenes ok, %d failure(s)\n",
		failures ? "": "", (int) (sizeof scens / sizeof *scens - (size_t) bad_scens),
		sizeof scens / sizeof *scens, failures);
	return failures ? 1: 0;
}
