---
id: inv-i36
type: inv
title: "I-36 — the kernel may page a binary in from the filesystem, under seven conditions"
number: I-36
guards: [sub-kernel-exec, sub-kernel-image, sub-kernel-fault, sub-kernel-burrow]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-03
updated: 2026-08-03
---
## Statement

A running program's read-only segments may be demand-paged from the filesystem
rather than copied in at exec — **if and only if seven conditions hold jointly**.

1. **The backing identity is immutable.** A pinned content version, never a
   mutable path, so the file cannot change under a running binary.
2. **Every page is integrity-verified before install.** A hostile or buggy
   filesystem server cannot inject text of its choosing.
3. **The mapping is read-only, rights-reduced and capability-mediated** — a
   Burrow over a kernel-pinned Spoor, never a raw server handle — and W^X holds
   ([[inv-i12]]): text executable, rodata neither writable nor executable.
4. **Writable data terminates in private anonymous memory.** Never a file-backed
   writable mapping, never written back.
5. **The page-in is death-interruptible.** A wedged filesystem server turns an
   in-flight fault into a per-Proc terminate — never a stuck thread, never a
   dead machine.
6. **An I/O error is bounded and fails closed.** A per-Proc terminate
   attributable to the page; never a silent zero-fill of executable text.
7. **Pages are resource-accounted**, and shared text is charged once.

## Why seven and not one

Because six of them were already true of something else, and the design's real
claim is that the *combination* is sound rather than that any piece is new.

The tree's standing refusal is of a userspace writable file mapping — an
`mmap(MAP_SHARED)` over a server-backed file, whose failure modes (torn writes
mid-execution, `SIGBUS` on a vanished page, a wedged thread waiting on a dead
pager) are the reason Mach's external pager is remembered the way it is.
Condition 4 keeps that refusal intact. What I-36 permits is the narrow inverse:
read-only, kernel-internal, from an immutable snapshot.

Conditions 5 and 6 are the two that were genuinely new work, and they are the
two Mach did not have.

## The two conditions this tree cannot check

Conditions 1 and 2 are enforced in Stratum, not here. The kernel samples a
version into a cache key and trusts that a changed file produces a changed
version; it asks for a page and trusts the bytes were Merkle-checked before it
saw them.

That is a correct division of labour — integrity belongs to the filesystem — but
it means **two of the seven clauses have no enforcement site in this
repository**. A prosecutor auditing exec, the fault arm and the Image cache can
confirm five conditions and must take two on the strength of a cross-project
contract. Worth stating plainly rather than leaving as an absence, because the
usual reading of an invariant note is that everything in it is checkable from
the code it points at.

## Where the other five are enforced

**Condition 3** is split. [[sub-kernel-exec]]'s dispatch gate admits only
non-writable segments to the shared path; `vma_alloc` refuses any
writable-and-executable mapping regardless of origin ([[sub-kernel-vma]]); the
Burrow holds a kernel-pinned Spoor rather than anything the server can reach
back through.

**Condition 4** is the `!PF_W` conjunct of the same gate, and it is the one that
must never be relaxed for convenience — it is the whole refusal, expressed as
one bit test.

**Conditions 5 and 6** are [[sub-kernel-fault]]'s FILE arm: the page-in read is
interruptible by the death cascade, and an I/O error terminates the Proc rather
than installing zeros where instructions should be.

**Condition 7** is the charge in the map layer ([[inv-i32]]), with the sharing
clause falling out of [[inv-i7]]'s dual refcount — one set of pages, one charge.

## The condition that is a cache design

Condition 1 is realized as a *key field* rather than a check. The
[[sub-kernel-image]] cache keys on the content version, so a replaced binary is
a different entry and an already-running Proc keeps the bytes it started with.
There is no invalidation path, no coherence protocol, and nothing to get wrong
at runtime — the immutability requirement and the sharing mechanism turn out to
be the same mechanism.

## Caveats

**No model.** Prose, the focused audit round, and the test suite. The
death-interruptible leg composes [[inv-i9]]'s machinery, which is modelled, but
nothing models the seven conditions as a set.

**The design document is cited under a name that does not exist.** Five source
files reference `docs/REVENANT.md`; the file is `docs/EXEC-LOAD-DESIGN.md`. The
sections resolve, the filename never did. Task #64.
