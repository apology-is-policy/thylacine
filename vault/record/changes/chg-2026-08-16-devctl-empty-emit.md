---
id: chg-2026-08-16-devctl-empty-emit
type: chg
title: "Zero bytes written, on purpose, is a thing the convention cannot say"
date: 2026-08-16
arc: arc-vault
commits: ["25caa9b8"]
touched: [sub-kernel-devctl]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
An eighth leaf landed on `/ctl` — a live 9P session listing, built as an
instrument for a reply-loss investigation — and it arrived with a defect in
exactly the convention this dossier had already flagged, in the direction it had
not.

## The sentinel collision

The formatting macros treat **a return of zero as "the buffer is full"**: set the
flag, abandon the row, stop. That is the whole discipline, and every field goes
through it.

An empty string writes zero bytes.

So the new leaf's conditional suffix — a ternary whose alternative was `""` —
aborted the format at the first live connection. Every read truncated at
precisely the same offset, which is the detail that made it diagnosable: a
deterministic truncation is not interleaving, so the console was exonerated and
the formatter was the only suspect left.

**A success that produces nothing is indistinguishable from a failure that
produces nothing.** The return channel has one value and two meanings, and
nothing in the signature hints at it.

That is the same shape as a gauge reading zero because the measured thing never
started — the family this project has hit repeatedly, here reached from the
producing side rather than the reading side.

## My caveat had the right convention and one direction of it

The dossier already said the formatters were careless with this return value. It
described a genuine failure being **ignored**: a numeric append that did not fit,
whose visible effect is a line missing its value.

The defect was the mirror image — a genuine success being **read as failure** —
and it is the worse of the two. An ignored failure loses one field. An invented
failure loses the remainder of the file.

Having enumerated one direction is what made the caveat read as coverage. It
named the fragile convention correctly and then described the hazard as if it
had one side. **The general form: a caveat that gives an example teaches the
example, and a reader who has seen the example believes the area is understood.**
Same shape as the width rule in [[chg-2026-08-16-hwcap-widths]] — a hazard
stated over one of its instances.

## The test could not see it

The regression asserted a **prefix** of the row, and the prefix sat before the
truncation point. So the assertion passed on precisely the failure it existed to
catch.

It now asserts through the row's **tail**, which is the only form a mid-row
abort cannot satisfy. The rule generalizes: for anything that can fail *part
way*, the assertion has to reach past where the failure would stop.

## What the repair got right

It did not just delete the offending ternary. The rule is written at the site,
and it covers the case that had not happened yet: a **runtime-computed** value
that could be empty — a session label — now emits a placeholder rather than
nothing.

That is the difference between fixing an instance and fixing a class. The literal
`""` was the visible bug; any expression that *can be* empty is the hazard, and
only the second framing survives the next author.

Worth noting the surrounding code had been following the unwritten rule all
along — every pre-existing ternary in those rows has two non-empty arms. **The
convention was real, correct, and undocumented, so it protected everyone who had
absorbed it and nobody who had not.**

## A count that only prose can get wrong

The dossier said seven leaves; there are eight. The code cannot make this
mistake — its count is derived from the table with a size division, so adding a
row updates it. The prose restates the number by hand.

Fourth count drift found in this sweep. This one is the cleanest illustration of
why they happen: the same fact expressed once as a derivation and once as a
literal, where only the literal can rot.

## The default-allow gate fired, and the default was right

The new leaf is world-readable, because everything except the KASLR base is.
That is exactly the shape the dossier's caveat warns about — a new leaf is
readable by everyone unless someone remembers a line.

Here it was the correct outcome: peer identifiers, buffer counters and frame
counts are ordinary introspection, narrower than the process list already sitting
beside them. Recorded as evidence about the *rate* rather than as a refutation. A
default-allow surface is right every time until it is catastrophically wrong
once, and watching it be right is not evidence that it is safe.
