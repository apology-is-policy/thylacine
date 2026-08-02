---
id: spec-burrow
type: spec
title: "burrow.tla"
models: [sub-kernel-burrow]
pins: [inv-i7]
cfgs:
  - "burrow.cfg -- clean: RefcountConsistent + NoUseAfterFree (100 distinct, depth 9)"
  - "burrow_buggy_free_on_close.cfg -- free when the handle count hits zero: NoUseAfterFree, premature (66)"
  - "burrow_buggy_free_on_unmap.cfg -- free when the mapping count hits zero: NoUseAfterFree, premature (54)"
  - "burrow_buggy_never_free.cfg -- never free: NoUseAfterFree, delayed (43)"
gate: "any change to the dual refcount, the free decision, or the type-dispatched release arm"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

The smallest useful model in the tree and one of the oldest — a hundred states
at depth nine. Objects, two counters each, and a boolean for whether the pages
are alive. Create, open a handle, close a handle, map, unmap; free exactly when
both counters reach zero.

Its value is entirely in the **biconditional**. `NoUseAfterFree` is written as
an iff, so it fails in both directions: freeing early is a use-after-free,
freeing late is a leak, and a one-directional statement would be satisfied by an
implementation that never frees anything. The three counterexample
configurations are precisely the three ways to get a two-counter lifetime
wrong, and the third exists to make the delayed direction executable rather than
merely asserted.

**Deliberately beneath the model:**

- **the lock.** Transitions are atomic, so what is proven is the *arithmetic* of
  the dual count, not the mutual exclusion that makes the arithmetic hold on
  real hardware. This gap is not academic: the real bug was that the counts were
  correct as written and wrong as executed, because the zero-zero test appeared
  at two sites and raced. The lock discipline is prose and audit;
- **the backing types.** The model has one abstract kind of pages. The
  implementation has six, in two families — contiguous and sparse — and the
  sparse family did not exist when this was written;
- the **cross-Proc share**, where one object is reachable from two address
  spaces. Same arithmetic, references originating in different Procs, resting on
  the argument that a count does not care where a reference came from;
- **partial unmap**, which does not exist in either;
- the **magic sentinel** and the slab recycling it is placed against — a
  defense against violating the invariant, not part of stating it.

## Action-site map

| Action | Site |
|---|---|
| `Init` | `burrow_init` — the slab cache; no objects alive at boot |
| `VmoCreate(v)` | the six `burrow_create_*` constructors, each setting `handle_count = 1, mapping_count = 0` and taking whatever its backing requires — a buddy chunk, a hardware object's reference, a pinned Spoor, or a sparse array |
| `HandleOpen(v)` | `burrow_ref` — under the object's lock, with the both-counts-zero resurrection guard |
| `HandleClose(v)` | `burrow_unref` — decrement and compute `should_free` under the lock, free outside it |
| `MapVmo(v)` | `burrow_acquire_mapping` — the resurrection guard, the per-type liveness check, and the increment, all under the lock |
| `UnmapVmo(v)` | `burrow_release_mapping` — symmetric with `burrow_unref` |
| the free transition | `burrow_free_internal` — outside the lock; re-asserts both counts zero, type-dispatches the release, clears the magic before returning the object |
| `BuggyFreeOnHandleClose` / `BuggyFreeOnUnmap` / `BuggyNoFree*` | no sites — each is prevented by the dual check, and the re-assertion inside the free path is the tripwire for a caller that bypasses it |

| Invariant | Obligation |
|---|---|
| `NoUseAfterFree` | [[inv-i7]] as a biconditional: pages alive **iff** a count is above zero |
| `RefcountConsistent` | counts are zero for objects not alive — the implementation's analogue is that a freed object's magic is clobbered, so any subsequent operation extincts rather than reading a stale count |

## Where the model is behind the code

Three backing types postdate it, and one of the gaps is substantive rather than
cosmetic.

The model assumes an object's pages are **one thing**, allocated at create and
released at free. Two of the six types are **sparse**: their pages arrive one at
a time on fault and can be released individually while the object stays alive.
For those, "the pages are alive" is not a boolean, and the release arm walks an
array rather than making a single call.

The dual-refcount arithmetic is genuinely unaffected — the object's lifetime is
still exactly both-counts-above-zero, and the sparse array is released *at* the
free transition like any other backing. What is unmodeled is the **per-slot**
lifecycle underneath: install-once under the lock, the blocking page-in done
outside it and installed on re-entry, and the decommit path that frees a
resident slot while the object lives. Those are governed by the object's lock
and by the fault-arm's own discipline, and are validated by audit and tests
rather than here.

Two further types hold a **reference to a separately refcounted object** instead
of pages at all, so their release decrements a foreign count and the underlying
resource dies on someone else's schedule. The invariant survives — it is about
*this* object's lifetime — but the model's "pages alive" flag is standing in for
"the held reference", which is a translation the reader has to make.

The last type is byte-identical to anonymous backing and differs only in
**admissibility** — it is the only backing from which userspace may hold an
executable mapping. That distinction is invisible here by construction, since
this model has no notion of protection.

None of this makes the model wrong. It makes it **narrower than its subject**,
which is worth saying plainly in a spec note whose job is to say what a green
run does and does not buy.
