/* See LICENSE for licence details. */
/*
 * status.c: top status bar in the margin strip above the terminal grid.
 *
 * The grid is shifted down by one line of the 4x8 status font
 * (term->oy, see term_init()); this file polls state at most once per
 * second and repaints the 8px strip only when its content or the
 * palette changes. Layout (right side right-anchored):
 *
 * [window title ........]        [HH:MM]   [wifi stair] [battery][NN%][->]
 *
 * Sources, all optional (a missing source hides its segment):
 *   title      window title the foreground app reports via OSC 0/2;
 *              captured by the parser (parse.h) into term->win_title
 *   wifi       /sys/class/net/<if>/operstate (phy80211 devices only)
 *   signal     /proc/net/wireless Level column, dBm
 *   battery    /sys/class/power_supply/<ps>/{capacity,status}
 */
#define CRUFT_NO_GLOBALS
/* util.h, like conf.h, defines its helpers; exactly one TU (cruft.c) may
 * include it. Declare what this TU instantiates (logging plus the e*
 * wrappers fb/common.h and fb/linux.h bodies call) instead. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#include <stddef.h>
#include <sys/types.h>
enum loglevel_t { DEBUG = 0, WARN, ERROR, FATAL };
void logging(enum loglevel_t loglevel, char *format, ...);
int eopen(const char *path, int flag);
int eclose(int fd);
void *emmap(void *addr, size_t len, int prot, int flag, int fd, off_t offset);
int emunmap(void *ptr, size_t len);
void *ecalloc(size_t nmemb, size_t size);
int my_ceil(int val, int div);
#include "cruft.h"
#include "conf.h"
/* fb/fb.h: the framebuffer struct shapes + inlines. Deliberately NOT
 * fb/common.h — that header defines the fb backend functions (one-TU
 * rule); status.c only needs the layout to paint the strip. */
#include "fb/fb.h"
#include "status.h"

#include <dirent.h>
#include <sys/stat.h>

#define SB_PATHLEN 512
#define SB_BUFLEN  64
/* dirents are up to 255 chars; room for NUL */
#define SB_NAMELEN 264
#define SB_DEF_FONT_PATH "/usr/share/cruft/status.cruftfont"

/* Strip palette (indices into the 256-color palette; see color.h) */
enum {
	SB_C_BG  = 0,  /* strip background (black) */
	SB_C_TXT = 7,  /* light grey: clock, percents, brackets, lit meter */
	SB_C_OFF = 8,  /* dark grey: unlit segments, discharge arrow */
	SB_C_RED = 9,  /* alarm: battery < 20%, weak signal */
	SB_C_GRN = 10, /* ok: battery >= 50%, strongest signal */
	SB_C_YEL = 11, /* caution: 20-49%, mid signal, charging */
};

enum {
	SEG_TIME  = 1 << 0,
	SEG_WIFI  = 1 << 1,
	SEG_BATT  = 1 << 2,
	SEG_TITLE = 1 << 3,
};

/* Test seams: sysfs roots */
const char *sb_net_root = "/sys/class/net";
const char *sb_ps_root  = "/sys/class/power_supply";

/* Icons (all present in miniwi); sb_ascii substitutes text labels when a
 * swapped-in 4x8 font lacks them. */
enum {
	I_UP   = 0x2191,
	I_DOWN = 0x2193,
	/* wifi stair columns, low to high */
	I_W1 = 0x2581, I_W2 = 0x2582, I_W3 = 0x2584, I_W4 = 0x2586,
	/* horizontal fill, 1/8 .. full (left blocks) */
	I_F1 = 0x258F, I_F2 = 0x258E, I_F3 = 0x258D, I_F4 = 0x258C,
	I_F5 = 0x258B, I_F6 = 0x258A, I_F7 = 0x2589, I_F8 = 0x2588,
};
static const uint32_t fill_glyph[9] = { 0x20, I_F1, I_F2, I_F3, I_F4,
	I_F5, I_F6, I_F7, I_F8 };

static int sb_on;
static uint8_t sb_seg;
static int sb_h12;
static int sb_ascii;
static time_t sb_last = -1;
static int sb_dirty = 1;
static int sb_prev_ok;
static struct sb_draw sb_prev;
static char sb_wifi_ifn[SB_NAMELEN];
static char sb_batt_ent[SB_NAMELEN];

/* ------------------------------------------------------------------ */
/* small file helpers (quiet: absence is a normal, unlogged state)     */
/* ------------------------------------------------------------------ */

static int sb_stat_ok(const char *p)
{
	struct stat st;

	return stat(p, &st) == 0;
}

static int sb_read_i(const char *p, long *v)
{
	FILE *fp;
	char b[48];
	char *end;
	long val;

	fp = fopen(p, "r");
	if (!fp)
		return 0;
	if (!fgets(b, sizeof b, fp)) {
		fclose(fp);
		return 0;
	}
	fclose(fp);
	val = strtol(b, &end, 10);
	if (end == b)
		return 0;
	*v = val;
	return 1;
}

/* First whitespace/newline-delimited token of a file */
static int sb_read_tok(const char *p, char *b, size_t blen)
{
	FILE *fp;

	b[0] = '\0';
	fp = fopen(p, "r");
	if (!fp)
		return 0;
	if (!fgets(b, (int)blen, fp)) {
		fclose(fp);
		return 0;
	}
	fclose(fp);
	for (char *c = b; *c; c++)
		if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
			*c = '\0';
			break;
		}
	return b[0] != '\0';
}

/* --------------------------------------------------------------- */
/* device discovery (cache + recover on disappearance/appearance)  */
/* --------------------------------------------------------------- */

typedef int (*sb_pick_fn)(const char *entry, void *ctx);

/* Lexicographically first directory entry in root satisfying fn() */
static const char *sb_first_match(const char *root, sb_pick_fn fn, void *ctx)
{
	DIR *d;
	struct dirent *e;
	static char name[SB_NAMELEN];
	int found = 0;

	name[0] = '\0';
	if ((d = opendir(root)) == NULL)
		return NULL;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		if (!fn(e->d_name, ctx))
			continue;
		if (!found || strcmp(e->d_name, name) < 0) {
			snprintf(name, sizeof name, "%.255s", e->d_name);
			found = 1;
		}
	}
	closedir(d);
	return found ? name: NULL;
}

static int pick_wifi(const char *n, void *ctx)
{
	char p[SB_PATHLEN];

	snprintf(p, sizeof p, "%s/%.255s/phy80211", (const char *)ctx, n);
	return sb_stat_ok(p);
}

static int pick_batt(const char *n, void *ctx)
{
	char p[SB_PATHLEN];

	snprintf(p, sizeof p, "%s/%.255s/capacity", (const char *)ctx, n);
	if (!sb_stat_ok(p))
		return 0;
	/* some capacity-bearing entries are USB buses/chargers; require a
	 * battery type, but tolerate drivers that omit the file */
	snprintf(p, sizeof p, "%s/%.255s/type", (const char *)ctx, n);
	if (sb_stat_ok(p)) {
		char t[32] = "";

		if (sb_read_tok(p, t, sizeof t) && strcmp(t, "Battery") != 0)
			return 0;
	}
	return 1;
}

static void sb_retain(const char *root, char *slot, const char *probe)
{
	char p[SB_PATHLEN];

	if (!slot[0])
		return;
	snprintf(p, sizeof p, "%s/%s/%s", root, slot, probe);
	if (!sb_stat_ok(p))
		slot[0] = '\0'; /* disappeared: rediscover next tick */
}

/* ----------------------------------------------------- */
/* wifi signal: /proc/net/wireless Level column, in dBm  */
/* ----------------------------------------------------- */

/* TODO(signal): /proc/net/wireless is the pragmatic source on the target
 * stack (6.1.99-rockchip + vendorized rtw89): its ctrl family refuses
 * by-name genl resolution (ENOENT), so the nl80211 GET_STATION path that
 * used to live here could never retrieve a family id there, while the
 * proc Level column is live and tracks the AP distance well. Revisit on
 * a newer kernel: GET_STATION|NLM_F_DUMP with the nested
 * NL80211_ATTR_STA_INFO/NL80211_STA_INFO_SIGNAL attributes gives a
 * per-BSS dBm reading and outlives drivers that stop populating the WEXT
 * compat proc; resolve the family via CTRL_CMD_GETFAMILY, genl version 2. */

/* Level column of one /proc/net/wireless line. The kernel field-formats
 * the interface name (six-char right-justified), so rows look like
 * " wlan0: 0000   49.  -61.  -256  ..."; leading/trivial padding is
 * tolerated. Fields after the colon: status, quality, Level in dBm,
 * noise. -999 when the line is not for our interface or the Level is
 * absent/invalid. Pure; golden-tested. */
int sb_wireless_level(const char *line, const char *ifname)
{
	size_t n = strlen(ifname);
	const char *s, *c;
	double lvl;

	s = line;
	while (*s == ' ')
		s++;
	if (strncmp(s, ifname, n) != 0)
		return -999;
	s += n;
	while (*s == ' ')
		s++;
	if (*s != ':')
		return -999;
	c = s + 1;
	if (sscanf(c, " %*s %*s %lf", &lvl) != 1)
		return -999;
	if (lvl >= -1 || lvl < -128)
		return -999;
	return (int)lvl;
}

/* Own-station signal in dBm (-999 when absent). One tiny read per
 * poll; no sockets. */
static int sb_proc_rssi(const char *ifname)
{
	FILE *fp = fopen("/proc/net/wireless", "r");
	char line[128];
	int v = -999;

	if (!fp)
		return -999;
	while (v == -999 && fgets(line, (int)sizeof line, fp))
		v = sb_wireless_level(line, ifname);
	fclose(fp);
	return v;
}

/* ---------------------------------------------------------- */
/* polled state                                                */
/* ---------------------------------------------------------- */

void statusbar_poll_now(struct sb_state *st, time_t now)
{
	memset(st, 0, sizeof *st);
	st->now  = now;
	st->wifi = -1;
	st->battery = -1;

	if (sb_seg & SEG_WIFI) {
		char p[SB_PATHLEN], b[32] = "";

		if (!sb_wifi_ifn[0]) {
			const char *n = sb_first_match(sb_net_root, pick_wifi,
				(void *)sb_net_root);
			if (n)
				snprintf(sb_wifi_ifn, sizeof sb_wifi_ifn,
					 "%.255s", n);
		}
		if (sb_wifi_ifn[0]) {
			int lvl;

			sb_retain(sb_net_root, sb_wifi_ifn, "operstate");
			if (sb_wifi_ifn[0]) {
				snprintf(p, sizeof p, "%s/%s/operstate",
					sb_net_root, sb_wifi_ifn);
				sb_read_tok(p, b, sizeof b);
				/* associated per 80211: RUNNING ("up") or
				 * carrier (associated, not promoted yet);
				 * "down"/"unknown" = not associated */
				if (strcmp(b, "up") == 0
				    || strcmp(b, "carrier") == 0) {
					lvl = sb_rf_level(sb_proc_rssi(sb_wifi_ifn));
					st->wifi = lvl ? lvl: 1; /* no reading: weakest */
				} else {
					st->wifi = 0;
				}
			}
		}
	}

	if (sb_seg & SEG_BATT) {
		char p[SB_PATHLEN], t[32] = "";
		long cap;

		if (!sb_batt_ent[0]) {
			const char *n = sb_first_match(sb_ps_root, pick_batt,
				(void *)sb_ps_root);
			if (n)
				snprintf(sb_batt_ent, sizeof sb_batt_ent,
					 "%.255s", n);
		}
		if (sb_batt_ent[0]) {
			sb_retain(sb_ps_root, sb_batt_ent, "capacity");
			if (sb_batt_ent[0]) {
				snprintf(p, sizeof p, "%s/%s/capacity",
					sb_ps_root, sb_batt_ent);
				if (sb_read_i(p, &cap)) {
					if (cap < 0)
						cap = 0;
					if (cap > 100)
						cap = 100;
					st->battery = (int)cap;
					snprintf(p, sizeof p, "%s/%s/status",
						sb_ps_root, sb_batt_ent);
					/* "Charging"/"Discharging"/"NotCharging"/
					 * "Full"; match the exact token ("NotCharging"
					 * contains "Charging" as a substring) */
					st->charging =
						sb_read_tok(p, t, sizeof t) &&
						strcmp(t, "Charging") == 0;
				}
			}
		}
	}

}

/* ------------------------------------------------------------- */
/* config + pure formatting (golden-tested from the selftest)    */
/* ------------------------------------------------------------- */

uint8_t sb_parse_segments(const char *spec, int *unknown)
{
	uint8_t mask = 0;
	char b[64];
	char *tok;

	*unknown = 0;
	if (!spec || !spec[0])
		return 0;
	snprintf(b, sizeof b, "%s", spec);
	for (char *p = b; *p; p++) { /* lowercase; squeeze blanks */
		if (*p == ' ' || *p == '\t')
			*p = ',';
		else if (*p >= 'A' && *p <= 'Z')
			*p = (char)(*p + 'a' - 'A');
	}
	if (strcmp(b, "all") == 0)
		return (uint8_t)(SEG_TIME | SEG_WIFI | SEG_BATT | SEG_TITLE);
	if (strcmp(b, "none") == 0 || strcmp(b, "0") == 0)
		return 0;
	for (tok = strtok(b, ","); tok; tok = strtok(NULL, ",")) {
		if (strcmp(tok, "time") == 0)
			mask |= SEG_TIME;
		else if (strcmp(tok, "wifi") == 0)
			mask |= SEG_WIFI;
		else if (strcmp(tok, "battery") == 0)
			mask |= SEG_BATT;
		else if (strcmp(tok, "title") == 0)
			mask |= SEG_TITLE;
		else if (tok[0])
			*unknown = 1;
	}
	return mask;
}

uint8_t sb_select(void)
{
	return sb_seg;
}

void sb_fmt_time(time_t now, int h12, char *out)
{
	struct tm tm;

	localtime_r(&now, &tm);
	if (h12)
		snprintf(out, 12, "%d:%02d%c%c",
			(tm.tm_hour % 12) ? (tm.tm_hour % 12): 12,
			tm.tm_min,
			tm.tm_hour < 12 ? 'a': 'p', 'm');
	else
		snprintf(out, 12, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

int sb_rf_level(int rssi)
{
	if (rssi <= -128 || rssi >= 0)
		return 0; /* no reading */
	if (rssi >= -55)
		return 4;
	if (rssi >= -67)
		return 3;
	if (rssi >= -78)
		return 2;
	return 1;
}

uint8_t sb_batt_color(int pct)
{
	if (pct >= 50)
		return SB_C_GRN;
	if (pct >= 20)
		return SB_C_YEL;
	return SB_C_RED;
}

/* ---------------------------------------------------------- */
/* window title (OSC 0/2): capture + left-zone fit            */
/* ---------------------------------------------------------- */

/* Length of the well-formed UTF-8 sequence starting at s, 0 when s does
 * not begin one. Conservative on the extremes: the goal is to consume a
 * non-ASCII sequence whole so its bytes never reach the title, not to
 * police every pathological codepoint (paths beyond U+10FFFF, 0xF6/0xF7
 * leads, are simply treated as malformed and shed one byte at a time).
 * s must hold at least 1 byte; rem is the bytes remaining at s. */
static int sb_utf8_seq_len(const unsigned char *s, size_t rem)
{
	unsigned char c = s[0], lo1 = 0x80, hi1 = 0xBF;
	int seq = 0;

	if (c >= 0xC2 && c <= 0xDF)
		seq = 2;
	else if (c == 0xE0)
		seq = 3, lo1 = 0xA0;       /* no overlong */
	else if (c == 0xED)
		seq = 3, hi1 = 0x9F;       /* no surrogates */
	else if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF))
		seq = 3;
	else if (c == 0xF0)
		seq = 4, lo1 = 0x90;       /* no overlong */
	else if (c == 0xF5)
		seq = 4, hi1 = 0x8F;       /* U+14_0000 .. U+10FFFF only */
	else if (c >= 0xF1 && c <= 0xF4)
		seq = 4;
	if (!seq || rem < (size_t)seq || s[1] < lo1 || s[1] > hi1)
		return 0;
	for (int k = 2; k < seq; k++)
		if (s[k] < 0x80 || s[k] > 0xBF)
			return 0;
	return seq;
}

int sb_title_set(char *out, const char *in)
{
	char tmp[SB_TITLE_MAX + 1];
	const unsigned char *s = (const unsigned char *)in;
	size_t len = strlen(in), oi = 0;

	for (size_t i = 0; i < len; i++) {
		int n;

		if (s[i] < 0x80) {
			if (s[i] >= 0x20 && s[i] <= 0x7E && oi < SB_TITLE_MAX)
				tmp[oi++] = (char)s[i];
			continue; /* control chars and DEL dropped */
		}
		/* non-ASCII: consume the sequence whole (anyway, it is
		 * dropped - the status font covers no such glyphs) */
		n = sb_utf8_seq_len(s + i, len - i);
		i += n ? (size_t)n - 1: 0;
	}
	tmp[oi] = '\0';

	if (strcmp(tmp, out) == 0)
		return 0;
	memcpy(out, tmp, oi + 1);
	return 1;
}

int sb_fit_left(int left_n, int center_n, int total_cells)
{
	/* leftmost cell of the centered run: the renderer anchors it at
	 * (w - center*wcell) / 2, which in whole cells is exactly
	 * (total - center_n) / 2 (the sub-cell pixel remainder divided by
	 * two can never cross another cell boundary). Leave one blank
	 * cell between the runs. */
	int c0 = (total_cells - center_n) / 2 - 1;

	if (c0 < 0)
		return 0;
	return (left_n < c0) ? left_n: c0;
}

/* ----------------------------------------------------------------- */
/* item builder (pure): polled state -> styled runs per zone         */
/* ----------------------------------------------------------------- */

static void run_push(struct sb_run *r, uint32_t cp, uint8_t col)
{
	if (r->n < SB_RUN_MAX) {
		r->it[r->n].cp = cp;
		r->it[r->n].col = col;
		r->n++;
	}
}

static void run_str(struct sb_run *r, const char *s, uint8_t col)
{
	for (; *s; s++)
		run_push(r, (uint32_t)(unsigned char)*s, col);
}

static void run_sp(struct sb_run *r, int k)
{
	for (int i = 0; i < k; i++)
		run_push(r, 0x20, SB_C_BG);
}

void sb_build_items(const struct sb_state *st, uint8_t seg, int h12,
                    int ascii, struct sb_draw *d)
{
	char num[16];

	memset(d, 0, sizeof *d);

	/* left zone: window title (OSC 0/2); already sanitized to
	 * printable ASCII at capture (sb_title_set) */
	if ((seg & SEG_TITLE) && st->title && st->title[0])
		run_str(&d->left, st->title, SB_C_TXT);

	sb_fmt_time(st->now, h12, num);
	if (seg & SEG_TIME)
		run_str(&d->center, num, SB_C_TXT);

	/* right zone, left to right: wifi stair (when present), then
	 * battery; the battery keeps the right edge (arrow rightmost) */
	{
		int did_wifi = 0;

		if ((seg & SEG_WIFI) && st->wifi >= 0) {
			static const uint32_t stair[4] = { I_W1, I_W2, I_W3,
				I_W4 };
			uint8_t lc = (st->wifi == 4) ? SB_C_GRN:
				(st->wifi == 3) ? SB_C_YEL: SB_C_RED;

			if (!ascii) {
				for (int c = 0; c < 4; c++)
					run_push(&d->right, stair[c],
						(st->wifi > 0 && c < st->wifi)
							? lc: SB_C_OFF);
			} else {
				run_str(&d->right, "WIFI", SB_C_TXT);
				run_sp(&d->right, 1);
				if (st->wifi > 0) {
					snprintf(num, sizeof num, "%d/4",
						 st->wifi);
					run_str(&d->right, num, SB_C_TXT);
				} else {
					run_str(&d->right, "off", SB_C_TXT);
				}
			}
			did_wifi = 1;
		}

		if ((seg & SEG_BATT) && st->battery >= 0) {
			uint8_t bc = sb_batt_color(st->battery);

			if (did_wifi)
				run_sp(&d->right, 2);
			if (!ascii) {
				/* 4 cells, each 0..1 full; fill in cell-hundredths
				 * (100% == 4 cells == 400) */
				run_push(&d->right, '[', SB_C_TXT);
				for (int c = 0; c < 4; c++) {
					int f100 = st->battery * 4 - c * 100;
					int e;

					if (f100 > 100)
						f100 = 100;
					if (f100 < 0)
						f100 = 0;
					e = (int)((f100 * 8u + 50u) / 100u);
					run_push(&d->right, fill_glyph[e], bc);
				}
				run_push(&d->right, ']', SB_C_TXT);
			} else {
				run_str(&d->right, "BAT", SB_C_TXT);
				run_sp(&d->right, 1);
			}
			snprintf(num, sizeof num, "%d%%", st->battery);
			run_str(&d->right, num, SB_C_TXT);
			run_push(&d->right,
				ascii ? (st->charging ? '+': '-')
				       : (st->charging ? I_UP: I_DOWN),
				st->charging ? SB_C_YEL: SB_C_OFF);
		}
	}
}

/* ----------------------------------------------------------------- */
/* renderer                                                          */
/* ----------------------------------------------------------------- */

static void sb_draw_items(struct framebuffer_t *fb, int x0, int maxw,
			  const struct sb_run *r)
{
	uint8_t *dest = fb_draw_dest(fb);
	int w = g_sb_cell_w, h = g_sb_cell_h;
	int fit = maxw / w;
	int n = (r->n < fit) ? r->n: fit;

	for (int i = 0; i < n; i++) {
		const struct glyph_t *g = glyph_sb_lookup(r->it[i].cp);
		uint32_t pix;
		int x = x0 + i * w;

		if (!g)
			continue; /* missing glyph: leave the strip bg */
		/* real_palette[] holds packed device pixels (rebuilt by refresh()
		 * on palette changes); draw_line writes them directly — the same
		 * applies here. Feeding them through color2pixel() again garbles
		 * sub-8bpp visuals (e.g. RGB565 white turns cyan). */
		pix = fb->real_palette[r->it[i].col];
		for (int hh = 0; hh < h; hh++) {
			for (int ww = 0; ww < w; ww++) {
				int pos = (x + (w - 1 - ww)) * fb->info.bytes_per_pixel
					+ (hh) * fb->info.line_length;

				/* same LSB=rightmost-pixel convention as draw_line */
				if (g->bitmap[hh] & (0x01 << ww))
					memcpy(dest + pos, &pix, fb->info.bytes_per_pixel);
			}
		}
	}
}

void statusbar_render(struct framebuffer_t *fb, struct terminal_t *term)
{
	struct sb_state st;
	struct sb_draw draw;
	uint8_t *dest;
	uint32_t bgpix;
	int h, w;

	if (!sb_on)
		return;

	time_t now = time(NULL);
	if (!sb_dirty && now == sb_last)
		return;
	sb_last = now;
	sb_dirty = 0;

	statusbar_poll_now(&st, now);
	st.title = term->win_title;
	sb_build_items(&st, sb_seg, sb_h12, sb_ascii, &draw);
	w = term->width;
	/* the left run must stop a full cell short of the centered clock */
	draw.left.n = sb_fit_left(draw.left.n, draw.center.n, w / g_sb_cell_w);
	if (sb_prev_ok && memcmp(&draw, &sb_prev, sizeof draw) == 0)
		return; /* nothing to repaint */
	sb_prev = draw;
	sb_prev_ok = 1;

	h = g_sb_cell_h;
	dest = fb_draw_dest(fb);

	/* solid strip: wallpaper mode shows the wallpaper in the side/bottom
	 * margins only; the bar itself is opaque for legibility. Palette
	 * entries are already packed pixels (see sb_draw_items). */
	bgpix = fb->real_palette[SB_C_BG];
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			memcpy(dest + y * fb->info.line_length +
			       x * fb->info.bytes_per_pixel,
			       &bgpix, fb->info.bytes_per_pixel);

	sb_draw_items(fb, 0, w, &draw.left);
	sb_draw_items(fb, (w - draw.center.n * g_sb_cell_w) / 2,
		      w, &draw.center);
	sb_draw_items(fb, w - draw.right.n * g_sb_cell_w, w, &draw.right);

	/* shadow mode: per-line blits only flush grid rows; flush the strip */
	if (fb->buf)
		memcpy(fb->fp, fb->buf, (long)h * fb->info.line_length);
}

/* ----------------------------------------------------------------- */
/* lifecycle                                                         */
/* ----------------------------------------------------------------- */

static int sb_have_icons(void)
{
	static const uint32_t need[] = {
		I_UP, I_DOWN, I_W1, I_W2, I_W3, I_W4,
		I_F1, I_F2, I_F3, I_F4, I_F5, I_F6, I_F7, I_F8,
		'[', ']', ':', '%',
	};

	for (size_t i = 0; i < sizeof need / sizeof need[0]; i++)
		if (!glyph_sb_lookup(need[i]))
			return 0;
	return 1;
}

void statusbar_init(void)
{
	const char *env = getenv("CRUFT_SB");
	const char *fenv = getenv("CRUFT_SB_FONT");
	const char *cenv = getenv("CRUFT_SB_12H");
	const char *font, *spec;
	uint8_t mask;
	int unknown = 0;

	sb_h12 = (cenv && strcmp(cenv, "1") == 0) ? 1: 0;
	spec = (env && env[0]) ? env: sb_default_segments;
	mask = sb_parse_segments(spec, &unknown);

	if (mask == 0) {
		logging(DEBUG, "cruft: statusbar: off (CRUFT_SB)\n");
		return;
	}
	font = (fenv && fenv[0]) ? fenv: SB_DEF_FONT_PATH;
	if (!glyph_sb_init(font)) {
		if (env && env[0])
			logging(WARN,
				"cruft: statusbar: off, status font %s unusable\n",
				font);
		return;
	}
	if (!sb_have_icons()) {
		sb_ascii = 1;
		logging(WARN,
			"cruft: statusbar: font %s lacks icon glyphs, using text\n",
			font);
	}
	sb_on = 1;
	sb_seg = mask;
	sb_dirty = 1;
	sb_prev_ok = 0;
	sb_last = -1;
	statusbar_reset_discovery();
	if (unknown)
		logging(WARN, "cruft: statusbar: ignoring unknown segment names\n");
	logging(DEBUG, "cruft: statusbar: on (%s, %s clock)\n",
		env && env[0] ? env: "default",
		sb_h12 ? "12h": "24h");
}

void statusbar_die(void)
{
	glyph_sb_die();
	sb_on = 0;
	sb_seg = 0;
	sb_dirty = 1;
	sb_prev_ok = 0;
	sb_last = -1;
}

int statusbar_h(void)
{
	return (sb_on && g_sb_cell_h) ? g_sb_cell_h: 0;
}

void statusbar_invalidate(void)
{
	sb_dirty = 1;
	sb_prev_ok = 0;
}

void statusbar_reset_discovery(void)
{
	sb_wifi_ifn[0] = '\0';
	sb_batt_ent[0] = '\0';
}
