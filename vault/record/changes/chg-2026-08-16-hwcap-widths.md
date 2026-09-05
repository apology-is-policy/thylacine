---
id: chg-2026-08-16-hwcap-widths
type: chg
title: "Two width rules in one file, and neither implied the other"
date: 2026-08-16
arc: arc-vault
commits: ["2b7d16ee"]
touched: [sub-kernel-hwcap]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The GPU arc landed on the hardware-lending surface: a second address arena, a
placement rule, an availability fix, a shared-memory capability, and a third
buffer class. Four commits, one of them a round-2 audit close that found a P0 and
described itself as *three of my own fixes* — which is the re-audit rule earning
its keep, and the reason this dossier's caveats were worth re-reading rather than
re-asserting.

## The dossier had a width rule and the bug was a different width

The dossier already prosecuted width: the size-decoding dance inverts a mask, and
inverting a 32-bit mask at 64 bits yields a multi-exabyte size. That rule was
right and stayed right.

The defect was in **placement**. Routing picked an arena purely by size, so a
window too large for the low arena went high — including one that is not
64-bit-capable, whose high half is never written back. The device would then
decode a truncated address somewhere inside RAM while the kernel's exclusivity
claim sat on the untruncated one: two views of the same window, disagreeing, with
the device's pointing at memory that belongs to someone else.

Same file. Same 32-versus-64 confusion. **Getting the first right was no evidence
at all about the second**, because one decodes a *size* and the other writes an
*address*, and only the second can leave two parties reading different memory.

This is the category-versus-property failure from the other direction. The known
form is a teardown that enumerates a *category* and misses fields sharing the
*property*. Here a prosecution rule named a *mechanism* — "the inversion must
stay width-correct" — where the property is *width discipline anywhere a 32-bit
quantity meets a 64-bit one*. **A rule stated as a mechanism protects that
mechanism and nothing beside it**, and reads as full coverage of the hazard it
names. The dossier now states both and says explicitly that neither implies the
other.

## Capacity is not availability

A third bug in the same routing: the low arena's fit test compared the request
against the window's **total span** instead of what remained. So once the arena
was exhausted, every later window failed there rather than falling through to the
arena that could still hold it.

**A full container that reports its size still answers "yes, that fits."** The
fallback existed, was correct, and was unreachable exactly when it was needed —
the failure mode of a fallback tested against the wrong quantity is that it looks
present and never runs.

## The subtype the caller cannot get wrong

The new buffer class could have been a second boolean beside the existing one.
It is an **enum instead**, and the comment says why where it is enforced: with one
boolean per class a caller can pass both, and the struct would hold a state with
no meaning. The bits are still separate in memory; only the door is narrow.

That is the **third** distinct instance in this vault of *the shape of the
interface is the safety property* — with the charge claim that returns pages
rather than a record, and the share-drop that returns a verdict rather than a
count. Three is enough to call it an idiom of this tree rather than three good
decisions: the recurring move is to make the wrong call **unwritable** instead of
rejected.

## What that idiom costs, which is the part worth keeping

I went looking for who reads the class bits, and found a consumer in the network
dataplane that maps them to a binding kind with an ordered if-else: framebuffer
first, GPU buffer second, otherwise refuse.

That is unambiguous **only because both bits can never be set at once** — a
guarantee that lives in a constructor, in a different subsystem. The reader's
comment is conscientious: it explains why it re-checks that the region is
admissible at all. It says nothing about why the ordering is safe, because from
where it sits there is nothing to notice.

Verified before writing it down: the constructor is the sole writer of either bit
across the whole kernel and architecture trees, so the guarantee holds today.

**Making a state unconstructible removes the check at the reader and puts the
argument in another file.** That is a good trade and it is not a free one — the
constraint stops being visible where it is relied on. It is the same shape as
[[sub-kernel-stalk]]'s dot gates, which look identical and must stay apart: two
sites coupled by an argument that neither one states, which is precisely the kind
of thing a dossier can hold and a comment cannot.

## Method note

Every claim above was checked against the current files rather than derived from
the four commit messages, and the check paid twice: once for the sole-writer
verification, and once for the reader whose existence the diffs never mention
because it was not modified. **A diff shows what changed, never who was relying
on it.**
