---
id: spec-cow
type: spec
title: "cow.tla"
models: [sub-kernel-addrspace]
pins: [inv-i44]
cfgs:
  - "cow.cfg -- clean, three sharers: Safety (TypeOk + NoAliasedWritable + NoUseAfterFree + NoDoubleFree) and the EventuallyReleased liveness"
  - "cow_buggy_break.cfg -- the drop/decide/act sequence is not atomic: NoAliasedWritable"
  - "cow_buggy_teardown.cfg -- the share is dropped before the copy and no pin is taken: NoUseAfterFree"
  - "cow_buggy_vfork.cfg -- the vfork parent observes outside the lock and parks after: EventuallyReleased (Safety still holds)"
gate: "any change to kernel/cow.c, to addrspace_clone's phase order, or to the vfork suspend/release path"
created: 2026-08-06
updated: 2026-08-06
---
## Abstraction

Written **model-first** — TLC-green before the L-4 implementation — under
the spec-first re-enablement, on the same argument that re-enabled
[[spec-asid]] and [[spec-death-wake]]: a break racing a concurrent break,
a sharer's exit, and the vfork release is exactly the subtle SMP class
machine-checked exploration catches and tests do not.

One shared anonymous page reached by N sharers — the address spaces a fork
produced — each mapping it read-only. A write faults and the fault arm
breaks the share.

**Atomicity is modeled by step granularity**, the standard TLA+ idiom: a
sequence performed under one lock hold is ONE action, and the buggy
variants split it so a peer can interleave. An explicit mutex variable
would add state without adding reachable interleavings, because every
critical section in this protocol is straight-line.

**Deliberately beneath the model:**

- **the intra-address-space install-once race** — two *threads* of *one*
  address space faulting one page. That is the already-audited REVENANT /
  lazy-arm shape (loser frees, winner installs) and sits below an
  abstraction that treats a sharer as one agent per address space. L-4b
  reuses that arm rather than inventing a second one;
- **the page table.** "Installed writable" is a program counter, not a
  PTE, so whether `mmu_uninstall_user_range` covers the right range with
  the right TLBI is answered in prose and in [[sub-kernel-mmu]];
- **the Burrow clone itself.** `Sharers` are the cloned Burrows and
  `share` is their common page's `cow_share`; the cloning is assumed to
  have happened correctly;
- **allocation failure.** The copy path always gets its private page. OOM
  during a break is a real path and is handled by prose plus the
  wholesale-discard structure, not here;
- **more than one page.** The model is one page's protocol, so nothing
  about ordering *across* pages — including the clone's walk — is
  covered.

The `ASSUME` restricting the flags to at most one enabled carries its own
lesson in the source: it is counted **arithmetically**, not as the
cardinality of a set of the three flags, because a set collapses
duplicates — `{TRUE, TRUE, FALSE}` has cardinality 2 and would pass a
`<= 1` test with two bugs enabled. The flags are values, not identities.

## Action-site map

| Action | Site |
|---|---|
| `Fault(s)` | the write-fault arm of `userland_demand_page` reaching a `VMA_FLAG_COW` region |
| `DecideLocked(s)` | `cow_page_break_is_sole` — the whole decide under [[lock-cow]] as one step; `share = 1` -> take in place, else pin and copy |
| `BreakFinish(s)` | the break's tail: install the private page, then `cow_page_put` — **the drop happens here, after the copy** |
| `Exit(s)` | `addrspace_unref`'s last drop -> `vma_drain_in` -> the Burrow's slot release -> `cow_page_put` |
| `FreePristine` | `cow_page_put` returning true, and the caller freeing **outside** the lock |
| `VChildRelease` | the vfork child's exec or exit releasing the shared address space |
| `VParentCheck` | the L-3c-2 suspend: check-and-park in one atomic step |
| `DropUnlocked` / `LookUnlocked` / `VParentParkLate` | **no site** — they exist only under a buggy flag |

| Invariant | Obligation |
|---|---|
| `NoAliasedWritable` | [[inv-i44]]'s core: at most one sharer holds the pristine page writable. A `private` sharer is writable only through its own copy, which no one else can reach |
| `NoUseAfterFree` | nothing still references the pristine page after it is freed — the break-vs-teardown race |
| `NoDoubleFree` | the page returns to the buddy at most once |
| `EventuallyReleased` | the vfork parent is always resumed (L-3c-2's NoStrand) |

`FreePristine` is deliberately **the real free decision** — it trusts
`share` and `pin` exactly as the implementation does — so a protocol that
lets the count lie gets caught there rather than papered over by a guard
the kernel would not have.

## The three counterexamples

`cow_buggy_break` is the headline: two sharers each drop and *then* look,
so both read zero, both conclude "I am the last", and both take the same
page in place, both writable. One address space's writes land in
another's. This is the bug the **global** lock exists to prevent — a
per-Burrow lock cannot, because the two sharers hold different ones.

`cow_buggy_teardown` inverts the drop and the copy and removes the pin. A
concurrent exit then drives the count to zero and frees the pristine page
while the breaker is still reading it. It is why the correct path pins
across the copy rather than trusting the count to stay put — and the pin
is not a separate mechanism, it is *the breaker's own retained share*.

`cow_buggy_vfork` is the odd one out and is instructive for that reason:
**Safety still holds.** The parent observes "has the child released?"
outside the lock and parks after, so a release landing in the window is
lost and the parent parks forever. Nothing is corrupted; the system hangs.
That is why the witness has to be a liveness property, and why a spec that
only ever checks invariants would have called this protocol correct.

The vfork machine was modeled **retroactively** — the mechanism had
already shipped at L-3c-2 — on the [[spec-death-wake]] precedent that a
shipped mechanism on the death lineage earns a model. The release
condition is deliberately not a *record* of the release: it **is** the
release, "the child no longer maps my space".
