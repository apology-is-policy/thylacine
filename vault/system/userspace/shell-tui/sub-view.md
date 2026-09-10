---
id: sub-view
type: sub
title: "view -- the inline-media viewer: decode in a sacrificial process, hand halcyond a raster"
parent: moc-userspace-shell-tui
code:
  - usr/view/src/lib.rs
  - usr/view/src/main.rs
  - usr/view/Cargo.toml
  - usr/lib/inlinewire/src/lib.rs
  - usr/lib/inlinewire/Cargo.toml
audit: hard
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

`view <path>` shows a file in the Halcyon transcript. If the bytes are a
recognized image it decodes them and the picture appears INLINE in the
scrollback (letterboxed to the pane width, at native size when it fits); if they
are not, it falls back to `cat`. This is the writer half of inline media (I-47,
`docs/HALCYON.md` 14.7); the reader half -- the `/srv/halcyon` place channel and
the transcript injection -- lives in [[sub-halcyond]]. The FULLSCREEN sibling
(`view --fullscreen`) is [[sub-gallery]], which reuses this crate's decode and
blits to its own tapestryd surface instead of the transcript.

The load-bearing design choice: **the image decode runs HERE, in the
short-lived, unprivileged `view` process, never in halcyond** (the blast-radius
amendment to 14.7's original "decode in halcyond"). A codec is a format-fuzz
surface; hostile image bytes must parse in a process whose death costs a shell
line, not the whole-session compositor. `view` is native (libthyla-rs), so the
pure-Rust fuzz-friendly posture is kept either way.

## Contract

- `view <path>`: sniff the leading bytes. A recognized image (PNG or JPEG) ->
  decode -> hand halcyond the raster over `/srv/halcyon` -> "view: <path> placed
  inline (WxH)". A recognized image with no renderer channel (no `/srv/halcyon`,
  or a short write) is NOT an error: view reports the decode ("... decoded WxH
  ...; not displayed (why)") so it is useful standalone. Anything else ->
  `cat <path>` (the operator's spec: "text would fallback to cat"). PNG and JPEG
  share the same headers-only budget gate then decode-and-place path, factored
  into `check_budget` + `place_decoded`.
- The wire it speaks is `inlinewire` (this dossier's other half), shared verbatim
  with halcyond's reader so writer and reader cannot drift: a 16-byte header
  (magic `HPL1`, `FORMAT_ARGB8888`, w, h -- all LE) then w*h ARGB `u32`s as LE
  bytes.

## Mechanism

### The decoder (`view` lib, pure, host-tested)

`sniff` reads magic, never the extension (the bytes are the truth; the obj-verb
menu offers `view` on any file). `decode_png` drives the zune no_std decoders
(`zune-png`/`zune-core`, vendored) and NORMALIZES every colorspace zune reports
after its own palette/sub-8-bit expansion -- Luma / LumaA / RGB / RGBA, 8- or
16-bit -- to one `0xAARRGGBB` per pixel (16-bit taken high-byte `>> 8`; Luma
replicated across RGB; a missing alpha opaque). `cartoon::Op::Image` composites
the alpha over the pane ground, so a transparent PNG shows the pane through.
`MAX_PIXELS` (64 Mpx) is the absolute ceiling; the REAL bound is the caller's
heap. `png_dimensions` reads the IHDR WITHOUT decoding and `within_pixel_budget`
compares `w*h` (in u64, no overflow) to a caller budget, so a viewer rejects an
over-budget image from the headers alone -- BEFORE the heap-hungry decode
(`decode_png` allocates the samples buffer + the ARGB buffer + holds the input,
peak ~8*npx). Both are reused by [[sub-gallery]]; the channel's tighter cap is
halcyond's.

`decode_jpeg` is the JPEG twin (`zune-jpeg`, vendored, `default-features=false`
so `x86`/`neon`/`std` are all OFF -- which activates zune-jpeg's own
`forbid(unsafe_code)`, so hostile JPEG bytes decode in ENTIRELY SAFE Rust: the
worst case is a panic, caught by the sacrificial-process boundary, never memory
unsafety). It reads the OUTPUT colorspace from `get_output_colorspace` (never
assumed) and normalizes Luma (replicated across R/G/B) or RGB (straight) to
opaque `0xAARRGGBB`; JPEG carries no alpha, and a 4-component (CMYK/YCCK) output
is REFUSED rather than mis-rendered as RGBA (unlike PNG's 4th channel, JPEG's is
not alpha). `jpeg_dimensions` reads the headers only (JPEG dims are 16-bit, so
`w*h` cannot overflow a u32) for the same pre-decode budget gate.

### The channel writer (`view` bin)

The bin flow: read (`slurp_capped`, 16 MiB cap) -> sniff -> **reject over-budget
from the headers** (`check_budget` on `png_dimensions`/`jpeg_dimensions` +
`within_pixel_budget` vs `VIEW_MAX_PIXELS` = 3 Mpx) -> `decode_png`/`decode_jpeg`
-> `drop(bytes)` -> `place_decoded` (`place_on_halcyon` + the standalone report).
The decode runs on a **64 MiB `ThylaAllocN` heap**; `VIEW_MAX_PIXELS` = 3 Mpx is
the real bound, sized for the WORST decode mode's peak + the held input, checked
before decode. The worst mode is a PROGRESSIVE JPEG: zune holds a full-image
coefficient buffer per input component (~2 B * components * npx) alongside the
output, so its peak ~= READ_CAP + 12*npx (vs baseline/PNG ~8*npx) -- 12*3M + 16
MiB = 52 MiB fits 64; the pre-JPEG-slice 6M would OOM a progressive JPEG
([[sub-gallery]]'s JPEG-round F1, the sibling of its own R-GALLERY-1 OOM). The
channel re-caps inline to ~1 Mpx downstream, so 3 Mpx rarely binds. Both image
arms produce a `Raster`; `Kind::Other` falls back to `cat`.

`open_place_write` picks the channel (I-47, HALCYON.md 14.7.2). In a SESSION the
compositor put THIS pane's full address in `/env/HALCYON_PLACE`
(`/srv/halcyon-<user>/<hex(token)>/place`); `view` opens it in a TWO-STEP
(`split_service_addr` -> open the service root `/srv/halcyon-<user>` O_READ to
CONNECT, then open the `<hex>/place` subpath relative O_WRONLY) and does NOT fall
back to the console channel (a session has no global `/srv/halcyon`, and a
fallback would misplace; a stale/dead address just fails). The two-step is
load-bearing, NOT a stylistic echo of the console: a `/srv` posted service
connects on OPEN and the resolver WALKS intermediate components without opening
them, so a SINGLE deep open of the full address cannot cross the service to reach
the token dir (proven at the session-image E2E -- the deep open never connected;
the two-step does). Absent that env (console mode, the spike), it opens the
global `/srv/halcyon` two-step (root fid -> `place` O_WRONLY). `place_on_halcyon` then
writes the `inlinewire` header then the ARGB payload in bounded chunks
(`write_all` loops on the returned count -- a 9P fid caps a Twrite at the
negotiated msize). The payload is the `&[u32]` reinterpreted as its LE bytes
(aarch64 is little-endian, so the in-memory bytes ARE the wire bytes the reader
reconstructs with `from_le_bytes`). The token never enters the payload -- it is
the path, validated once by the server at the walk. On an absent service or a
short write it returns `Err`, and the caller falls back to reporting the decode.

### inlinewire (the shared wire, pure, zero deps)

The sole home of the place-request format so writer and reader never drift; it
carries NO decoder and NO syscalls, so depending on it drags neither zune into
halcyond nor libthyla-rs into a host test. `PlaceHeader::pack` builds the 16-byte
header; `PlaceHeader::parse` FULLY validates it -- magic, `FORMAT_ARGB8888`, a
non-zero in-bounds w/h, and `w*h <= MAX_PIXELS` (16 Mpx) -- before returning,
so a `Some` result is safe to size an allocation from (`payload_len`/`total_len`
cannot overflow). This is the first line of the format-fuzz defense; halcyond's
`inlineaccum` adds the heap-safe per-image cap on top.

## Data structures

- `Kind` (`view`) -- Png / Jpeg / Other, from `sniff`.
- `Raster` (`view`) -- `{ w, h, argb: Vec<u32> }`, w-tight `0xAARRGGBB` rows.
- `PlaceHeader` (`inlinewire`) -- `{ format, w, h }`; `MAGIC`/`FORMAT_ARGB8888`/
  `HEADER_LEN`=16/`MAX_W`=`MAX_H`=8192/`MAX_PIXELS`=16 Mpx.

## Concurrency

None. `view` is a single-threaded, short-lived process: read the file, decode,
write the channel, exit. inlinewire is pure functions.

## Invariants enforced

I-47 (reserved in ARCH/CLAUDE section 28; no vault `inv-` node yet) -- the
writer side. The decode is contained to this sacrificial process (never
halcyond); the handoff is a bounded WRITE, not shared memory (Weft needs
`CAP_HW_CREATE`, which `view` lacks -- the wrong trust direction for inline); the
format is validated at the header before either side allocates. The RESOURCE
bound and the enforcement of the parse against hostile bytes are the reader's
([[sub-halcyond]] `inlineaccum` + [[haz-budget-stored-not-derived]] in spirit).

## Error paths

- Non-UTF-8 / missing operand -> usage, exit 2.
- Open / read failure -> "view: <path>: ..." to stderr, exit 1.
- Decode failure (malformed PNG) -> "view: <path>: <reason>", exit 1.
- Decode OK but no channel / short write -> the decode is REPORTED (not an error
  exit): the decode succeeded, only the display did not.
- Not an image -> `cat` (its exit status is view's).

## Performance

A one-shot: one file read (`slurp_capped`, 16 MiB cap) on a 64 MiB heap, a
headers-only dimension read, one zune decode (`bytes` freed after), one channel
write. No steady state.

## Prosecution

- **The decoder against hostile image bytes.** Malformed / truncated / oversize
  PNGs and JPEGs; the colorspace normalization (every zune arm; JPEG's CMYK 4th
  channel refused, not mis-read as alpha); garbage rejected. Runs in the
  sacrificial process, so a decode crash is one shell line. BOTH decoders are
  pure SAFE Rust: `zune-png` and `zune-jpeg` are vendored `default-features=false`,
  which drops their `x86`/`neon`/`sse` SIMD features and so activates each crate's
  `forbid(unsafe_code)` -- hostile bytes cannot reach memory unsafety, only a
  panic (fuzz-friendlier than a ported C codec).
- **The heap against a dimension bomb.** A small compressed PNG/JPEG can declare
  huge dimensions; `png_dimensions`/`jpeg_dimensions` + `within_pixel_budget`
  reject `w*h > VIEW_MAX_PIXELS` from the headers before the decoder allocates, so
  an over-budget image is a clean report, never a silent OOM-exit (the pre-fix
  defect: a bare `MAX_PIXELS` far above the heap was a phantom bound --
  [[sub-gallery]]'s holotype F1).
- **The wire against drift.** `inlinewire::parse` validates before it returns;
  the pack/parse round-trip + the bounds rejections are host-tested; the magic
  reads as `HPL1` in a hexdump (a true-comment/wrong-value guard).
- **The handoff trust direction.** A bounded WRITE to a per-endpoint service, not
  a shared mapping; `view` holds no elevated capability; an absent renderer is a
  clean fallback, never a hang.

## Seams

- JPEG decode (`zune-jpeg`) LANDED: `decode_jpeg` + `jpeg_dimensions`, the same
  headers-only-budget-then-decode path as PNG, wired into both viewers' `Jpeg`
  arms; fixtures `testdata/test.jpg` (E2E) + `src/testdata/quad.jpg` (lib).
- `--fullscreen` LANDED as `gallery` (a native libtapestry pane, [[sub-gallery]]);
  `Embed` (the out-of-band pixel surface for video) is unbuilt (I-47 / the HALCYON
  14.7 medium split: images native, video a ported C codec, audio -> Nocturne).
- The obj-verbs (`path view view {}` + `path gallery gallery {}` in
  `/lib/beacon/verbs`) that put both viewers on the Esc+w/b menu LANDED
  ([[sub-beacon]]).
- The per-user SESSION-path channel LANDED (halcyond's `paneplace.rs`, see
  [[sub-halcyond]]): `view` now prefers the per-pane `/env/HALCYON_PLACE` address
  (`/srv/halcyon-<user>/<hex(token)>/place`) via `open_place_write`, falling back
  to the console `/srv/halcyon` only when that env is absent.

## Caveats

- The ARGB payload is the raw `&[u32]` LE bytes: correct on little-endian
  aarch64, and both sides agree via `inlinewire`; a big-endian target would need
  an explicit swap.
- The witness card `usr/view/testdata/test.png` (+ `make-test-png.py`, stdlib
  zlib) is the PNG E2E fixture, baked to `/test.png` under `THYLACINE_HALCYON=1`;
  `testdata/test.jpg` (the same card re-encoded) is the JPEG one, baked to
  `/test.jpg`. Three `src/testdata/*.jpg` lib fixtures: `quad.jpg` (32x32 colour,
  baseline -- `decode_jpeg_quadrants_to_argb` asserts APPROXIMATE colours in a
  +-48 band, JPEG being lossy), `gray.jpg` (16x16 grayscale -- the Luma arm, every
  px r==g==b), and `prog.jpg` (32x32 SOF2 PROGRESSIVE -- the coefficient-buffer
  path the holotype's F1 rode in on). Generated by `make-test-jpg.sh` (needs
  `sips` + libjpeg's `cjpeg`/`jpegtran`); like the PNG card they are COMMITTED, so
  the pool bake and host tests need no encoder. `view` lib tests: 11.
- The `decode_jpeg`/`gallery` `Jpeg` E2E is `tools/interactive/ls-gfx-jpeg.exp`
  (view inline + gallery fullscreen, HVF, SKIP-clean on aurora).

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
