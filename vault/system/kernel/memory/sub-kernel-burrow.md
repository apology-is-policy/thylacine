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
updated: 2026-08-24
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

**Charge attribution** is three more calls — `burrow_charge_record` /
`burrow_charge_claim` / `burrow_charge_restore` — plus
`burrow_is_shared_out`. They exist for the same reason
`burrow_unmap_reporting` does, one axis over; see below.

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

### The type says SHAPE. Everything else must be carried, not inferred

This is the layer's organising lesson, and it arrived three separate times on
three different axes before anyone named it.

A Burrow's type tells you how its pages are *arranged*. It has been asked three
times to answer a question about something else, and it was wrong every time:

| the question | inferred from | why it broke |
|---|---|---|
| may userspace map this executable? | "it is anonymous" | a caller could assert it at map time |
| did *this* unmap free the pages? | the type + a sampled count | neither is the drop's own effect |
| who **paid** for these pages? | the region's shape | shape stops naming a payer the moment two Procs can reach the region |

Each was fixed the same way: **the property moved onto the object, minted by
the kernel, unforgeable by the caller.** `BURROW_TYPE_CODE` carries
executability. `burrow_unmap_reporting` reports its own effect. The charge
record names its payer. The three landed independently, in different arcs, for
different reasons — which is what makes the convergence worth writing down
rather than treating as one design.

### The charge record: a refund must be attributed

`struct Burrow` records who paid: `charge_pid` (a pid, never a pointer — the
payer can die while the region lives on in a consumer), `charge_pages` (the
buddy-rounded count actually billed), and `shared_out`.

- **`burrow_charge_record`** stamps the payer at each eager charge — the
  attach, the JIT create, the Loom ring.
- **`burrow_charge_claim`** is a **read-and-clear**, returning what this Proc
  paid or zero if it is not the recorded payer. The clear is what makes a
  refund exactly-once: two paths racing to settle the same region cannot both
  win, so the counter can never be refunded twice — the direction that would
  inflate a Proc's budget.
- **`burrow_charge_restore`** puts a claim back when the caller decides not to
  settle. Callers must claim **before** the drop, because a freeing drop takes
  the record with it. The window's failure mode is stated and asymmetric: a
  concurrent settler that sees the momentarily-cleared record simply skips, so
  the charge outlives its region until the payer's next release point — an
  over-charge on the payer, never a refund to a Proc that did not pay.

`charge_pages`, not `charge_pid`, is the held sentinel — **pid 0 is a
legitimate identity**, since `proc_alloc` stamps 0 and the fork path assigns
later.

**The release rule is user-voted and is not "follow the pages".** A detach
settles on `freed || shared_out`. `freed` is sufficient — if nothing holds the
region, this Proc certainly does not — but not necessary: once a region is
shared out and this Proc has unmapped it, the Proc cannot reach those pages,
and charging it for memory it cannot touch caps it for nothing. From there the
consumer's shared-mapping axis accounts them.

`shared_out` rather than "does anything still hold it" is load-bearing: the
Proc's *own* other claim — a Loom pin on its own buffer — also keeps the region
alive, and there the charge must **stay** until that claim drops. The prior art
was surveyed and the three answers genuinely differ: Linux memcg keeps the
charge with the allocator and reparents on death, seL4 lets it follow the
capability holder, Zircon counts shared pages in every mapper. Thylacine's dual
axis takes seL4's answer for the sharer half.

A nonzero claim also replaced the older "is this an eager anon VMA" boolean
outright, and is **strictly narrower**: an eager region that was never charged
now refunds nothing instead of a recomputed occupancy. Attribution rather than
an enumeration of shapes.

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

**It is no longer anon-only**, and the widening is the worked example of this
note's own prosecution rule being honoured rather than bypassed. Admissible
now: `BURROW_TYPE_ANON`, or a DMA Burrow whose hardware object carries one of
two **kernel-minted, create-immutable, mutually-exclusive** subtype bits —
`weave` (device-passive: pinned Normal-WB RAM the device only DMA-*reads*,
pixels outbound) or `gpu_bo` (device-*written*: a render target or readback
destination). Plain DMA and MMIO remain structurally unshareable.

The two bits are not one relaxation with two names; **their safety arguments
differ and each lives on its own field.** The weave's is that the device only
reads. The GPU BO's is that what the device may *write* is bounded by GPU-side
address translation that only the trusted device owner programs — a claim about
hardware the kernel does not itself enforce, which is why it is a distinct bit
carrying a distinct argument rather than a widened `weave`. Neither conveys
hardware authority: the client's PTEs are the same cacheable attributes an anon
share installs (never Device-nGnRnE), and the share prot is RW with the VMA
layer rejecting X, so W^X holds.

Note the pattern, again: both bits are set **only** by their own minting
syscall. Same discipline as `BURROW_TYPE_CODE` — the admissibility is a
property the kernel mints at creation, never one the caller asserts at map
time.

## Data structures

`struct Burrow`: magic, type, size, page count, the lock, the two counts, the
charge record (`charge_pid` / `charge_pages` / `shared_out`), and then a
union-by-convention of per-type fields — `pages`/`order` for contiguous
backings, a hardware-object pointer and PA for the foreign ones, a Spoor plus
file offset plus cache-key scalars for file-backed, the sparse array shared
between file-backed and lazy-anon.

The fields are not an actual union; each type leaves the others zero. That is
what lets the free arm's per-type double-free guards be simple null tests.

The charge triple is under the same `lock` as the counts. `shared_out` is
**monotonic** — a region is never un-shared in a way that returns the charge to
the sharer — which is what makes `burrow_is_shared_out` safe to read without
the lock: false→true only ever *adds* a reason to release, so a stale read is
stale in the harmless direction.

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

A third reader arrived with the Warp host-visible ring ([[sub-tapestryd]],
V-3b-1c-2b): `burrow_total_refs` sums *both* counts under `lock` and returns them
as one value. It exists because a caller reclaiming an **out-of-band** backing — a
QEMU subregion tapestryd owns, whose host bytes live *outside* this dual count —
needs a reclaim-safe `handle + mapping == 1` predicate, and summing the two
individual ACQUIRE accessors (`burrow_handle_count` + `burrow_mapping_count`) is
**not** that. Those are two separately-acquired lock-free loads whose operand order
is unspecified, so a peer CPU mutating one count between them can make the sum read
reclaim-safe while a reference is genuinely in flight — the first draft did exactly
this and a Fable round-2 caught it. The lesson generalizes past the caller: *a sum
of two lock-free counters is not one read* — a predicate over both counts must read
them under the lock that guards **the counts**, not merely under *a* lock (the
buggy draft held `as->lock`, which guards the VMA→Burrow link, not the counts). The
sibling contrast is `image.c`'s cache eviction, which reads its own joint
`handle==1 && mapping==0` predicate lock-free and is *still* sound — but only
because of an external-stability proof (`g_image_lock` + ref-before-map), not
because the read is atomic. A caller without that proof needs the locked sum. It
takes a non-const `v` (it locks) and returns 0 for a NULL/dead Burrow — never a
reclaim-safe 1.

## Invariants enforced

[[inv-i7]] — the pages live iff at least one count is above zero. Both
directions are failures: freeing early is a use-after-free, freeing late is a
leak, and the model checks it as an iff for that reason.

[[inv-i32]] participates through `burrow_backing_pages`: this layer is where a
Proc's page charge is computed, and the eager types charge occupancy at create
while the lazy type charges per page at fault, because a free reservation that
charged its whole extent would defeat its own purpose.

It now participates a second way, and this half is the one an audit should
prosecute: the **refund** is attributed rather than inferred. Two defects in
opposite directions made the case. In one, a Loom registered-buffer refund went
to the Loom's owner on the argument that registering requires a loom fd from
that Proc's own table — an argument that proves who owns the *Loom* and says
nothing about who paid for the *buffer*, so a consumer could be refunded for a
sharer's pages (an under-count, inflating a non-exempt Proc's budget, reachable
through the public API). In the other, nothing settled the sharer's charge at
all: the last drop was the *guest's* teardown, in another Proc, holding that
Proc's lock, structurally unable to name the payer — pages leaked per closed
flow. **Neither is visible from the region's shape**, which is exactly why the
payer had to become a recorded fact.

The second one had no live bound breach only because the leaking daemon happens
to run as the system principal, which is exempt — a coincidence of two
independent gates rather than an enforced property. That is worth keeping as a
reasoning pattern: *a bound that holds only because of who happens to be
running is not a bound*, and the first non-exempt driver on that path converts
it to a real monotonic leak.

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
- **Cross-Proc sharing admits ANON plus the two kernel-minted DMA subtype
  bits, and nothing else.** ~~anon-only~~ — the widening happened, and it
  arrived carrying exactly the hardware-isolation analysis this rule demanded
  rather than a relaxed type check, which is the outcome the rule was written
  for. The rule now binds the new boundary: a third admissible kind needs its
  **own** argument on its **own** field. `weave` and `gpu_bo` are separate bits
  precisely because device-read and device-written are different claims, and
  collapsing them into one "shareable" flag would silently extend the weaker
  argument over the stronger case.
- **A share-admissibility bit is minted, never asserted.** Any path that lets a
  caller set `weave` or `gpu_bo` outside its own creating syscall breaks the
  same rule `BURROW_TYPE_CODE` exists to enforce for executability.
- **A charge is claimed before the drop, and a claim is read-and-clear.**
  Claiming after a freeing drop reads a dead Burrow; making the claim
  non-clearing lets two racing settlers both refund, which under-counts — the
  direction that breaks the bound. A caller that claims and then declines must
  `burrow_charge_restore`, never simply drop it.
- **Never snapshot the VMA to reach the Burrow across an unmap.**
  `burrow_unmap_reporting` frees the `Vma`, so `vma->burrow` dangles the moment
  it returns. Snapshot the Burrow pointer first.
- **`charge_pages` is the sentinel, not `charge_pid`.** Pid 0 is a legitimate
  identity, so a zero-pid test reads "unpaid" on a real payer.

## Seams

- **[[seam-kobj-handle-release]]** — the hardware-backed types hold a reference
  to a separately-refcounted object, so the user's handle to *that* object and
  the user's mapping of *this* Burrow can be dropped in either order.
- Partial unmap does not exist: an unmap must match a VMA's range exactly.
  Splitting a VMA is unbuilt.

## Caveats

**The header's own summary contradicts its own enum, in the same file**
(re-verified 2026-08-16, unchanged). The preamble says the backing type is
"`BURROW_TYPE_ANON` at v1.0; PHYS at Phase 3; FILE post-v1.0" and, twenty lines
later, "At v1.0: `BURROW_TYPE_ANON` only" — while sixty lines below that the
enum defines **six** types, including the two sparse ones and the
executable-memory one, each with a substantial comment of its own. The stale
text is the file's opening, which is what a reader reads first.

Two arcs have since landed through this file — the charge record and the share
widening — and both wrote extensive, careful comments *at their own sites*
while leaving the opening summary alone. That is the normal and locally-correct
behaviour, and it is why an opening summary decays faster than anything else in
a file: every author is drawn to the line they are changing, and nobody's change
is ever *about* the preamble.

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

**The charge record's own test fixture nearly proved nothing.** Its two Procs
initially both sat at pid 0 — the value `proc_alloc` stamps before the fork path
assigns a real one — so the payer check matched by *coincidence* and the test
would have passed without exercising attribution at all. Giving the two Procs
distinct pids is what turned it into a test. It is the same class as any
fixture whose default state happens to satisfy the assertion: the fix was in the
fixture, and nothing about the assertion looked wrong.

The two regressions were then **revert-probed on distinct assertions** — undoing
the Loom-side claim fails only the foreign-charge leg, undoing the `shared_out`
arm fails only the payer-settles legs, and neither masks the other. Two fixes,
two independently-failing tests, which is the bar a single test covering both
would have quietly missed.
