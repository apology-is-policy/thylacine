---
id: sub-kernel-addrspace
type: sub
title: "The shared address space and the copy-on-write break"
parent: moc-kernel-memory
code: ["kernel/addrspace.c", "kernel/include/thylacine/addrspace.h", "kernel/cow.c", "kernel/include/thylacine/cow.h"]
audit: hard
guarded-by: [inv-i44, inv-i32]
validated-by: [spec-cow, spec-capacity, gate-smp]
locks: [lock-vma, lock-cow]
design: ["docs/LINEAGE.md", "docs/ARCHITECTURE.md"]
created: 2026-08-06
updated: 2026-09-23
---
## Purpose

"Two Procs, one address space" was **unrepresentable** until L-1, because
the address space was six inline fields of `struct Proc`. Both
`rfork(RFPROC|RFMEM)` and copy-on-write `fork` need it, and they block on
the *same* extraction — which is why `struct AddrSpace` is the arc's stage
zero rather than a COW implementation detail.

This dossier owns that object's lifecycle, the clone that makes a fork,
and the per-page share count the break decides against. What lives *in*
the address space — the VMA list's own operations, the fault dispatch, the
Burrow — belongs to [[sub-kernel-vma]], [[sub-kernel-fault]] and
[[sub-kernel-burrow]].

The membership test for the struct is one question: **does this describe
the TRANSLATION, or the PROCESS?** The page table, the ASID, the VMA list,
the lock, and the three I-32 counters describe the translation. Identity,
handles, territory, notes and the process tree describe the process and
stay on `Proc`.

## Contract

| Entry | Effect |
|---|---|
| `addrspace_alloc(page_budget)` | fresh L0 table, `ref = 1`; **NULL on a 0 budget** |
| `addrspace_ref(as)` | +1; extincts on NULL or on a dead object |
| `addrspace_unref(as)` | -1; the last drop **drains the VMA list**, destroys the table, frees |
| `addrspace_clone(src, exempt)` | the COW fork; a fresh space, or NULL having freed everything |
| `addrspace_ref_count(as)` | for "am I the last holder" only — **not a lock** |
| `addrspace_charge_*` / `uncharge_*` | the six I-32 counter operations, per address space only: the machine-wide bound is physical and lives with the allocator ([[sub-kernel-mm-phys]]; B-1a' round-1 close) |
| `addrspace_charge_table(as, exempt)` / `addrspace_uncharge_table(as)` | one hardware page table charged to the space (`page_count` + the `pgtable_pages` telemetry) or returned; the MMU calls them around `alloc_user_pages` / `free_pages` ([[sub-kernel-mmu]]; B-1a' audit F1) |
| `addrspace_charge_file(as, exempt)` / `addrspace_uncharge_file(as, n)` | one mapped FILE page charged to the space (`page_count` + the `file_pages` telemetry) per leaf the fault installs; `n` returned per leaf a range clear removes ([[sub-kernel-fault]], [[sub-kernel-vma]]; B-1a' audit F8) |
| `cow_page_set_sole(pg)` | establish `cow_share = 1` — **overwrites** |
| `cow_page_get/put(pg)` | +1 / -1; `put` returns "you were the last, you own the free" |
| `cow_page_break_is_sole(pg)` | the break's decide, as one atomic step |

`as == NULL` **means kernel-only.** kproc is built by a direct zeroing
alloc and never calls `addrspace_alloc`, so its `as` stays NULL for free —
exactly the equivalence the old `pgtable_root == 0` test encoded, now held
in one pointer. A NULL deref on a kernel-only path is deliberately a
**loud** failure, the opposite of the silent-miss class where a
half-converted predicate quietly treats a live user address space as a
kernel Proc.

**The conversion stopped at the mmu API on purpose.** `mmu_install_user_pte`
and its family keep taking a bare `paddr_t pgtable_root`, not an
`AddrSpace *` — that layer has no business knowing what a Proc is, and its own
`pgtable_root == 0` rejects are *parameter validation*, not the kernel-Proc
test. So the `pgtable_root == 0` sites split by what they actually meant: the
ones encoding the kernel-Proc question became `as == NULL`; the ones that were
parameter checks stayed as they were. A reader who "finishes" the conversion by
threading `AddrSpace` down into the mmu layer would be converting checks that
were never about a Proc.

`page_budget == 0` is refused rather than read as "unlimited", because an
uncapped address space is precisely the DoS hole [[inv-i32]] exists to
close.

**The user pool is physical, and lives with the allocator** (B-1a' round-1
close, 2026-09-23; ARCH 6.5 "Capacity, and the I-32 default";
[[sub-kernel-mm-phys]] "The user pool"). As first built the pool was a counter
above `page_count`, charged by `addrspace_charge_pages` before the space's own
cap -- and that shape refused a fork of a Proc holding more than half the
pool's room while free memory existed (the round-1 audit's F5: a COW-shared
page charged to both sharers is one physical page), and had no return path
for the hardware page tables the audit's F1 made chargeable. So the pool
moved: `alloc_user_pages(order, flags, exempt)` in `mm/phys.c` charges it at
ALLOCATION and tags the head page `PG_USER`, and `free_pages` -- the one place
a page can leave -- returns the charge, whoever frees it. This file's counters
keep the HOLDER reading under the space's cap: `addrspace_charge_pages` and
`_uncharge_pages` are per-space only (the CAS loops and the clamp unchanged), a
COW-shared page is charged to both sharers and a fork is bounded by the cap,
which is the Linux memcg shape; and `addrspace_charge_table` /
`addrspace_uncharge_table` are the same pair for a page table, with
`pgtable_pages` beside `page_count` as telemetry, and `addrspace_charge_file`
/ `_uncharge_file` the same pair for a FILE page this space MAPS (the page is
the Image cache's; the mapping is the holder's -- charged per leaf installed,
refunded per leaf cleared; `file_pages`; the round-2 audit's F8)
(`/proc/<pid>/status` `tables:` and `file:`; the capacity probe's data census
is `pages:` minus both). The
address space carries a `u64 id` from a global counter at `addrspace_alloc`
-- never reused, unlike a pid -- because the eager charge record
([[sub-kernel-burrow]]) must name the space that PAID: a non-CLOEXEC Loom that
outlives its exec would otherwise refund against the successor's space (the
round-1 audit's F4).

**A dying address space returns nothing to the pool, because its frees
already do.** WIP 3 of the chunk had `addrspace_unref` read `page_count` after
the drain and uncharge it from the counter-shaped pool: `vma_drain_in` frees
Proc-agnostically (`burrow_free_internal` refunds nothing) and the count used
to die with the space, which was fine while it was the only bound and a leak
of every dead Proc's RSS once a machine-wide bound sat above it
(`capacity.death_returns_charges_to_pool` found it in the chunk's first
build). With the pool physical that return would DOUBLE-return what the
drain's `free_pages` calls return page by page, so it is gone; the test still
stands, now asserting the pool exact after the death, tables included, and
`proc_pgtable_destroy` frees the tables the drain's reclaim left (the L0, and
any table under a leaf the drain did not clear) through the same `free_pages`
([[sub-kernel-mmu]]). The counter dies with the space, as it always did; the
model's one address space never dies ([[spec-capacity]]), and now there is
nothing for it to miss.

## Mechanism

**The refcount was atomic from the first commit, when nothing shared.** An
`int` that is "always 1 today" and becomes contended two chunks later is
the latent-P1 shape, and the cost of getting it right immediately was one
line.

**The drain moved to the last reference (L-3), and that is a fix rather
than a tidy-up.** It used to run in `proc_free` and in
`proc_exec_replace`, which was correct only while `ref` could never exceed
1: draining at *a death* and draining at *the last reference* are the same
event exactly when there is one reference. Under RFMEM they separate, and
the old placement would have had the first sharer to die free a VMA list
the survivor was still translating through. Both callers existed — the
second reached by a vfork child execing, which is the `posix_spawn` shape
exactly — so this is one fix at the right layer rather than a gate
repeated at each site.

**No TLB flush at teardown**, the Linux model. What makes it sound is the
**ASID tag**, not any earlier invalidation: every user PTE is non-global,
so a stale entry is reachable only under this address space's own ASID,
and the rollover's per-CPU flush runs before that value can go live again
([[sub-kernel-asid]]). Each caller separately owes only "no CPU translates
under this ASID *now*" — `proc_free` by having reaped and `on_cpu`-spun
every thread, `proc_exec_replace` by writing the new TTBR0 (a *different*
ASID) and `isb`-ing first. `vma_drain` issues **no TLBI at all**, measured
at L-2; an earlier claim that it did was fiction.

**The clone is three phases, and the order is the whole safety argument.**

1. **Uninstall the parent's own writable PTEs** for every COW range,
   *before* anything is shared.
2. **Build the child** — the only phase that can fail.
3. **Flag the parent**, on success only.

Holding `src->lock` is *not* a substitute for phase 1 running first. The
lock only reaches a peer that **faults**; a peer holding an
already-installed writable PTE stores in hardware — no fault, no kernel
entry, no lock — so with the uninstall after the snapshot there is a
window, lasting the rest of the clone, in which the child already holds a
share of a page the parent can still write, silently. Uninstalling first
closes it by construction: once the PTE is gone the peer *must* fault, and
faulting needs this lock. Zero window, and it is Linux's own structure. The
clear KEEPS the parent's tables (`mmu_uninstall_user_range_keep_tables`, the
round-2 close): every leaf it takes the parent re-faults, so the emptied
tables stay linked -- charged, at occupancy 0 -- for the re-installs to find,
as Linux keeps them across fork; reclaiming them made every break free and
rebuild the path, and at the pool's edge could refuse a page the parent
already held ([[sub-kernel-mmu]]).

Phase 3 is separate from phase 1 *despite both touching the parent*
because the two are asymmetric in recoverability: the uninstall is undone
by re-faulting, and `VMA_FLAG_COW` is never cleared. So a failed fork
leaves the parent unflagged and semantically intact — it pays some faults
and never learns a fork was attempted.

**What each VMA kind becomes**, and the one where writability decides:

| Kind | Becomes | Why |
|---|---|---|
| `ANON_LAZY` | **cloned ONCE per source Burrow**, both sides flagged COW | per-page ownership exists to break; the pieces a protect left all map the one clone (the cursor, below) |
| `FILE` | **shared** | read-only by construction; sharing is the point of the I-36 Image cache |
| `ANON` with a read-only CEILING | **shared** | one indivisible buddy block that nothing can EVER write -- the ceiling, not the current prot (B-1a) |
| `ANON` with WRITE in its ceiling | **refused** | no per-page ownership for a break to take, and a raise could make it writable again |
| guard | reproduced as a guard | dropping it silently deletes the child's stack guard page |
| MMIO / DMA / SHARED_IN | **refused** | a device window is an *authority transfer*, not a copy — at any prot |

The read-only `ANON` arm is not a corner case, it is **every Proc**: the
vDSO clock page is a kernel-owned eager-anon page mapped read-only into
every address space, so without that arm no real fork clones at all and
the tests pass on synthetic spaces only.

Since B-1a the test is on `vma_prot_max`, never on `prot`. `SYS_BURROW_PROTECT`
means "read-only now" is no longer "read-only forever": a mapping minted RW
and protected down to R can be raised back by either side, and a share of it
would be one address space's writes landing in another's -- the [[inv-i44]]
violation, and the second of the three findings the protect made live. The
vDSO's ceiling is R (a mint's ceiling is its mint prot), so it still shares;
an eager attach protected down to R is refused
(`cow.clone_refuses_eager_anon_with_writable_ceiling`). The child's VMA copies
the parent's ceiling bits -- `nv->flags = flags | (src_vma->flags &
VMA_FLAG_MAX_MASK)` -- so a child can raise exactly what its parent could and
nothing more.

**One clone per source Burrow, not per VMA** (the third finding). The
per-address-space clone above walks the WHOLE source, taking a share on every
resident page. Until B-1a one lazy Burrow had one VMA, so per-VMA and
per-Burrow were the same count; a protect (or a D-3b window) splits a lazy
mapping into pieces that all name one Burrow, and a clone per piece leaves
the child holding k shares of every page while being one holder -- the child
charged k x the resident count, the parent never able to take a page in place
until the child died. That is `cow.tla::BUGGY_CLONE_PER_PIECE`, where
`ShareIsHolderCount` fails in the INITIAL state, because the fork itself is
the bug. `clone_one_vma` dedupes through `struct Burrow.clone_cursor`: the
first piece mints the clone and parks it on the source; later pieces map that
clone, taking only a mapping ref (the resident charge rides the mint). Every
cursor the clone set is cleared in a sweep after phase 2, on every outcome,
while `src->lock` still excludes the next clone of this space -- and on the
two failure paths inside `clone_one_vma` before the minted clone is unreffed
-- so a failed child's drain can never leave a cursor naming a freed Burrow.
`cow.clone_dedupes_split_pieces` counts it: every page has exactly TWO
holders, and the child is charged the resident count once. (B-1a': the clone's
slot table is a `pagemap_mirror` over a pool the source's node count sizes,
allocated uncharged by `burrow_clone_cow`; `clone_one_vma` charges
`burrow_lazy_footprint` -- the resident pages PLUS the mirrored nodes -- so the
child pays for exactly what its own takes will refund. It charged the resident
count alone at first; the chunk's self-audit caught it, and
`capacity.fork_clone_charges_pages_and_nodes` is the witness. Seams, below.) The holotype
audit's F3 (P3): the `vma_alloc` failure arm cleared the cursor through the
`backing` local, which the lazy arm had REBOUND to the clone -- so it wrote
the clone's own (already NULL) slot and freed the clone while the source still
named it; the sweep overwrote the dangling pointer before anything could read
it, which is why nothing failed. It now clears the source's, as the insert
failure arm two lines below always did.

**The COW count lives on the page, not the slot**, and the reasoning is
worth keeping because a slot-indexed count *almost* works. A break makes
my slot and the page the count describes diverge, so a later fork bumps an
entry covering two different pages. Take-in-place survives that (the entry
is the sum over the groups sharing it, so `== 1` still implies a sole
holder) — but the **free decision corrupts**: one group drives the entry
to zero and frees its page, leaving another group's page at zero to leak
or underflow. Freeing a shared page requires knowing how many holders
remain, and that is a fact about the *page*. A fork therefore clones the
Burrow per address space — same size, its own slots, the same page
pointers — which is Plan 9's `dupseg`.

**The break's decide is one step, and the copy path deliberately does not
drop.** Sole holder → take in place, no copy, no count change (leaving the
count at 1 is what stops anything freeing the page underneath). Otherwise
→ pin, copy, *then* drop. The retained share **is** the pin: the page
cannot reach zero holders while a breaker is still reading it. Both halves
have a buggy cfg in [[spec-cow]].

## Data structures

`struct AddrSpace` — 72 bytes, asserted (a drift alarm, not an ABI).
`ref` / `lock` / `pgtable_root` / `context_id` / `vmas` / `page_count` /
`vma_count` / `shared_map_pages` / `page_budget` / `page_peak` /
`pgtable_pages` (B-1a' audit F1: the hardware tables inside `page_count`,
telemetry) / `file_pages` (audit F8: the mapped FILE pages inside it,
telemetry) / `id` (a u64 from a global counter, never reused; the eager
charge record's key, audit F4).

`context_id` lives here because **the ASID names a translation table**,
which is what the allocator always semantically meant. Two Procs sharing
an address space share one ASID: correct, and cheaper than two. The
inverse — two tables, one ASID — is the [[inv-i31]] corruption the ASID
arc exists to prevent, and holding the field here makes it *structurally
unrepresentable*.

`page_budget` is the enforced cap and sits **beside the count it bounds**,
which is I-32 shape (A). Sharing an address space means sharing the pages,
so it must mean sharing the cap; a low-budget Proc that RFMEMs into a
high-budget space acquires the high cap, and that is **not** an escalation
because RFMEM already means the two can write each other's memory — they
are mutually trusting by construction. The rejected alternative (count
here, cap on the Proc) would let two RFMEM siblings return *different*
verdicts on one shared counter, making the effective bound depend on which
sibling faulted. **A resource bound whose value depends on scheduling is
not a bound.**

`page_peak` is pure telemetry and lives here rather than on `Proc` because
it mirrors `page_count` — a high-water mark separated from the counter it
tracks is how a merge nearly shipped a silently-reverted CL-5.

`page.cow_share` — the per-page count, taking a previously-free pad word,
so `sizeof(struct page)` is unchanged. Its contract is stated at both
declaration sites: **meaningful only while the page sits in an anon
Burrow's slot, and ESTABLISHED, NEVER INHERITED.** A page recycled through
the buddy carries whatever its last owner left, so every site that puts
one into such a slot calls `cow_page_set_sole` — the closed set is the
lazy populate, the demand-zero fault install, and the break's private
page.

## Concurrency

[[lock-vma]] (`AddrSpace.lock`) serializes every `vmas` mutation, the
demand-page reader, and the clone's whole three-phase walk. Order is
unchanged by the extraction: `as->lock -> burrow v->lock -> buddy
zone->lock`.

[[lock-cow]] is a **global leaf**, and being global is the point: two
sharers of one page hold *different* Burrow locks, so no per-Burrow lock
could serialise the decide. Held across the decide only — never across the
copy or the allocation. Plan 9 serialises `Page.ref` under the global
`palloc.lock` for the same reason.

**The child's counters are guarded by unreachability, not by its lock.**
`dst` is unpublished for the whole clone: no Proc points at it, so no
other CPU can reach it to charge, fault, or drain. The page charge a few
lines down *does* take `dst->lock`, redundantly and deliberately — it
costs an uncontended acquire on a lock nobody can hold, and it keeps every
charge call site uniform, so the rule stays "these run under `as->lock`"
with exactly one documented exception rather than two shapes to remember.
**Anything that publishes `dst` earlier invalidates this**, and
`vma_insert_in` would then need a lock supplied (it does not take one; it
*requires* one).

**The six counter operations are CAS loops and are correct with no lock
held.** They were plain load-decide-store pairs until the uncharge moved
to where the pages actually *free*: a Loom's ring pages are released by
`loom_free`, reached from `handle_close`/`handle_table_free`, which hold
no address-space lock — so a sibling thread's `SYS_BURROW_ATTACH` can
interleave between the load and the store and lose one of the two updates.
A multi-threaded Proc closing a Loom while another thread attaches is the
ordinary Go shape, not a corner. L-1 made it worse, not better: the
counters now live on the *shared* AddrSpace, so under RFMEM the
interleaving is between **Procs**, not merely between threads of one.

A lost update announces itself in neither direction. A lost charge
deflates the counter and the space escapes its cap; a lost uncharge
inflates it and the space can never allocate again, permanently, with no
error anywhere naming the cause.

Two properties every loop must keep: **re-decide, don't re-store** (a
failed CAS writes the observed value into `cur`, and the next iteration
must re-run the *cap decision* against it — a loop that retries only the
store lets a raced charge land over cap), and **recompute the uncharge
clamp inside the loop** (hoisting it lets the CAS succeed writing a value
derived from a stale read — the same lost update wearing a CAS).

**What the CAS does not buy, deliberately: the cap decision may still
overshoot.** Two concurrent charges from outside the lock can both pass
and both land, bounded by the smaller. That is the documented I-32
tolerance — *a floor, not an exact accountant* — and reaching for a lock
to make it exact is what produced the stale comments this dossier's
Caveats record. **No update is ever lost**, which is the property an
accounting bound cannot do without; exactness is not.

## Invariants enforced

- [[inv-i44]] — a fork's address spaces diverge on the first write. The
  parent's PTE uninstall closes the aliasing window; the single-step
  decide stops two breakers both taking the page in place; the retained
  share stops a concurrent exit freeing it mid-copy.
- [[inv-i32]] — the three resource axes, per address space rather than per
  Proc, with the page cap beside its count. The uncharges clamp at 0
  rather than wrapping: every uncharge pairs with a charge, so a wrap
  means the pairing is already broken and a silent 4-billion-page counter
  would hide it. B-1a' added the machine-wide half, and its round-1 close made it
  physical: the user pool is charged where a page is ALLOCATED and returned
  where it is freed ([[sub-kernel-mm-phys]]), exempt allocations counted but
  never refused, so this file's counters are per-space holder counts under a
  cap that IS the pool by default (`proc_default_page_budget`,
  [[sub-kernel-proc]]) -- the cap a parent's narrowing, the pool the everyday
  bound -- and the hardware page tables and the mapped file pages are inside
  them (`addrspace_charge_table`, `addrspace_charge_file`;
  `demand_page.file_pages_charge_the_holder`, `capacity.pool_refuses_users_keeps_tcb`,
  `capacity.page_tables_charged_and_reclaimed`,
  `capacity.fork_costs_the_pool_only_its_nodes`,
  `capacity.death_returns_charges_to_pool`).
- [[inv-i31]] — by construction: one address space, one `context_id`.
- [[inv-i12]] / [[inv-i44]] — the eager-`ANON` share is sound only because
  the CEILING is permanent. `SYS_BURROW_PROTECT` exists since B-1a, so
  "read-only now" no longer means "read-only forever"; the share keys on
  `vma_prot_max`, which nothing raises. EXEC is never a protect target, so a
  shared read-only mapping cannot become executable through a raise either.
- [[inv-i44]], second half (B-1a) — one clone per source Burrow, so the
  per-page share count equals the number of address spaces holding the page
  (`ShareIsHolderCount`); the cursor is cleared before the source lock drops on
  every outcome.
- [[inv-i5]] and [[inv-i34]] — MMIO and DMA VMAs are refused by the fork
  at any prot, so a child never inherits a second mapping of a device
  window.

## Error paths

Allocation failures return NULL having freed what they got, and callers
roll back exactly as they did for the old page-table create. The clone
discards a partial child **wholesale**: the last unref drains its VMA
list, which drops every mapping ref, which frees every Burrow the call
cloned, which puts back every COW share it took — which is why every
failure inside simply returns `false` with no bespoke unwind.

Charging failures are `-ENOMEM` to the caller. Resident shared pages are
charged to the child *as well as* the parent — each address space maps
them, so each counts them, the Linux RSS reading. That over-counts
physical memory between the fork and the break, **in the safe direction**:
the fork fails up front rather than the break OOMing later, when there is
nowhere good to put the failure.

The COW layer **extincts rather than guesses**: a zero count in `get`,
`put` or `break_is_sole` means some site put a page into an anon slot
without establishing it, and continuing would compute a free decision from
a previous owner's value. `cow_page_get`'s overflow check is a drift alarm
— it needs 2^32 address spaces sharing one page, which `PROC_CHILD_MAX`
and the I-32 axes bound far below.

## Performance

`alloc`/`ref`/`unref` are O(1) plus a page-table create/destroy. The clone
is **three walks of the parent's VMA list** under one lock hold — two full
passes plus the per-VMA build — so its cost is linear in region count, and
it runs with interrupts off. A `fork` in an address space with tens of
thousands of regions is therefore a long interrupts-off hold; nothing
bounds it today beyond `PROC_VMA_MAX`, and that is recorded as a seam
rather than measured.

The counter CAS loops are uncontended in the common case. The COW lock is
held for a single compare; contention exists only between concurrent
breaks.

## Prosecution

What a change must re-establish:

- **the clone's phase order** — uninstall before share, flag after
  success. Reordering either is silent cross-address-space corruption;
- **that the two passes agree on `vma_is_cow`.** Pass 1 uninstalls and
  pass 3 flags; a predicate that drifts between them leaves a range
  flagged but not uninstalled, or the reverse. The test is written once
  for exactly this reason;
- **the unpublished-`dst` argument**, if anything publishes the child
  earlier;
- **the CAS loops' two properties** (re-decide, and the clamp inside);
- **the ESTABLISHED-NEVER-INHERITED contract** — a new site that puts a
  page into an anon slot without `cow_page_set_sole` extincts at the next
  break, which is the intended loud failure, but the fix belongs at the
  new site;
- **the drop-after-copy ordering.** Dropping first and taking no pin is
  `BUGGY_TEARDOWN_NO_PIN` and is a use-after-free.
- **that the pool is decided at the allocation, never here** (B-1a' round-1
  close). A user page minted by a bare `alloc_pages` instead of
  `alloc_user_pages`, or a `PG_USER` page freed at another order than it was
  allocated, drifts the machine-wide count in the loosening or the tightening
  direction respectively; both are silent ([[sub-kernel-mm-phys]]
  Prosecution). This file's counters bound one space; they never touch the
  pool.
- **that no death return comes back.** `addrspace_unref` returns nothing to
  the pool; the drain's `free_pages` calls do. A return added here would
  double-return (the pool drifting below what is held, the loosening
  direction) -- `capacity.death_returns_charges_to_pool` asserts the pool
  exact after a death, tables included.
- **that the table pair stays symmetric with the MMU.**
  `addrspace_charge_table` runs before `alloc_user_pages` and
  `addrspace_uncharge_table` after `free_pages` ([[sub-kernel-mmu]]
  `user_table_alloc` / `user_table_free`); a table freed by any other path
  (`proc_pgtable_destroy` at death) returns its pool charge through
  `free_pages` and leaves `page_count` to die with the space, which is what
  every other page does.
- **that `id` is never reused and never 0.** The charge record's claim
  compares it; a recycled id would let a successor space claim a predecessor's
  refund -- the pid bug (audit F4) in a new coat.

## Seams

- **The clone's unbounded walk.** Three passes under one interrupts-off
  hold, linear in VMA count. The [[lock-vma]] discipline requires
  consumers to bound their own walks, and this one does not.
- **A per-page hashed lock array** for [[lock-cow]], recorded and
  deliberately not taken now: contention is only between concurrent
  breaks, each holding the lock for a single compare.
- **Writable eager-anon cannot be forked.** The refusal is correct rather
  than provisional — one indivisible buddy block has no per-page ownership
  — but it is a real reach limit, and the fix is to make eager anon
  page-granular rather than to weaken the check.
- **The fork clone's nodes were uncharged** (B-1a'; found by the chunk's
  self-audit and, independently, by the documentation pass; FIXED in the
  chunk). `burrow_clone_cow`'s node pool is allocated uncharged on the
  promise that `clone_one_vma` charges the footprint; it charged the resident
  count, and the child's later takes refunded the nodes anyway, so its
  `page_count` and the pool dropped below what is held -- the loosening
  direction, by the node count of each touched lazy Burrow per fork. One call
  (`burrow_lazy_footprint` for `burrow_lazy_resident_count`) closed it;
  `capacity.fork_clone_charges_pages_and_nodes` counts the nodes across a
  fork and its RED (`noclonefootprint`) is the bug itself
  ([[sub-kernel-pagemap]]).
- **The pool's counters are u32 pages** (16 TiB), clamped at
  `capacity_init`; a machine above that reads a smaller pool than it has.

## Caveats

- **The header states a lock precondition the implementation explicitly
  documents as corrected — three times, and the correction sits three
  lines below one of them.** `addrspace.h` says the lock "guards the three
  I-32 counters below so they stay EXACT", that all three "are
  charged/uncharged under `lock` so they are exact", and — at the
  declaration of the six operations — "**PRECONDITION: caller holds
  as->lock**, which is what makes each cap EXACT". `addrspace.c` opens the
  same six functions with "All six are CAS loops, so they are correct with
  **no lock held**", names the live call site that holds none, and states
  the resulting property is a floor. `burrow.c` repeats the header's
  version at its own call site, and `proc.c` carries **both**: a block
  comment saying the precondition "is unchanged and still belongs to the
  caller", immediately above a function whose own comment says
  charge/uncharge "cannot assume a lock". The declaration site is where a
  new charge site's author and an I-32 auditor both read, and the drift
  makes the review criterion wrong in *both* directions — a lock-free site
  looks like a violation, and the overshoot the implementation documents
  looks impossible. Tracked as task #165.
- **`proc_page_charge` — the wrapper the header calls "what ordinary code
  calls" — takes no lock.** So most real charge sites do not satisfy the
  stated precondition, correctly.
- **The three counter axes are not three per-space caps.** `page_count` is
  bounded by the per-space `page_budget`; `vma_count` and
  `shared_map_pages` are bounded by the *global* `PROC_VMA_MAX` and
  `PROC_SHARED_MAP_MAX_PAGES` constants. Only the page axis got shape (A),
  so a reader who generalises "the cap lives beside the count" to all
  three will not find the other two.
- **`addrspace_ref_count` is answerable only by a caller that can argue no
  new reference can appear.** A dying Proc can argue that; a live one
  cannot. Using it to decide whether a concurrent sharer may appear is a
  race, and the header says so.
- **The `FILE`-writable refusal is fail-closed on a shape that cannot
  occur.** REVENANT's dispatch gate admits only non-writable segments, so
  the check guards against a future loader rather than a present path —
  a different guarantee from one this file enforces today.
- **Do not rename the `AddrSpace` fields by grep.** Two of the seven names
  are overloaded elsewhere in the tree — `page_count` is also a `struct Burrow`
  field and `context_id` is also a `psci_cpu_on` parameter — so a blind rename
  hits unrelated code. The L-1 field-move was done compiler-driven instead:
  delete the fields from `struct Proc` first and let every now-broken reference
  surface (measured at the time as 239 sites, all genuinely on `struct Proc`,
  zero false positives).

## Tests

`kernel/test/test_addrspace.c` — `addrspace.alloc_shape` (a fresh space has
`ref == 1` and an empty VMA list), `addrspace.refcount` (ref/unref arithmetic;
the last drop frees), `addrspace.kproc_has_none` (kproc's `as` stays NULL),
`addrspace.charge_helpers_refuse_without_as` (the six I-32 operations fail
closed on a kernel-only Proc), `addrspace.proc_alloc_in_shares` (an
`RFPROC|RFMEM` child shares the parent's space), and
`addrspace.share_drains_at_last_ref` (the VMA drain runs at the last unref, not
the first death — the L-3 fix). The COW break's two arms are pinned by
[[spec-cow]]'s buggy cfgs and exercised at runtime through the fork path.

B-1a (`kernel/test/test_protect.c`, [[sub-kernel-protect-witness]]):
`cow.clone_dedupes_split_pieces` (a Burrow split into pieces is cloned ONCE
per fork; every page has exactly two holders; the child is charged once),
`cow.clone_refuses_eager_anon_with_writable_ceiling` (an eager mapping
protected down to R is NOT shared; one whose ceiling is R is), and
`protect.cow_split_then_break` (a protect on a forked mapping cuts it soundly:
the child's break on a page in one piece copies, the parent's page is
untouched). The `nodedupe` sabotage -- a clone per VMA again -- fails exactly
"each page has exactly TWO holders, not one per piece". The three new
counterexample cfgs are [[spec-cow]]'s.

B-1a' (`kernel/test/test_capacity.c`, [[sub-kernel-pagemap]]):
`capacity.default_is_ram_minus_reserve` (the reserve and pool formulas; the
default budget and the hard maximum are the pool; a fresh Proc and its space
carry it), `capacity.pool_refuses_users_keeps_tcb` (with the pool parked at K
pages a user Proc far below its own budget is refused at exactly K, the TCB is
not, every charge returns), `capacity.death_returns_charges_to_pool` (a Proc
dying with pages, nodes and an eager region returns all of it) and
`capacity.replace_window_releases_orphans` (through `burrow_map_fixed_in`).
`test_resource.c`'s cap tests run on a NARROWED budget (`proc_alloc_in(NULL,
4096)`) because the default is now a figure every live space draws on, and its
spawn-resolve test exercises the raise from a narrowed parent -- the only
parent a raise can matter to when the default is already the maximum.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
