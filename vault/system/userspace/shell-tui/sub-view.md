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
  - usr/view/src/testdata/gray.jpg
  - usr/view/src/testdata/2x2.png
  - usr/view/src/testdata/prog.jpg
  - usr/view/src/testdata/quad.jpg
  - usr/view/testdata/make-test-jpg.sh
  - usr/view/testdata/test.jpg
  - usr/view/testdata/test.png
  - usr/view/testdata/make-test-png.py
  - usr/view/testdata/test-large.png
  - usr/view/testdata/make-rgba16-png.py
  - usr/view/src/testdata/rgba16.png
audit: hard
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/HALCYON.md", "docs/LANTERN-DESIGN.md section 14"]
created: 2026-09-09
updated: 2026-09-29
---
## Purpose

`view <path>` shows a file in the Halcyon transcript. If the bytes are a
recognized image it decodes them and the picture appears INLINE in the
scrollback (letterboxed to the pane width, at native size when it fits); if they
are not, it falls back to `cat`. This is the writer half of inline media (I-47,
`docs/HALCYON.md` 14.7); the reader half -- the `/srv/halcyon` place channel and
the transcript injection -- lives in [[sub-halcyond]]. The separate graphical sibling
`gallery <path>` is [[sub-gallery]], which reuses this crate's decode and
blits to its own tapestryd surface instead of the transcript.

Two modes serve programs (2026-09-29, HALCYON 14.7's refinement of that date):
`--check` decodes and shows nothing, and `--embed` places the picture and
prints only its reference. [[sub-lantern]] is their first caller.

The load-bearing design choice: **the image decode runs HERE, in the
short-lived, unprivileged `view` process, never in halcyond** (the blast-radius
amendment to 14.7's original "decode in halcyond"). A codec is a format-fuzz
surface; hostile image bytes must parse in a process whose death costs a shell
line, not the whole-session compositor. `view` is native (libthyla-rs), so the
pure-Rust fuzz-friendly posture is kept either way.

## Contract

- `view [--check | --embed] <file | ->` (`parse_args`: exactly one operand,
  `--` ends the options, the two modes exclude each other; otherwise exit 2).
  `-` reads standard input (`read_input`), so a caller can hand `view` a file it
  opened itself.
- The interactive form (`Mode::Show`): sniff the leading bytes. A PNG or JPEG
  decodes, is reduced to the pane's limit, and is placed -> "view: <path> placed
  inline (WxH)", with ", reduced from W0xH0" when it was reduced, exit 0.
  Decoded but not displayed -> "view: <path> decoded WxH (N argb px); not
  displayed (why)", exit 1 (it was exit 0 until 2026-09-29, when `--check` took
  over the standalone decode check). Anything else -> `cat <path>` (the
  operator's spec: "text would fallback to cat"; standard input is written back
  out), whose status is view's. `cat` is spawned as `CAT`, `/bin/cat`: a bare
  name resolves against the working directory, so `view notes.txt` failed
  wherever no `cat` sat beside the file and ran the one that did (audit
  IMG-SLIDE r2 F1, 2026-09-29; `ls-gfx-inline-view`'s text-fallback leg). The
  path follows `--`, so `view -- -n` shows the file `-n` rather than handing
  cat an option (self-found beside r2; the leg's `-zq.txt` step).
- `--check`: read, sniff, the headers-only budget, the full decode, nothing
  shown. Exit 0 and silent, or one bare reason line on stderr (no `view:`
  prefix, because the caller names the file) and exit 1. Never `cat`.
- `--embed`: needs `/env/HALCYON_PLACE`, asked BEFORE the read and the decode
  (`NO_CHANNEL`); decode, reduce, place, then write only the caption object and
  a LF to stdout, exit 0. A failure is one bare reason on stderr with stdout
  empty, exit 1. Its stdout's tier is not checked: the caller composes the
  reference.
- The wire it speaks is `inlinewire` (this dossier's other half), shared verbatim
  with halcyond's reader so writer and reader cannot drift: a 32-byte header
  (magic `HPL2`, format, w, h and u128 raster ID -- all integer fields LE) then w*h ARGB `u32`s as LE
  bytes.

## Mechanism

### The decoder (`view` lib, pure, host-tested)

`sniff` reads magic, never the extension (the bytes are the truth; the obj-verb
menu offers `view` on any file). `decode_png` drives the zune no_std decoders
(`zune-png`/`zune-core`, vendored) and NORMALIZES every colorspace zune reports
after its own palette/sub-8-bit expansion -- Luma / LumaA / RGB / RGBA, 8- or
16-bit -- to one `0xAARRGGBB` per pixel (a 16-bit image narrowed to its high
bytes inside zune's own sample buffer, `png_set_strip_to_8bit`; Luma
replicated across RGB; a missing alpha opaque). `cartoon::Op::Image` composites
the alpha over the pane ground, so a transparent PNG shows the pane through.
`MAX_PIXELS` (64 Mpx) is the absolute ceiling; the REAL bound is the caller's
pixel budget, since the heap grows to hold the decode. `png_dimensions` reads
the IHDR WITHOUT decoding and `within_pixel_budget`
compares `w*h` (in u64, no overflow) to a caller budget, so a viewer rejects an
over-budget image from the headers alone -- BEFORE the heap-hungry decode
(the peak is zune's: the whole inflated stream beside its output buffer, ~8*npx
for an 8-bit PNG and ~16*npx for a 16-bit one, ~12 and ~24 interlaced, more on a
stream longer than its dimensions -- `decode_png`'s own samples + ARGB stage
stays below it). Both are reused by [[sub-gallery]]; the channel's tighter cap is
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

The bin flow: `parse_args` -> (`--embed` without a session channel fails here)
-> `read_input` (`slurp_capped`, `READ_CAP` 16 MiB) -> `sniff` (`Kind::Other`
in the interactive form -> `cat_fallback`) -> `decode`: **reject over-budget
from the headers** (`within_budget` on `png_dimensions`/`jpeg_dimensions` +
`within_pixel_budget` vs `VIEW_MAX_PIXELS` = 3 Mpx), then
`decode_png`/`decode_jpeg` -> `drop(bytes)` -> `--check` exits 0, the
interactive form runs `show` (`place_on_halcyon` + the report), `--embed` runs
`place_on_halcyon` + `caption`.
The decode runs on libthyla-rs's growable heap ([[sub-thyla-heap]], B-1c);
`VIEW_MAX_PIXELS` = 3 Mpx is the real bound on the WORST decode mode's peak + the
held input, checked before decode. The draw per pixel depends on the format. A
PROGRESSIVE JPEG holds a full-image coefficient buffer per input component (~2 B
* components * npx) alongside the output, ~READ_CAP + 12*npx -- 12*3M + 16 MiB
= 52 MiB. A PNG (zune-png 0.4.10) holds its whole inflated stream beside its
output buffer and counts its input twice (held, and its IDAT data copied): at
3 Mpx, 80 MiB for a 16-bit PNG, 104 MiB interlaced, and 128 MiB for a malformed
16-bit stream, which doubles zune-inflate's buffer before it is refused (the
I-47 close's round, F2). The 3 Mpx figure was sized to the fixed 64
MiB heap the program declared until B-1c, where the pre-JPEG-slice 6M would OOM a
progressive JPEG ([[sub-gallery]]'s JPEG-round F1, the sibling of its own
R-GALLERY-1 OOM); whether it should now follow the system's memory is an open
policy question (OPEN-BUGS). The
channel applies the current per-pane raster allowance downstream; a successful
decode alone does not guarantee display admission. Both image
arms produce a `Raster`; `Kind::Other` falls back to `cat`.

`connect` picks the channel (I-47, HALCYON.md 14.7.2). In a SESSION the
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
short write it returns `Err`, and the caller reports the failure.

**The fit (2026-09-29).** After connecting, `place_on_halcyon` has `read_limit`
open `place` a second time `T_OREAD` (the upload's handle must start at offset
0, which the channel requires) and read up to `LIMIT_TEXT_MAX + 1` bytes, which
`inlinewire::parse_limit` accepts only in the canonical form. `fit(r, limit)`
then reduces the raster: `fitted_size` takes the largest `w' x h'` with the
source's aspect holding at most `limit` pixels and at most `MAX_SIDE` (8192,
`inlinewire::MAX_W`/`MAX_H`, the header's own bound) on a side -- the width
each bound allows, the narrowest winning, then `h' = w'h/w`, then both clamped
so `w'h' <= limit`, never 0, never above the source -- and each new pixel is the AREA AVERAGE of the source pixels its span
covers, colour weighted by alpha (straight alpha: a transparent pixel lends no
colour), rounded to nearest. A read that fails or does not parse holds the
raster to the side bound alone and lets the server judge its size; a limit that falls between the
read and the upload is refused as before. The fit's peak is the source raster
beside the reduced one (4 B/px each: at most 12 MiB + 4 MiB at the 3 Mpx budget
and the 1 Mpx ceiling), below the decode's own peak, because the compressed
input and the decoder's buffers are already freed.

Session placement uses a fresh u128 raster ID, followed by a standalone Beacon
`inline-image` caption with that ID. The text stream establishes output order;
Halcyon's per-pane cache supplies the raster. The interactive form requires
rich output when the per-pane endpoint is inherited, avoiding invisible
placements through a pipe; `--embed` hands the reference to its caller instead,
and without a session channel it refuses, because the console path carries no
reference.
Console placement retains ID zero and direct transcript insertion.

### inlinewire (the shared wire, pure, zero deps)

The sole home of the place-request format so writer and reader never drift; it
carries NO decoder and NO syscalls, so depending on it drags neither zune into
halcyond nor libthyla-rs into a host test. `PlaceHeader::pack` builds the 32-byte
header; `PlaceHeader::parse` FULLY validates it -- magic, `FORMAT_ARGB8888`, a
non-zero in-bounds w/h, and `w*h <= MAX_PIXELS` (16 Mpx) -- before returning,
so a `Some` result is safe to size an allocation from (`payload_len`/`total_len`
cannot overflow). This is the first line of the format-fuzz defense; halcyond's
`inlineaccum` adds the heap-safe per-image cap on top.

The limit text is inlinewire's too (2026-09-29): `limit_text` formats the
channel's current per-image cap as ASCII decimal and a LF (`LIMIT_TEXT_MAX` =
21 bytes, the 20 digits of `u64::MAX` and the newline), `limit_read(px, offset,
count, buf)` returns the window a 9P read at `offset` for `count` covers (end of
file past the text; the arithmetic saturates), and `parse_limit` accepts exactly
that text: digits with no leading zero, one LF, checked arithmetic, non-zero.
Both of halcyond's servers answer a read of `place` with it ([[sub-halcyond]]).

## Data structures

- `Kind` (`view`) -- Png / Jpeg / Other, from `sniff`.
- `Mode` (`view` bin) -- Show / Check / Embed, from `parse_args`.
- `Raster` (`view`) -- `{ w, h, argb: Vec<u32> }`, w-tight `0xAARRGGBB` rows.
- `PlaceHeader` (`inlinewire`) -- `{ id, format, w, h }`; `MAGIC`/`FORMAT_ARGB8888`/
  `HEADER_LEN`=32/`MAX_W`=`MAX_H`=8192/`MAX_PIXELS`=16 Mpx.

## Concurrency

None. `view` is a single-threaded, short-lived process: read the file, decode,
write the channel, exit. inlinewire is pure functions.

## Invariants enforced

I-47 (ENFORCED since 2026-09-29 in ARCH section 28; no vault `inv-` node yet) -- the
writer side. The decode is contained to this sacrificial process (never
halcyond); the handoff is a bounded WRITE, not shared memory (Weft needs
`CAP_HW_CREATE`, which `view` lacks -- the wrong trust direction for inline); the
format is validated at the header before either side allocates. The RESOURCE
bound and the enforcement of the parse against hostile bytes are the reader's
([[sub-halcyond]] `inlineaccum` + [[haz-budget-stored-not-derived]] in spirit).
The fit runs here too, so the reduced raster is computed in the sacrificial
process; and the limit read is additive: the header wire is unchanged, an old
client never reads, and an unparseable answer degrades to the old behaviour,
where the server refuses an over-cap upload.

## Error paths

- A non-UTF-8 argument, no operand or two, an unknown option, both modes ->
  a diagnostic and the usage, exit 2.
- The interactive form: an open or read failure -> "view: <path>: cannot
  open|read: ..." and a decode failure -> "view: <path>: <reason>", both exit
  1; decoded but not displayed -> the report, exit 1; not an image -> `cat`
  (its exit status is view's).
- `--check` and `--embed`: every failure is ONE bare reason line on stderr and
  exit 1 -- `cannot open: ...`, `larger than the 16 MiB view reads`, `not a PNG
  or JPEG picture`, `image too large (WxH; over the 3 Mpx budget)`, a decoder's
  reason, `NO_CHANNEL`, a channel failure, `cannot write the reference`.

## Performance

A one-shot: one file read (`slurp_capped`, 16 MiB cap), a headers-only
dimension read, one zune decode (`bytes` freed after), one read of the pane's
limit, one pass over the source pixels when it must be reduced, one channel
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
  an over-budget image is a clean report, never a death at a page fault when the
  system runs out of memory mid-decode (the pre-fix defect, on the fixed heap: a
  bare `MAX_PIXELS` far above the heap was a phantom bound -- [[sub-gallery]]'s
  holotype F1).
- **The wire against drift.** `inlinewire::parse` validates before it returns;
  the pack/parse round-trip + the bounds rejections are host-tested; the magic
  reads as `HPL2` in a hexdump (a true-comment/wrong-value guard).
- **The fit's arithmetic.** `fitted_size` works in u64 (`max_px * w`
  saturates; the square root's Newton step from `n/2 + (n&1)` cannot overflow);
  `fit` sums each block in u64 (a block is at most the whole 8192x8192 source:
  255 * 2^26 * 255 < 2^64) and returns the raster unchanged when
  `argb.len() != w*h` rather than index past it. Tests:
  `isqrt_rounds_down_everywhere_it_is_asked`,
  `a_fitted_size_keeps_the_aspect_under_the_limit` (exact figures, the side
  bound, and a grid property), `fit_averages_what_each_new_pixel_covers`,
  `fit_tiles_the_source_exactly`, each sabotaged red.
- **The limit text.** `parse_limit` refuses everything `limit_text` does not
  write (a leading zero, a sign, a space, a second LF, overflow, zero), and
  `limit_read`'s window saturates for any offset and count; inlinewire's three
  limit tests, each sabotaged red. halcyond's two `h_read`s live in bin-only
  modules, so the device fit legs exercise them.
- **The program modes' contract.** Every exit path's status; `--embed`'s stdout
  carries nothing but the reference; its channel check precedes the read.
- **The handoff trust direction.** A bounded WRITE to a per-endpoint service, not
  a shared mapping; `view` holds no elevated capability; an absent renderer is a
  clean fallback, never a hang.

## Seams

- JPEG decode (`zune-jpeg`) LANDED: `decode_jpeg` + `jpeg_dimensions`, the same
  headers-only-budget-then-decode path as PNG, wired into both viewers' `Jpeg`
  arms; fixtures `testdata/test.jpg` (E2E) + `src/testdata/quad.jpg` (lib).
- The separate `gallery` command is implemented (a native libtapestry pane, [[sub-gallery]]);
  `Embed` (the out-of-band pixel surface for video) is unbuilt (I-47 / the HALCYON
  14.7 medium split: images native, video a ported C codec, audio -> Nocturne).
- The obj-verbs (`path view view {}` + `path gallery gallery {}` in
  `/lib/beacon/verbs`) that put both viewers on the Esc+w/b menu LANDED
  ([[sub-beacon]]).
- The per-user SESSION-path channel LANDED (halcyond's `paneplace.rs`, see
  [[sub-halcyond]]): `view` now prefers the per-pane `/env/HALCYON_PLACE` address
  (`/srv/halcyon-<user>/<hex(token)>/place`) via `connect`, falling back
  to the console `/srv/halcyon` only when that env is absent.
- [[sub-lantern]] is the first caller of `--check` and `--embed`; its
  `is_reference` accepts only the exact bytes `caption` writes.

## Caveats

- The ARGB payload is the raw `&[u32]` LE bytes: correct on little-endian
  aarch64, and both sides agree via `inlinewire`; a big-endian target would need
  an explicit swap.
- The witness card `usr/view/testdata/test.png` (+ `make-test-png.py`, stdlib
  zlib) is the PNG E2E fixture, baked to `/test.png` under `THYLACINE_HALCYON=1`;
  `testdata/test.jpg` (the same card re-encoded) is the JPEG one, baked to
  `/test.jpg`. `testdata/test-large.png` (`make-test-png.py large`, the same card
  at 2048x1536: 3 Mi pixels, the decode budget, three times the largest pane
  limit) is baked to `/test-large.png`, and the fit legs of
  `ls-gfx-inline-view` and `ls-gfx-session-image` show it only if `view` read
  the limit and reduced the raster. Three `src/testdata/*.jpg` lib fixtures: `quad.jpg` (32x32 colour,
  baseline -- `decode_jpeg_quadrants_to_argb` asserts APPROXIMATE colours in a
  +-48 band, JPEG being lossy), `gray.jpg` (16x16 grayscale -- the Luma arm, every
  px r==g==b), and `prog.jpg` (32x32 SOF2 PROGRESSIVE -- the coefficient-buffer
  path the holotype's F1 rode in on). Generated by `make-test-jpg.sh` (needs
  `sips` + libjpeg's `cjpeg`/`jpegtran`); like the PNG card they are COMMITTED, so
  the pool bake and host tests need no encoder. `src/testdata/rgba16.png` (2x2
  RGBA at 16 bits a sample, from `testdata/make-rgba16-png.py`) feeds
  `decode_png_takes_the_top_byte_of_a_16_bit_sample`, red without the strip. `view`
  lib tests: 16.
- The fit averages gamma-encoded (sRGB) values, not linear light, so a reduced
  high-contrast pattern comes out a little darker than a linear-light average
  would. `gallery` scales for display with its own code and does not use it.
- The `decode_jpeg`/`gallery` `Jpeg` E2E is `tools/interactive/ls-gfx-jpeg.exp`
  (view inline + gallery fullscreen, HVF, SKIP-clean on aurora).

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
