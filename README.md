# cruft

Calculinux reduced-footprint framebuffer terminal (yaft lineage).

See [CALCULINUX.md](CALCULINUX.md) for fork notes and memory design.

## Build

```bash
make          # cruft + mkcruftfont
make test     # host selftests (no /dev/fb0)
make install  # cruft, cruft_wall, terminfo, man
```

## Environment

- `FRAMEBUFFER` — framebuffer device (default `/dev/fb0`)
- `CRUFT_FONT` / `YAFT_FONT` — path to `.cruftfont` blob
- `CRUFT=wall` / `YAFT=wall` — wallpaper mode (enables FB shadow buffer)

```bash
cruft_wall /path/to/image.png   # sets CRUFT=wall and execs cruft
```

## License

MIT (see LICENSE). Upstream yaft © haru / uobikiemukot.
