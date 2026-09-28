---
id: sub-kernel-burrow
type: sub
parent: moc-kernel-memory
title: "The Burrow — a memory object with two refcounts and six backings"
code: ["kernel/burrow.c", "kernel/include/thylacine/burrow.h"]
audit: hard
guarded-by: [inv-i7, inv-i32, inv-i44]
validated-by: [spec-burrow, spec-cow, spec-capacity, gate-smp]
locks: [lock-burrow]
created: 2026-08-02
updated: 2026-09-23
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
| `burrow_create_anon(size, exempt)` | one contiguous buddy chunk from the user pool, zeroed | eager |
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
`burrow_map_fixed(p, ..)` / `burrow_map_fixed_in(as, exempt, payer, v, vaddr,
length, prot, burrow_offset, &out_free)` place a mapping at a caller-chosen
address over whatever is there (the MAP_FIXED primitive; since B-1a'
[[sub-kernel-vma]]'s range detach followed by an insert, so the wrapper owns
only the address arithmetic and answers `-T_E_*`; the PTE teardown that used to
run here unconditionally is the detach's own, after its refusals).
`burrow_share_into(dst, v, vaddr, prot)` is the cross-Proc form.
`burrow_decommit(p, ..)` / `burrow_decommit_in(as, vaddr, length)` release the
resident pages of lazy mappings without unmapping them -- since B-1a' across the
pieces a protect cut, every refusal decided before the first release, and since
B-1b speaking errno (`-T_E_NOMEM` for a hole, `-T_E_INVAL` for a mapping the
release cannot apply to) for the phenotype `madvise` row and the Pouch wrapper
-- the native `SYS_BURROW_DECOMMIT` still flattens to -1;
`burrow_release_lazy_range_in(as, v, lo, hi)` is the per-mapping half the
decommit and the range detach both loop over, returning the slots it released.
`burrow_lazy_resident_count` is O(1) (the pagemap keeps the count) and
`burrow_lazy_footprint` adds the map's node pages to it.
`burrow_image_resident_count(v)` / `burrow_image_strip(v, want)` are the FILE-side
pair the Image cache's reclaim uses (the round-2 close, below): the resident
count, and every resident page taken out of the map and freed, the Burrow left
live, empty and cached.

**The permission change** is `burrow_protect(p, vaddr, length, prot, seal)` /
`burrow_protect_in(as, exempt, ...)` (B-1a, 2026-09-23): move
`[vaddr, vaddr+length)` to `prot` in {none, R, RW} under each mapping's
mint-time ceiling, `seal` lowering the ceiling to `prot` for good. Caller
holds `as->lock`, exactly as for `burrow_map`. Returns 0 or `-T_E_*` -- the
first Burrow entry to speak errno, because its callers are `SYS_BURROW_PROTECT`
and the phenotype `mprotect` row, both of which hand the value to userspace as
is.

**The deferred-free pair** — `burrow_release_mapping_deferred` and
`burrow_free_deferred` — exists because a FILE-backed Burrow's free reaches
`spoor_clunk`, which may **sleep**, while every VMA mutator holds `as->lock`, a
spinlock. `burrow_release_mapping_deferred` drops the mapping ref and settles the
I-32 uncharge under the lock but does **not** free; it hands the caller the
Burrow that still owes its physical free, collected on a `deferred_free_next`
stack and passed to `burrow_free_deferred` after the unlock. The full mechanism —
the `out_free` out-parameter every teardown path must thread and must never drop
(a NULL `out_free` would leak the chain) — lives on [[sub-kernel-vma]].
`burrow_free_deferred(v)` frees `v` and every Burrow chained behind it on
`deferred_free_next` (B-1a': a range detach drops several last refs in one
locked pass), NULL-safe, unlinking as it goes.

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
arm calls `free_pages` once. Two types hold a **sparse per-page slot table** --
the pagemap `pm` ([[sub-kernel-pagemap]]; B-1a' replaced the flat `filepages`
array), each slot absent until faulted in, its nodes allocated as slots fill and
charged to the mapping address space for ANON_LAZY (uncharged for FILE, the
Image cache's posture; a FILE map's PAGES are charged to each space that maps
them, per leaf, by the fault -- [[sub-kernel-fault]]) -- and the free arm destroys it, putting every resident
page (a plain free for FILE, a COW put for ANON_LAZY) and freeing the nodes.

That split is the reason the type-dispatched free arm exists at all, and it is
also why the per-type **liveness check** on every mapping acquire reads a
different field per type: `pages` for the contiguous types, the held hardware
object for the foreign ones, the pinned Spoor for file-backed, `pagemap_live`
for lazy-anon. For the sparse types an EMPTY map is the normal freshly-mapped
(or fully decommitted) state, not a use-after-free — so their check is on the
map's liveness, not its contents.

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

`struct Burrow` records who paid: `charge_as_id` (the paying ADDRESS
SPACE's `id` -- a u64 from a global counter, never reused, never a pointer:
the payer can die while the region lives on in a consumer; the B-1a' audit's
F4 replaced the pid, which survives an exec and let a non-CLOEXEC Loom's
close refund against the successor's space), `charge_pages` (the
buddy-rounded count actually billed), and `shared_out`.

- **`burrow_charge_record`** stamps the payer at each eager charge — the
  attach, the JIT create, the Loom ring.
- **`burrow_charge_claim`** is a **read-and-clear**, returning what this Proc's
  ADDRESS SPACE paid or zero if it is not the recorded payer -- a record whose
  space has died is never claimed: that space's count died with it and the
  region's pages return to the pool when they are freed, so there is nothing
  left to settle and the record simply stays. The clear is what makes a
  refund exactly-once: two paths racing to settle the same region cannot both
  win, so the counter can never be refunded twice — the direction that would
  inflate a Proc's budget.
- **`burrow_charge_restore`** puts a claim back when the caller decides not to
  settle. Callers must claim **before** the drop, because a freeing drop takes
  the record with it. The window's failure mode is stated and asymmetric: a
  concurrent settler that sees the momentarily-cleared record simply skips, so
  the charge outlives its region until the payer's next release point — an
  over-charge on the payer, never a refund to a Proc that did not pay.

`charge_pages`, not `charge_as_id`, is the held sentinel — a charge of zero
pages is meaningless, so zero pages IS "nothing held" (ids start at 1, but the
rule does not lean on that).

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
the share. It maps the **whole** Burrow — the signature carries no length,
because a share is always whole-region (`length = size`). The dual refcount is
what makes it safe, now spanning Procs: the mapping ref taken for the
destination keeps the pages alive independently of whatever the source does with
its own refs.

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

### The protect is one locked step, and the uninstall sits in the middle of it

`burrow_protect_in` is `vma_reprotect_precheck_in` -> `mmu_uninstall_user_range`
over the range -> `vma_reprotect_range_in`, under one `as->lock` hold
([[sub-kernel-vma]] owns the cut and the merge). Every refusal is decided
first, so a refused call costs the range's resident pages nothing -- not even
a re-fault. Then the leaf PTEs go, and only then does any `prot` change. The
order is the D-3b rule and `addrspace_clone`'s phase-1 argument: hardware
resolves a PTE without taking `as->lock`, so a peer thread holding an installed
writable PTE stores with no fault, no kernel entry and no lock, and a writable
PTE must be gone before the permission that justified it is
(`cow.tla::BUGGY_PROTECT_KEEPS_PTE`; `NoWritablePteBeyondProt` is the witness).
The uninstall runs on a raise as well, because `mmu_install_user_pte` refuses a
mismatching install over a valid leaf: the next fault must find the slot empty
to install at the new prot. `mmu_uninstall_user_range` allocates nothing and
cannot fail, which is what lets it run ahead of the one thing after it that
still can -- no memory for a split piece -- and that failure leaves every prot
unchanged and merely costs the range a re-fault. A resident page keeps its slot
and its charge across a protect to none, as Linux keeps a `PROT_NONE`
mapping's contents; returning pages is `burrow_decommit`'s job.

The precheck admits ANON, ANON_LAZY and FILE only (`reprotect_admits`, in
[[sub-kernel-vma]]). CODE is the I-42 pair -- one charge, two aliases, neither
a plain mapping -- and MMIO / DMA / HOSTMEM are [[inv-i34]] windows whose
permissions were conferred, not chosen. X is never a target; the mechanism
refuses it in the precheck and the boundary refuses it before any lookup.

### One clone per SOURCE Burrow: the fork's dedupe cursor

Until B-1a a lazy Burrow had exactly one VMA, so "clone per VMA" and "clone per
Burrow" were the same count. A protect (or a D-3b window) splits a lazy mapping
into pieces that all name ONE Burrow, and `burrow_clone_cow` walks the WHOLE
source, taking a COW share on every resident page -- so a clone per piece
would leave the child holding k shares of every page while being one holder,
charged k x the resident count, and the parent unable to take a page in place
until the child died (`cow.tla::BUGGY_CLONE_PER_PIECE`, violated by the
initial state: the fork itself is the bug). `struct Burrow.clone_cursor` is
the fix: the first piece `addrspace_clone` meets mints the clone and parks it
on the source's cursor; later pieces of the same Burrow map that clone, taking
only a mapping ref and charging nothing. The cursor is meaningful ONLY under
the source address space's lock for the duration of one `addrspace_clone`,
which clears every cursor it set before it unlocks, on every outcome -- a
cursor that outlived the clone would name a Burrow a failed child's drain may
already have freed. Nothing else reads it ([[sub-kernel-addrspace]] owns the
clone; `cow.clone_dedupes_split_pieces` counts exactly two holders per page).

The clone's slot table is a `pagemap_mirror` (B-1a'): `burrow_clone_cow(src, exempt)`
allocates the struct, an empty map of the same count, and a pool of exactly
`pagemap_node_count(src)` node pages from the user pool
(`pagemap_pool_alloc(.., exempt)`: the physical pool's only cost of a fork,
[[sub-kernel-mm-phys]]) -- all OUTSIDE `src->lock`, charged to no address
space until `clone_one_vma` charges the child the footprint --
then mirrors under `src->lock` with `clone_take_share` (one `cow_page_get` per
page, in the hold that writes the slot), frees the unconsumed pool, and on a
short pool (a caller bug: the source grew under a lock the caller did not hold)
destroys the partial clone through `lazy_put_page`, which puts back exactly the
shares taken. The header says the caller charges the clone's footprint
(`burrow_lazy_footprint`, resident + nodes) as one decision after the mapping
ref lands; as built `clone_one_vma` charges the resident count only, so the
clone's nodes are uncharged ([[sub-kernel-pagemap]] Seams).

## Data structures

`struct Burrow`: magic, type, size, page count, the lock, the two counts, the
charge record (`charge_as_id` / `charge_pages` / `shared_out`), B-1a's
`clone_cursor` (the fork's per-source dedupe slot, meaningful only under the
source's lock during one clone -- NULL at all other times), and then a
union-by-convention of per-type fields — `pages`/`order` for contiguous
backings, a hardware-object pointer and PA for the foreign ones, a Spoor plus
file offset plus cache-key scalars for file-backed, the pagemap `pm` (32
bytes, embedded) shared between file-backed and lazy-anon
([[sub-kernel-pagemap]]).

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

**A release runs BEFORE its mapping goes, and the free refunds nothing**
(B-1a', the law of [[spec-capacity]]). Once one Burrow has several VMAs, the
detach path's refund could no longer be the whole Burrow's resident count per
piece (B-1a's under-count, fixed then by a per-piece `burrow_decommit` in the
exact-match detach); B-1a' made the refund the range core's own:
`vma_detach_range_in` calls `burrow_release_lazy_range_in` over every plain
ANON_LAZY mapping's overlap while the mapping still names the slots, and only
then trims, splits or removes it. `burrow_free_internal` is Proc-agnostic -- it
puts every resident page and frees every node UNCHARGED -- so a Burrow freed
through a detach must arrive empty of anything this address space paid for;
the D-3b window inside a touched lazy mapping (the B-1a audit's F5) is served
by the same core and so releases its window's slots by construction. The
operator's memory bar -- relinquished memory returns -- is
`sys_burrow.detach_piece_frees_only_its_pages` and the six `detach.*` tests.
`BURROW_RESERVE_MAX` is the burrow window since B-1a' (it was 1 GiB; the
detach's own 256 MiB `BURROW_ATTACH_MAX` bound is gone): a reservation costs its
struct and nothing per reserved page, so its size is not the resource -- the
pages touched (and the nodes that index them) and the mappings held are
(`detach.lazy_over_256mib_detaches`, `detach.four_gib_reservation_round_trips`).

[[inv-i44]] — two of its mechanisms sit in this file since B-1a: the
uninstall-before-prot order in `burrow_protect_in` (a writable PTE never
outlives the permission that justified it), and the one-clone-per-source
cursor (the per-page share count equals the number of address spaces holding
the page). Both have a named buggy cfg in [[spec-cow]].

## Error paths

Constructors return NULL on allocation failure, having released anything they
took. `burrow_map` returns -1 on misalignment, zero length, an address-space
overflow, an address above the user ceiling, a W+X protection, a VMA overlap, or
allocation failure — and `-T_E_NOMEM` when the address space is at its VMA cap
(the resource refusal reported as one since B-1a' round 4, F18) — with no state
changed on any of them.

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
- **`charge_pages` is the sentinel, not `charge_as_id`.** Zero pages is
  "nothing held"; the id is the KEY -- the paying address space, never a pid,
  because a pid survives an exec and an address space does not.

## Seams

- **[[seam-kobj-handle-release]]** — the hardware-backed types hold a reference
  to a separately-refcounted object, so the user's handle to *that* object and
  the user's mapping of *this* Burrow can be dropped in either order.
- `burrow_unmap` / `burrow_unmap_reporting` still match a VMA's range exactly;
  the range form is [[sub-kernel-vma]]'s `vma_detach_range_in`, which both
  detach syscalls use. The exact-match removers remain for the paths that own a
  whole mapping (the weft share, the hardware maps).
- The fork clone's node pages are uncharged as built ([[sub-kernel-pagemap]]
  Seams).

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

[[chg-2026-09-06-burrow-borrowed]] re-verified this after a same-day-tie stale
flag. The only burrow change since the 2026-08-24 update is `3de39ad0`
(V-3b-1c-2b round-3, 16:29 the same day) — a COMMENT-only refinement of
`burrow_total_refs`'s rationale from round-2's "IRQ-preemptible" to the true
"SMP cross-CPU" (masking cannot serialize two CPUs; only `v->lock` can), and this
dossier's prose already carries the SMP reasoning ("a peer CPU mutating one count
between them"). The code is unchanged. Borrowed — nothing owed.

## PCI mapping lifetime and routing protection (2026-09-17)

`burrow_create_mmio_range` retains a whole MMIO claim while mapping a page-aligned
subrange; its stored PA is the subrange base. `burrow_create_pci_mmio` additionally
retains the owning PCI object, after checking MSI-X page exclusions. MMIO Burrows
may therefore hold both `kobj_mmio` and `kobj_pci`; freeing drops both references.
This prevents reassignment of a function while its old register mappings live.
The hostmem constructor independently rejects protected table/PBA pages, so a
valid shared-memory descriptor does not circumvent routing isolation. The guest
PCI mapping lifetime and hostmem alias tests pass. [[abi-pci-windows]].

## B-1a': the pagemap arms, the range release, the window (2026-09-23)

Every `filepages` reader and writer in this file became a pagemap call, and
the discipline each kept is unchanged in substance. Every page this file
mints comes from `alloc_user_pages(.., exempt)` since the round-1 close -- the
eager `ANON` and `CODE` chunks (`burrow_create_anon(size, exempt)` /
`_code`), the populate's pages, the clone's node pool -- and returns its pool
charge at `free_pages` wherever it is freed ([[sub-kernel-mm-phys]]); the
creators' `exempt` is the calling Proc's (`proc_resource_exempt(p)` at the
syscalls, exec's own, `true` for the vDSO), because the machine-wide bound is
decided at the allocation now, not at a charge. `burrow_lazy_populate`
(exec's writable-segment fill) installs each page with `pagemap_install(&v->pm,
&v->lock, slot, pg, as, exempt, &winner)`, so the map's nodes are charged to
`as` on top of the run's own whole-run charge; a refused install (a cap hit or
a node OOM) breaks the run off and the all-or-nothing unwind takes every page
back through `pagemap_take` (the nodes it empties handed back and freed outside
`v->lock`, uncharged from `as` inside the take) and puts them through their
share count. `burrow_lazy_swap_slot` is `pagemap_swap`; `burrow_lazy_slot_kva`
is `pagemap_get`; the mapping-acquire liveness check for the two sparse types
is `pagemap_live` (an empty map is the normal state; a dead one is the UAF the
`{0,0}` guard already covers); the test helpers install through the same
install-once and read through `pagemap_get`.

`burrow_release_lazy_range_in` admits a plain ANON_LAZY mapping only
(`lazy_release_admits`: a Burrow with the right magic and type, not
`SHARED_IN` -- a shared-in mapping is the sharer's commit, never this space's
charge; a guard has no Burrow) and `[lo, hi)` inside it, maps the range to
slots through `burrow_offset + (lo - vaddr_start)`, and loops
`pagemap_take_next` from the first slot to the last, ending on the take's own
answer (`if (!pg) break;` -- NULL iff nothing was taken; the round-1 audit's
F3: the loop ran on its own unclamped bound, which a mapping past its
Burrow's end would have spun, and `vma_alloc` now refuses that shape at the
one constructor, [[sub-kernel-vma]]): each present node's page
is put (`cow_page_put`; the buddy gets it only from the last holder), the
nodes the take emptied are freed outside `v->lock`, and `freed` counts SLOTS,
not pages returned -- this address space stops mapping the page either way, so
its RSS drops whether or not a co-sharer keeps the page alive (LINEAGE L-4b).
The pages' uncharge settles once at the end for the whole overlap; the nodes'
settled inside each take. The walk is by resident slot, never by slot, because
a reservation costs nothing and may be the whole window: a release must cost
what was touched, the bound `mmu_uninstall_user_range` keeps for the PTEs (the
B-1a audit's F2 shape) and `pagemap_walk_steps` witnesses.

`burrow_decommit_in` is the same release over a RANGE of mappings: an
admission pass (contiguous cover by plain ANON_LAZY mappings from
`vma_next_overlap_in` and successors; a hole at the head, between or at the
tail, or any other kind of mapping -- eager ANON, FILE, hardware, a shared-in,
a guard -- answers with nothing changed: since B-1b `-T_E_NOMEM` for a hole
and `-T_E_INVAL` for another kind of mapping, Linux's madvise errnos, which
`sys_burrow_decommit_core` extends with the window's `-T_E_NOSYS` and the
native 84 flattens to -1), then the range's PTEs cleared
(the burrow_unmap discipline: TLBI before any page reaches the buddy), then
each mapping's overlap released. It spans the pieces a protect cut, as Linux's
`madvise` spans VMAs. `burrow_decommit(p, ..)` wraps it on `p->as`.

`burrow_map_fixed_in` gained `payer` and lost its unconditional PTE teardown:
the surgery is `vma_replace_range_in`, whose detach uninstalls after its
refusals are decided, so a refused MAP_FIXED no longer costs the window a
re-fault. Its shape refusals are `-T_E_INVAL` now, its surgery's `-T_E_*`
pass through, and `burrow_map_fixed` passes the owner as the payer.

**The strip (the round-2 close; bounded at round 3).** `burrow_image_strip(v,
want)` is the Image cache's reclaim on a FILE Burrow ([[sub-kernel-image]]):
`pagemap_take_next` by resident slot from 0 with `as = NULL` (a FILE map's
nodes were never charged to a space), the nodes and the page freed OUTSIDE
`v->lock` (leaf order), no COW put (FILE never shares a page), until `want`
pages are freed or the map holds nothing (B-1a' audit F14: a pick costs what
was asked, never a whole image; the next strip of the same image continues
from the lowest slot left);
the Burrow stays live and cached, so the next mapper's fault pages it in
again from the pinned Spoor. The caller guarantees no mapping and no handle
but the cache's can reach the map -- the {1,0} idleness the cache proves
under its own lock -- which is what makes freeing a page nothing else names
safe. No address space is refunded here: a FILE page's holders were refunded
when their leaves went ([[sub-kernel-vma]]), and an idle image has none. The
protect, the unmap, the decommit and the detach's phase 2 clear their leaves
through `vma_uninstall_range_in` now, mapping by mapping, so a FILE mapping's
refund lands on the right counter.
