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
- Keys (man page, KEYBOARD): only the native page forms (`ESC [5~` /
  `ESC [6~` - Shift+Up / Shift+Down on the PicoCalc) and the
  Ctrl+Home / Ctrl+End forms (`ESC [1;5H` / `ESC [1;5F`, bound in the
  PicoCalc console keymap) are consumed; bare arrows, bare Home/End,
  and other modifier forms always pass through so readline history
  stays intact, and any non-nav byte snaps the view back to the live
  tail (mouseless console: typing blind over stale history is the
  trap). In the alt screen every key belongs to the app.
- Hygiene: RIS clears the ring; DECALT entry forces `view = 0`;
  `drcs_clear_charset()` scrubs stored cells that referenced the freed
  DRCS table (address-range test, substitute glyph substituted — a
  DRCS redefinition would otherwise leave the ring dangling).

Selftest section T covers feeding rules, eviction order, viewport
mapping, nav keys, the pixmap cap and the disabled mode.

## Status bar

An 8px strip in the top margin, above the cell grid, rendered in the
4x8 status font (miniwi; `CRUFT_SB_FONT`, default
`/usr/share/cruft/status.cruftfont`): clock centre, wifi stair +
battery right. `term_init` shrinks the grid by
`statusbar_h()` pixels (320x320 + 6x12 font: 26 lines, no loss).

- Data: sysfs `power_supply`/`net` (`operstate`) and the Level column of
  `/proc/net/wireless` for the signal dBm (see the TODO(signal) in
  `status.c` before revisiting nl80211), polled once per second; the
  strip repaints only when the rendered content changes.
- Each source degrades independently: absent source hides its segment;
  no signal reading degrades the stair to on/off; a missing status font
  disables the bar and the grid reclaims the strip.
- `CRUFT_SB` selects segments (`time,wifi,battery`, `all`, `none`);
  `CRUFT_SB_12H=1` switches the clock to 12h. Compile-time
  default: `sb_default_segments` in `conf.h`.
- Dual-font: `glyph_sb_*` in `glyph_mmap.c` is a second instance of the
  demand-paged `CRUFTFN1` loader (4x8/24 metrics only); the main font
  path is untouched. FB type shapes live in `fb/fb.h` (definitions-free,
  so `status.c` can see them without dragging in the one-TU fb backend).

## Image protocols (sixel / kitty DCS / iTerm2)

Three image paths land on the same sparse per-cell pixmaps, so scroll/erase/
alt-screen/VT-deactivate behave identically for all of them:

- Sixel (DCS 62 t): long-standing path, unchanged (`sixel_canvas.c`,
  `terminal_cell.c`).
- **Kitty graphics protocol (DCS `+` / APC `_G`)** — `kitty_image.c`. The real
  kitty wire protocol (kovidgoyal/kitty graphics-protocol spec); ratatui-image
  speaks this, and the capability probes let its picker upgrade to Kitty
  instead of falling back to halfblocks:
  - Both envelopes are accepted: `ESC + <par>;<b64> ESC \` (classic DCS) and
    `ESC _ G <par>;<b64> ESC \` (APC — normative in the shipped spec, and what
    ratatui emits). `ESC _` parks the parser in `STATE_KPND`; only the `G`
    introducer promotes it, and `esc_t.dcs_kind` steers `dcs_sequence()` so
    `ESC P` stays on the sixel path.
  - Parameters per spec: `i/I/s/v/f/m/c/r/q/a/U/t/o/d`. Chunked transfers
    (`m=1` ... `m=0`) assemble in a growing raw buffer; the completion chunk
    carries none of the header's parameters, so the header's are kept in the
    transfer state and steer the outcome (store vs paint, the id-ed reply,
    `q=` suppression).
  - Formats: f=24/32/34 (RGB/A, red-first), f=33/35 (BGR/A, blue-first),
    f=100 PNG. `U=1` stores the fitted image in a small slot table keyed by
    image id; `a=p` re-places it at the cursor, `a=d` deletes it, replies are
    `ESC _ G i=<id>;OK ESC \`-shaped and honour `q=`. Direct placement (a=T
    without U) paints at the cursor with the spec epilogue: cursor right of
    the placed columns, down the placed lines, clamped to the grid (never
    scrolls).
  - Display: the renderer prints U+10EEEE placeholder cells carrying the id in
    a truecolor SGR foreground (low 24 bits as rgb, the high byte riding on a
    third diacritic); every row head wears its (row, col) diacritics, the rest
    inherit. `kitty_ph_feed()` (addch front door) swallows those cells —
    combining marks would otherwise be dropped as zero-width — advances the
    cursor as a width-1 glyph, resolves the anchor (nd>=2 at row 0, col 0)
    into the stored image, fits it into the cell box and paints with cursor
    save/restore; newlines count the region's rows until the placement is
    complete.
  - Capability replies: DA1 gains the sixel bit (`ESC [?62;4;6c`), `CSI 16t`
    reports the cell pixel size (`ESC [16;<h>;<w>t`, height first, matching
    ratatui's matcher), `i=31` gets the byte-exact `ESC _ G i=31;OK ESC \`,
    DSR/CPR were already answered.
  - Documented deviations: truecolor SGR 38;2/48;2 parks the exact RGB in
    reserved palette slots 254/255 so ids survive unquantized (those slots
    shadow two palette grays while the dynamic colour is in effect);
    zlib/SHM/file payloads are declined politely; no file/t= transport.
- **iTerm2 graphics (OSC 1337 `File ;`)** — the third transport, sharing
  `raw_expand`/`png_decode` with the DCS path:
  - `File ;` transfer only: raw BGR/BGRA (byte order per the protocol) and PNG.
    `w=/h=` are mandatory for raw streams; PNG natural size is trusted over any
    claimed `w=/h=`. Channel count (3 vs 4) is inferred from the byte count.
  - `Image ;` operators (chunked upload, query, delete, resize, ...) are not
    implemented: every one is answered `ESC ]1337 ; Rep ; i=<id> ; ok=false ;
    t=<reason> BEL` so probing clients degrade to text rather than hanging.

Shared placement for all image paths:
- Budgets: decoded image capped at `KITTY_MAX_PIXELS` (512x512) and payload at
  ~8 MB; over-budget transfers are dropped with a stderr note, screen untouched.
- Anchors at the cursor and fits (nearest-neighbour, aspect preserved by
  default, `a=no` for 1:1+crop on the OSC path) into the region to the grid
  edges; it never scrolls. On the OSC path the cursor ends at column 0 of the
  first row below the image; the DCS path uses the spec's right/down epilogue.
- Alpha composites over the current background colour when the payload has one
  (the DCS `t=d` flag and the OSC `t=yes`); cells have no alpha channel of
  their own, unfilled sub-cell fringe stays black (parity with sixel).

PNG is optional (system libpng, high-level `png_image` API only). The makefile
prefers an explicit `HAVE_PNG=1/0`, then `PNG_CFLAGS`/`PNG_LIBS` hints (the
cross recipe forwards pkg-config output), otherwise it probes the toolchain for
the `png_image` entry points. Without libpng the binary builds and runs, and
PNG payloads (DCS f=100 and OSC `File ;`) are dropped with a note; raw pixel
transfers on every path and the Image/Rep path are unaffected.

## Build / test

```bash
make clean && make && make test
```

`CRUFT_FONT` selects the font blob.
`TERM` is `cruft-256color`.

`make HAVE_PNG=0 all` builds the no-libpng variant (PNG path dropped at build
time); `make check` runs lint/tidy/cppcheck/shellcheck/test.
