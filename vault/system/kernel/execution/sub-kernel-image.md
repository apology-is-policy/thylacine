---
id: sub-kernel-image
type: sub
parent: moc-kernel-execution
title: "The Image cache — one copy of a binary's text, and a proof that eviction cannot race a mapper"
code: [kernel/image.c, kernel/include/thylacine/image.h]
audit: hard
guarded-by: [inv-i36, inv-i7]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/EXEC-LOAD-DESIGN.md"]
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

Two Procs running the same binary should fault into the same physical text
pages. This is the registry that makes that true: a fixed table keyed on a
file's identity, holding one reference to a file-backed Burrow per cached
segment. The first exec creates; the second finds.

The heritage name is kept deliberately — this is the Plan 9 Image, rebuilt on
the dual-refcount Burrow lifecycle.

## Contract

One production entry point, and its ownership rule is the whole contract:

**`image_lookup_or_create` always consumes the caller's Spoor on success** —
adopted into a new Burrow on a miss, clunked as redundant on a hit — and returns
a Burrow carrying one handle reference the caller owns. On a NULL return it
consumed nothing.

That asymmetry is the part a caller gets wrong. The function reads like a
lookup and behaves like a transfer.

The key is seven fields: device class, device number, qid path, **qid version**,
file offset, page-rounded size, and executability. Two of those carry arguments
worth stating.

**The qid version is what makes coherence free.** A binary replaced atomically
gets a new version, so it is a different key and misses the old entry; a Proc
that already exec'd stays pinned to the bytes it started with. No invalidation
path exists because none is needed.

**Executability is in the key because of a fault-arm coupling.** A crafted ELF
can declare two `PT_LOAD`s over an identical file window with different X bits.
Without that field they would share one Burrow mapped at two protections, and
the fault handler's I-cache sync is gated on whether the fill is executable — so
a non-executable fill could leave an executable mapping of the same pages
unsynced. Splitting the key makes a dual-protection Burrow unconstructible. A
legitimate binary never notices: the same segment always carries the same bit.

## Mechanism

Three phases, and the middle one is outside the lock.

**Search** under the cache lock. On a hit: take the caller's reference, stamp
the LRU clock, drop the lock, and clunk the redundant Spoor *outside* — because
closing a Spoor can sleep.

**Create** with the lock released, because building a file-backed Burrow can
sleep too. This opens a window in which a second Proc can register the same
image.

**Re-search and install.** If the window was lost, the loser takes a reference
on the winner and unrefs its own surplus Burrow, which frees it and clunks the
Spoor it adopted. If the table is full of live images, the fresh Burrow is
returned **unregistered** — it lives on its mapping and is simply not shared.
Degrading rather than failing is the right call: an exec must not fail because a
cache is full.

The refcount claim across all three outcomes is *one clunk per Spoor*, and the
file states it as an invariant and then walks each path against it. That is the
correct shape for a rule that is easy to satisfy twice.

## Data structures

`struct image_entry` — the seven key scalars, the Burrow holding the cache's
single reference, and an LRU stamp. The table is BSS, so zero means empty and
there is no initialization allocation; `image_cache_init` only flips a flag and
extincts if called twice.

The key is sampled into the entry at install rather than read back through the
Spoor, which matters: the Burrow outlives any particular caller's handle on that
file, so the cache must not depend on being able to re-read it.

## Concurrency

One global lock, plain rather than IRQ-saving, justified by exec being process
context only.

**Lock order is cache lock → Burrow lock**, never the reverse, and the reason it
can never be the reverse is structural: the free path never re-enters the cache,
because an entry is always detached from the table *before* the reference that
frees it is dropped.

**The eviction-safety proof is the best argument in this area**, and its shape is
worth naming. The question "could a concurrent mapper be part-way through
claiming this entry?" is a race question, and racing questions are usually
answered with more locking. Here it is answered by *reachability*: to take a
reference or add a mapping, a Proc must first come through
`image_lookup_or_create`, which takes this lock. So while eviction holds it, no
such Proc exists — and an entry showing exactly one handle reference and no
mappings therefore has no in-flight claimant, cannot gain one, and once detached
is reachable by nobody. The final unref happens outside the lock and still cannot
race.

That converts an SMP timing argument into a static claim about who can reach
what. It is the same move the MMU makes when it pre-demotes the allocator zone
at boot to make a block-split race unreachable rather than guarded — and it is
the strongest form of this kind of reasoning in the tree.

One subtlety worth flagging for a future editor: the victim scan does not test
`used`, relying on the free-slot scan above it having returned early if any slot
were free. Correct, and stated in prose at the function level, but the two loops
are coupled by an argument rather than by a check.

## Invariants enforced

[[inv-i36]] — condition 1 (the pinned version *is* the key), part of 3 (the
cached Burrow is read-only over a kernel-pinned Spoor), and 7's sharing clause:
shared text is charged once because the dual refcount means one set of pages.

[[inv-i7]] — the cache is a handle-count holder. Its reference is what keeps
text resident after the last Proc unmaps, which is the temporal half of the
Plan 9 design: exit and re-exec finds the pages still there.

## Error paths

A bad-magic Spoor extincts — it means a use-after-free reached here, and
continuing would corrupt a cross-Proc structure. Zero length or an overflowing
length returns NULL *without* consuming the Spoor, matching the Burrow
constructor's convention. Allocation failure returns NULL the same way. A full
table of live entries is not an error at all.

## Performance

Linear scans over a fixed 128-entry table, twice per miss. Trivial against the
page-ins it saves. The cap is sized at two entries per binary since rodata
joined text — roughly sixty-four binaries.

## Prosecution

On any change: that every path still clunks the Spoor exactly once — miss adopts,
hit clunks, race-loser frees-and-clunks; that the create stays outside the lock
*and* the re-search stays after it, since dropping either turns the create race
into a double registration; that eviction still selects only on
`handle_count == 1 && mapping_count == 0` and still detaches under the lock
before unreffing outside it; that the executability field stays in the key, in
both search passes and the install; and that the lock order to the Burrow lock
stays one-directional.

## Seams

- **No pressure-triggered reclaim.** The LRU cap is the only bound; a memory
  shortage cannot ask the cache to give pages back.
- **Bypass is invisible.** A cache full of live images silently stops sharing.
  There is a counter, and nothing reads it.
- **No `/ctl` surface** for the four diagnostic counters, which are maintained
  and only reachable from tests.

## Caveats

**The header says there is no production caller.** Its closing paragraph reads
*"At R-3 there is NO production caller: exec still slurps (R-4 wires
`image_lookup_or_create` in place of the eager whole-ELF read)"* — naming the
exact sub-chunk that would land the consumer. That sub-chunk landed;
[[sub-kernel-exec]] calls it, and `main` calls the initializer. A reader who
trusts the paragraph concludes this file is dead code. Task #64.

**Content-keyed deduplication is refused permanently.** Sharing here is by file
identity only. The cross-binary content scan that would be KSM is declined as an
ASLR-defeating side channel — a decision, not a gap, and the header is careful
to say so.

## Provenance

[[arc-revenant]] R-3 built the cache with no consumer; R-4 wired exec into it.
#45 widened it from text to every non-writable segment and doubled the cap; that
same change's audit added the executability field to the key.

## Tests

`image.*` drives it in isolation with synthetic Spoors: hit and miss, distinct
files, the version discriminator, distinct offsets within one file, the
executability split, table exhaustion, and the zero-length reject. The idle-evict
helper exists both for test isolation and as a real regression on the
detach-under-lock / unref-outside lifetime.

## Referenced by

[[moc-kernel-execution]] · [[inv-i36]] · [[inv-i7]] · [[sub-kernel-exec]] ·
[[sub-kernel-fault]] · [[sub-kernel-burrow]]
