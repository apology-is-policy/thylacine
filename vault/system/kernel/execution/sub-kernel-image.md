---
id: sub-kernel-image
type: sub
parent: moc-kernel-execution
title: "The Image cache — one copy of a binary's text, and a proof that eviction cannot race a mapper"
code: [kernel/image.c, kernel/include/thylacine/image.h]
audit: hard
guarded-by: [inv-i36, inv-i7, inv-i32]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/EXEC-LOAD-DESIGN.md"]
created: 2026-08-03
updated: 2026-09-23
---
## Purpose

Two Procs running the same binary should fault into the same physical text
pages. This is the registry that makes that true: a fixed table keyed on a
file's identity, holding one reference to a file-backed Burrow per cached
segment. The first exec creates; the second finds.

Since DISTRO D-3 the same registry also backs the phenotype file-backed `mmap`
arm, so a shared library mapped read-only into many Linux-phenotype Procs dedups
exactly as a shared `a.out`'s text does — the consumer generalised from exec to
any read-only file-backed map.

The heritage name is kept deliberately — this is the Plan 9 Image, rebuilt on
the dual-refcount Burrow lifecycle.

## Contract

Two production entry points since the round-2 close of B-1a' (2026-09-23):
the lookup, whose ownership rule is the whole of its contract, and
`image_cache_reclaim(want)`, the user pool's reclaim step (below).

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

**The backing-size stamp (#194).** `image_lookup_or_create` takes the
caller-sampled backing file size and stamps it on the fresh Burrow (`file_limit`)
before publication — the Burrow is private at that point, so no lock is needed.
The fault arm reads it to refuse a page wholly past `round_up(file_limit)` with
SIGBUS, closing the lying-ELF mint: a phdr claiming `filesz` beyond the real file
end used to demand-zero the difference against the uncharged FILE posture. The
failure policy is the caller's, and the two consumers differ — the guest-facing
`mmap` arm fail-closes on an unknown or hostile-near-2^64 size (`-EIO`) before
mapping, while exec passes `spoor_file_size` and tolerates
`BURROW_FILE_LIMIT_UNKNOWN`, sound only because the sole size-less backing Dev is
the baked, immutable ramfs where a lying ELF cannot be authored. On a cache HIT
the entry keeps its creation-time limit and the caller's value is ignored: one
sample per cached image, the close-to-open shape.

## Data structures

`struct image_entry` — the seven key scalars, the Burrow holding the cache's
single reference, and an LRU stamp. The table is BSS, so zero means empty and
there is no initialization allocation; `image_cache_init` only flips a flag and
extincts if called twice.

The key is sampled into the entry at install rather than read back through the
Spoor, which matters: the Burrow outlives any particular caller's handle on that
file, so the cache must not depend on being able to re-read it.

## Concurrency

One global lock, plain rather than IRQ-saving, justified by every entry being
process context: exec, the phenotype `mmap` arm, and since the round-2 close
the pool's reclaim, which runs in the allocating context -- a fault or an
attach, under that space's `as->lock`.

**Lock order is [`as->lock` →] cache lock → Burrow lock → the buddy**, never
the reverse -- the reclaim adds the two ends: it may be entered with an
address space's lock held and it frees pages under the Burrow lock's shadow
(outside it, leaf order) -- and the reason it
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
Since #194 the `file_limit` stamp keeps that posture honest: a page wholly past
the backing file's end faults to SIGBUS rather than minting an uncharged
demand-zero page, so the uncharged FILE charge is justified by real, shared file
bytes — the condition that always justified it.

[[inv-i7]] — the cache is a handle-count holder. Its reference is what keeps
text resident after the last Proc unmaps, which is the temporal half of the
Plan 9 design: exit and re-exec finds the pages still there -- and, since the
round-2 close, finds the ENTRY still there even when pressure has taken the
pages: the Image outlives its pages, as Plan 9's does.

[[inv-i32]] — the cache is the pool's reserve under pressure and its pages are
charged to their holders (the round-2 audit's F8, below): a cached file's pages
are pool memory, a mapping of them is the mapping space's charge, and an idle
image gives its pages back before a user allocation is refused.

## Error paths

A bad-magic Spoor extincts — it means a use-after-free reached here, and
continuing would corrupt a cross-Proc structure. Zero length or an overflowing
length returns NULL *without* consuming the Spoor, matching the Burrow
constructor's convention. Allocation failure returns NULL the same way. A full
table of live entries is not an error at all.

## Performance

Linear scans over a fixed 128-entry table, twice per miss. Trivial against the
page-ins it saves. The cap is sized at two entries per binary since rodata
joined text — roughly sixty-four binaries. Since D-3 the same table also holds
phenotype library maps, so under a Linux workload the 128 slots are shared
between exec text and mmap'd `.so` text and the effective binary count is lower.

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

- **Reclaim is per page since round 3, but still under the cache lock.** A
  pick strips only the pages still wanted (B-1a' audit F14: a 256 MiB idle
  image no longer costs one refused allocation 65536 takes), under
  `g_image_lock` and the allocator's `as->lock`; an LRU within an image
  (rather than lowest slot first) is a later refinement, not a hole.
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
trusts the paragraph concludes this file is dead code. Task #64. Since DISTRO
D-3 there are two production consumers, not one: exec and the phenotype
file-backed `mmap` arm in the syscall layer both call `image_lookup_or_create`.

**Content-keyed deduplication is refused permanently.** Sharing here is by file
identity only. The cross-binary content scan that would be KSM is declined as an
ASLR-defeating side channel — a decision, not a gap, and the header is careful
to say so.

## The cache is the pool's reserve under pressure (2026-09-23; B-1a' audit F8)

The round-2 audit of B-1a' prosecuted the cache against the physical user pool
([[sub-kernel-mm-phys]]): a FILE page-in takes a pool page and installs it
against no address space, so a toucher's text pages counted against no cap
(a confined Proc never saw its own text), and when the toucher died its
images stayed -- handle 1 (the cache's), mapping 0, pool-charged -- evicted
only when 120 more distinct images pushed them out of the 128-slot table.
128 slots by 256 MiB of `sys_mmap_file` windows is 32 GiB of admissible
hoard; with the pool physical, "eat all RAM" had become "hold the whole pool
after death, while free-able memory exists", every later user allocation
refused. Plan 9 answers it with `imagereclaim()` when the page pool runs low;
Linux with page-cache reclaim under pressure and memcg charging the page cache
to the toucher. Both halves are built.

**The reclaim.** `image_cache_init` registers `image_cache_reclaim` with the
allocator (`capacity_set_reclaim`); `alloc_user_pages` calls it with the pages
it needs whenever `pool_charge` refuses a NON-exempt charge, and asks the pool
again if it freed anything. Under `g_image_lock` the function picks, repeatedly,
the least recently used IDLE entry that still holds pages -- `handle_count ==
1`, `mapping_count == 0`, `burrow_image_resident_count > 0` -- and strips it
(`burrow_image_strip(b, want - freed)`, [[sub-kernel-burrow]]: resident
pages taken out of the map lowest slot first and freed, the nodes they empty
too, no COW put since FILE never shares, stopping at the pages still wanted
-- round 3's F14), until `want` pages are freed or no idle image holds a
page; an image stripped in part stays the least recently used and yields the
rest on the next pick. The entry STAYS cached, empty once every page has
gone: the next lookup hits it and its next fault pages the same bytes in
again from the pinned Spoor -- the Image outlives its pages.
Diagnostics: `g_image_reclaims` (calls that freed a page) and
`g_image_reclaimed_pages`.

**Why the strip is safe is the eviction proof, reused.** Idle is stable under
`g_image_lock`: to take a reference or add a mapping a Proc must first come
through `image_lookup_or_create`, which takes this lock, so an entry showing
exactly one handle reference and no mappings has no in-flight claimant and
cannot gain one while the lock is held. Two more facts make the pages
themselves unreachable: no PTE names them, because every mapping teardown
clears its leaves (and invalidates) before it releases the mapping reference,
and a dead space's stale translations cannot go live under its retired ASID;
and no fault is filling the map, because a filler holds a handle reference
(`file_demand_page_slow`'s pin) and so is not idle. What the reclaim may hold
when it runs -- the allocator's caller's `as->lock` -- sits ABOVE the cache
lock in the order; nothing takes the cache lock or a Burrow lock and then
allocates a user page (the lookup creates outside the lock; the pagemap
allocates its nodes outside `v->lock`), so the hook cannot deadlock against
its own callers. An exempt charge never refuses, so the TCB never reclaims.

**The holder charge.** Each FILE leaf a fault installs is charged to the
mapping space (`addrspace_charge_file`: `page_count` + a `file_pages`
telemetry; [[sub-kernel-fault]], [[sub-kernel-addrspace]]) and refunded per
leaf a range clear removes (`vma_uninstall_range_in`, [[sub-kernel-vma]]);
`/proc/<pid>/status` prints `file:` beside `tables:` ([[sub-kernel-devproc]]).
The page is the cache's, the mapping is the holder's -- the Linux RSS reading,
and the same shape this tree already used for a COW-shared page.

Witnesses: `demand_page.idle_image_reclaimed_under_pressure` (an idle image
with two resident pages; the pool parked full; one user allocation served by
one reclaim that freed exactly those two; the image empty and still the one
the next lookup returns) and `demand_page.file_pages_charge_the_holder`.

## Provenance

[[arc-revenant]] R-3 built the cache with no consumer; R-4 wired exec into it.
#45 widened it from text to every non-writable segment and doubled the cap; that
same change's audit added the executability field to the key. DISTRO D-3c
generalised the cache to the phenotype file-backed `mmap` arm and added the
`file_limit` stamp (#194), re-justifying the uncharged-FILE posture by real,
shared file bytes. The B-1a' round-2 close (2026-09-23) made the cache the
pool's reserve under pressure and charged its pages to their holders
([[chg-2026-09-23-b1a-prime-close-r2]]).

## Tests

`image.*` drives it in isolation with synthetic Spoors: hit and miss, distinct
files, the version discriminator, distinct offsets within one file, the
executability split, table exhaustion, and the zero-length reject. The idle-evict
helper exists both for test isolation and as a real regression on the
detach-under-lock / unref-outside lifetime.
`demand_page.idle_image_reclaimed_under_pressure` drives the reclaim end to
end (the pool parked full; one allocation; the image stripped and still
cached; the next lookup a hit) and `demand_page.file_pages_charge_the_holder`
the holder charge.

## Referenced by

[[moc-kernel-execution]] · [[inv-i36]] · [[inv-i7]] · [[inv-i32]] ·
[[sub-kernel-exec]] · [[sub-kernel-fault]] · [[sub-kernel-burrow]] ·
[[sub-kernel-mm-phys]]
