---
id: chg-2026-08-16-pouch-offset-pins
type: chg
title: "A per-artifact assert certifies that artifact, never the one in the link"
date: 2026-08-16
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-pouch-process]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
One commit widening a mirrored struct and pinning every field offset. Its own
landing then exposed a defect that makes the best statement I have seen of what
a compile-time assertion is worth.

## A size assert is blind to the mismatch it looks like it covers

The boundary struct carried one assertion, on its size. That is the standard
move, and it reads as pinning the layout.

**Swap two same-width fields and the size is unchanged.** It still compiles, the
assertion still passes, and a caller then fills the wrong field — here a page
budget written into a phenotype word, which the kernel reads as an unknown-bit
phenotype request and refuses. The failure lands at runtime, in the kernel, as a
validation error that names neither field and points at neither branch.

Twenty offset assertions now pin each field individually.

**A pin on an aggregate property cannot see a permutation of its parts.** Size,
alignment, a checksum of the field *set* — all of them are invariant under
reordering, and reordering is the mistake a hand-mirrored struct actually makes,
because the two copies are edited by different people at different times.

## A reserved pad slot is a cross-branch collision point

The struct grew because two branches each placed a new field in the same
reserved slot — a page budget on one, a phenotype word on the other. Neither is
wrong. Each is a correct, size-preserving use of documented slack, and **neither
is visible as a hazard from its own branch**, where the slot is simply free.

The merge could not let either quietly take the other's bytes, so the struct
widened.

That is the real argument for the offset pins, beyond permutation: **a reserved
slot is where independent work collides, and the collision is silent on both
sides until they meet.** With per-field offsets, the next claimant disagrees with
the kernel loudly at build time instead of in a refused syscall.

## Every assertion held and the binaries were wrong

Landing the pins exposed an unrelated defect: a workaround elsewhere was a
**blind directory overlay**, which reverted the sysroot's C archive to a day-old
snapshot. Twenty-five ported binaries relinked against the **old, narrower**
struct, and the kernel's validator refused their spawns.

**Both sets of assertions held throughout.** The new offset pins were compiled
into the header the fresh build used and were correct about it. The stale archive
carried its own then-correct assertion for its own older layout and was correct
about *itself*.

Two artifacts. Two true self-descriptions. One broken link.

**A per-artifact assert certifies that artifact, never that the artifact in the
link is the one you just built.** Compile-time checking cannot reach across a
build-system substitution, because the substituted object *was* compiled —
correctly — at a different time against a different truth. The gap is not in the
assertions and cannot be closed by adding more of them: nothing in the compiler's
world compares **the thing you built** to **the thing that got linked**.

That is a different failure from the ones this project usually catalogues. It is
not a check that cannot fire, nor a check on an untested premise, nor a control
that fails open. Every check fired, on the right premise, and was right. The
system was broken between them.

## The count that is trusted rather than verified

The patch's hunk header had to move for the added lines, and the new count was
obtained **by counting the hunk body** rather than by arithmetic on the delta.

The tool trusts the stated count and **silently drops added lines past it**. So
an arithmetic slip removes content while reporting success — which is the same
family as the overlay above, one layer down: a step that reports having done what
you asked and did something else.
