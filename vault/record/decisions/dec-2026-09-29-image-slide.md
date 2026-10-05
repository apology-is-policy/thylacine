---
id: dec-2026-09-29-image-slide
type: dec
title: "The image slide: view's program modes, the pane's limit read on `place`, and a deck's files opened no-follow"
date: 2026-09-29
status: standing
decided-by: research-collapsed
affects: [sub-lantern, sub-view, sub-halcyond]
created: 2026-09-29
---
## Fork

The operator voted on 2026-09-28 that a deck's manifest names a picture and
`lantern` runs the fixed `view` for it, validated before the talk. The
Fable-diversity round on inline media (FABLE-1, F5) had found `view`'s contract
unfit for that caller: it exits 0 when it could not display, prints a status
line into the output, passes a non-image to `cat`, requires its own standard
output to be a rich pane, and can be refused mid-talk by a per-image cap that
moves with the display and the panes. Four questions followed: how `lantern`
asks `view` for a picture, how the frame stays one write, how the size stops
failing on the stage, and what "inside the deck" means.

## Research

- **Plan 9.** `page(1)` and the `jpg(1)` family are programs that show pictures,
  beside the text tools. `draw(3)` answers a read of `/dev/draw/new` with the
  screen's geometry, so a client sizes itself from what the server reports.
- **SOTA.** kitty's `icat --unicode-placeholder` uploads the image and prints
  placeholder cells that the calling program positions in its own output: the
  same split as a Beacon reference. `icat` scales to the window's pixel size
  before transmitting. sent chooses its image filter in `config.h`, not in the
  slides.
- **The tree, 2026-09-29.** The session cap is `min(residual/16, 4 Mi/n)`
  clamped to 64 Ki..1 Mi pixels (`session.rs:1171-1182`, `tile.rs:344-347`):
  1 Mi at four panes on a display to about 1920x1200, 896 Ki at 2560x1600,
  384 Ki at 3840x2160, 512 Ki at eight panes. An over-cap header is refused
  `E_INVAL`, the answer a malformed one gets (`paneplace.rs:515-536`). A tile
  holds a synchronized frame's paint for at most 150 ms (HALCYON 14.3). Only
  the session channel carries a reference; the console channel appends a
  raster to its transcript on arrival, with nothing in the stream to order it.
  Symbolic links exist (DISTRO D-1) and `T_ONOFOLLOW` refuses one at the final
  component.

## Decision

1. **Two program modes in `view`, named for what they do.** `view --check`
   decodes and shows nothing; `view --embed` places the picture and writes only
   its reference. Both are strict: no `cat`, exit 0 only on success, one bare
   reason line on failure, `-` reads standard input. Rejected: lantern-only
   flags (`--quiet`, `--strict`) that describe a caller rather than an effect,
   and teaching `lantern` to decode, which would put hostile bytes in the
   presenter's own process. The interactive `view` keeps its caption, status
   line and `cat`, and now exits 1 when the picture was not displayed.
2. **The picture is placed before the frame, and its reference goes inside
   it.** `lantern` runs `view --embed` with its output piped back, then writes
   the frame in one write. Rejected: letting `view` write its caption into the
   open frame, which the 150 ms hold cannot cover when a decode is slower.
3. **The channel reports its cap on a read of `place`, and `view` fits to it.**
   Rejected: shrink-and-retry on refusal, which reads meaning into an errno
   shared with a malformed header; a static 1 Mi target, which still fails on
   a large display or with many panes. The read is additive: the header wire is
   unchanged, an old client never reads, and an unparseable answer leaves the
   raster held to the header's side bound alone.
4. **A deck's files are regular files in the deck directory.** `lantern`
   opens the manifest and every slide with `T_ONOFOLLOW` and hands a picture to
   `view` as the opened file. The name rules keep a name inside the deck; a
   link would carry the content out of it, and a deck someone else wrote could
   put the presenter's private picture on a projector. The deck directory is
   resolved as the presenter names it, links included; on a Haul mount a link
   in that path is the export author's, and containing a served link belongs
   to the mount (IMG-SLIDE F6, tracked).
5. **The stand-in is the manual's own aside**, rendered from a synthesized
   block quote with every punctuation character escaped: framed in a tile,
   boxed on a console with a width, plain down a pipe.

## Reverses when

A pane gains a way to report its pixel size to its programs (then `view` could
fit to the tile, not only to the cap); or the console channel gains an ordered
reference (then a console renderer could show pictures too); or the compositor's
budget stops being derived from a notional heap (FABLE-1 F6), which would
change the figures above but not the read.
