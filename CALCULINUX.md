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
