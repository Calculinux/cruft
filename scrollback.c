/* Scrollback ring + history browsing (Calculinux/cruft).
 * Rows are COPIED into the ring at drain time (see sb_feed_line); the
 * ring then owns them independently of the live screen. */
#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"

#include <stdlib.h>
#include <string.h>

/* Drop one stored row (sixel payloads first, then the row array). */
static void sb_row_free(const struct terminal_t *term, struct cell_t *row)
{
	if (!row)
		return;
	for (int x = 0; x < term->cols; x++)
		cell_pixmap_clear(&row[x]);
	free(row);
}

/* Slot of the kth-newest stored row (k: 0 = newest, count-1 = oldest). */
static struct cell_t *sb_row_at(const struct terminal_t *term, unsigned k)
{
	const struct scrollback_t *sb = &term->sb;

	return sb->rows[(sb->head + sb->cap - 1 - k) % sb->cap];
}

/* Enforce the total sixel payload cap, stripping the oldest art first.
 * Stripped cells keep their glyph (a space after erase), so history
 * degrades to blank where it once drew pictures. Cold path: runs only
 * after a row carrying pixmap cells lands in a hot ring. */
static void sb_shrink_pixmaps(struct terminal_t *term)
{
	struct scrollback_t *sb = &term->sb;
	const size_t cap_b = (size_t)SB_PIXMAP_CAP_KIB * 1024;
	const size_t cell_b = cell_pixmap_bytes();

	if (sb->pixmap_bytes <= cap_b)
		return;
	for (unsigned k = sb->count; k-- > 0 && sb->pixmap_bytes > cap_b;) {
		struct cell_t *row = sb_row_at(term, k);

		for (int x = 0; x < term->cols && sb->pixmap_bytes > cap_b; x++)
			if (row[x].has_pixmap) {
				cell_pixmap_clear(&row[x]);
				sb->pixmap_bytes -= cell_b;
			}
	}
}

/* CRUFT_HISTORY: total depth in rows (default SB_DEFAULT_DEPTH, 0 =
 * off, clamped to SB_DEPTH_MAX); CRUFT_SB_RAM: how many of those stay
 * in the RAM ring (default SB_RAM_LINES). Deeper requests spill the
 * older rows to the disk tier. Unusable values degrade to the
 * defaults. */
void sb_init(struct terminal_t *term)
{
	const char *env;
	long total = SB_DEFAULT_DEPTH, ram = SB_RAM_LINES;

	term->sb = (struct scrollback_t) {0};
	term->sb.disk.fd = -1; /* zeroed struct must not read as stdin */

	env = getenv("CRUFT_HISTORY");
	if (env && *env) {
		char *ep;

		total = strtol(env, &ep, 10);
		if (ep == env || total < 0)
			total = SB_DEFAULT_DEPTH;
	}
	if (total > SB_DEPTH_MAX)
		total = SB_DEPTH_MAX;

	env = getenv("CRUFT_SB_RAM");
	if (env && *env) {
		char *ep;

		ram = strtol(env, &ep, 10);
		if (ep == env || ram < 1)
			ram = SB_RAM_LINES;
	}
	if (ram > SB_DEPTH_MAX)
		ram = SB_DEPTH_MAX;

	term->sb.cap = (unsigned)(total < ram ? total: ram);
	if (term->sb.cap == 0)
		return;

	/* Bound-checked above to [0, SB_DEPTH_MAX]; the opt-in taint
	 * analyzer cannot see through strtol and would flag the size. */
	term->sb.rows = (struct cell_t **) calloc(term->sb.cap, sizeof(*term->sb.rows)); /* NOLINT(clang-analyzer-optin.taint.TaintedAlloc) */
	if (!term->sb.rows) {
		fprintf(stderr, "cruft: scrollback: ring alloc failed, disabled\n");
		term->sb.cap = 0;
		return;
	}
	term->sb.disk.cap = (unsigned)total - term->sb.cap;
}

/* Rows in flight across both tiers (0 = nothing to browse). */
unsigned sb_history_total(const struct terminal_t *term)
{
	return term->sb.count + term->sb.disk.count;
}

void sb_reset(struct terminal_t *term)
{
	struct scrollback_t *sb = &term->sb;

	if (sb->rows)
		for (unsigned i = 0; i < sb->cap; i++) {
			sb_row_free(term, sb->rows[i]);
			sb->rows[i] = NULL;
		}
	sb->count = sb->head = 0;
	sb->pixmap_bytes = 0;
	sb->view = 0;
	sbdisk_detach(term); /* unlinks the session file */
}

void sb_die(struct terminal_t *term)
{
	sb_reset(term);
	free(term->sb.rows);
	term->sb.rows = NULL;
	term->sb.cap = 0;
}

/* Copy a live row into the RAM ring (evicting the oldest, which spills
 * to disk when the tier is armed) and take ownership of any sixel
 * payloads it carried. While the viewer sits on stored rows, a push
 * increments sb.view so the browsed row stays in place. */
void sb_feed_line(struct terminal_t *term, const struct cell_t *src)
{
	struct scrollback_t *sb = &term->sb;
	const size_t cell_b = cell_pixmap_bytes();
	struct cell_t *row;
	unsigned slot, total0;
	bool pin;

	if (sb->cap == 0 || !sb->rows || term->cols < 1)
		return;

	total0 = sb_history_total(term);
	pin = sb->view >= (unsigned)term->lines && sb->view <= total0;

	slot = sb->head;
	if (sb->count == sb->cap) {
		struct cell_t *old = sb->rows[slot];

		/* The evicted row is the disk tier's intake: hand it over
		 * BEFORE freeing (store deep-copies the pixmap payloads). */
		if (sb->disk.cap)
			sbdisk_store(term, old);
		for (int x = 0; x < term->cols; x++)
			if (old[x].has_pixmap)
				sb->pixmap_bytes -= cell_b;
		sb_row_free(term, old);
		sb->count--;
	}

	row = (struct cell_t *) calloc((size_t)term->cols, sizeof(*row));
	if (!row) {
		fprintf(stderr, "cruft: scrollback: row alloc failed, line dropped\n");
		return;
	}
	memcpy(row, src, (size_t)term->cols * sizeof(*row));

	for (int x = 0; x < term->cols; x++) {
		if (!row[x].has_pixmap || !row[x].pixmap)
			continue;
		/* The row copy still ALIASES the live screen's pixmap block;
		 * splice in a deep copy and never free the aliased one. */
		struct cell_t tmp = {0};

		if (cell_pixmap_dup(&tmp, &row[x]) == 0) {
			row[x].pixmap = tmp.pixmap;
			sb->pixmap_bytes += cell_b;
		} else
			row[x].has_pixmap = false, row[x].pixmap = NULL; /* no alias kept */
	}

	sb->rows[slot] = row;
	sb->head = (slot + 1) % sb->cap;
	sb->count++;
	if (pin)
		sb->view++;
	/* Compaction can shed the oldest rows out from under the viewer. */
	if (sb->view > sb_history_total(term))
		sb->view = sb_history_total(term);
	sb_shrink_pixmaps(term);
}

/* Resolve logical line (0 = top of the rendered window) to a row.
 * Distances from the live tail under lines land in the live primary;
 * the next sb.count distances walk the RAM ring (newest first); deeper
 * distances pread the disk tier. Never returns NULL: a disk read
 * failure hands out a blank row and degrades the tier. */
const struct cell_t *sb_view_row(struct terminal_t *term, int line)
{
	const struct scrollback_t *sb = &term->sb;
	unsigned d = sb->view + (term->lines - 1 - line);

	/* Live rows come from whichever screen is active (view is forced to
	 * 0 on the alt screen, so the history arms are unreachable there). */
	if ((int)d < term->lines)
		return term->scr[term->active].cells[term->lines - 1 - d];
	d -= term->lines;
	if (d < sb->count)
		return sb->rows[(sb->head + sb->cap - 1 - d) % sb->cap];
	return sbdisk_fetch(term, d - sb->count);
}

/*
 * Navigation keys, recognised as a leading run of complete sequences;
 * anything not matching is forwarded to the program verbatim. The four
 * gestures:
 *   ESC [5~  ESC [6~    native page forms - on the PicoCalc the
 *                       keyboard driver maps Shift+Up / Shift+Down
 *                       to them
 *   ESC [1;5H ESC [1;5F xterm Ctrl+Home / Ctrl+End - the PicoCalc's
 *                       console keymap (picocalc.map) binds them to
 *                       the Ctrl+Home / Ctrl+End key pairs
 * Half-page steps; Home/End jump to the oldest row / the live tail.
 * Bare arrows, and bare Home/End (ESC [1~ / ESC [4~), stay the
 * program's. A page form that is the mere prefix of a longer
 * parameterised sequence (ESC [5;<n>~, modified page keys of some
 * terminals) is not matched. Interception happens on the main screen
 * only; on the alt screen the program owns every key.
 */
enum sb_action {
	SB_PAGE_UP, SB_PAGE_DOWN, SB_TOP, SB_BOTTOM,
};

static const uint8_t nav_seqs[][8] = {
	{ESC, '[', '5', '~'},
	{ESC, '[', '6', '~'},
	{ESC, '[', '1', ';', '5', 'H'},
	{ESC, '[', '1', ';', '5', 'F'},
};
static const int nav_seq_lens[] = {
	4, 4, 6, 6,
};
static const enum sb_action nav_actions[] = {
	SB_PAGE_UP, SB_PAGE_DOWN, SB_TOP, SB_BOTTOM,
};

/* The view snaps back to the live tail. Stored rows are immutable, so
 * the live lines stayed clean while we browsed and the lazy drawer
 * would leave history pixels on screen; re-flag every line so the
 * next refresh repaints the whole grid. */
static void sb_flag_all_lines(struct terminal_t *term)
{
	struct screen_t *scr = active_screen(term);

	for (int y = 0; y < term->lines; y++)
		scr->line_dirty[y] = true;
}

static void sb_apply(struct terminal_t *term, enum sb_action action)
{
	struct scrollback_t *sb = &term->sb;
	unsigned total = sb_history_total(term);
	unsigned half = term->lines / 2;
	unsigned old_view;

	if (half < 1)
		half = 1;
	old_view = sb->view;
	switch (action) {
	case SB_PAGE_UP:
		sb->view = (sb->view + half < total) ? sb->view + half: total;
		break;
	case SB_PAGE_DOWN:
		sb->view = (sb->view > half) ? sb->view - half: 0;
		break;
	case SB_TOP:
		sb->view = total;
		break;
	case SB_BOTTOM:
		sb->view = 0;
		break;
	}
	if (sb->view == 0 && old_view != 0)
		sb_flag_all_lines(term);
}

/* Longest suffix of s[len] that is a STRICT prefix of a navigation
 * sequence (0 = nothing worth holding). Raw stdin can deliver one
 * page key in separate reads (VMIN=1: "ESC", then "[5~"); handing the
 * prefix to the shell early splits the key. */
static unsigned sb_nav_hold_len(const uint8_t *s, unsigned len)
{
	unsigned maxl = 0, l;

	for (unsigned i = 0; i < sizeof(nav_seqs) / sizeof(nav_seqs[0]); i++)
		if ((unsigned)nav_seq_lens[i] - 1 > maxl)
			maxl = (unsigned)nav_seq_lens[i] - 1;
	/* Cap the CANDIDATE length, not the anchor: the suffix lives at
	 * the true end of the input even when foreign input precedes it. */
	for (l = (len > maxl) ? maxl: len; l > 0; l--)
		for (unsigned i = 0; i < sizeof(nav_seqs) / sizeof(nav_seqs[0]); i++)
			if (l < (unsigned)nav_seq_lens[i]
			    && memcmp(s + len - l, nav_seqs[i], l) == 0)
				return l;
	return 0;
}

/* Bytes of the leading navigation run (0 = nothing to intercept: the
 * whole buffer is the shell's). If keep is non-NULL it receives the
 * number of trailing bytes to carry into the next read: an unfinished
 * navigation sequence the terminal may complete in a later read (the
 * caller prepends them to that buffer). While held, the snap back to
 * the live tail is deferred too, so an ESC alone does not dismiss
 * browsing. */
int sb_consume_nav(struct terminal_t *term, const uint8_t *buf, int n,
		   unsigned *keep)
{
	unsigned consumed = 0, hold = 0;

	if (keep)
		*keep = 0;
	if (!vt_active || term->active != 0)
		return 0; /* alt screen: the app owns every key */
	if (sb_history_total(term) == 0)
		return 0; /* nothing stored: nav sequences pass through */

	while (consumed < (unsigned)n) {
		int matched = -1;
		uint8_t c;

		for (unsigned i = 0; i < sizeof(nav_seqs) / sizeof(nav_seqs[0]); i++) {
			int len = nav_seq_lens[i];

			if (consumed + (unsigned)len <= (unsigned)n
			    && memcmp(buf + consumed, nav_seqs[i], (size_t)len) == 0) {
				matched = (int)i;
				break;
			}
		}
		if (matched < 0)
			break;
		c = 0;
		if (consumed + (unsigned)nav_seq_lens[matched] < (unsigned)n)
			c = buf[consumed + (unsigned)nav_seq_lens[matched]];
		/* parameter bytes follow: a longer (modified) sequence,
		 * not our page key - let it through whole */
		if (c == ';' || (c >= '0' && c <= '9'))
			break;
		sb_apply(term, nav_actions[matched]);
		consumed += (unsigned)nav_seq_lens[matched];
	}
	/* Whatever follows a nav run is meant for the live screen: classic
	 * terminals snap back to the bottom on the first foreign key, and a
	 * mouseless console needs that (typing blind over stale history is
	 * the trap). Lone ESC included - unless the buffer ends on an
	 * unfinished nav prefix, whose verdict waits for the next read. */
	if (consumed < (unsigned)n) {
		hold = sb_nav_hold_len(buf + consumed, (unsigned)n - consumed);
		if (hold < (unsigned)n - (unsigned)consumed && term->sb.view != 0) {
			term->sb.view = 0;
			sb_flag_all_lines(term);
		}
	}
	if (keep)
		*keep = hold;
	return (int)consumed;
}

/* DRCS redefinition frees a charset table (drcs.c); history rows still
 * pointing into it would dangle. Scrub those cells to the substitute
 * glyph. The test is an ADDRESS RANGE comparison only: freed glyphs are
 * never dereferenced, the surviving width tag picks the stand-in. */
void sb_scrub_freed_glyphs(const struct terminal_t *term, const struct glyph_t *freed, int n)
{
	const struct glyph_t *lo = freed;
	const struct glyph_t *hi;

	if (!freed || n <= 0)
		return;
	hi = freed + n;
	for (unsigned k = 0; k < term->sb.count; k++) {
		struct cell_t *row = sb_row_at(term, k);

		for (int x = 0; x < term->cols; x++) {
			if (row[x].glyphp < lo || row[x].glyphp >= hi)
				continue;
			row[x].glyphp = (row[x].width == WIDE || row[x].width == NEXT_TO_WIDE)
				? glyph_get(SUBSTITUTE_WIDE): glyph_get(SUBSTITUTE_HALF);
		}
	}
}
