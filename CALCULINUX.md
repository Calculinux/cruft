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

## Status bar

An 8px strip in the top margin, above the cell grid, rendered in the
4x8 status font (miniwi; `CRUFT_SB_FONT`, default
`/usr/share/cruft/status.cruftfont`): wifi stair left, clock centre,
backlight meter + battery right. `term_init` shrinks the grid by
`statusbar_h()` pixels (320x320 + 6x12 font: 26 lines, no loss).

- Data: sysfs `power_supply`/`backlight`/`net` (`operstate`) and an
  nl80211 station dump for the signal dBm, polled once per second; the
  strip repaints only when the rendered content changes.
- Each source degrades independently: absent source hides its segment;
  no signal reading degrades the stair to on/off; a missing status font
  disables the bar and the grid reclaims the strip.
- `CRUFT_SB` selects segments (`time,wifi,battery,brightness`, `all`,
  `none`); `CRUFT_SB_12H=1` switches the clock to 12h. Compile-time
  default: `sb_default_segments` in `conf.h`.
- Dual-font: `glyph_sb_*` in `glyph_mmap.c` is a second instance of the
  demand-paged `CRUFTFN1` loader (4x8/24 metrics only); the main font
  path is untouched. FB type shapes live in `fb/fb.h` (definitions-free,
  so `status.c` can see them without dragging in the one-TU fb backend).

## Image protocols (sixel / kitty)

Two image paths land on the same sparse per-cell pixmaps, so scroll/erase/
alt-screen/VT-deactivate behave identically for both:

- Sixel (DCS 62 t): long-standing path, unchanged (`sixel_canvas.c`,
  `terminal_cell.c`).
- Kitty graphics protocol (OSC 1337): `kitty_image.c`. Clients that speak
  kitty (kitten icat, catimg, chafa -O kitty) are served instead of stalling.

Kitty scope:
- `File ;` transfer only: raw BGR/BGRA (byte order per the protocol) and PNG.
  `w=/h=` are mandatory for raw streams; PNG natural size is trusted over any
  claimed `w=/h=`. Channel count (3 vs 4) is inferred from the byte count.
- `Image ;` operators (chunked upload, query, delete, resize, ...) are not
  implemented: every one is answered `ESC ]1337 ; Rep ; i=<id> ; ok=false ;
  t=<reason> BEL` so probing clients degrade to text rather than hanging.
- Budgets: decoded image capped at `KITTY_MAX_PIXELS` (512x512) and payload at
  ~8 MB; over-budget transfers are dropped with a stderr note, screen untouched.
- Placement anchors at the cursor and fits (nearest-neighbour, aspect preserved
  by default, `a=no` for 1:1+crop) into the region to the grid edges; it never
  scrolls. The cursor ends at column 0 of the first row below the image.
- Alpha: `t=yes` composites over the current background colour; the default
  `t=no` renders opaque (alpha dropped). Unfilled sub-cell fringe stays black
  (parity with sixel; cells have no alpha channel).

PNG is optional (system libpng, high-level `png_image` API only). The makefile
prefers an explicit `HAVE_PNG=1/0`, then `PNG_CFLAGS`/`PNG_LIBS` hints (the
cross recipe forwards pkg-config output), otherwise it probes the toolchain for
the `png_image` entry points. Without libpng the binary builds and runs, and
PNG `File ;` payloads are dropped with a note (raw BGR/BGRA and the Image/Rep
path are unaffected).

## Build / test

```bash
make clean && make && make test
```

`CRUFT_FONT` selects the font blob.
`TERM` is `cruft-256color`.

`make HAVE_PNG=0 all` builds the no-libpng variant (PNG path dropped at build
time); `make check` runs lint/tidy/cppcheck/shellcheck/test.
