---
id: dec-2026-09-28-sync-output-seam
type: dec
title: "A synchronized frame (DEC ?2026) reaches halcyond as two seam records"
date: 2026-09-28
status: standing
decided-by: user-vote
affects: [sub-lib-vt, sub-kaua-term, sub-halcyond, sub-aurora, sub-lantern]
created: 2026-09-28
---
## Fork

The operator saw a Lantern slide change flicker over Haul (2026-09-24) and put
synchronized output (DEC private mode 2026) in their order. Read from the code,
the flicker has two causes. Lantern writes the clear, then reads the slide (over
Haul, a network round trip), then writes the slide in about forty small writes.
And halcyond renders a tile after every read of its record pipe (at most 8 KiB),
so every piece that arrives on its own is shown: the blank screen, then a partial
slide. Even one read of a whole slide carries the clear's own records --
`ScrollOff` (the old slide, into the history), the blank `CellDiff`,
`ScreenErased`, the new slide's `CellDiff` -- and a read boundary between the
blank and the slide shows the blank.

A frame boundary that halcyond can act on has to cross the kaua seam, and a new
seam record is a wire ABI change, so it went to the operator.

## Research

- **The mode.** `CSI ? 2026 h` opens a frame and `CSI ? 2026 l` closes it
  (Christian Parpart's spec, 2021-06-25, preferred by iTerm2's own spec over its
  2018 `DCS = 1 s` / `DCS = 2 s` form). DECRQM `CSI ? 2026 $ p` is answered
  `CSI ? 2026 ; Ps $ y`: 1 while set, 2 while reset, 0 unsupported.
- **SOTA, 13 terminals read in source.** Every one that implements it keeps
  PARSING while a frame is open and holds only the RENDER (Alacritty alone
  buffers raw bytes, to 2 MiB). Timeouts: kitty 2 s; foot, iTerm2, tmux,
  Ghostty and Konsole 1 s; Alacritty, contour and mintty 150 ms; Windows
  Terminal 100 ms; WezTerm none. A resize ends the hold in kitty and Ghostty; a
  reset ends it in kitty, iTerm2, contour and Windows Terminal. A repeated BSU
  extends the timeout in foot, tmux, Ghostty, Alacritty and iTerm2, and not in
  kitty, Windows Terminal or contour. neovim, helix (termina), notcurses and
  Textual use the mode only after DECRQM says yes.
- **Heritage.** Plan 9's draw(3) buffers a client's drawing and `flushimage`
  (the `v` message) makes it visible: the program, not the screen, says where a
  frame ends. 9front's vt(1) redraws when its input drains. rio and 9term
  interpret no escapes.

## Options

1. **A new seam record.** kaua-term forwards the frame's open and close as
   `Control::SyncBegin` and `Control::SyncEnd` (subtags 7 and 8, no payload, the
   `ScreenErased` precedent), after flushing the pending `CellDiff`. halcyond
   applies every record exactly as before and holds only that tile's paint until
   the close, a resize, the program's exit, or 150 ms.
2. **kaua-term holds the records; no wire change.** Not atomic: halcyond renders
   after every 8 KiB read, and one full-screen `CellDiff` is about 40 KB, so a
   read boundary inside the held burst can still show Lantern's blank.
3. **Lantern alone.** Read the slide first and write the frame once, with no
   mode. Fixes most of Lantern's case and nothing else, and the vt would not
   claim 2026.

## The call

Option 1 (operator, 2026-09-28). The implementer's, not part of the vote: the
vt answers DECRQM for every DEC mode it tracks; aurora holds its paint the same
way; Lantern reads the slide before it writes and writes one bracketed frame;
the hold is 150 ms, measured from the first paint it defers, and a repeated
open does not extend it.

## Rationale

The render is halcyond's, so the frame boundary has to reach halcyond: holding
records upstream cannot stop a paint between two reads. Deferring only the paint
leaves every ordering contract on the seam as it was (HALCYON 14.3, 14.13), and
the program, not a timing heuristic, says where its frame ends -- the draw(3)
`flushimage` idiom carried into the terminal.
