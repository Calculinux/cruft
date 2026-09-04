#!/usr/bin/env python3
"""Assert mmap glyph packing matches fb draw bit_base (no BDF byte pad)."""

def draw(bitmap, bit_base, cell_w=6):
    # w=0 is RIGHT of cell, w=cell_w-1 is LEFT (same as fb/common.h)
    out = ["."] * cell_w
    for w in range(cell_w):
        if bitmap & (1 << (bit_base + w)):
            out[cell_w - 1 - w] = "#"
    return "".join(out)

# native half: left=bit5, right=bit0
assert draw(0x21, 0) == "#....#"
# old upstream-style pad=2 mis-renders
assert draw(0x21, 2) != "#....#"
# wide left half uses bit_base=cell_w
wide = (1 << 11) | (1 << 6) | (1 << 5) | 1
assert draw(wide, 6) == "#....#"
assert draw(wide, 0) == "#....#"
print("ok")
