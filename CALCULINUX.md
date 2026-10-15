# Calculinux/cruft

Reduced-footprint framebuffer terminal for Calculinux (PicoCalc / Luckfox Lyra),
descended from [uobikiemukot/yaft](https://github.com/uobikiemukot/yaft) via the
Calculinux yaft fork. Linux `/dev/fb0` only; no BSD/X11 ports.

## Memory changes vs yaft

- Sparse cell sixel pixmaps (heap block per cell, not inline array)
- Lazy DRCS charset tables and lazy sixel canvas
- Optional FB shadow buffer only when `CRUFT=wall`
- Paged `CRUFTFN1` fonts (`tools/mkcruftfont`, default `/usr/share/cruft/console.cruftfont`)
- VT deactivate: main loop frees sixel canvas (not the signal handler); cell pixmaps kept for redraw

## VT deactivate

`SIGUSR2` only flips `vt_active` (async-signal-safe). The main loop calls
`term_release_transient` then `sigsuspend` so sixel decode cannot UAF on `free`.
Cell sixel pixmaps are kept for redraw after the next `SIGUSR1`.
`sigsuspend` deliberately allows `SIGTERM`/`SIGINT`/`SIGCHLD` so `systemctl stop`
can restart inactive VTs when changing `CONSOLE_FONT` (otherwise only tty1 updated).

## Screen state

Per-screen state (cell matrix, dirty flags, tabstops, scroll margins,
cursor, wrap flag) lives in `struct screen_t scr[2]`; `active_screen()`
(cruft.h) is the single routing point for all of it. Screen-independent
state (SGR/attribute, G0-G1, palettes, DRCS, sixel canvas) intentionally
stays in `struct terminal_t`. Selftest section R pins the routing.

DECALT (`CSI ?1049h/l`) is supported: `enter_alt_screen()` allocates
`scr[1]` lazily on first entry and keeps it between excursions (~one
cell matrix), wiping it on every entry (DECRLN). While the alt screen is
resident the primary is never written to — only the cursor and scroll
margins are parked in `term->alt_saved`. Doubled/mismatched 1049s are
no-ops; RIS ends the residence and drops the matrix; each transition
frees the sixel canvas (decode must not straddle a screen) and forces a
full repaint of the side being switched to. Terminfo advertises
`smcup`/`rmcup`, so ncurses full-screen apps (vim, htop, less, fzf) use
the alt screen and stop smearing the main buffer. Selftest section S
covers the lifecycle.

## Scrollback

History is **two tiers** over the rows drained from the primary.
Feeding: `scroll()` hands a row to `sb_feed_line()` only when a
*full-primary* scroll-up would erase it — sub-region scrolls (editor
panes) and the alt screen (wiped on entry anyway) never feed history.
Every tier OWNS its rows: cell structs and sixel pixmaps are
deep-copied at drain time (`cell_pixmap_dup`), so the screen's
subsequent erase cannot alias or corrupt stored content.

- RAM tier (`scrollback.c`): a ring of the newest `min(depth,
  CRUFT_SB_RAM)` rows (defaults: 96 / 512). Rows are allocated lazily
  as they are stored, so an idle session pays only for the pointer
  array (< 1 KiB at the default). Worst case at 6x12/53 cols:
  ~1.25 KiB per stored row, ~640 KiB at the 512-row RAM cap; within
  the 1536/4096 kB RSS budgets.
- Disk tier (`sbdisk.c`): when `CRUFT_HISTORY` exceeds the RAM tier,
  rows falling out of the ring spill to a session-scoped file in
  /var/tmp (`mkstemp`, 0600, unlinked on exit/reset). One
  variable-length record per row — packed cells (fg, bg, attr, width,
  bitmap) plus a sixel table, capped at 32 KiB of pixmaps per record
  (near-all-art rows store as blank text). Glyph POINTERS are not
  persistable, so records encode bitmaps and decode through an intern
  table: a row in the small decoded-row cache shares one glyph block
  per distinct (width, bitmap). An in-RAM dir of record offsets keeps
  browsing O(1) (128 KiB at the 16384 ceiling, grown lazily). When the
  file is full the oldest quarter is compacted away (rewrite burst,
  amortized over a quarter-cap of rows). Any unreadable file degrades
  the tier for the session — RAM keeps serving, the file is dropped.
  Little-endian, versioned header, session-scoped (never adopted
  across restarts).
- Sixel budget: the RAM ring carries at most 64 KiB of pixmap payload
  (`SB_PIXMAP_CAP_KIB`); overflowing strips the oldest art first
  (those cells keep their glyph, usually the space).
- Depth: `CRUFT_HISTORY` is the TOTAL across both tiers (default 96,
  clamp 16384, 0 = off); `CRUFT_SB_RAM` sizes the RAM tier (default
  512). Depths ≤ the RAM tier behave exactly as the pure-RAM ring.
- Viewport: `term->sb.view` counts stored rows lifted above the live
  screen; `sb_view_row()` maps logical lines to live, RAM or disk
  rows. While the window sits on stored rows, a new push increments
  `view` so the browsed row stays in place. While `view > 0` every
  refresh repaints every line (stored rows are immutable; the live
  dirty flags index live rows, not logical ones). The renderer anchors
  the grid left (the centred leftover moves wholesale right, a one-time
  1px shift at 6x12/320) and `fb_scrollbar` draws the 2px track/thumb
  in the yielded strip (sized with the total depth). Fonts that divide
  the width exactly have no strip and therefore no bar — the keys
  still work.
- Keys (man page, KEYBOARD): only Meta-prefixed sequences are consumed
  (`ESC ESC [ A/B/5~/6~/7~/8~`, CSI and SS3 forms); bare arrows always
  pass through so readline history stays intact, and any non-nav byte
  snaps the view back to the live tail (mouseless console: typing
  blind over stale history is the trap). In the alt screen every key
  belongs to the app.
- Hygiene: RIS clears the ring; DECALT entry forces `view = 0`;
  `drcs_clear_charset()` scrubs stored cells that referenced the freed
  DRCS table (address-range test, substitute glyph substituted — a
  DRCS redefinition would otherwise leave the ring dangling).

Selftest section T covers feeding rules, eviction order, viewport
mapping, nav keys, the pixmap cap and the disabled mode.

## Status bar

An 8px strip in the top margin, above the cell grid, rendered in the
4x8 status font (miniwi; `CRUFT_SB_FONT`, default
`/usr/share/cruft/status.cruftfont`): window title left, clock centre,
wifi stair + battery right. `term_init` shrinks the grid by
`statusbar_h()` pixels (320x320 + 6x12 font: 26 lines, no loss).

- Data: sysfs `power_supply`/`net` (`operstate`) and the Level column of
  `/proc/net/wireless` for the signal dBm (see the TODO(signal) in
  `status.c` before revisiting nl80211), polled once per second; the
  strip repaints only when the rendered content changes.
- Window title: the parser (`osc_sequence` in `parse.h`) captures the
  foreground program's window title (OSC 0/2 — the channel xterm-style
  emulators show in their title bars; mode 1, icon only, has no home
  on a framebuffer and is dropped) into `term->win_title`, sanitized
  to printable ASCII by `sb_title_set` at capture (byte-oriented UTF-8
  consumption, no locale: the 4x8 status font covers no non-ASCII
  glyphs, so accents/CJK would render as holes). The raw payload is
  snapshotted before `parse_arg` NUL-splits the esc buffer, since a
  title may contain `;`. Cleared on RIS; not part of save/restore
  (ESC 7/8). The left run is clamped by `sb_fit_left` to one cell
  short of the centred clock.
- Each source degrades independently: absent source hides its segment;
  no signal reading degrades the stair to on/off; a missing status font
  disables the bar and the grid reclaims the strip.
- `CRUFT_SB` selects segments (`time,wifi,battery,title`, `all`,
  `none`);
  `CRUFT_SB_12H=1` switches the clock to 12h. Compile-time
  default: `sb_default_segments` in `conf.h`.
- Dual-font: `glyph_sb_*` in `glyph_mmap.c` is a second instance of the
  demand-paged `CRUFTFN1` loader (4x8/24 metrics only); the main font
  path is untouched. FB type shapes live in `fb/fb.h` (definitions-free,
  so `status.c` can see them without dragging in the one-TU fb backend).

## Build / test

```bash
make clean && make && make test
```

`CRUFT_FONT` selects the font blob.
`TERM` is `cruft-256color`.
