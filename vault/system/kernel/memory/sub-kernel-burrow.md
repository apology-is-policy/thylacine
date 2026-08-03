---
id: sub-kernel-burrow
type: sub
parent: moc-kernel-memory
title: "The Burrow — a memory object with two refcounts and six backings"
code: ["kernel/burrow.c", "kernel/include/thylacine/burrow.h"]
audit: hard
guarded-by: [inv-i7, inv-i32]
validated-by: [spec-burrow, gate-smp]
locks: [lock-burrow]
created: 2026-08-02
updated: 2026-08-03
---
## Purpose

A Burrow is a region of memory **independent of any address space** — the thing
a handle names and a mapping points at, so that "who is still using these
pages" is a question with one answer rather than one per Proc.

Its defining property is the **dual refcount**. A Burrow is reachable two
different ways — through a handle in some Proc's table, and through a VMA in
some Proc's address space — and those are not the same reachability. A Proc can
map a Burrow and close its handle; a Proc can hold a handle to something it has
not mapped. So one counter cannot express liveness, and the pages must survive
until *both* reach zero.

## Contract

**Creation** is per-backing-type and there are six of them, each returning a
Burrow with `handle_count = 1, mapping_count = 0`:

| constructor | backing | shape |
|---|---|---|
| `burrow_create_anon` | one contiguous buddy chunk, zeroed | eager |
| `burrow_create_mmio` | a device PA range, via a held hardware object | eager, foreign |
| `burrow_create_dma` | a kernel-chosen contiguous chunk, via a held hardware object | eager, foreign |
| `burrow_create_file` | a byte range of a file, via a pinned Spoor | **sparse** |
| `burrow_create_anon_lazy` | anonymous, demand-zeroed | **sparse** |
| `burrow_create_code` | one contiguous buddy chunk — identical to anon | eager |

**Refcounting** is four calls: `burrow_ref`/`burrow_unref` on the handle side,
`burrow_acquire_mapping`/`burrow_release_mapping` on the mapping side. The
mapping pair is not for general use — the VMA layer owns it — but is public so
the lifecycle can be exercised in isolation against the model.

**Mapping** is `burrow_map(p, v, vaddr, length, prot)` / `burrow_unmap`, which
install and remove a VMA and take the mapping ref as a side effect.
`burrow_unmap_reporting` is that same removal with one extra out-parameter:
whether *this* unmap was the drop that freed the pages. It exists because no
caller can compute that beforehand — the Burrow's type does not say it, and a
handle count sampled before the drop answers a different question — so the
operation has to report its own effect. Resource accounting is its only caller.
`burrow_share_into(dst, v, vaddr, prot)` is the cross-Proc form.
`burrow_decommit` releases resident pages of a lazy region without unmapping it.

`burrow_backing_pages(size)` reports what a region of that size actually
**occupies**, which is not what it requests. Every resource-accounting site must
use it.

## Mechanism

### The free decision, not the counts, is the mechanism

The counts are ordinary integers. What needed a lock was the **decision computed
from them**, which appears in two places — the handle drop and the mapping drop
— and is the same test in both:

```c
v->handle_count--;                       /* or mapping_count-- */
bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
spin_unlock(&v->lock);
if (should_free) burrow_free_internal(v);
```

The `should_free` boolean crossing the unlock is the whole design. Exactly one
of two racing droppers observes the zero-zero edge, so the free happens exactly
once — and `burrow_free_internal` runs **outside** the lock, because it reaches
the buddy allocator, the hardware-object refcounts, and a 9P clunk. See
[[lock-burrow]].

`burrow_free_internal` re-asserts both counts are zero on entry and extincts
otherwise. That is redundant with the caller's test by construction, and
deliberately so: it is the tripwire for a future caller that frees without it.

### Eager and sparse are two different lifetimes

Three types hold **one contiguous chunk** in `pages` with an `order`; the free
arm calls `free_pages` once. Two types hold a **sparse per-page array**
(`filepages`), each slot null until faulted in, and the free arm walks it
freeing every resident page individually before releasing the array.

That split is the reason the type-dispatched free arm exists at all, and it is
also why the per-type **liveness check** on every mapping acquire reads a
different field per type: `pages` for the contiguous types, the held hardware
object for the foreign ones, the pinned Spoor for file-backed, the array itself
for lazy-anon. For the sparse types an *all-null* array is the normal
freshly-mapped state, not a use-after-free — so their check is on the array's
existence, not its contents.

### Contiguity is bought with rounding, and the rounding is charged

`burrow_backing_pages` exists because the buddy allocator hands out
power-of-two runs: a 2049-page request occupies 4096. Charging the request
rather than the occupancy let a Proc hold up to twice its page budget — bounded
at 2x, since the next order is never more than double, which is why it was an
understated floor rather than an unbounded hole.

The waste itself stays. A Burrow's backing must be **one physically contiguous
run** — the exec direct-map alias, the async ring's kernel address, and the
dataplane ring view all index `pages` as a single chunk — and contiguity is what
the rounding buys. The helper shares `order_for_pages` with the two eager
constructors so charge and allocation cannot drift, and a test pins that
agreement against a real Burrow's recorded order.

### The magic sentinel is placed, not merely present

`magic` sits at **offset 0** so that the slab allocator's freelist write on free
clobbers it. A subsequent operation on a freed Burrow sees a wrong magic and
extincts with a use-after-free diagnostic rather than proceeding into
half-recycled memory. The free path also clears it explicitly before returning
the object.

### The cross-Proc share

`burrow_share_into` makes one Burrow reachable from **two** Procs — the first
path in the tree that does. No handle crosses: the destination gets only a
mapping, and the capability is holding the namespace-gated fid that motivated
the share. The dual refcount is what makes it safe, now spanning Procs: the
mapping ref taken for the destination keeps the pages alive independently of
whatever the source does with its own refs.

Its preconditions are the caller's to satisfy and are not checked: hold the
destination's address-space lock, and guarantee the Burrow stays live across the
call.

## Data structures

`struct Burrow`: magic, type, size, page count, the lock, the two counts, and
then a union-by-convention of per-type fields — `pages`/`order` for contiguous
backings, a hardware-object pointer and PA for the foreign ones, a Spoor plus
file offset plus cache-key scalars for file-backed, the sparse array shared
between file-backed and lazy-anon.

The fields are not an actual union; each type leaves the others zero. That is
what lets the free arm's per-type double-free guards be simple null tests.

## Concurrency

See [[lock-burrow]]. Two guards beyond the counts, both use-after-free defenses:
the **both-counts-zero check on every acquire** (ref-ing a dead identity
resurrects it), and the per-type liveness switch — which was originally outside
the lock, safe only while the sole caller held a handle, and moved inside once a
sibling Thread could free the backing concurrently.

The lock was pulled forward as the precursor to the handle-table lifetime pass,
whose handle-put drops the Burrow ref *outside* the table lock — which is
exactly the situation that requires the Burrow's own refcount to be
independently safe.

## Invariants enforced

[[inv-i7]] — the pages live iff at least one count is above zero. Both
directions are failures: freeing early is a use-after-free, freeing late is a
leak, and the model checks it as an iff for that reason.

[[inv-i32]] participates through `burrow_backing_pages`: this layer is where a
Proc's page charge is computed, and the eager types charge occupancy at create
while the lazy type charges per page at fault, because a free reservation that
charged its whole extent would defeat its own purpose.

## Error paths

Constructors return NULL on allocation failure, having released anything they
took. `burrow_map` returns -1 on misalignment, zero length, an address-space
overflow, an address above the user ceiling, a W+X protection, a VMA overlap, or
allocation failure — with no state changed on any of them.

Everything else extincts, because everything else is structural: a null or
corrupted Burrow, a ref on a zero-zero object, an unref below zero, a free with
a count still held, a double-free of any per-type backing, an invalid type.

The address-space ceiling check carries a static assertion tying it to the
page-table layer's own bound. It was added because one map path accepted an
out-of-range address and deferred rejection to the page-table walk — too late,
because the VMA had already been inserted.

## Performance

Refcount operations are a spinlock acquire around a few integer operations. The
free path is the expensive one and it is off the lock. The eager constructors
zero their whole chunk at create; the sparse types zero one page per fault,
which is the point.

## Prosecution

- **The free decision must stay under the lock and the free must stay outside
  it.** Moving the free inside nests the buddy lock under the Burrow lock on one
  path while the map path nests it outside — the cycle.
- **Every new backing type needs four arms**, not one: a constructor, a free
  arm, a liveness case in the mapping acquire, and a decision about whether it
  is eager or sparse. The switch statements extinct on an unknown type, so a
  missed arm fails loudly — but only when that type is exercised.
- **A new eager creator must charge through `burrow_backing_pages`**; a new
  order-0 per-page path must not.
- **The magic must stay at offset 0** and be cleared before the object is
  returned to the slab.
- **Cross-Proc sharing is anon-only.** Widening it to the foreign types needs
  the hardware-isolation analysis that was explicitly deferred, not just a
  relaxed type check.

## Seams

- **[[seam-kobj-handle-release]]** — the hardware-backed types hold a reference
  to a separately-refcounted object, so the user's handle to *that* object and
  the user's mapping of *this* Burrow can be dropped in either order.
- Partial unmap does not exist: an unmap must match a VMA's range exactly.
  Splitting a VMA is unbuilt.

## Caveats

**The header's own summary contradicts its own enum, in the same file.** The
preamble says the backing type is "`BURROW_TYPE_ANON` at v1.0; PHYS at Phase 3;
FILE post-v1.0" and, twenty lines later, "At v1.0: `BURROW_TYPE_ANON` only" —
while sixty lines below that the enum defines **six** types, including the two
sparse ones and the executable-memory one, each with a substantial comment of
its own. The stale text is the file's opening, which is what a reader reads
first.

The model is three types behind for the same reason, and that gap is
substantive rather than cosmetic: it predates the sparse backings entirely. See
[[spec-burrow]] for what that does and does not leave unproven.

## Provenance

The dual refcount was specified before it was built, and the specification's
three counterexample configurations are the three ways to get it wrong — free
when the handle count hits zero, free when the mapping count hits zero, never
free. The lock came much later, when the first heavily-threaded server made the
race reachable, and its arrival is what made the two-site free decision a
problem worth naming.
