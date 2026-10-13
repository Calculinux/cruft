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

## Build / test

```bash
make clean && make && make test
```

`CRUFT_FONT` selects the font blob.
`TERM` is `cruft-256color`.
