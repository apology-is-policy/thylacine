---
id: chg-2026-08-15-syscall-abi-collision
type: chg
title: "The syscall ABI re-swept: two branches allocated the same number, and the fifth mirror had nothing to grep for"
date: 2026-08-15
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-syscall-abi]
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/syscall.h, kernel/syscall.c, usr/lib/libt/include/thyla/syscall.h, usr/lib/libthyla-rs/src/lib.rs, kernel/include/thylacine/vivarium.h]
depth: rich
created: 2026-08-15
---
This dossier's substance is a **census**, so re-sweeping it means re-running the
measurement rather than re-reading the prose. Every figure moved and every
structural claim held.

| | 2026-08-03 | now | |
|---|---|---|---|
| live syscall numbers | 100 | **103** | |
| dispatch arms | 100 | **103** | sets equal, **both** differences empty |
| allocated span | 104 | **107** | |
| holes | 4 | **4** | 26, 30, 43, 80 — same four |
| C mirror | 74 | **75** | |
| Rust mirror | 92 | **95** | |
| kernel asserts | 100 | **109** | |
| C / Rust asserts | 46 / 33 | **50 / 43** | |
| "mirror" lines | 73 | **80** | |
| "must mirror" | 22 | **23** | case-insensitive, both times |

## Three ways the census could have lied, and what stopped each

**Counts can match while sets differ**, so a count comparison passes
vacuously. The claim worth making is the empty difference in *each* direction,
which is what was measured.

**The prefix does not discriminate.** A census keyed on `T_SYS_` returns 97 for
the Rust mirror and two of those are not syscalls — the argv count and data
caps live in the same namespace. The kernel keeps the categories apart by
**form** (enum for numbers, `#define` for bounds); the Rust mirror flattens both
into `pub const`. The negative control fired here and the honest reading is that
it fired on my extraction, not on a defect. Intersect against the kernel enum.

**A control can be satisfied by an empty input.** The first attempt returned 0
for both mirrors — the C mirror is an enum not a `#define`, and the Rust names
carry a `T_` prefix — and the set-difference control was *empty*, which read
exactly like a pass. What caught it was the **count** being zero: a positive
expectation. The check I had called the control was the vacuous one. Pair a
negative assertion with a positive one or it is satisfied by a broken fixture.

## The event: two branches allocated the same two numbers

Both live, both with real consumers, allocated independently on unmerged
branches.

**Duplicate enum values are legal C.** The merge would have compiled *silently*
and stayed silent until two dispatch cases collided — and fixing the collision
is not fixing the bug, because every mirror carries its own copy of the number.
This dossier's "nothing pins a mirror to the kernel" section describes drift
between mirror and kernel; this is a second hazard the section did not name,
**concurrent allocation from one free list**, and it is worse because there is
no drift to detect: every copy is internally consistent and two of them mean
different calls.

Which side moved was decided on **measured edit cost**, not seniority: one side
embedded the literal once, in-tree; the other three times inside **patch files**
against an out-of-tree dependency rebuilt remotely. Editing a patch file is the
riskiest edit in this tree — `patch` trusts the hunk header and silently drops
added lines past it, which this project has already been bitten by.

## The fifth site, and it is the part that generalizes

Four sites were findable: the kernel enum, two Rust constants, and a naked
`mov x8, #N` in assembly. The fifth was a constant defined as *"the highest
assigned native syscall number"*.

**It contains no syscall number to grep for.** It is a *semantic* mirror,
invalidated by a renumber of the top because the renumber moves what it is
defined against. Both agents' censuses missed it and neither was looking for
that kind of thing.

The consequence would have been silent and security-shaped: the phenotype
collision argument is keyed to that ceiling, so a stale ceiling voids the
argument for every row at or below the new value, with nothing failing.

That is the sharpest addition to this dossier's prosecution list, and it
generalizes past syscalls: **enumerate mirrors by what they MEAN, not by what
they CONTAIN.** "The highest assigned N", "one past the last", "the same as X" —
all mirrors, none searchable. Every census in this vault that greps for a value
is blind to this whole class, including the ones I have run today.

## What caught it is the vault's own ratified lesson, in its positive form

A `_Static_assert` written **at the point of the hazard**, whose message says
what to do and why — left by someone who had already lived the same failure,
since the header records that this constant *"was previously written out as a
literal in four places and was stale in all four."* And the re-check the message
demands is itself mechanized: the ceiling-dependent rows each assert
individually, so the compiler adjudicates a bump rather than a hand scan.

This is the one place on this surface where enforcement is a **mechanism**
rather than an instruction to a person — which is exactly what the cutover
decision ratified: a rule saying "keep these in sync" is safe-if-remembered;
only a check that fails is safe-by-default. Here one existed, and it worked.

## The append-only rule survives its apparent violation

A renumber is precisely what the dossier's first prosecution rule forbids. It
holds anyway, because **append-only is a property of the SHIPPED number space**,
and two unmerged branches do not have one shipped space between them. The rule
binds allocation *from* a released ABI; it cannot adjudicate two branches
drawing concurrently from the same free list.

Nothing prevents the recurrence except that the free list is now shorter — so
the rule earns a companion: a number allocated on an unmerged branch is not
allocated, and the far branch's tip is part of the check.
