---
id: chg-2026-09-30-image-slide
type: chg
title: "The image slide: a deck names a picture, and view shows it"
date: 2026-09-30
arc: arc-tapestry
commits: ["2f2cff77"]
touched:
  - sub-view
  - sub-lantern
  - sub-halcyond
  - sub-substrate-interactive
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-30
---
Operator vote 3 of 2026-09-28 asked for the image slide: a deck's manifest
names a PNG or JPEG and lantern runs `view` to show it
([[dec-2026-09-29-image-slide]]). This closes FABLE-1 F5. `view` gains two
program modes, `--check` (decode, show nothing) and `--embed` (place, print
only the reference), which never pass a file to `cat`, exit 0 only on success
and report one bare reason line; the interactive form exits 1 when the picture
is not displayed ([[sub-view]]). A read of `place` answers the pane's live
per-image cap, which moves with the display and the pane count, and `view`
area-averages its raster to it and to 8192 on a side before the header, so a
picture checked before the talk is not refused during it ([[sub-halcyond]]).
lantern treats a manifest entry ending `.png`, `.jpg` or `.jpeg` as a picture
slide: `view --check` decodes it at startup and `view --embed` places it
before the slide's one-write frame, which carries the reference; elsewhere the
slide shows a stand-in naming the picture and why. Every file of a deck is
opened with `T_ONOFOLLOW` as a regular file, closing a link out of the deck
that the text-slide path had left open ([[sub-lantern]]). New gate legs cover
the check, the stand-in, the picture's pixels in a tile, a linked slide over
Haul and the fit on a 2048x1536 fixture ([[sub-substrate-interactive]],
[[sub-substrate-build]]). The chunk's audit round (Fable 5.1 reviewing Opus
5.5): 0 P0 / 0 P1 / 0 P2 / 7 P3, six fixed; F6, a link served by a Haul export
redirecting the deck directory, is a Haul design item; the device gates then
found a P0 it missed -- lantern spawned `view` by a bare name, which a spawn
resolves against the working directory, so an executable `view` there ran in
its place -- fixed by spawning /bin/view. A second round (r2) on the
gate-fixed tree found the same defect in `view`'s text fallback, a bare `cat`,
fixed by spawning /bin/cat; its stale spawn doc and `lantern ""` reading the
namespace root are fixed too. A third round (r3) found four P3s: the
leading-dash name, two overstated sentences and an unbounded reap are fixed,
and viv's unresolved manifest command is tracked. The host suite
(tools/test-rust.sh) at the final tree runs 2185 tests in 29 crates and fails
none -- view 16, lantern 30, inlinewire 5, halcyond 445 -- and the three bakes
built every guest crate. Fourteen sabotages of the new code and data (S1-S14)
each turned exactly its predicted tests red, and so did three of four on the
side bound (SB1, SB3, SB4); SB2 also reddened fit_averages, whose 4x1 case at
side 2 is width-bound, a wrong prediction rather than a gap; on the device, a
spawn that tried the working directory first turned lantern.exp's leg (h) red
both ways; and each leg added after the audit rounds was red on the image
before its fix: the text fallback ran a planted `cat` from the working
directory (the status arrived without the note), `lantern ''` opened
`/slides.toml` at the root, and `view -- -zq.txt` refused the name as an
invalid option. Device: three bakes at the final tree, eight gates green on
one attempt each -- the CI image (`lantern` 41 s, `manual` 41 s), the session
instrument image (`ls-halcyon-lantern` 99 s, `ls-halcyon-lantern-haul` 74 s,
`ls-gfx-session-image` 56 s, `ls-halcyon-session-media` 76 s) and the console
Halcyon image (`ls-gfx-inline-view` 44 s, `ls-gfx-jpeg` 42 s) -- and the
kernel suite on the CI image, 1782 of 1782, with debug-probe's three held legs
and ambush-probe's stages C and D.
