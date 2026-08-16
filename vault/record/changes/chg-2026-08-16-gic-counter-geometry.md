---
id: chg-2026-08-16-gic-counter-geometry
type: chg
title: "The number did not change; the reason did"
date: 2026-08-16
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-gic]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
One commit, and the best thing in it is a constant that stayed exactly the same
while the argument underneath it was replaced.

## A correct answer on the wrong axis reads as complete

A per-CPU interrupt counter shipped as eight eight-byte slots — **exactly one
coherency granule.** One cache line that every processor stores to on every
interrupt, hit at tick rate by the timer alone before any device interrupt or
inter-processor interrupt exists. The worst false-sharing site the kernel could
have, on its hottest path.

The reasoning that accompanied it was *"single-writer per CPU, no
read-modify-write needed."* Every word of that is true. It is an answer about
**correctness**, and the defect is about **geometry**, and nothing marks the
boundary.

**That is what makes this class hard: the argument is not wrong, so checking it
does not find the problem.** A reader verifying the single-writer claim
verifies it, correctly, and moves on. Completeness on one axis is
indistinguishable from completeness.

The tell was present at the time and I would not have read it as one. **A
sibling counter added in the same change got the right geometry for free**,
because it lives inside a large per-CPU structure rather than in a bare array.
So two counters were introduced together, one accidentally fine and one not —
and the accidental correctness of the first is exactly what makes the second's
problem invisible. *A neighbour that happens to be right hides its twin.*

## Padding without alignment is a half-fix

Each slot is now padded to the maximum granule **and the array is aligned**, and
the second half is the one that reads as decoration.

Padding separates the slots from each other. Only aligning keeps slot zero out
of the granule occupied by whatever precedes it in the uninitialized data
section. Pad alone and the first processor still shares a line — with something
unrelated, which is worse to diagnose than sharing with a sibling.

## The measurement corrected the plan without changing the code

The tracked fix sketch proposed a 64-byte granule. A first draft justified 128
by asserting the development host's silicon reports a 128-byte coherency
granule.

**One boot falsified it.** The granule equals the minimum line size, at 64, under
both hardware virtualization on the real host and under full emulation.

The constant stayed at 128. The *justification* changed from a fabricated
hardware claim to a margin argument, and the margin argument is the durable one:
over-padding costs a few hundred bytes of uninitialized data, once;
**under-padding silently restores the contention with no symptom any test would
catch.** An asymmetry that severe carries the decision on its own — the invented
fact was never load-bearing, only comforting.

Worth recording precisely because nothing about the artefact moved. **A
measurement that confirms your number and destroys your reason is a real
result**, and the temptation is to file it as "no change" and keep the sentence
you had.

## Two cache fields that are not the same field

Getting there required decoding a field the kernel had never read. **The
coherency granule governs false sharing; the minimum line size is the smallest a
level will allocate**, and the architecture permits them to differ. The kernel
had only ever decoded the second, which is why the geometry question had no
answer available to ask.

A granule field of zero means the part **declines to report**, and that is
recorded verbatim rather than decoded into a size or promoted to the
architectural maximum — because *no information* and *small* are different
facts, and collapsing them manufactures a reading nobody took.

Which also bounds the fix: with no outer-level line size exposed anywhere, **any
hardware-queried pad is a lower bound rather than an answer.** So "measure it
and pad to that" is not available even in principle.

## The scope of the claim equals the scope of the evidence

Stated explicitly in the source, and it is the discipline worth copying: **no
speedup is claimed.** The emulator does not model coherence traffic, so
quantifying the win needs real multi-core hardware and a targeted
microbenchmark. The change is justified as geometry, and the test proves
geometry — slot alignment, one-granule stride, an out-of-range guard, and that
every reported granule fits the pad.

Nothing more. A change whose stated benefit exceeds its evidence is the ordinary
failure here, and this one deliberately refuses the upgrade.

The revert-probes match that care: two of them, **each hitting the same test on
a different assertion** — restoring the unpadded layout fails the alignment
assert; shrinking the constant below the measured granule fails the
covers-this-part assert. Two properties, two probes, one test, no collateral.

## A tidiness suggestion refused with reasons

A tracked note preferred folding the counter into the scheduler's per-CPU
structure, which is genuinely the tidier shape and would have fixed the geometry
as a side effect.

Refused, and the refusal is recorded rather than silent: that structure is
private to the scheduler, so the fold adds a cross-translation-unit call on the
hottest path in the kernel and points the architecture layer at scheduler
internals — to save about a kilobyte of uninitialized data. **The interrupt
layer owns dispatch the way the scheduler owns context switches**, and the
counter is a dispatch fact.
