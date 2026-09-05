---
id: sub-kernel-addrspace
type: sub
title: "The shared address space and the copy-on-write break"
parent: moc-kernel-memory
code: ["kernel/addrspace.c", "kernel/include/thylacine/addrspace.h", "kernel/cow.c", "kernel/include/thylacine/cow.h"]
audit: hard
guarded-by: [inv-i44, inv-i32]
validated-by: [spec-cow, gate-smp]
locks: [lock-vma, lock-cow]
design: ["docs/LINEAGE.md", "docs/ARCHITECTURE.md"]
created: 2026-08-06
updated: 2026-08-06
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
| `addrspace_charge_*` / `uncharge_*` | the six I-32 counter operations |
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

`page_budget == 0` is refused rather than read as "unlimited", because an
uncapped address space is precisely the DoS hole [[inv-i32]] exists to
close.

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
faulting needs this lock. Zero window, and it is Linux's own structure.

Phase 3 is separate from phase 1 *despite both touching the parent*
because the two are asymmetric in recoverability: the uninstall is undone
by re-faulting, and `VMA_FLAG_COW` is never cleared. So a failed fork
leaves the parent unflagged and semantically intact — it pays some faults
and never learns a fork was attempted.

**What each VMA kind becomes**, and the one where writability decides:

| Kind | Becomes | Why |
|---|---|---|
| `ANON_LAZY` | **cloned**, both sides flagged COW | per-page ownership exists to break |
| `FILE` | **shared** | read-only by construction; sharing is the point of the I-36 Image cache |
| `ANON` read-only | **shared** | one indivisible buddy block, but nothing can ever write it |
| `ANON` writable | **refused** | no per-page ownership for a break to take |
| guard | reproduced as a guard | dropping it silently deletes the child's stack guard page |
| MMIO / DMA / SHARED_IN | **refused** | a device window is an *authority transfer*, not a copy — at any prot |

The read-only `ANON` arm is not a corner case, it is **every Proc**: the
vDSO clock page is a kernel-owned eager-anon page mapped read-only into
every address space, so without that arm no real fork clones at all and
the tests pass on synthetic spaces only.

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

`struct AddrSpace` — 56 bytes, asserted (a drift alarm, not an ABI).
`ref` / `lock` / `pgtable_root` / `context_id` / `vmas` / `page_count` /
`vma_count` / `shared_map_pages` / `page_budget` / `page_peak` / pad.

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
  would hide it.
- [[inv-i31]] — by construction: one address space, one `context_id`.
- [[inv-i12]] — the read-only `ANON` share is only sound because
  read-only is *permanent*; there is no prot-mutation syscall.
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

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
