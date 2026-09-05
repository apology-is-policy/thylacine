---
id: inv-i44
type: inv
title: "I-44 — a fork's address spaces diverge on the first write"
number: I-44
guards: [sub-kernel-addrspace]
validated-by: [spec-cow, gate-smp]
strength: spec
created: 2026-08-06
updated: 2026-08-06
---
## Statement

Two address spaces that share a page after a fork are **independent from
the first write onwards**. Neither can observe the other's stores, and
neither can free the page the other is still reading.

Three properties, each separately violable and each with its own
counterexample:

- **No aliased writable.** At most one address space holds a shared page
  writable. A copy-on-write break either takes the page in place — which
  is legitimate only when the breaker is the *sole* remaining holder — or
  installs a private copy no one else can reach.
- **No use after free.** The pristine page is not returned to the
  allocator while any address space still maps it or any breaker is still
  copying out of it.
- **No strand.** A vfork parent suspended on its child is always resumed;
  a release cannot be lost between the parent's check and its park.

The first is the invariant's core and the reason it is stated at all. The
other two are what the *implementation* of the first can break: a
protocol that gets the aliasing right and the lifetime wrong corrupts
just as thoroughly, and one that gets both right and hangs is unusable.

## Enforcement

Held by [[sub-kernel-addrspace]], across three mechanisms that must
compose:

- **The parent's PTEs are uninstalled before anything is shared.**
  `addrspace_clone`'s first phase drops every writable PTE covering a COW
  range *before* the child gets a share. Holding the address-space lock is
  not a substitute for the ordering: the lock only reaches a peer that
  **faults**, and a peer holding an already-installed writable PTE stores
  in hardware with no fault, no kernel entry and no lock. Uninstalling
  first makes the next store *have* to fault, and faulting takes the lock.
- **The break's decide is one step under [[lock-cow]].** Sole holder →
  take in place, leaving the count at 1 so nothing frees underneath.
  Otherwise → copy. Splitting the drop from the decide lets two breakers
  both read zero and both take the same page writable.
- **The breaker's own share is the pin.** It is retained *across* the copy
  and dropped only when the copy is done, so a concurrent exit cannot
  drive the count to zero and free a page still being read.

The count lives **on the page**, not on the Burrow slot, and that
placement is itself enforcement: after a break the slot and the page a
slot-indexed count describes diverge, so the *free decision* computed from
it would free a page another address space still maps.

The **fork refuses** anything it cannot give correct sharing semantics —
writable eager-anon (no per-page ownership to break), MMIO and DMA at any
prot (a device window is an authority transfer, not a copy), and
cross-Proc shared-in mappings. Refusing is what keeps the invariant from
being defended by a mechanism that does not exist for those kinds.

## Validation

[[spec-cow]], model-first, with a clean cfg and three counterexamples —
one per property, so each failure names its own mechanism. `NoAliasedWritable`,
`NoUseAfterFree` and `NoDoubleFree` are invariants; the vfork property is
**liveness**, and that asymmetry is load-bearing: the lost-wake bug leaves
safety entirely intact and produces a hang, so a spec that checked only
invariants would have certified the broken protocol.

Runtime: the fork and COW test suites, plus the SMP multi-boot gate, which
is the only witness that exercises a genuine concurrent break.

**blind-to:** everything outside one page's protocol.

- **Ordering across pages.** The model is a single page, so
  `addrspace_clone`'s *walk* — the order in which it uninstalls, clones
  and flags across a whole VMA list — is prose-argued only. The two-passes-
  must-agree property (pass 1 uninstalls, pass 3 flags, on the same
  predicate) is exactly the kind of thing the model cannot see.
- **The page tables themselves.** "Installed writable" is a program
  counter in the model, not a PTE. Whether the uninstall covers the right
  range with the right invalidation is [[inv-i12]]'s and
  [[sub-kernel-mmu]]'s question.
- **Allocation failure mid-break.** The copy path always gets its page in
  the model. OOM during a break is a real path defended by structure
  rather than by the spec.
- **The intra-address-space race.** Two threads of *one* address space
  faulting one page is the REVENANT lazy-arm shape, deliberately below the
  abstraction, which treats a sharer as one agent per address space.
