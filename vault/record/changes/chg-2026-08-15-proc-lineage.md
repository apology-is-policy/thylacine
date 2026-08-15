---
id: chg-2026-08-15-proc-lineage
type: chg
title: "proc.c re-swept after LINEAGE: the address-space extraction, the second rfork shape, and the vfork park"
date: 2026-08-15
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-proc, sub-kernel-death, seam-rfork-flags-unimplemented]
established: []
closed: []
opened: []
mirrors-checked: [kernel/proc.c, kernel/include/thylacine/proc.h]
depth: rich
created: 2026-08-15
---
Seventeen commits on `kernel/proc.c` since the two dossiers were written, almost
all of them the LINEAGE fork/exec arc. Swept as one read serving both, since
`sub-kernel-proc`, `sub-kernel-death`, `sub-kernel-caps` and `sub-kernel-jobctl`
all cite the same file.

## What was falsified

- **"`rfork` accepts `RFPROC` and nothing else."** It now accepts
  `RFPROC|RFMEM` as well — the vfork shape. Seven reserved flags remain, not
  eight, and [[seam-rfork-flags-unimplemented]] is partly closed.
- **"`struct Proc` is 400 bytes."** 392, and it no longer holds a page table at
  all — it holds a pointer to a refcounted address space. The extraction is the
  **only change in the struct's recorded history that ever made it smaller**
  (408 -> 376 in one commit, against appends before and after). Verified from the
  assert's own `git log -L`, not inferred.

## What is new and load-bearing

**Three shapes, one discriminator.** There are now exactly three answers to
"what address space does the child get" — a fresh empty one (spawn, which will
exec), the parent's shared (vfork), or a copy-on-write clone (stock fork) — and
the discriminator is the presence of a fork context. **The handle table two
hundred lines away is discriminated by the same thing, and the source says this
is deliberate rather than coincidence**: a fork context means the child *is* the
parent continuing on its frame, so it must see the parent's memory AND the
parent's descriptors. One fact, two consequences.

**The vfork park, and why death pays nothing for it.** The parent suspends until
the child leaves its address space, and the park reuses the parent's existing
child-waiter list — which makes the *death* release free, because the ZOMBIE
chokepoint already wakes that list. Only the exec release needed a new wake, one
line under the same lock at the swap.

Its stated principle is the same one [[dec-2026-08-15-cutover]] rests on:

> The release condition is not a *record* of the release, it IS the release.

"The child is off my frame" means "the child no longer maps my address space",
which is a fact already written down. A flag would record the release somewhere
other than where it happens, so a third release path added later would silently
strand every vfork parent.

Three supporting properties, each recorded: the pointer comparison is not an ABA
only because the parent still holds a reference to the shared space (a dividend
of the extraction having moved the VMA drain to the last drop); "gone from the
children list" counts as released **deliberately**, failing toward terminating
rather than hanging; and a parent killed while parked unwinds and does not loop.

## The finding worth carrying as a shape

exec resets the signal dispositions, and for one release it did so by **freeing**
the table, reasoning from the exec-alone gate that there could be only one
reader. That gate bounds the *threads of this process*. The note-post path
reaches this process's table with somebody else as the poster on essentially
every call — the child-exit note to a parent, an explicit post, the process-group
fan, the console interrupt, a terminal hangup — and those readers load the
pointer with a bare acquire holding no lock of exec's. A use-after-free across
CPUs.

The comment was not vague. It was **precise about the wrong scope**, and it cited
a real guarantee that really does hold. Forty lines below, the same exec path
clears the hardware breakpoint slots under the same gate and there the reasoning
is sound. Same gate, one valid use and one invalid one, in one function.

## Prediction evaluated

The seam note's "risk while open" said proofs relying on "always cloned" would
get quietly weaker the day a share flag landed, and that the reliant places were
not all marked. A share flag landed; [[inv-i1]] held anyway, because `RFMEM`
shares memory and the namespace proof rests on the Territory. The warning's shape
was right and its target was not reached — which is luck of ordering rather than
something anyone arranged, and leaves its actual point untouched: nothing records
which invariants rest on which unimplemented flag.
