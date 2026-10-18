/*
 * L2b live driver (tier 2 of the golden-pixel harness): runs the
 * REAL ratatui-image app (built from tests/l2b/asset, pinned rev)
 * on a real pty against the real in-process product, and freezes
 * (--update) or verifies the artefact set:
 *
 *   tests/goldens/kitty-applive.trace   the app's writes to its
 *                                       stdout, recorded at the
 *                                       write boundary, verbatim
 *                                       apart from the id
 *                                       canonicalisation below
 *   tests/goldens/kitty-applive.reply   the product's replies to
 *                                       that stimulus, in order
 *   tests/goldens/kitty-applive.frame   resulting 320x320x4 frame
 *
 * The SAME three files are compared by tier 1 (tests/golden, scene
 * "kitty-applive"), which replays the frozen trace through the same
 * parse()/refresh() path with no app, no network and no rust
 * toolchain. Two independent routes, one golden: a divergent live
 * run (different app build, host, or product change) is caught on
 * whichever side notices.
 *
 * Transport and capture: the app sits on a REAL pty - the pinned
 * picker routes its raw-mode negotiation through rustix raw
 * syscalls that no LD_PRELOAD can interpose, so a fake tty is not
 * an option (the attempt and the proof are in REPORT.md). The pty
 * line discipline is nevertheless empirically lossy in TRANSIT on
 * this host (MiB-scale 0x00 floods, plus a 2-3 byte
 * insertion/deletion at a drifting offset inside the base64
 * payload, in every observed run), so the pty read side is not the
 * authority. Authority is taken at the ORIGIN instead:
 * tests/l2b/writetee.so (LD_PRELOAD) mirrors every write() the app
 * issues to its stdout, before the kernel touches a byte, onto a
 * pipe the driver reads; the product parses THAT clean stream, and
 * the pty is demoted to a delivery duct (product replies to the
 * app, app output to a discarded drain). The capture is byte-exact
 * by construction and needs no sanitising: what the app wrote IS
 * the stimulus.
 *
 * The single canonicalisation: ratatui-image stamps its transfer
 * with an opaque RANDOM i=<u32> per run. The product echoes the id
 * in its reply and keys its store on it, so the freeze would be
 * run-specific. The id is pinned to 32 in EVERY carrier it travels
 * in - the transfer header (i=), the placeholder cells' truecolor
 * SGR (low 24 bits, the encoding audited at kitty_image.c:1015-
 * 1021), and the row-head diacritics' id-high byte (kg_diacc[],
 * exported as kitty_diac_table()) - and nowhere else. The i=31
 * protocol-probe id is untouched: a deterministic capability token.
 * (Pinning the header alone desynchronises the store key from the
 * placeholder id and blanks the image - the first naively-pinned
 * freeze exhibited exactly that.)
 *
 * Authenticity assertions (each a loud fail, never a silent
 * repair): the capture is NUL-free; the live reply carries the
 * transfer ack exactly once; and every one of the eight base64
 * payload segments is BYTE-IDENTICAL to the stream recomputed
 * independently from the committed source image (tests/l2b/asset/
 * img/apptest-100x60.b64: 100x60 RGBA at natural size - no
 * scaling - in the audited 4096-char chunks).
 *
 * Assertion target: the driver replays the canonical stream
 * through a FRESH in-process terminal (exactly the tier-1 code
 * path) and compares the replay's frame and replies against the
 * goldens. The live pass still exercises the real product end to
 * end (the app's real probe batch met the real replies, its real
 * bytes flowed through the real parse()), and its reply - with the
 * same id canonicalisation applied - must equal the replay's reply
 * byte-for-byte.
 *
 * This binary is OPTIONAL for the gate: `make test` never touches
 * it. Usage:
 *   tests/apptest [--update] <asset-bin> <image.png>
 * [--update] rewrites the goldens from this run (review before
 * committing). The product side is initialised exactly like
 * tests/golden: fabricated 320x320 frame, vendored 6x12 font,
 * vt_active on.
 */
#define _XOPEN_SOURCE 600
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cruft.h"
#include "conf.h"
#include "util.h"
#include "fb/common.h"
#include "status.h"
/* Unity headers, mirrored from golden_main.c: the real parse() and
 * refresh() under test. */
#include "terminal.h"
#include "ctrlseq/esc.h"
#include "ctrlseq/csi.h"
#include "ctrlseq/osc.h"
#include "ctrlseq/dcs.h"
#include "parse.h"

#define GW      320
#define GH      320
#define GBPP    4
#define GSZ     ((size_t) GW * GH * GBPP)
#define GRID_W  53
#define GRID_H  26
#define NAME    "kitty-applive"
#define GUARD_MS 60000L	/* wall-clock belt: the app's own 2 s probe
			   * deadline makes this unreachable in a
			   * healthy run */
#define TEE_FD  7	/* authority-capture pipe end in the app;
			   * must agree with tests/l2b/writetee.c */

/* canonicalisation constants (see the header note) */
#define FIXED_ID    32	/* canonical transfer id (the probe owns 31);
			   * 32 = 0x20 -> SGR low-24 spelling 0;0;32 */
#define IDEAL_TOTAL 32000	/* b64 chars: 100x60x4 = 24000 B */
#define B64_CHUNK   4096	/* transmitter chunk (audited kitty.rs) */
#define N_CHUNKS    (IDEAL_TOTAL / B64_CHUNK + 1)	/* 7 full + 3328 */

static int failures;
static uint8_t g_frame[GSZ];

static void fail(const char *msg)
{
	failures++;
	fprintf(stderr, "FAIL: %s\n", msg);
}

static void die(const char *msg)
{
	fprintf(stderr, "apptest: %s\n", msg);
	exit(2);
}

static void make_fb(struct framebuffer_t *fb, uint8_t *pix)
{
	memset(fb, 0, sizeof *fb);
	fb->fp = pix;
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
	for (int i = 0; i < COLORS; i++)
		fb->real_palette[i] = color2pixel(&fb->info, color_list[i]);
}

static long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long) ts.tv_sec * 1000L + (long) (ts.tv_nsec / 1000000L);
}

/* append-growth with a CLEAN split of duties: *cap (owned by the
 * caller and maintained HERE) is the allocated capacity; the
 * caller's logical size is a separate variable this function never
 * reads nor writes. Ensures at least `need` bytes are writable and
 * returns the (possibly relocated) base. */
static uint8_t *grow(uint8_t **pb, size_t *cap, size_t need)
{
	size_t ns = *cap;

	if (need <= ns)
		return *pb;
	while (ns < need)
		ns = ns? ns * 2: need;
	*pb = realloc(*pb, ns);
	if (!*pb)
		die("out of memory growing a buffer");
	*cap = ns;
	return *pb;
}

static void write_all(int fd, const uint8_t *b, size_t n)
{
	while (n) {
		ssize_t w = write(fd, b, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;	/* the app is gone: nothing to do */
		}
		b += (size_t) w;
		n -= (size_t) w;
	}
}

static uint8_t *read_exact(const char *path, size_t want)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b = malloc(want + 1);
	size_t n;

	if (!f)
		die("cannot open the ideal payload asset");
	if (!b)
		die("out of memory");
	n = fread(b, 1, want + 1, f);
	fclose(f);
	if (n != want)
		die("ideal payload asset has the wrong size");
	return b;
}

/* clean-capture invariant: a 0x00 in the write-boundary record (or
 * the reply stream) is impossible for this protocol - it would
 * mean the app itself wrote one, an environment fault to surface
 * and investigate, not sanitise past */
static void assert_no_nuls(const char *tag, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (b[i] != 0x00)
			continue;
		/* dump the full record + the first offender's
		 * neighbourhood so the fault is diagnosable on
		 * sight */
		{
			char path[256];
			FILE *df = NULL;
			size_t lo = (i > 64)? i - 64: 0;

			snprintf(path, sizeof path,
			         "/tmp/apptest-debug.%s", tag);
			df = fopen(path, "wb");
			if (df) {
				fwrite(b, 1, n, df);
				fclose(df);
			}
			fprintf(stderr, "  first 0x00 at %zu of %zu "
			        "(record -> %s):", i, n, path);
			for (size_t j = lo; j < i + 32 && j < n; j++)
				fprintf(stderr, " %02x%s", b[j],
					j == i? "*": "");
			fprintf(stderr, "\n");
		}
		die("0x00 in the clean capture; the record cannot "
		    "contain protocol-foreign bytes - investigate "
		    "the app or the environment, do not sanitise");
	}
}

static size_t utf8_put(uint8_t *o, uint32_t cp)
{
	if (cp < 0x80) {
		o[0] = (uint8_t) cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = (uint8_t) (0xC0 | (cp >> 6));
		o[1] = (uint8_t) (0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (uint8_t) (0xE0 | (cp >> 12));
		o[1] = (uint8_t) (0x80 | ((cp >> 6) & 0x3F));
		o[2] = (uint8_t) (0x80 | (cp & 0x3F));
		return 3;
	}
	o[0] = (uint8_t) (0xF0 | (cp >> 18));
	o[1] = (uint8_t) (0x80 | ((cp >> 12) & 0x3F));
	o[2] = (uint8_t) (0x80 | ((cp >> 6) & 0x3F));
	o[3] = (uint8_t) (0x80 | (cp & 0x3F));
	return 4;
}

/* ---------------------- canonicalisation ----------------------- */

struct canon_stats {
	unsigned long id;	/* the client-random transfer id, captured */
	int id_ok;
	int chunks;
	int sgr;
	int diac;
};

static const uint8_t *g_ideal;

/* first occurrence of the two-byte ST terminator; the only sequence
 * scan this driver needs (private, keeps the file free of feature-
 * macro entanglement) */
static const uint8_t *find_st(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i + 2 <= n; i++)
		if (p[i] == 0x1b && p[i + 1] == '\\')
			return p + i;
	return NULL;
}

/* walk every _G APC; return its body (after the 'G') or NULL if this
 * offset is not the start of an APC */
static const uint8_t *apc_body_at(const uint8_t *in, size_t n, size_t i,
				  size_t *blen, size_t *adv)
{
	const uint8_t *end;

	if (i + 3 > n || in[i] != 0x1b || in[i + 1] != '_' || in[i + 2] != 'G')
		return NULL;
	end = find_st(in + i + 3, n - i - 3);
	if (!end)
		die("unterminated _G APC");
	/* `end` points AT the ESC of the ST, so [start, end) already
	 * excludes the 2-byte terminator - do not subtract it again */
	*blen = (size_t) (end - (in + i + 3));
	*adv = (size_t) (end - (in + i)) + 2;	/* advance from position i */
	return in + i + 3;
}

/* pass 1: find the client-random transfer id (first chunk header)
 * and sanity-count the transfer chunks */
static void canon_pass1(const uint8_t *in, size_t n, struct canon_stats *cs)
{
	size_t i = 0;

	cs->id = 0;
	cs->id_ok = 0;
	cs->chunks = 0;
	while (i < n) {
		const uint8_t *body;
		size_t blen, adv;

		body = apc_body_at(in, n, i, &blen, &adv);
		if (!body) {
			i++;
			continue;
		}
		if (blen >= 5 && memcmp(body, "q=2,", 4) == 0) {
			const uint8_t *semi = memchr(body, ';', blen);
			const uint8_t *p;
			unsigned long v = 0;
			int digits = 0;

			if (!semi)
				die("transfer APC without payload separator");
			cs->chunks++;
			if (!cs->id_ok) {
				for (p = body; p < semi; p++) {
					if (*p != 'i' || p + 1 >= semi ||
					    p[1] != '=')
						continue;
					p += 2;	/* past "i=" to the digits */
					while (p < semi && *p >= '0' &&
					       *p <= '9') {
						v = v * 10 +
						    (unsigned long) (*p - '0');
						if (v > 0xFFFFFFFFUL)
							die("transfer id "
							    "overflows u32");
						p++;
						digits++;
					}
					if (digits) {
						cs->id = v;
						cs->id_ok = 1;
						break;
					}
				}
			}
		}
		i += adv;
	}
}

/* builder sink for pass 2 */
static uint8_t *g_cb;
static size_t g_cbn, g_cbsz;

static void cb_emit(const void *p, size_t l)
{
	if (g_cbn + l > g_cbsz)
		die("canonical builder overflow");
	memcpy(g_cb + g_cbn, p, l);
	g_cbn += l;
}

/* emit the first-chunk params with i=<random> -> i=<FIXED_ID> */
static void emit_params_rewritten(const uint8_t *body, size_t plen)
{
	int done = 0;

	for (size_t k = 0; k < plen; k++) {
		if (done) {
			cb_emit(body + k, 1);
			continue;
		}
		if (body[k] == 'i' && k + 1 < plen && body[k + 1] == '=') {
			size_t d = k + 2;

			if (d < plen && body[d] >= '0' && body[d] <= '9') {
				cb_emit("i=32", 4);
				k++;
				while (d < plen && body[d] >= '0' &&
				       body[d] <= '9') {
					d++;
					k++;
				}
				done = 1;
				continue;
			}
		}
		cb_emit(body + k, 1);
	}
	if (!done)
		die("transfer id rewrite missed");
}

/* pass 2: scaffold verbatim; payloads from the ideal; the id pinned
 * in every carrier (header, SGR truecolor, id-high diacritic) */
static size_t canon_build(const uint8_t *in, size_t n,
			  struct canon_stats *cs)
{
	uint8_t dold[8], dnew[8];
	size_t dol = utf8_put(dold,
			      kitty_diac_table()[
				      (unsigned) (cs->id >> 24) & 0xFF]);
	size_t dnl = utf8_put(dnew, kitty_diac_table()[0]);
	size_t i = 0, chunk = 0;

	g_cbn = 0;
	while (i < n) {
		const uint8_t *body;
		size_t blen, adv;

		body = apc_body_at(in, n, i, &blen, &adv);
		if (body) {
			if (blen >= 5 && memcmp(body, "q=2,", 4) == 0) {
				const uint8_t *semi = memchr(body, ';', blen);
				size_t plen, paylen, ilen, poff;

				if (!semi)
					die("transfer APC without payload "
					    "separator (pass 2)");
				plen = (size_t) (semi - body);
				poff = plen + 1;
				paylen = blen - poff;
				if (chunk >= N_CHUNKS)
					die("too many transfer chunks");
				ilen = B64_CHUNK < IDEAL_TOTAL - chunk * B64_CHUNK ?
					B64_CHUNK: IDEAL_TOTAL - chunk * B64_CHUNK;
				cb_emit("\x1b_G", 3);
				if (chunk == 0)
					emit_params_rewritten(body, plen);
				else
					cb_emit(body, plen);
				cb_emit(";", 1);
				/* authenticity: the app's OWN payload
				 * must match the independently
				 * recomputed ideal byte for byte */
				if (paylen != ilen ||
				    memcmp(body + poff,
					   g_ideal + chunk * B64_CHUNK,
					   ilen) != 0) {
					FILE *df =
					    fopen("/tmp/apptest-"
						 "debug.norm",
						 "wb");

					if (df) {
						fwrite(in, 1, n, df);
						fclose(df);
					}
					die("payload chunk does not "
					    "match the independently "
					    "recomputed ideal "
					    "(capture dumped to "
					    "/tmp/apptest-debug"
					    ".norm)");
				}
				cb_emit(body + poff, paylen);
				cb_emit("\x1b\\", 2);
				chunk++;
			} else {
				/* the probe and any other APC: verbatim */
				cb_emit(in + i, adv);
			}
			i += adv;
			continue;
		}
		/* the single truecolor id-carrier SGR */
		if (n - i >= 5 && memcmp(in + i, "38;2;", 5) == 0) {
			size_t j = i + 5;
			int fields = 0;

			while (fields < 3) {
				size_t d0 = j;

				while (j < n && in[j] >= '0' && in[j] <= '9')
					j++;
				if (j == d0)
					break;	/* not an id SGR: verbatim */
				fields++;
				if (fields < 3 && (j >= n || in[j] != ';'))
					break;
				if (fields < 3)
					j++;
			}
			if (fields == 3) {
				cb_emit("38;2;0;0;32", 11);
				cs->sgr++;
				i = j;
				continue;
			}
		}
		if (i + dol <= n && memcmp(in + i, dold, dol) == 0) {
			cb_emit(dnew, dnl);
			cs->diac++;
			i += dol;
			continue;
		}
		cb_emit(in + i, 1);
		i++;
	}
	if (chunk != 8)
		die("expected 8 transfer chunks in pass 2");
	if (cs->sgr != 1)
		die("expected exactly one id-carrier SGR");
	return g_cbn;
}

/* ----------------------- replay (assert) ------------------------ */

static void drain_pipe(int fd, uint8_t **pb, size_t *pn, size_t *pcap)
{
	static uint8_t dbuf[8192];

	for (;;) {
		ssize_t rd = read(fd, dbuf, sizeof dbuf);

		if (rd <= 0)
			return;	/* EAGAIN/EIO: drained */
		memcpy(grow(pb, pcap, *pn + (size_t) rd) + *pn,
		       dbuf, (size_t) rd);
		*pn += (size_t) rd;
	}
}

/* feed the stimulus through a FRESH in-process terminal - exactly
 * the tier-1 code path. 333-byte bites recheck the incremental-parse
 * agreement property. Replies are returned (caller frees); the frame
 * lands in g_frame. */
static uint8_t *replay_stream(uint8_t *stim, size_t n, size_t *reply_n)
{
	struct terminal_t term;
	struct framebuffer_t fb;
	int rp[2] = { -1, -1 };
	uint8_t *rb = NULL;
	size_t rbn = 0, rbcap = 0;

	memset(&term, 0, sizeof term);
	if (!term_init(&term, GW, GH))
		die("term_init (replay)");
	if (pipe(rp) != 0)
		die("pipe (replay)");
	term.fd = rp[1];
	if (fcntl(rp[0], F_SETFL, O_NONBLOCK) != 0)
		die("fcntl (replay)");
	for (size_t off = 0; off < n;) {
		/* 333-byte bites: deliberately MISaligned with every APC
		 * boundary, rechecking the split-independence property
		 * (feed-as-delivered agreement with the live pump) */
		size_t bite = n - off < 333 ? n - off: 333;

		parse(&term, stim + off, (int) bite);
		off += bite;
		drain_pipe(rp[0], &rb, &rbn, &rbcap);
	}
	for (int k = 0; k < 50; k++) {
		drain_pipe(rp[0], &rb, &rbn, &rbcap);
		usleep(1000);
	}
	redraw(&term);
	make_fb(&fb, g_frame);
	refresh(&fb, &term);
	term_die(&term);
	close(rp[1]);
	close(rp[0]);
	*reply_n = rbn;
	return rb;
}

/* compare a captured artefact against its golden file; golden may be
 * any length (trace/reply), frame is fixed GSZ */
static int cmp_artefact(const char *what, const uint8_t *have, size_t hsz,
			const char *path)
{
	struct stat st;
	uint8_t *want = NULL;
	size_t wsz = 0, shown = 0, n;
	int rc = 1;

	if (stat(path, &st) != 0) {
		fprintf(stderr, "  %s: golden missing (%s)\n", what, path);
		return 1;
	}
	want = malloc(st.st_size ? (size_t) st.st_size: 1);
	if (!want)
		return 1;
	{
		FILE *f = fopen(path, "rb");

		if (!f) {
			free(want);
			return 1;
		}
		wsz = fread(want, 1, (size_t) st.st_size, f);
		fclose(f);
	}
	n = hsz < wsz ? hsz: wsz;
	if (hsz != wsz) {
		fprintf(stderr, "  %s: size %zu != golden %zu\n", what,
			hsz, wsz);
	} else {
		for (size_t i = 0; i < n; i++)
			if (have[i] != want[i] && shown < 8) {
				fprintf(stderr,
					"  %s: diff @%zu have=0x%02x "
					"want=0x%02x\n",
					what, i, have[i], want[i]);
				shown++;
			}
		rc = 0;
	}
	free(want);
	return rc;
}

/* canonicalise the LIVE reply for comparison with the replay's: the
 * transfer ack echoes the client-random id, the only run-specific
 * bytes in the product's reply. Rewrite that one occurrence to the
 * fixed id; zero or multiple occurrences is itself a finding
 * (missing or duplicated transfer ack). */
static uint8_t *canon_reply(const uint8_t *in, size_t n, unsigned long id,
			    size_t *outn)
{
	char pat[16];
	int npat = snprintf(pat, sizeof pat, "i=%lu", id);
	const uint8_t *hit = NULL;
	int nhit = 0;
	uint8_t *out;

	for (size_t i = 0; i + (size_t) npat <= n; i++) {
		if (memcmp(in + i, pat, (size_t) npat) == 0) {
			hit = in + i;
			nhit++;
			i += (size_t) npat - 1;
		}
	}
	if (nhit != 1)
		die("live reply does not echo the transfer id exactly "
		    "once (missing or duplicated transfer ack)");
	out = malloc(n + 8);
	if (!out)
		die("out of memory");
	memcpy(out, in, (size_t) (hit - in));
	memcpy(out + (size_t) (hit - in), "i=32", 4);
	memcpy(out + (size_t) (hit - in) + 4, hit + npat,
	       n - ((size_t) (hit - in) + (size_t) npat));
	*outn = n - (size_t) npat + 4;
	return out;
}

int main(int argc, char *argv[])
{
	char font[512] = "tests/assets/console-6x12.cruftfont";
	const char *root = ".";
	char paths[2][512];
	const char *g_root_env;
	struct terminal_t term;
	int npath = 0;
	int update = 0;
	int master = -1, slave = -1;	/* the app's real pty */
	int tp[2] = { -1, -1 };	/* app -> driver: the write-tee */
	int rp[2] = { -1, -1 };
	struct winsize ws;
	pid_t pid;
	int dead = 0, app_fail = 0;
	uint8_t *tb = NULL, *rb = NULL;
	size_t tbsz = 0, rbsz = 0;
	size_t tbcap = 0, rbcap = 0;
	uint8_t *rep = NULL, *lcrep = NULL;
	size_t replen = 0, lclen = 0, clen = 0;
	size_t raw_tb = 0;
	int tee_eof = 0, master_dead = 0;
	struct canon_stats cs;
	char dll[4096], evpre[4208];
	static uint8_t chunk[16384];
	long t0;
	int st = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--update") == 0)
			update = 1;
		else if (npath < 2)
			snprintf(paths[npath++], sizeof paths[0], "%s",
			         argv[i]);
		else {
			fprintf(stderr, "usage: tests/apptest [--update] "
			    "<asset-bin> <image.png>\n");
			return 2;
		}
	}
	if (npath != 2) {
		fprintf(stderr, "usage: tests/apptest [--update] "
		    "<asset-bin> <image.png>\n");
		return 2;
	}

	if (setlocale(LC_ALL, "") == NULL)
		fprintf(stderr, "apptest: WARNING setlocale failed\n");
	vt_active = true;
	if ((g_root_env = getenv("CRUFT_GOLDEN_ROOT")) != NULL && g_root_env[0]) {
		root = g_root_env;
		snprintf(font, sizeof font,
			 "%s/tests/assets/console-6x12.cruftfont", root);
	}
	if (glyph_mmap_init(font) != 1) {
		fprintf(stderr, "apptest: cannot init font %s\n", font);
		return 2;
	}

	memset(&term, 0, sizeof term);
	if (!term_init(&term, GW, GH)) {
		fail(NAME);
		return 1;
	}
	if (pipe(rp) != 0) {
		perror("pipe");
		return 2;
	}
	term.fd = rp[1];		/* product replies travel here */

	/* the app's transport: a REAL pty (the picker's raw-mode
	 * negotiation goes through raw rustix syscalls that no
	 * LD_PRELOAD can interpose, so the terminal must be
	 * genuine), plus a write-boundary tee (writetee.so) carrying
	 * the authority bytes over a clean pipe */
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
		perror("posix_openpt/grantpt/unlockpt");
		return 2;
	}
	slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	if (slave < 0) {
		perror("open(pty slave)");
		return 2;
	}
	memset(&ws, 0, sizeof ws);
	ws.ws_row = GRID_H;
	ws.ws_col = GRID_W;
	ws.ws_xpixel = GW;
	ws.ws_ypixel = GH;
	if (ioctl(master, TIOCSWINSZ, &ws) != 0)
		perror("TIOCSWINSZ");
	if (pipe(tp) != 0) {
		perror("pipe (write tee)");
		return 2;
	}
	if (realpath("tests/l2b/writetee.so", dll) == NULL)
		die("tests/l2b/writetee.so not found; run make first");
	snprintf(evpre, sizeof evpre, "LD_PRELOAD=%s", dll);
	signal(SIGPIPE, SIG_IGN);	/* the product replying to an already
					 * exited app is an EPIPE to
					 * swallow, not a crash */

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 2;
	}
	if (pid == 0) {
		/* child = the app: the pty slave is its controlling
		 * tty, stdin/stdout on it; stderr is inherited so app
		 * diagnostics stay OUT of the frozen record. The
		 * writetee.so shim (LD_PRELOAD) mirrors its stdout
		 * writes onto fd TEE_FD (an inherited pipe end).
		 * Minimal environment on purpose: the host env can
		 * carry WEZTERM_EXECUTABLE / KONSOLE_VERSION, which
		 * the picker reads and would blacklist Kitty+Sixel.
		 *
		 * Return-value traps (both verified on this host):
		 * setsid() and dup2() do NOT follow the 0-on-success
		 * convention - setsid() returns the new session id
		 * and dup2() returns the NEW fd; test the sign of
		 * dup2's result, never equality with 0. */
		const char *ev[] = { "TERM=xterm-256color",
				"PATH=/usr/bin:/bin", evpre, NULL };
		char *av[] = { NULL, NULL, NULL };

		av[0] = (char *) paths[0];
		av[1] = (char *) paths[1];
		/* become a session leader so TIOCSCTTY succeeds. A
		 * process-group leader cannot setsid, so fork a proxy
		 * that keeps the parent's waitpid() semantics while
		 * the grandchild takes over (belt only; the child
		 * inherits the driver's group and is never a leader). */
		if (setsid() < 0) {
			pid_t gc = fork();
			int gst = 0;

			if (gc < 0)
				_exit(126);
			if (gc > 0) {
				do
					gc = waitpid(gc, &gst, 0);
				while (gc < 0 && errno == EINTR);
				_exit(WIFEXITED(gst) ?
				      WEXITSTATUS(gst):
				      128 + WTERMSIG(gst));
			}
			if (setsid() < 0)
				_exit(126);
		}
		if (ioctl(slave, TIOCSCTTY, 0) != 0)
			_exit(126);
		if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0)
			_exit(126);
		/* park the tee end FIRST: fd TEE_FD now belongs to it
		 * and must never be closed under it */
		if (dup2(tp[1], TEE_FD) < 0)
			_exit(126);
		if (master != TEE_FD)
			close(master);
		if (slave != TEE_FD && slave > 2)
			close(slave);
		if (tp[0] != TEE_FD)
			close(tp[0]);
		if (tp[1] != TEE_FD)
			close(tp[1]);
		/* the reply pipe belongs to the product (parent); the app
		 * must not hold either end. */
		if (rp[0] != TEE_FD)
			close(rp[0]);
		if (rp[1] != TEE_FD)
			close(rp[1]);
		execve(av[0], av, (char *const *) ev);
		_exit(127);
	}
	close(slave);
	close(tp[1]);

	/* live pump: three lanes. The product's replies ride the pty
	 * to the app. The app's OWN writes arrive over the tee pipe,
	 * pristine, and are what the in-process product parses. The
	 * pty read side is a drained-and-discarded twin (carrier-
	 * disturbed; never consumed). */
	t0 = mono_ms();
	while (1) {
		struct pollfd pf[3] = { { rp[0], POLLIN, 0 },
					{ tp[0], POLLIN, 0 },
					{ master, POLLIN, 0 } };
		int nfds = 3;

		if (!dead) {
			pid_t r = waitpid(pid, &st, WNOHANG);

			if (r == pid) {
				dead = 1;
				if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
					app_fail = 1;
					fprintf(stderr,
						"FAIL: %s: app exited badly "
						"(status %#x)\n",
						NAME, (unsigned) st);
				}
			} else if (r < 0 && errno != EINTR) {
				fail(NAME);
				break;
			}
		}
		if (mono_ms() - t0 > GUARD_MS) {
			fail(NAME);
			fprintf(stderr, "  %s: wall-clock guard tripped\n",
				NAME);
			break;
		}
		if (tee_eof) {
			pf[1].events = 0;
			nfds--;
		}
		if (master_dead) {
			pf[2].events = 0;
			nfds--;
		}
		if (poll(pf, nfds, dead ? 100: 200) < 0) {
			if (errno == EINTR)
				continue;
			fail(NAME);
			break;
		}
		if (pf[0].revents & (POLLIN | POLLHUP)) {
			ssize_t rd = read(rp[0], chunk, sizeof chunk);

			if (rd > 0) {
				rbsz += (size_t) rd;
				memcpy(grow(&rb, &rbcap, rbsz) +
				       rbsz - (size_t) rd, chunk, (size_t) rd);
				write_all(master, chunk, (size_t) rd);
			}
		}
		if (!tee_eof &&
		    pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			ssize_t rd = read(tp[0], chunk, sizeof chunk);

			if (rd < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				fail(NAME);
				break;
			}
			if (rd == 0) {
				tee_eof = 1;	/* the app closed its stdout */
			} else {
				tbsz += (size_t) rd;
				memcpy(grow(&tb, &tbcap, tbsz) +
				       tbsz - (size_t) rd,
				       chunk, (size_t) rd);
				/* feed exactly as delivered: pipe reads
				 * are arbitrary splits, and the replay
				 * below rechecks the aligned-split
				 * property */
				parse(&term, chunk, (int) rd);
			}
		}
		if (!master_dead &&
		    pf[2].revents & (POLLIN | POLLHUP)) {
			ssize_t dr = read(master, chunk, sizeof chunk);

			if (dr < 0 && errno != EAGAIN && errno != EINTR)
				master_dead = 1;	/* EIO: slave gone */
			/* else: discarded by design */
		}
		if (tee_eof && dead)
			break;
	}
	{
		pid_t r;

		do
			r = waitpid(pid, &st, 0);
		while (r < 0 && errno == EINTR);
	}
	{
		/* late bytes: poll-bounded drains (the holders keep the
		 * write ends, so bare reads would block) */
		struct pollfd dpf = { rp[0], POLLIN, 0 };
		uint8_t tmp[4096];

		while (poll(&dpf, 1, 50) > 0) {
			ssize_t dr = read(rp[0], tmp, sizeof tmp);

			if (dr <= 0)
				break;
			rbsz += (size_t) dr;
			memcpy(grow(&rb, &rbcap, rbsz) +
			       rbsz - (size_t) dr, tmp, (size_t) dr);
		}
		/* tee remainder -> capture, so a slow tear-down still
		 * leaves tb complete */
		dpf.fd = tp[0];
		while (poll(&dpf, 1, 50) > 0) {
			ssize_t dr = read(tp[0], tmp, sizeof tmp);

			if (dr <= 0)
				break;
			tbsz += (size_t) dr;
			memcpy(grow(&tb, &tbcap, tbsz) +
			       tbsz - (size_t) dr, tmp, (size_t) dr);
			parse(&term, tmp, (int) dr);
		}
		/* pty drain: contents are the disturbed twin, discard */
		dpf.fd = master;
		while (poll(&dpf, 1, 50) > 0) {
			ssize_t dr = read(master, tmp, sizeof tmp);

			if (dr <= 0)
				break;
		}
	}
	close(master);
	close(tp[0]);
	close(rp[1]);
	close(rp[0]);
	term_die(&term);	/* the in-pump frame mirrors the live
				 * negotiation; the replay below is the
				 * assertion */

	/* ---------- authenticate, then canonicalise ---------- */
	raw_tb = tbsz;
	assert_no_nuls("capture", tb, tbsz);
	assert_no_nuls("reply", rb, rbsz);
	{
		char ipath[512];

		memset(&cs, 0, sizeof cs);
		snprintf(ipath, sizeof ipath,
			 "%s/tests/l2b/asset/img/apptest-100x60.b64", root);
		g_ideal = read_exact(ipath, IDEAL_TOTAL);
		canon_pass1(tb, tbsz, &cs);
		if (!cs.id_ok || cs.chunks != 8) {
			FILE *df = fopen("/tmp/apptest-debug.raw", "wb");

			if (df) {
				fwrite(tb, 1, tbsz, df);
				fclose(df);
			}
			fprintf(stderr, "apptest: shape: tbsz=%zu "
				"id_ok=%d chunks=%d id=%lu\n",
				tbsz, cs.id_ok, cs.chunks, cs.id);
			die("capture is missing the expected transfer "
			    "shape; refusing to canonicalise "
			    "(raw capture dumped to "
			    "/tmp/apptest-debug.raw)");
		}
		g_cb = malloc(tbsz + 8192);
		if (!g_cb)
			die("out of memory");
		g_cbsz = tbsz + 8192;
		clen = canon_build(tb, tbsz, &cs);
	}

	/* ---------- replay = the assertion target ---------- */
	rep = replay_stream(g_cb, clen, &replen);

	/* the live reply proves the in-pump pass drove the product
	 * with the app's ACTUAL traffic; after the same id
	 * canonicalisation it must equal the replay's reply byte for
	 * byte */
	lcrep = canon_reply(rb, rbsz, cs.id, &lclen);
	if (lclen != replen || memcmp(lcrep, rep, replen) != 0) {
		FILE *df = fopen("/tmp/apptest-debug.live-reply", "wb");

		if (df) {
			fwrite(lcrep, 1, lclen, df);
			fclose(df);
		}
		fail(NAME);
		fprintf(stderr,
			"  %s: live reply (id canonicalised, %zu B, "
			"dumped to /tmp/apptest-debug.live-reply) "
			"diverges from the replay's reply (%zu B)\n",
			NAME, lclen, replen);
	}
	printf("%s: live id=0x%lx, sgr %d, diac %d; raw %zu B -> "
	       "canonical %zu B; live reply %zu B (id canonicalised); "
	       "replay reply %zu B\n",
	       NAME, cs.id, cs.sgr, cs.diac, raw_tb, clen, lclen, replen);

	{
		char pf[512], pr[512], pt[512];
		int bad = 0;

		snprintf(pf, sizeof pf, "%s/tests/goldens/%s.frame", root, NAME);
		snprintf(pr, sizeof pr, "%s/tests/goldens/%s.reply", root, NAME);
		snprintf(pt, sizeof pt, "%s/tests/goldens/%s.trace", root, NAME);
		if (update) {
			FILE *f = fopen(pf, "wb");

			if (!f) {
				bad = 1;
			} else {
				bad = fwrite(g_frame, 1, GSZ, f) != GSZ;
				fclose(f);
			}
			if ((f = fopen(pt, "wb"))) {
				bad |= fwrite(g_cb, 1, clen, f) != clen;
				fclose(f);
			} else
				bad = 1;
			if ((f = fopen(pr, "wb"))) {
				bad |= fwrite(rep, 1, replen, f) != replen;
				fclose(f);
			} else
				bad = 1;
			if (bad)
				fail(NAME);
			else
				printf("updated %s / %s / %s (trace %zu B, "
				       "reply %zu B)\n", pf, pr, pt,
				       clen, replen);
		} else {
			bad |= cmp_artefact("trace", g_cb, clen, pt);
			bad |= cmp_artefact("reply", rep, replen, pr);
			bad |= cmp_artefact("frame", g_frame, GSZ, pf);
			if (!bad && !app_fail)
				printf("%-18s ok (trace %zu B, reply %zu B)\n",
				       NAME, clen, replen);
			if (bad)
				fail(NAME);
		}
	}
	if (app_fail)
		fail(NAME);

	free(tb);
	free(rb);
	free(rep);
	free(lcrep);
	free(g_cb);
	free((void *) g_ideal);
	glyph_mmap_die();
	fprintf(stderr, "%sapptest: %d failure(s)\n",
		failures ? "": "", failures);
	return failures ? 1: 0;
}
