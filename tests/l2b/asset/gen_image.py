#!/usr/bin/env python3
"""Deterministic source image for the kitty-applive golden (no randomness).

Layout (100x60 RGBA):
  - 1 px black border all around
  - top half:    left  solid #3366CC   |  right solid #CC3366
  - bottom half: 10x10 checkerboard #00AA00 / #AAAA00

Flat halves exercise the raw-RGBA chunk loop at app scale; the checker
exercises scaling of dense edges. Re-running must yield the same bytes.
"""
import struct
import sys
import zlib

W, H = 100, 60


def px(x: int, y: int):
    if x == 0 or y == 0 or x == W - 1 or y == H - 1:
        return (0, 0, 0, 255)
    if y < 30:
        return ((0x33, 0x66, 0xCC, 255) if x < 50 else (0xCC, 0x33, 0x66, 255))
    return (((0, 0xAA, 0, 255) if ((x // 10) + (y // 10)) % 2 == 0
             else (0xAA, 0xAA, 0, 255)))


def main() -> None:
    out = sys.stdout.buffer
    raw = bytearray()
    for y in range(H):
        raw.append(0)  # filter: none
        for x in range(W):
            raw.extend(px(x, y))

    def chunk(tag: bytes, data: bytes) -> bytes:
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)  # 8-bit RGBA
    out.write(b"\x89PNG\r\n\x1a\n")
    out.write(chunk(b"IHDR", ihdr))
    out.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
    out.write(chunk(b"IEND", b""))
    out.flush()


if __name__ == "__main__":
    main()
