# Pixel-accuracy test programme — report (Phase 2, to acceptance)

Branch `wt-kitty-p10` (stacked on `pr-10`; base `48df802`), 2026-10-17.
Scope: make the image-transport code paths (sixel, kitty DCS) provably
pixel-accurate in-tree with no host dependencies, governed by one rule:

> We must not invent semantics. As long as our tests agree with an
> outside source (kitty spec, DEC/ECMA-48, ratatui-image byte-for-byte)
> that is fine — but behaviour that no outside authority sanctions is a
> finding, never a baked-in expectation.

## 1. Deliverables on this branch

| commit | content |
|---|---|
| `3fbcfe0` | `sgr:` honour CSI `22m` (neither bold nor faint) — SGR-22 was inverted; N5 closed |
| `16a30af` | `tests:` L2a golden-pixel harness + first three goldens (sixel-quadrants, kitty-composite, halfblocks-utf8) |
| `b4d98e1` | `tests:` correct halfblocks-scene braille codepoint labels (comment-only; goldens unchanged) |
| `61922c1` | `kitty_image:` remove the OSC 1337 (iTerm2-style) graphics transport (−756 L net) |

Issues filed in the course of this phase:

- **Calculinux/cruft#13** (low): the CRUFTFN1 width field is an advance
  *class* (1|2), undocumented; the renderer ignores it. Oracle parity is
  unaffected — the field just needs documentation or removal.
- **Calculinux/meta-calculinux#262**: the console-font braille page's
  dot-7/dot-8 glyphs render the "tall" portion as a detached two-row
  segment (row h8 left blank). Proven verbatim in the pinned upstream
  **Terminus 4.49.1** `ter-u12n.bdf` (pipeline rebuilt byte-identically,
  sha256-verified); `mkcruftfont` merges first-wins and Terminus is loaded
  first. Decision requested there: accept-and-document, push upstream,
  or add a labelled normalisation step in `mkcruftfont` with its own
  frozen golden.

## 2. Retractions and closed investigations (no product defect)

- **Braille "2 px nit" — RETRACTED.** The L2a oracle originally queried
  the wrong codepoint (U+28BC); the golden cell byte `E2 A3 BC` is
  U+28FC, dots 3–8, which the 6×12 font renders exactly. The product
  blits the font record untouched end-to-end (`draw_line` in
  `fb/common.h`, `addch` in `terminal.h:234-259`); an isolated-render
  probe reproduced the golden cell byte-for-byte. What looked like a
  product defect is the upstream font's deliberate Terminus glyph design
  → became #262 instead.
- **N6 / P1 — RETIRED.** The `kg_format()` byte-order "defect" was the
  spec's own byte order (RGB/RGBA/BGRA/BGR per kitty
  graphics-protocol.txt); the misleading doc comment was corrected in
  `16a30af`.
- **ESHORT saga — CLOSED.** The transfer-shortage fault traced to a test
  fixture (hard-capacity base64 buffer in the harness, not the product's
  growing `kg_dbuf`); the fixture now refuses to overfill at compile-
  visible runtime checks.
- **Guest pty-EIO (forkpty-on-target)** — reclassified as a guest
  harness limitation from Phase 1, not a product defect; parked.

## 3. External-authority audit — ratatui-image

Subject: `ratatui-image` v12.0.0-rc.2 @ `a08dc3c` (shallow copy under
`/tmp`, audit artefact only, not persisted). Verified files:
`picker.rs`, `picker/cap_parser.rs`, `protocol/{kitty,sixel}.rs`, plus
the `icy_sixel` encoder (`mkrueger/icy_sixel` HEAD `18a36d3`, the
crate ratatui delegates sixel encoding to).

### 3.1 Capability negotiation

- Default probe batch (`cap_parser.rs:140-221`):
  `ESC _ G i=31 ; s=1 ; v=1 ; a=q ; t=d ; f=24 ; AAAA ST` **+**
  `ESC [ c` **+** `ESC [ 1 6 t` **+** sentinel `ESC [ 5 n`.
  SHM (`o=h`) and compression (`i=32`, `o=z`) probes are **off by
  default** (`:107`,`:108`) — ratatui therefore never probes them
  unless opted in.
- Reply grammar enforced strictly (`:256`,`:332`): DA1 must contain
  marker `4` (the only sixel marker it accepts); the kitty probe must
  reply exactly `ESC _ G i=31 ; OK ST`; `16t` must be exactly three
  numeric fields.
- **Product conformance**: the probe replies are frozen byte-for-byte in
  `tests/goldens/kitty-composite.reply` (96 B):
  `ESC [ ? 6 2 ; 4 ; 6 c` · `ESC _ G i=31 ; OK ST` ·
  `ESC [ 1 6 ; 1 2 ; 6 t` · then per-transfer `i=N ; OK`. The `16t`
  answer is in **pixels** (12;6) — deliberately matching what ratatui
  consumes for its `FontSize`/cell-size math on this 6×12 font, not
  xterm's character-unit habit. Frozen by golden, cited here by
  authority.
- Selection: Kitty beats Sixel; Sixel chosen only when Kitty failed
  (`picker.rs:481-492`); WezTerm/Konsole are blacklisted for both
  (`:131-138`); iterm2 reachable only via desktop-env heuristics —
  unreachable on this bare-metal guest (`:365-383`). Stdin probe window
  is 2000 ms; **any** shortfall drops to `DEFAULT_PICKER` = halfblock
  10×20 (`:323-332`,`:567-589`) — this is exactly what was observed in
  the Phase-1 guest run, now explained rather than a mystery.
- Parser tolerance: the stdin parser is char-incremental over 64-byte
  polls — fragmented
  replies are fine; product's growing ESC buffer handles APC fragments
  inbound.

### 3.2 Kitty transfer (what a promoted app would actually send)

- **Default format is `f=32` raw RGBA**, not PNG (`kitty.rs:381,457`);
  PNG appears only in the compress+PNG "S" variant. Chunking is
  4096 base64 **chars** per continuation APC (`:432-436`). Placement is
  virtual (`U=1`) with U+10EEEE unicode placeholders in the buffer;
  `a=d` deletes a placement when its placeholder stops rendering;
  transfers are **fire-and-forget** — ratatui never
  waits for the transfer's `OK` reply (no reply-reading on the transmit
  path), so the product's replies are informational, frozen by golden.
- Compression: the product refuses an `a=q`+`o=z` probe with
  `EBADMSG:unsupported query` (`kitty_image.c:857-858,877`) — precisely
  the "anything-but-OK ⇒ no capability" form ratatui's
  `test_parse_compression_refused` expects. An opted-in app therefore
  degrades to uncompressed transfers the product serves; `o=z` transfer
  bodies get a clean ENOSYS reply honouring `q=`.
- Real-emitter corroboration (URLs only; capture not persisted):
  kovidgoyal/kitty discussion **#6331** + the
  `kitty-TGP-001.tar.gz` attachment — an 18-frame alpha animation
  speaking `o=z`, `q=2`, `C=1` in 4096-char b64 chunks. Against it:
  `o=z` refused (documented product gap, ratified), `C=1` cursor policy
  ignored, `q` honoured. Same dialect otherwise, incl. the 4096 chunk
  boundary.

### 3.3 Sixel (via icy_sixel, the crate ratatui uses)

Dialect emitted (`crates/icy_sixel/src/encoder.rs` HEAD `18a36d3`),
cross-checked against the product verb table (`ctrlseq/dcs.h:278-298`):

| icy_sixel emits | product handler | |
|---|---|---|
| `"` Pad;Pan;W;H raster attribute | `sixel_attr` | ✅ |
| `#P<i>;2;r;g;b` percentile palette (rounded 0-100) | `sixel_color`, non-HLS → percent | ✅ |
| `#<idx>` select | argc==1 path | ✅ |
| `!Pn ch` runs ≥ 4 | `sixel_repeat` | ✅ |
| raw sixels `?`-`~` | bitmap path | ✅ |
| `$` carriage return **between colour layers** (full-band overlay model) | `sixel_cr` | ✅ |
| `-` band newline, ST terminate | `sixel_nl` / end | ✅ |

Colour definitions are percentile RGB — the same dialect as the
frozen golden scene and as the DEC spec's coordinate-system-2 form
(type 1 is HLS per spec, which the product maps correctly too).
Unknown verbs are skipped one byte; icy_sixel emits none outside the
table. Conclusion: **ratatui-generated sixel traffic decodes
losslessly** on the product, including multi-colour per-band overlays
(the `16a30af` sixel golden already pins the single-layer core).

### 3.4 iTerm2

Transport removed from the product (`61922c1`) — see §5. Ratatui's
own iTerm2 backend is consequently moot for this deployment (it is
selected only under terminal.env iTerm2 heuristics, unreachable on the
bare-metal guest, and its payload — PNG over OSC 1337 `File ;` — no
longer has a receiver).

## 4. Banked performance figures (Phase-1 guest instrumented run)

Throughput soft-score **0.077**; PTY volumes: app side
wchar ≈ 20 518 / rchar ≈ 5 948, cruft side rchar ≈ 14 K.
Notably, the 311 KB kitty DCS payload ratatui staged was **never
sent**: the probe missed the 2 s window twice and the app fell back to
half-blocks (§3.1) — consistency check passed, not a transport fault.

## 5. The OSC 1337 removal (and why it is the governing rule applied)

Per-protocol coverage audit of the three transports before removal:
KGP and sixel carry external anchors (spec + byte-verbatim peer
behaviour + same-dialect named emitters) and pixel-frozen cores; OSC
1337's raw-BGR + `t=` flavour was a **product-invented dialect** (neither
ratatui's iTerm2 backend nor real iTerm2 speaks it here), covered
behaviourally only — no goldens, no emitter to track. Under the
no-invented-semantics rule it was disqualified for shipping: decode
path, `Rep` responder, argument parser, intercept, selftest section and
advertised support removed; input is now consumed and ignored like any
unhandled OSC. Recorded in `CALCULINUX.md` so the path is not
resurrected half-tested. libpng retained (feeds the DCS f=100 path).

## 6. Current test matrix

- **Selftest** (unity, `tests/selftest.c`): sections incl. W = full
  kitty DCS lifecycle through the real `parse()` (probe replies,
  chunked transfer, placeholder display, `a=p` re-place, `a=d` delete,
  direct placement); K/M/Q sixel core. Tamper detection: any golden
  mismatch fails the suite with a per-scene diff summary.
- **L2a goldens** (`tests/golden_main.c` + `tests/goldens/`), 3 scenes,
  byte-frozen with external authority labels in their headers:
  `sixel-quadrants.frame` (409 600 B), `kitty-composite.frame` +
  `kitty-composite.reply` (96 B probe/transfer replies),
  `halfblocks-utf8.frame` — all on the vendored 6×12 console font
  (`tests/assets/console-6x12.cruftfont`, sha256-pinned, rebuilt
  byte-identically from the pinned Terminus/Fairfax sources).
- Status: `make test` and `make lint` green; goldens byte-identical
  before/after the 1337 removal.

Known residual gaps (**L2b**, defined, not started):
1. A chunked `f=32` RGBA transfer + unicode-placeholder-display pixel
   golden at application scale (the actual ratatui traffic class — §3.2),
   driven by the real emitter under a host pty with a prebuilt
   ratatui-image test asset.
2. A palette-heavy sixel scene (multi-layer `#P` + `$` overlays,
   icy_sixel-shaped traffic) frozen by golden.
3. Real-app guest run for photographic confirmation — deferred to the
   one-shot QEMU arbiter (320×320, hostfwd :2222).

## 7. Open items

- #13 (width field documentation) and #262 (Terminus tall-dot segment;
  decision pending upstream/maintainer) — both factual filings, neither
  blocks acceptance.
- `test-asan` blocked on host (no libasan/ubsan runtimes, no sudo).
- Ratified product-vs-authority deltas, documented in
  `CALCULINUX.md`: no `o=z` (refusal is spec-tolerable and
  probe-safe); `C=1` ignored; per-cell overwrite rather than
  stacked/alpha compositing; `a=d` accepts the delete-key letter set
  as a polite subset; `U=1` placeholder machinery present and exercised
  in W.
