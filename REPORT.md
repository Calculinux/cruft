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
- **"U=1 no-render" — STRUCTURALLY ELIMINATED.** The missing image
  placement was pty carrier corruption of the transfer bytes in
  transit; the L2b write-boundary tee (§8) takes the authority bytes
  before the kernel touches them, so the pty can no longer corrupt
  anything the tests consume. Closed without product change.

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
- **Golden-pixel suites** (`tests/golden_main.c` + `tests/goldens/`),
  5 scenes, byte-frozen with external authority labels in their
  headers: L2a's `sixel-quadrants.frame` (409 600 B),
  `kitty-composite.frame` + `kitty-composite.reply` (96 B
  probe/transfer replies), `halfblocks-utf8.frame` — all on the
  vendored 6×12 console font
  (`tests/assets/console-6x12.cruftfont`, sha256-pinned, rebuilt
  byte-identically from the pinned Terminus/Fairfax sources) — plus
  the L2b `kitty-applive` trio (application-scale real-emitter
  capture; §8) and the `sixel-icy-overlay` scene (icy_sixel-shaped
  traffic, DCS + frame frozen, no replies).
- **L2b live driver** (`tests/apptest` + `tests/l2b/`): optional
  target, deliberately outside `make test`; `make apptest-live` runs
  the check-mode verification against the frozen trio (§8).
- Status: `make test` and `make lint` green across all five scenes;
  the three L2a goldens are byte-identical canaries across the L2b
  round (verified by hash before/after the refreeze sweep).

Residual gaps:
1. ~~Application-scale `f=32` RGBA golden driven by the real
   ratatui-image asset~~ — **DONE, §8** (`kitty-applive` trio,
   write-boundary capture on a real pty).
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
- The L2b round (below) filed **no** issues: every failure
  encountered in it rooted in the new harness (own bugs, listed in
  §8.6) or in a harness assumption (fake-tty route), never in a
  product-vs-authority disagreement.

## 8. L2b: the application-scale golden (write-boundary capture)

The real `ratatui-image` asset (pinned rev `a08dc3c`, v12.0.0-rc.2;
crossterm 0.29.0; prebuilt binary, `cargo build` via
`make apptest-asset`, not committed) runs against the in-process
product and freezes the `kitty-applive` golden trio
(trace / reply / frame), which `tests/golden` thereafter replays
dependency-free (no app, no pty, no cargo).

### 8.1 The pty is lossy in transit (empirical)

With the app on a real pty and the master read side tapped: the
incoming stream is the app's exact protocol traffic interleaved with
heavy NUL floods whose volume varies run to run (observed 163 KB and
364 KB of NULs around the identical 32 711 B of protocol bytes, same
run shape). Measured at the write boundary via the shim's logger
(§8.3), the app issued exactly eight `write()` calls to its stdout,
total 32 711 B, **zero NULs**. Conclusion: the line discipline/carrier
destroys bytes in transit; the pty read side is *not* the authority
for what the app authored. The pty therefore serves only as a
delivery duct (product replies → master → app; app's outgoing pty
traffic drained and discarded).

### 8.2 The fake-tty route (dead end, proven from source)

Investigated first: give the app a tty *interface* without a kernel
tty (pipes + `LD_PRELOAD` termios emulation). Killed by the pinned
sources: `picker.rs` (rev `a08dc3c`) calls `enable_raw_mode()`
(unix, ~:383–404) **unconditionally** from `query_with_timeout`
(~:525–565) — no isatty gate — and `enable_raw_mode` calls
`rustix::termios::tcgetattr(stdin)` / `tcsetattr` directly. rustix
issues raw syscalls, which no `LD_PRELOAD` can interpose (PRELOAD
hooks libc PLT symbols only); on a pipe fd the raw `tcgetattr` fails
ENOTTY and the app dies before its probe window. crossterm 0.29.0's
unix backend routes termios through rustix by default; its `libc`
feature would flip to hookable calls but is off, and it is a knob of a
dependency we do not own — flipping it would tune a dependency to suit
the harness. Verdict: the terminal must be genuine. The faketty
prototype's remaining value (the app's full tty-surface map) is
banked here and the `.so` was deleted.

### 8.3 Architecture: real pty + in-tree write-tee shim

- **Driver** (`tests/apptest.c`): `posix_openpt`/`grantpt`/`unlockpt`/
  `ptsname`; `TIOCSWINSZ` 53×26 cells @ 320×320 px (product geometry);
  the app child does `setsid()` + `TIOCSCTTY`, slave on fd 0/1, stderr
  inherited (app diagnostics stay out of the frozen record); minimal
  env (`TERM`, `PATH`, `LD_PRELOAD` — host env can carry
  `WEZTERM_EXECUTABLE`/`KONSOLE_VERSION`, which the picker blacklists).
  Three-lane pump: reply pipe (product → pty master → app), the tee
  (authority capture, fed live into the product's real `parse()`), and
  the master read side (drained, discarded).
- **Shim** (`tests/l2b/writetee.so`, own, built in-tree): hooks
  `write()` only; a call qualifies iff its fd is the app's stdout —
  recognised by *file identity* (fd 1, or any fd with the same
  `(st_dev, st_ino)`, which covers the app's `/dev/tty` aliases), not
  number alone. Qualifying writes are mirrored byte-for-byte, in call
  order, onto an inherited pipe end (`TEE_FD` 7) **before** the real
  write proceeds. `SIGPIPE` ignored; an env-gated diagnostic logger
  (`CRUFT_TEE_LOG`, dormant by default) records per-call
  fd/length/zero-count/header — the instrument that settled §8.1.
  Built without `-pedantic` (the `dlsym` object→fnptr casts are
  ISO-UB; commented in the makefile).
- **Why own (OSS survey)**: no maintained recorder captures fd
  *writes* of one designated process; strace (absent here; text
  output), ltrace (unhealthy on this toolchain), eBPF/auditd/perf
  (privilege) do not fit a CI-capable in-tree test;
  `utempter`/`script`/tmux/dtach/abduco record the pty **read** side or
  screen state — precisely the lossy surface escaped here.
  Differentiators: binary-safe, single ordered record per call, zero
  provisioning, single-fd scope, verified against the pinned app.

### 8.4 The assertion ladder (in firing order)

1. app exits 0 (its happy path is part of the contract — it selects
   Kitty, promotes, transfers, draws, exits);
2. **NUL-free capture** — a true invariant of the tee stream; a
   violation is an environment regression and fails loudly. The
   pty-era repair machinery (NUL-stripping, banded dynamic-programming
   re-sync, ideal substitution) is retired wholesale: with clean
   authority bytes there is nothing to repair, and sanitising would
   be inventing semantics;
3. **shape**: the `i=31` probe APC intact + exactly eight `q=2`
   transfer chunks (the `a=q` probe is deliberately never rewritten);
4. **payload, zero budget**: each chunk's payload byte-compared
   against the independently recomputed ideal
   `tests/l2b/asset/img/apptest-100x60.b64` (32 000 B) —
   byte-identical or fail;
5. **canonicalisation** (the single permitted transform, and it is
   pinning, not invention): the client-random transfer id (varies per
   run) → `32`, applied to the three carriers — transfer-header
   `i=`, placeholder SGR truecolour `38;2;H;L;LO` → `38;2;0;0;32`,
   and the high-byte diacritic `kg_diacc[H]` → `kg_diacc[0]`. The
   canonical trace/reply/frame is thus byte-stable across runs while
   the live exchange stays verbatim-real;
6. **replay**: the product re-parses the canonical trace through the
   same in-process tier-1 path in 333-byte deliberately misaligned
   bites (rechecks split-independence against the live pipe-fed
   stream); the live reply (its id-echoes canonicalised the same way)
   must equal the replay's reply byte-for-byte;
7. **freeze + re-verify**: the trio is frozen and re-read back;
   `tests/golden` then replays the frozen trace with no app, no pty
   and no cargo, and must reproduce the frozen reply and frame
   (verified: byte-identical to the live freeze, by hash).

### 8.5 Geometry and anchoring facts (verified against the bytes)

- Chunking: seven full 4096-char b64 chunks + a 3328-char tail =
  32 000 — agrees with the pinned emitter's audit (§3.2,
  `kitty.rs:432–436`).
- `c=17`/`r=5` in the transfer header are **dimension** parameters
  (17 cells × 5 rows), not an anchor — verified: the stored image is
  composited at the *placeholder* top-left, and the placeholder block
  the app drew (VT row 5, col 7, 0-based 4,6) spans exactly 17×5
  cells.
- The 100×60 px image fits the 102×60 px cell box at scale 1 with a
  1 px centring margin; rendered origin is px **(37,48)**.
- Frame verification: **6 000/6 000 pixels exact (TOL 0)** against
  the source PNG RGBA — lossless end-to-end (PNG → app decode →
  raw RGBA → b64 → product decode → composite).
- Frozen reply is exactly **54 B**: `ESC[1;1R` (CPR) ·
  `ESC _ G i=31;OK ST` (probe) · `ESC [ ? 6 2 ; 4 ; 6 c` (DA1) ·
  `ESC [ 1 6 ; 1 2 ; 6 t` (cell size, pixels) · `ESC [ 0 n` (SDS
  sentinel) · `ESC _ G i=32;OK ST` (transfer ack, id pinned).
- Determinism: two successive check-mode runs with different
  client-random ids (e.g. `0xff3de49f`, `0x062ceeac`) produced
  byte-identical canonical artefacts — the goldens hold across the
  id's randomness by construction.

### 8.6 Banked traps and self-inflicted bugs (all in the harness)

Each cost iterations; listed so the next round does not repay them.

1. **`setsid()` returns the new session id on success** — test `< 0`,
   not `!= 0`. Misread, it forks the proxy unnecessarily and the
   grandchild `_exit(126)` reads as a mysterious app death.
2. **`dup2()` returns the *new* fd on success** — test `< 0`.
   Misread as `!= 0`, the `dup2(slave, 1)` success (return 1) fired a
   spurious sentinel; the recorded `errno=22` was *stale* (a benign
   failed `readlink` on `.` inside `/proc/self/fd` during
   diagnostics), which masked the true cause for a full hunt. Slots
   0-masked the same check trivially (dup2 to fd 0 returns 0).
3. **x86 ioctl constants on this host** (faketty era):
   `TCGETS=0x5401`, `TCSETS=0x5402`, `FIONBIO=0x5421`,
   `TIOCNOTTY=0x5422`, `TCGETS2=0x3C542A`, `TCSETS2=0x3C542B`; glibc
   here has no `struct termios2`; the std readline `read_output`
   ioctl slot is 4 bytes (a 60-byte termios write segfaulted the app
   against it).
4. **Size/capacity conflation in the accumulator** (`grow()`): the
   helper wrote its new *capacity* back into the caller's *size*
   variable, so every append jumped ahead by a doubling — the record
   ballooned into zero-filled deserts around the real 32 KB (volume
   nondeterministic, proportional to pipe partitioning). Same family:
   `drain_pipe` copied to the post-growth size and then advanced the
   size again. Fix: explicit capacity ownership; callers own size.
5. **ST double-subtraction** (`apc_body_at`): the terminator finder
   returns a pointer *at* the ST's `ESC`, so `[start, end)` already
   excludes it; an extra `−2` trimmed the last two payload bytes of
   **every** chunk (deterministic 4096→4094).
6. **Emitted introducer truncated** (`cb_emit("\x1b_G", 2)`): the
   literal is 3 bytes (`ESC _ G`); length 2 dropped the `G`, so every
   canonicalised transfer APC lost its introducer byte. The product
   then *correctly* declined to form a DCS from `ESC _ q…` and stayed
   silent under `q=2` — surfacing as "canonical replay misses the
   transfer ack" while raw replay acked. **Not a product defect**:
   the product behaved correctly against the malformed stream the
   emitter manufactured. Caught by an independent (non-C) parser over
   the frozen bytes plus raw-vs-canonical A/B replays.
7. **Phantom-nondeterminism lesson**: an apparent chunk-split variance
   (4094 vs 4096 "between runs") was the parser pair diverging (trap
   5), not app jitter. Rule adopted: before attributing variance to
   app or environment, diff two independent parsers of the same
   bytes.

### 8.7 Artefacts

- `tests/apptest.c` — live driver (pty + tee pump + assertion ladder).
- `tests/l2b/writetee.c` (→ `.so`) — the shim; makefile rule keeps it
  out of `-pedantic` with the reason commented.
- `tests/l2b/asset/` — pinned Cargo project (ratatui-image `a08dc3c`,
  crossterm 0.29.0, lockfile committed; `target/` ignored) +
  `img/apptest-100x60.png` (source image) and
  `img/apptest-100x60.b64` (the independent 32 000 B ideal payload).
- `tests/goldens/kitty-applive.{trace,reply,frame}` — 32 694 B /
  54 B / 409 600 B; plus the `sixel-icy-overlay.{dcs,frame,reply}`
  scene frozen in the same sweep; L2a goldens byte-unchanged.
- Diagnostic dumps (best-effort, `/tmp/apptest-debug.*`) are written
  beside each ladder rung that fails, so any future regression
  presents its own evidence.
