---
id: chg-2026-09-29-beacon-aside
type: chg
title: "A Markdown block quote is a Beacon aside -- checked by the manual, boxed where the console wraps, framed by Halcyon"
date: 2026-09-29
arc: arc-tapestry
commits: ["*(pending)*"]
touched:
  - sub-beacon
  - sub-manual
  - sub-lantern
  - sub-halcyond
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-29
---
Operator vote 2 of 2026-09-28 made code ([[dec-2026-09-28-beacon-aside]]).
Beacon gains `aside`, a paired block op with no arguments that names a passage
set apart from the flow, never a layout; it nests no block op, and its payload
flows ([[sub-beacon]]). The manual's checker, which rejected the Markdown
block quote, now accepts one that holds paragraphs and flat lists, and rejects
a nested quote, a heading, code block or table inside one, and a quote with no
content. At the rich tier the renderer writes it as an `aside` with only `em`
frames inside, so the stripped rich stream is still the unwrapped plain text;
at a plain tier that wraps, which is only the console at `/dev/winsize`'s
width, it draws a box of U+2500 furniture at most 256 columns wide, and
anywhere else it writes the text alone ([[sub-manual]]).
`manual::wraps_at_console` is the one copy of that rule, and `lantern` asks it
too: lantern had wrapped at the console's width on a tile's pts. Slide 2 of
the shipped deck ends with a block quote, which `lantern.exp` finds boxed on
the serial console and `ls-halcyon-lantern` finds framed in a capture
([[sub-lantern]]). halcyond gives each line the episode of the `pre` or aside
it was written in, carries `TAG_ASIDE` on a tile's cells with a registry of 32
block specs that maps a rebuilt row back to its episode, and frames an aside
with four 1 px `sheet.rule` hairlines at a `pre`'s margins and padding, capped
at the measure. The layout bridges the blank grid rows a tile splits a block
at, which also fixed a code block with a blank line in it showing as two
islands in a tile ([[sub-halcyond]]). One audit round (Fable 5.1 reviewing
Opus 5.5) was clean: 4 P3 from the reviewer and 2 from the implementer, five
fixed and one recorded (on the byte-fed console the line still pending inside
an open aside lays outside its frame until the newline). Host tests: beacon
40, manual 82, lantern 25, halcyond 442. Sabotage: 48 mutants, every one red,
each red set written down before its run (one of the close's nine, C8, red on
a different test than predicted, for a traced reason); a differential fuzz of
160,000 block-quote sections found no failure. CI image: `lantern.exp` and
`manual.exp` PASS in 46 s each (leg (f) matches the box by regex: macOS's
expect 5.45 faults on an `-ex` match of bytes above 0x7f). Instrument session
image: `ls-halcyon-lantern` PASS in 99 s, leg (9) finding the one hairline
frame slide two adds.
