# Calculinux/cruft

Reduced-footprint framebuffer terminal for Calculinux (PicoCalc / Luckfox Lyra),
descended from [uobikiemukot/yaft](https://github.com/uobikiemukot/yaft) via the
Calculinux yaft fork. Linux `/dev/fb0` only; no BSD/X11 ports.

## Memory changes vs yaft

- Sparse cell sixel pixmaps (heap block per cell, not inline array)
- Lazy DRCS charset tables and lazy sixel canvas
- Optional FB shadow buffer only when `CRUFT=wall` / `YAFT=wall`
- Paged `CRUFTFN1` fonts (`tools/mkcruftfont`, default `/usr/share/cruft/console.cruftfont`)
- VT deactivate: main loop frees sixel canvas (not the signal handler); cell pixmaps kept for redraw

## VT deactivate

`SIGUSR2` only flips `vt_active` (async-signal-safe). The main loop calls
`term_release_transient` then `sigsuspend` so sixel decode cannot UAF on `free`.
Cell sixel pixmaps are kept for redraw after the next `SIGUSR1`.
`sigsuspend` deliberately allows `SIGTERM`/`SIGINT`/`SIGCHLD` so `systemctl stop`
can restart inactive VTs when changing `CONSOLE_FONT` (otherwise only tty1 updated).

## Build / test

```bash
make clean && make && make test
```

`CRUFT_FONT` selects the font blob (`YAFT_FONT` accepted as alias).
`TERM` is `cruft-256color`.
