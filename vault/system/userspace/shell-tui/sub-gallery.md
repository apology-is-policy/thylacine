---
id: sub-gallery
type: sub
title: "gallery -- the fullscreen image viewer: view's decode + a tapestryd surface"
parent: moc-userspace-shell-tui
code:
  - usr/gallery/src/lib.rs
  - usr/gallery/src/main.rs
  - usr/gallery/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/HALCYON.md"]
created: 2026-09-09
updated: 2026-09-09
---
## Purpose

`gallery <image>` shows a picture FULLSCREEN. It is the `view --fullscreen`
variant of inline media (I-47, `docs/HALCYON.md` 14.7): where [[sub-view]] hands
a raster to halcyond for INLINE display in the transcript, gallery opens its own
fullscreen [[sub-tapestryd]] surface and blits the image letterboxed to it -- the
DOSBox/Quake client pattern. It rides I-40 (no torn scanout / surface-share) and
I-45 (GPU authority bounded by the context) purely as a CLIENT; it adds no
compositor or kernel code.

Two deliberate reuses keep the new surface small: the DECODE is [[sub-view]]'s
(the same already-audited format-fuzz path, decoded in this same short-lived
sacrificial process), and the compositor path is libtapestry's identical
`Surface::fullscreen` cycle tapestry-demo uses.

## Contract

- `gallery <image>`: sniff the leading bytes. PNG or JPEG -> decode -> open a
  fullscreen tapestryd surface -> blit the image letterboxed -> present, then
  wait. Esc or q (or a window CLOSE) exits.
- Unlike `view`, a non-image is an ERROR (`return 1`), not a `cat` fallback: a
  fullscreen `cat` is meaningless.
- Exit codes: 0 (shown then exited), 1 (read/decode/compositor error), 2 (usage
  / non-UTF-8 path).

## Mechanism

The crate is lib + bin (the view/halcyond pattern): the LIB is the pure display
geometry (host-tested); the BIN is the thin syscall/tapestry body.

**The letterbox fit (`lib.rs`).** `fit_rect(sw, sh, dw, dh) -> Fit{ox,oy,fw,fh}`
places an `sw x sh` image inside a `dw x dh` display: aspect-preserving, centred,
scaled to FILL. This is the deliberate divergence from the inline path's
native-if-fits ruling (view, `0b7741f1`): a fullscreen viewer upscales, so a
small image is shown as large as its aspect allows rather than marooned native on
black. The aspect comparison cross-multiplies in u64 (`sw*dh >= dw*sh` -> the
width binds) so there is no float and no overflow.

**The blit (`paint`).** Fill the frame with the opaque `LETTERBOX` colour, then
nearest-neighbor scale the source into the fit rect, forced opaque
(`0xFF000000 | rgb`). Every index into both `src` and `dst` is re-checked against
its slice length, so a truncated/hostile raster or a short frame clamps rather
than panics.

**The client body (`main.rs`).** Read (`slurp_capped`, 16 MiB cap) -> sniff ->
**reject an over-budget image from the headers alone** (`check_budget` on
`view::png_dimensions`/`jpeg_dimensions` + `within_pixel_budget` vs
`GALLERY_MAX_PIXELS` = 12 Mpx) -> `decode_png`/`decode_jpeg` -> `drop(bytes)`
(free the compressed input before the event loop) -> `Surface::fullscreen`
(bounded connect retry, a labelled block that yields the Surface -- no post-loop
`unwrap`) -> `FrameIntent::Static` -> `paint` into `pixels()` -> `present(None)`.
The decode runs on a **192 MiB `ThylaAllocN` heap**, sized for the WORST decode
mode: a PROGRESSIVE JPEG holds a full-image coefficient buffer per input component
(~2 B * components * npx, zune mcu_prog.rs) alongside the output, so its peak ~=
READ_CAP + 12*npx (vs baseline/PNG ~8*npx) -- 12*12M + 16 MiB = 160 MiB fits, to
view a ~12 Mpx photo. `GALLERY_MAX_PIXELS` (12 Mpx) is checked BEFORE decode, so
the pixel bound is REAL, not a phantom the allocator OOM-exits past (the pre-JPEG
128 MiB OOM-exited a 12 Mpx progressive JPEG -- the JPEG round's F1). ThylaAllocN
is lazy demand-zero overcommit, so the 192 MiB reservation commits only touched
pages, within the 256 MiB per-AddrSpace page budget (I-32). The success is announced on serial
(`gallery: <path> WxH shown FWxFH at OX,OY on DWxDH`, where WxH is the NATIVE
raster and FWxFH the fitted size) -- printed only after a successful present, so
it is the end-to-end witness the E2E keys on. Then an event loop: `TEV_KEY` ->
`is_exit_key`; `TEV_CLOSE` -> exit; `TEV_CONFIGURE` -> `handle_configure` and, on
ANY `Ok`, a full repaint + present.

## Data structures

- `Fit { ox, oy, fw, fh }` -- the letterbox placement (all `u32`).
- `Raster { w, h, argb: Vec<u32> }` -- borrowed from [[sub-view]] (the decode
  output); held for gallery's lifetime so a resize can re-fit from the source.

## Concurrency

Single-threaded. gallery does no `rfork`/`thread_spawn`; the one flow is
read -> decode -> present -> a serial event loop. No shared state, no locks. The
only external actor is [[sub-tapestryd]] over the libtapestry session (one
EventRing + one Loom ring), whose ring lifecycle libtapestry owns.

## Invariants enforced

- I-40/I-45 as a CLIENT: gallery names only its own surface and blits only into
  the weave the compositor mapped for it; it never touches another context.
- Memory safety on a hostile raster: `paint` re-checks every `src`/`dst` index
  against its slice, so a truncated or oversize-claiming decode output clamps,
  never OOB (the decoder's `MAX_PIXELS` is the upstream bound; [[sub-view]]).
- W^X / no new authority: gallery holds no capability beyond namespace
  reachability of `/srv/tapestry`; it is a leaf client (I-43 shape unchanged).

## Error paths

- `no compositor` (connect retries exhausted) -> exit 1.
- `not a recognized image` / a decode error (PNG or JPEG) -> exit 1 (no fallback).
- `present failed` / `re-present failed` / `event stream ended` -> exit 1.
- usage / non-UTF-8 path -> exit 2.

## Performance

Nearest-neighbor scale is one `dst`-pixel iteration: O(dw*dh), independent of the
source size (a huge source only changes the sample stride). The compressed input
is `drop`ped after decode, so only the raster (<= `GALLERY_MAX_PIXELS`*4 bytes,
~48 MiB at the 12 Mpx cap) is held for the viewer's lifetime -- comfortably
inside the 192 MiB heap (the transient DECODE peak, ~160 MiB for a progressive
JPEG, is the sizing constraint) and bounded by the per-AddrSpace page budget (I-32).
Present is once (a `Static` surface), plus one repaint per CONFIGURE.

## Prosecution

- A truncated / dimension-lying PNG: the decode is [[sub-view]]'s (bounded), and
  `paint`'s per-index re-checks contain any residual mismatch -- no OOB, no panic
  (host-tested: short-src and short-dst clamp).
- The display model: scanout ownership is FIRST-PRESENT-WINS (`usr/joey/joey.c`
  G-4), so a second fullscreen client's VISIBILITY over the console (the
  `THYLACINE_HALCYON=1` lever mode) is the tapestry-demo/DOSBox-proven scanout
  path, not gallery's to guarantee. gallery's HOME is the graphical SESSION,
  where the compositor tiles it as a pane. The `ls-gfx-gallery.exp` gate
  therefore keys on the SERIAL witness (the whole client path ran) and saves the
  screendump as a record; a session-mode visual+input gate is owed.
- The multi-slot rotation caveat (GPU-DESIGN 4.5.8b) does not bite: every present
  paints the FULL buffer, so no partial rect relies on stale slot content.

## Seams

- JPEG decode LANDED (reuses [[sub-view]]'s `decode_jpeg`; gallery's `Jpeg` arm
  decodes via the same `check_budget` gate as PNG -- witnessed by ls-gfx-jpeg.exp).
- The session-path per-pane channel (a distinct trust boundary, a design fork
  surfaced to the operator) -- gallery is unaffected (it is a compositor client,
  not a place-channel writer).
- Bilinear resample (shared v0-nearest posture with [[sub-view]]/cartoon's
  `Blob::scaled`); a source alpha composited over the letterbox rather than
  dropped.

## Caveats

- Nearest-neighbor scale (v1); a source alpha is dropped (forced opaque).
- Visibility over the console in `THYLACINE_HALCYON=1` mode depends on the
  scanout-focus model (see Prosecution).

## Provenance

Landed in the I-47 expand (gallery-first per the operator's 2026-09-09
sequencing vote). The obj-verbs `path view view {}` + `path gallery gallery {}`
are baked in `usr/lib/beacon/verbs.default` ([[sub-beacon]]). Host tests:
`gallery` lib 10 (`fit_rect`, `paint`, `is_exit_key`). E2E:
`tools/interactive/ls-gfx-gallery.exp` (the serial present witness, SKIP-clean on
aurora). Opus holotype round 1 (Fable credit-exhausted): 1 P1 + 2 P3, all fixed
-- **F1 (the P1)**: gallery had declared the default 4 MiB `ThylaAlloc`, so
`READ_CAP`/`MAX_PIXELS` were phantom bounds and any image past ~0.3-0.5 Mpx
OOM-exited silently (the E2E was green only because `/test.png` is 256 Kpx);
fixed with the 128 MiB heap + the headers-only `GALLERY_MAX_PIXELS` pre-check.
F2: `bytes` freed after decode. F3: the connect `unwrap` replaced by a
Surface-yielding block. The finding's whole-system note (view shared the OOM
ceiling) was fixed in the same chunk ([[sub-view]]). A Fable-diversity pass is
owed with the rest of the inline-media arc.

The JPEG slice's Opus round then found the R-GALLERY-1 heap sizing itself
under-modeled PROGRESSIVE JPEG (a full-image coefficient buffer per input
component, ~2 B * components * npx, alongside the output -- peak ~12*npx vs the
~8*npx the 128 MiB assumed): a 12 Mpx progressive would OOM 128 MiB. Fixed by
bumping the heap 128 -> 192 MiB (keeping the 12 Mpx cap; the JPEG round's F1) so a
12 Mpx progressive photo (~160 MiB peak) fits. See [[sub-view]] + the I-47
AUDIT-TRIGGERS row for the full JPEG-round close.
