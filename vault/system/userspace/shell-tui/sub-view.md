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

- `view <path>`: sniff the leading bytes. PNG -> decode -> hand halcyond the
  raster over `/srv/halcyon` -> "view: <path> placed inline (WxH)". A recognized
  image with no renderer channel (no `/srv/halcyon`, or a short write) is NOT an
  error: view reports the decode ("... decoded PNG WxH ...; not displayed
  (why)") so it is useful standalone. JPEG is a later slice. Anything else ->
  `cat <path>` (the operator's spec: "text would fallback to cat").
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
`MAX_PIXELS` (64 Mpx) bounds the decode; the channel's tighter cap is halcyond's.

### The channel writer (`view` bin)

`place_on_halcyon` opens `/srv/halcyon` (9p-mode -> a root fid), walks + opens
`place` O_WRONLY, and writes the `inlinewire` header then the ARGB payload in
bounded chunks (`write_all` loops on the returned count -- a 9P fid caps a
Twrite at the negotiated msize). The payload is the `&[u32]` reinterpreted as its
LE bytes (aarch64 is little-endian, so the in-memory bytes ARE the wire bytes the
reader reconstructs with `from_le_bytes`). On an absent service or a short write
it returns `Err`, and the caller falls back to reporting the decode.

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

A one-shot: one file read (`slurp_capped`, 64 MiB cap), one zune decode, one
channel write. No steady state.

## Prosecution

- **The decoder against hostile image bytes.** Malformed / truncated / oversize
  PNGs; the colorspace normalization (every zune arm); the `MAX_PIXELS` bound;
  garbage rejected. Runs in the sacrificial process, so a decode crash is one
  shell line. zune is pure Rust (fuzz-friendlier than a ported C codec).
- **The wire against drift.** `inlinewire::parse` validates before it returns;
  the pack/parse round-trip + the bounds rejections are host-tested; the magic
  reads as `HPL1` in a hexdump (a true-comment/wrong-value guard).
- **The handoff trust direction.** A bounded WRITE to a per-endpoint service, not
  a shared mapping; `view` holds no elevated capability; an absent renderer is a
  clean fallback, never a hang.

## Seams

- JPEG decode (`zune-jpeg`) is a later slice; `sniff` already classifies it.
- `--fullscreen` (`gallery`, a native libtapestry pane) and `Embed` (the
  out-of-band pixel surface for video) are unbuilt (I-47 / the HALCYON 14.7
  medium split: images native, video a ported C codec, audio -> Nocturne).
- The obj-verb (`path view view {}` in `/lib/beacon/verbs`) that puts `view` on
  the Esc+w/b menu is a one-line rule, unbuilt at the console spike.
- The per-user SESSION-path channel (a per-pane control endpoint + token/quota)
  is halcyond's seam; the console spike posts ONE `/srv/halcyon`.

## Caveats

- The ARGB payload is the raw `&[u32]` LE bytes: correct on little-endian
  aarch64, and both sides agree via `inlinewire`; a big-endian target would need
  an explicit swap.
- The witness card `usr/view/testdata/test.png` (+ `make-test-png.py`, stdlib
  zlib) is the E2E fixture, baked to `/test.png` under `THYLACINE_HALCYON=1`.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
