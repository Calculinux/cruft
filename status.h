/* See LICENSE for licence details. */
/*
 * status.h: top status bar rendered into the margin strip above the
 * terminal grid. 4x8 font (normally miniwi): clock in the center,
 * wifi stair + battery on the right.
 *
 * Data: sysfs (power_supply, net operstate) and /proc/net/wireless
 * (signal strength), polled once per second from the refresh() path.
 * Segments whose source is absent simply vanish; the bar itself is
 * disabled (strip reverts to normal margin) when the font is missing
 * or CRUFT_SB selects no segment.
 */
#ifndef CRUFT_STATUS_H
#define CRUFT_STATUS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct framebuffer_t;
struct terminal_t;

/* Strip height in px: status font cell height when the bar is on, 0 off.
 * Read by term_init() (grid geometry) and the painters. */
int statusbar_h(void);

/* Configure from env (CRUFT_SB, CRUFT_SB_12H, CRUFT_SB_FONT, falling back
 * to the sb_default_segments compile default) and load the status font.
 * Never fatal: any failure just leaves the bar off. Call before term_init. */
void statusbar_init(void);

/* Release the status font. */
void statusbar_die(void);

/* Poll at most once per second; repaint the strip when its content (or
 * the palette) changed. No-op when the bar is off. */
void statusbar_render(struct framebuffer_t *fb, struct terminal_t *term);

/* Force the next render (full redraws, palette changes). */
void statusbar_invalidate(void);

/* Polled values; -1 = source absent. */
struct sb_state {
	time_t now;
	int wifi;       /* -1 none / 0 off / 1-4 signal level */
	int battery;    /* 0-100 */
	int charging;   /* 0/1 */
};

/* Poll one state snapshot (readers + discovery). Exposed for tests;
 * render() uses the same path with a cached once-per-second gate. */
void statusbar_poll_now(struct sb_state *st, time_t now);

/* Styled item (glyph + palette index) and per-zone run */
#define SB_RUN_MAX 32

struct sb_item {
	uint32_t cp;
	uint8_t col;
};

struct sb_run {
	int n;
	struct sb_item it[SB_RUN_MAX];
};

struct sb_draw {
	struct sb_run left, center, right;
};

/* Poll + config -> styled runs per zone (left, center, right). Pure;
 * golden-tested from the selftest. h12: 12h clock; ascii: text labels
 * instead of icon glyphs. */
void sb_build_items(const struct sb_state *st, uint8_t seg, int h12,
                    int ascii, struct sb_draw *d);

/* Segment selection mask (status.c owns the bits; init/builder agree). */
uint8_t sb_parse_segments(const char *spec, int *unknown);
uint8_t sb_select(void);

/* "14:05" or "2:05pm" (out >= 12). */
void sb_fmt_time(time_t now, int h12, char *out);

/* RSSI dBm -> stair level 1-4 (0 = n/a). */
int sb_rf_level(int rssi);

/* /proc/net/wireless line -> Level in dBm (-999 when the line is not for
 * the interface or the reading is absent/invalid). Pure; golden-tested. */
int sb_wireless_level(const char *line, const char *ifname);

/* Battery fill color index for a capacity. */
uint8_t sb_batt_color(int pct);

/* Test seams: sysfs roots (production values below). */
extern const char *sb_net_root;
extern const char *sb_ps_root;
void statusbar_reset_discovery(void);

#endif /* CRUFT_STATUS_H */
