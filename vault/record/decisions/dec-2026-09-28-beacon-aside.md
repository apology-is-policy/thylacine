---
id: dec-2026-09-28-beacon-aside
type: dec
title: "The Markdown block quote becomes a Beacon aside: a passage the renderer frames"
date: 2026-09-28
status: standing
decided-by: user-vote
affects: [sub-beacon, sub-manual, sub-lantern, sub-halcyond]
created: 2026-09-29
---
## Fork

The operator asked for a boxed slide. A slide is an Operator's Manual section
(LANTERN-DESIGN 2: there is no slide format, and lantern adds no construct),
so a box is a construct of the manual's Markdown dialect, and every construct
of that dialect is realized as Beacon, whose vocabulary is closed and grows
only by a registry amendment (BEACON 3 and 12.2, the `mark k=prog` precedent
of 12.12). The manual's checker rejected block quotes (MANUAL-DESIGN 3.2)
because Beacon had no realization for one. The choice went to the operator in
the batch of four questions asked after FL-1 landed.

## Research

- Plan 9's talks were troff `-ms` with `pic`, where `box` is picture content,
  and `mpictures`' `.BP ... o` outlines a picture with a box: the frame is a
  directive the formatter draws, never glyphs the author types.
- Terminal presenters use four patterns: literal box glyphs in the source
  (mdp), a directive the tool draws (tpp's framed output), viewer chrome
  (presenterm frames only command output), and a library primitive that keeps
  the border inside the width (ratatui `Block::inner`, lipgloss `Width`,
  Textual's border box). tpp wraps framed text at the width less the frame.
- The U+2500 block is East Asian Ambiguous width; UAX #11 treats it as narrow
  where the width is unknown, as the tree's `vt` does. Every tier in the tree
  renders U+2500 today (`beacon::boxd`, `la`).
- Tree facts: the manual's plain output wraps only at a known console width
  (MANUAL-DESIGN 4.3); rich output stripped of its frames is the unwrapped
  plain output (BEACON 12.1 rule 1, MANUAL-DESIGN 4.2); 12.2 refuses layout
  and typography ops, so a box op must name a role.

## Options

1. **A block the renderer draws** (recommended): the Markdown block quote,
   rejected today so no section changes meaning, becomes a Beacon `aside`;
   Halcyon frames it through its stylesheet (proportional, reflowed); the
   plain tiers draw `beacon::boxd` U+2500 furniture with the text wrapped at
   the width less 4; a slide that is one block quote is a boxed slide, and
   the manual gains it too (one dialect).
2. **Chrome**: lantern frames every slide itself. No dialect change, but a
   manifest key choosing it would be display authority (LANTERN-DESIGN 5
   refuses one), so it could only be lantern's fixed behaviour.
3. **Defer**: box-drawing typed into a code fence already renders, as mono.

## The call

Option 1 (operator, 2026-09-28 ~18:30Z). As specified:

- `aside` is a paired block op with no arguments. It names a role, a passage
  set apart from the flow, and never a width, border, colour or position. It
  nests no block op, and its payload flows as document text does: each line
  is reflowed to the frame, and an empty line is a paragraph break (BEACON
  12.1 rule 5, 12.2).
- The manual's block quote holds paragraphs and flat lists only. A heading,
  code block or table is never wrapped, so at a plain tier it could not be
  kept inside the box; the checker rejects one inside a block quote, as it
  rejects a nested block quote and one with no content (MANUAL-DESIGN 3.2).
- At a plain tier the box exists only where the reader wraps, at a known
  console width: without a width there is nothing to draw a frame to, and
  the text alone keeps the unwrapped plain output equal to the rich output
  with its frames removed (MANUAL-DESIGN 4.2, 4.3).
- Halcyon draws a hairline frame in the `rule` ink with no ground of its own,
  the lines laid as prose inside a `pre`'s margins and padding
  (HALCYON-VISUAL 8.4, HALCYON-INSTRUMENT 7.5).
- The shipped demo deck carries one block quote, so both realizations are
  seen on the device.

## Rationale

It is the heritage shape: a frame the formatter draws from a directive, with
the border inside the width. It needs no new display authority, because the
role rides the content and the renderer owns the look. It changes no existing
section's meaning, since the construct was rejected until now. The vocabulary
stays semantic: `aside` says what a passage is, the way `hdr class=title`
names a heading's role, and each renderer decides what that looks like.
