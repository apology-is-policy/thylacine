---
id: chg-2026-08-16-gates-external-kill
type: chg
title: "A failing catch-all buries failures too, and my own correction left the summary behind"
date: 2026-08-16
arc: arc-vault
commits: ["6a275990"]
touched: [sub-substrate-gates, gate-smp]
established: []
closed: []
opened: []
depth: rich
created: 2026-08-16
---
The multi-boot classifier gained a fifth class. Recording it turned up that the
gate note's own machine-readable summary had been wrong since the sweep that
corrected its prose — mine.

## The half of the catch-all lesson that was missing

This dossier already carried the strong version of one lesson: a benign class
that over-matches becomes a catch-all and buries real failures by passing them.
The instance was a timing pattern that matched a *passing test's name*, present
in every healthy log, which absorbed twenty-three of forty real misses.

The conclusion drawn from it — that the unclassified bucket must FAIL, that
there is deliberately no bucket for "probably fine" — is right and was treated
as sufficient. It is not.

A boot failed with `OTHER fail: <unclassified>` and took the whole gate down.
The verdict was correct. The guest was provably healthy, and the last line of
the log was the emulator announcing it had been signalled from outside — a fact
the classifier was reading past and discarding.

**A failing catch-all buries failures exactly as a benign one does.** The benign
kind buries by silence. The failing kind buries by making the red routine: every
occurrence costs a full investigation, and any genuine failure landing in the
same bucket is camouflaged by the explained ones. Both destroy the signal; only
the direction differs.

The fix in both cases is the same and does not change any verdict: take the
explainable cause out of the bucket and give it an honest label of its own.

## Soundness by negative space

The new class keys on the emulator's own report of the signal and the sender
pid, and it would be easy to say the evidence is that message's content. It is
not.

The argument is what **cannot** produce that line. A guest cannot signal its own
hypervisor. The harness's only kill is the uncatchable one, for which the
emulator prints nothing at all. **The single killer that would make the message
ambiguous is structurally incapable of writing it** — so the line's mere
existence is the proof, and the signal number and sender pid are a bonus that
was previously being thrown away.

That is a nicer form of evidence than "the producer reports the fact", which is
the usual remedy. Here the producer reports it *and* the only confounder is
mute.

## The pattern was fitted to a fresh specimen, not to the incident

The regex was validated by generating a real kill from the emulator binary
currently in the loop — which revealed that the current build appends a
sender-name suffix after the pid that the original captured incident did not
have.

A pattern fitted to the capture, anchored at the line end, would have matched
history and nothing that will ever happen again. **A captured specimen dates
from the producer that existed then**, and a detector validated against it is
validated against a version of the world.

The pattern deliberately does not anchor the end; both forms match, and where
the sender is resolvable the suffix names it for free.

## Two structural details worth more than the class

**Ladder position was argued in both directions.** After the corruption class,
so a real corruption signature is never masked by a kill that arrived
afterwards; before the benign green-guest class, so a killed-but-green boot
cannot be absorbed into something that does not fail. Either move silently
loses a real failure — which makes a new class an edit to *every* verdict, not
an addition to them. That generalizes past this gate.

**Volatile forensics are captured at detection, not at report.** The sender pid
is resolvable for seconds and gone by the time an operator opens the log, so the
classifier resolves it immediately and appends the record to the capture. An
instrument that defers a perishable reading has not taken it.

## The correction that corrected only the prose

The gate note recorded, in its own body, that a previous sweep had corrected it
from two classes to four. That sweep was mine.

It updated the table. It did not update the `proves:` frontmatter, which still
described **two** classes and — worse — a failure condition that had been false
for longer than that: "fails iff any boot corrupts", when the unclassified
bucket had failed the gate since the earlier regression.

`proves:` is the field rendered into the views. So the corrected version lived
in the body a reader opens deliberately, and the wrong version lived in the
summary that gets carried everywhere — the same inverted readership as the
invariant mirror closed in this batch, arrived at by a different route.

**A correction updates the place where you noticed.** You notice while reading
the prose, so the prose is what gets fixed; the summary is somewhere else, is
not on screen, and is not what prompted you. Nothing about diligence prevents
this — the sweep that made the correction was *specifically* checking that
count.

Both places now agree, and the note says which was wrong rather than quietly
reading correct, because the failure mode is more useful than the fact.

## The count has no owner

Stated as a caveat on the dossier: a class count is a fact nobody's step
maintains. Adding a class is a change to the classifier; updating everything
that states the total is not part of it, and the total appears in a script
header, an architecture bullet, a reference section, a gate note's body, that
note's summary, and this dossier.

I checked the vault's own mirror set for this one rather than assuming it was
two files. It was two, both fixed. The neighbouring interactive gate has its own
classifier and is a different count that must not be conflated.

## Riding along

A fixed control-socket path was a **serialization constraint wearing a
default's clothes**: nothing forbade two boots at once in a tree, they would
simply have addressed the same file. Making it overridable is the whole of what
let the interactive gate run several scenarios at a time. The constraint was
invisible precisely because nothing enforced it — it announced itself only as a
collision, to whoever first tried the concurrency the tool never said it lacked.
