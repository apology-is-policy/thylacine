# Capacity-backed shared memory and retention accounts

Status: APPROVED by the operator, October 4, 2026. Implementation pending.
Approval includes both contracts and async-first implementation order.
Baseline: Astra 8b2212c0e. Companion: ASYNC-SERVICE-LIFECYCLE.md.
This replaces a fixed shared-mapping floor with explicit accounting and policy.
It does not replace the growable heap or promise unlimited/pinned/swap-backed RAM.

## 1. Observed limit and verified architectural gap

Halcyon uses ThylaAlloc and the reservation-backed growable heap. Its observed
failure was a separate address-space shared-mapping limit: 30387 existing pages
plus 2613 new pages exceeded 32768 (128 MiB). It returned generic Map failure;
existing tabs and the compositor survived. Hidden-buffer suspension now allows
the original workload within that limit and remains useful under this design.

PROC_SHARED_MAP_MAX_PAGES originated in 6599519d80 as an orphaned-weave defence:
a client could retain buffers after the allocating compositor crashed. Its
comment budgets two large weave generations and calls the bound a DoS floor,
not an accountant. addrspace_charge_shared_map enforces it atomically;
burrow_share_into charges every incoming VMA and teardown refunds that VMA.
The underlying physical pages are not newly allocated for each mapping.

The newer B-1a capacity policy already gives ordinary unconfined address spaces
a budget derived from RAM. mm/phys.c capacity_init sets the user pool to RAM
minus max(256 MiB,RAM/8), clamped so a small machine retains half its RAM.
alloc_user_pages charges backing once and free_pages returns a PG_USER block's
charge on actual freeing. Sparse private heaps grow against that mechanism.

However, kernel/dma_handle.c dma_create_body still uses alloc_pages, including
weaves/GPU BOs. Their backing bypasses that user pool. Per-buffer hardware
allowances and the64 MiB weave/BO envelope are different protections; a cumulative
per-driver budget is still noted as a refinement in syscall.c. HOSTMEM maps are
PCI BAR extents, not allocator-owned RAM. A production replacement must address
these distinctions, not equate every mapping with private heap pages.

## 2. Prior art and recommendation

Genode accounts RAM through capability-controlled budgets and lets clients fund
server-side session allocations. The useful property is durable, explicit
responsibility for a server's client-driven memory, not a globally privileged
"trusted server" exemption. [Resource trading](https://genode.org/documentation/genode-foundations/26.05/architecture/Resource_trading.html),
[RAM allocation](https://genode.org/documentation/genode-foundations/26.05/api/Physical_memory_allocation.html).

seL4 explicitly delegates physical-memory authority via untyped capabilities.
Adopt bounded authority and lifetime discipline, not seL4's whole allocation
model: Thylacine already has demand paging and Burrows.
[Untyped memory](https://docs.sel4.systems/Tutorials/untyped.html).

Fuchsia separates memory objects from mappings, negotiates shared buffer
collections among participants and tracks their lifetime. Its pressure signals
let clients discard nonessential memory. Adopt object identity, backend-specific
constraints and pressure notification; do not copy its termination/reboot policy.
[Sysmem](https://fuchsia.dev/fuchsia-src/development/graphics/sysmem/concepts/sysmem),
[VMO lifetime](https://fuchsia.dev/fuchsia-src/development/graphics/sysmem/concepts/vmos),
[Pressure](https://fuchsia.dev/fuchsia-src/concepts/memory/memory_reclamation).

Plan 9 draw's refresh
model separates semantic content from disposable pixels. That is not precedent
for a universal 128 MiB budget or a complete hierarchical memory accountant.
[draw](https://9p.io/magic/man2html/3/draw).

Recommendation: extend the existing capacity mechanism with durable allocation
accounts and separate retention limits; add asynchronous pressure reporting and
measured graphics admission. Keep explicit failure, no automatic victim killing,
no unsafe reclamation of a live mapping and no swap/compression side project.
A larger constant postpones the same failure. Combining all charges into RSS
would double-charge shared physical memory and hide who can keep it alive.

## 3. Four quantities, with separate units and invariants

| Quantity | Meaning | Enforcement / lifetime |
| --- | --- | --- |
| Physical backing P | Actual allocated RAM pages, including buddy rounding; counted once machine-wide. | Existing capacity pool extended to application-induced DMA/BO backing; refunded only when physical pages actually return. |
| Allocation charge A | Which durable account sponsored backing and related kernel storage. | Owned by a refcounted account independent of Proc lifetime; counters for resident bytes and admitted reservations are distinct. |
| Retention charge R | Maximum backing a consumer can keep alive through references, even after sponsor death. | One claim per retention unit per account, plus address-space limits; every independent holder must be represented. |
| Mapping/metadata M | VMA/PTE/handle/reference/table overhead and aperture address space. | Existing bounds plus charged membership records and class-specific aperture quotas; never silently counted as free. |

P is not sum(R). A server and two clients can share one 20 MiB buffer: physical
usage is 20 MiB, allocation sponsor carries 20 MiB, and each independent consumer
retention account carries 20 MiB. Parent aggregate R is the union of retained
objects in its subtree, not another physical allocation. Aliases in one account
add map metadata but not another R charge for the same retention unit.

A retention unit is the smallest independently freeable backing object, given a
stable kernel incarnation. If mapping one page holds an entire 64 MiB DMA object,
R is 64 MiB, not 4 KiB. Charge actual rounded backing for eager DMA; for lazy shared
anonymous objects reserve their maximum retainable backing, not today's touched
pages. If existing object semantics cannot independently release subranges,
do not invent subrange accounting. More precise split objects can follow later.

Object growth or backend import that increases the unit's retainable cost must
reserve the delta in every affected account before publication. Refuse growth
atomically if any cannot pay. Initial v1 shared objects are size-immutable;
resize uses a new object/generation. This avoids an unbounded account fan-out in
the first implementation. Existing lazy object bounds are stable at share mint.

Existing AddrSpace.page_count/page_budget remain the address-space holder
constraint; do not replace them with allocation sponsorship. New anonymous/COW
pages and page-table/ring metadata receive the requesting account as sponsor.
Shared executable/image-cache backing uses the bounded system cache account,
not whichever user happened to fault it first. Its existing clean-page reclaim
policy stays separate. The new R ledger covers independently retained foreign
anonymous/DMA/HOSTMEM backing and its escaped references; it is not another RSS
limit over shared executable text. Inventory remaining PG_USER paths explicitly;
legacy boot allocations receive a real boot account, never a null sponsor.

## 4. Durable account hierarchy and authority

Introduce a refcounted MemoryAccount identified by a non-repeating incarnation,
not a Proc pointer/PID/UID string. It has a parent, limit vector, current charges,
pending reservations, pressure generation and lifecycle state. Boot owns the
root. Login/session creation receives a child; application descendants remain
under it. Resident services have separate operating accounts; buffers produced
for a session use that session's explicitly supplied allocation authority.
There is no universal administrator/root-user shortcut in the allocation path.

Limits are ceilings, not promises or preallocated partitions. Default unconfined
session/application ceilings are the capacity pool's size, matching B-1a's
current elastic policy. Aggregate admission at shared ancestors and actual
physical allocation decide availability. An operator may confine a session or
application more tightly. This does not guarantee fair memory shares: one
unconfined session can consume most available capacity. Reserved per-user shares
would be a separate policy, not an unmentioned property of this design.

An account-control capability permits querying, creating attenuated descendants,
and adjusting their limits within its parent envelope. Applications receive
charge/use authority for their own account, not control of siblings or ancestors.
Only the boot/session policy owner receives adjustment authority. Raising a
ceiling needs that authority and cannot exceed ancestors; arbitrary CAP_HW_CREATE,
principal SYSTEM, possession of a share ID or successful imperium key entry does
not mint more memory. No new broad "change any user's memory" cap is introduced.
This composes with the pending authority work without importing its drafts.

A child inherits the same aggregate account unless explicitly assigned a child
account. Forking cannot clone an independent budget; RFPROC/RFMEM cannot reset
usage. RFMEM uses the same AddrSpace claim set, while distinct spaces retain
separate address-space constraints. Ordinary COW fork pre-admits the child's
claims and metadata transactionally and fails cleanly if it cannot; COW backing
remains one physical charge until copying actually allocates another page.

Closing or deleting an account freezes new admissions; it does not refund live
backing or pins. An orphan account remains charged to its ancestors until its
last backing/claim/reference retires. Existing parent identity and ancestry
cannot be rewritten to launder outstanding charges. Lowering below usage yields
an over-limit state: existing memory remains valid, net-new claims are refused,
pressure is signalled and charges can only fall until compliant. No forced unmap.

### Proposed interface surface

Use typed account/voucher handles and versioned, size-checked records. Proposed
operations are ACCOUNT_QUERY, ACCOUNT_CHILD, ACCOUNT_SET_LIMITS,
ACCOUNT_WATCH, VOUCHER_CREATE and ALLOCATE_FUNDED/MAP_ACCOUNTED. These names do
not allocate syscall numbers. The ABI may multiplex account control on a new
object operation, but must not reuse a retired number or overload hardware
rights. Reserve the concrete layout and C/Rust mirrors at MM-0. The operator chose
the shape on November 3: section 13.

Query/watch exposes separate used, reserved, limit and pressure-generation fields
per class in bytes, with checked page rounding internally. Every mutating request
has a version, size, flags-zero field and expected account/object incarnation.
Limit changes are vector-atomic: either every class changes or none does. Query
is informational; admission rechecks live state atomically and never trusts an
old available-bytes snapshot. Watch is level-triggered with a bounded current
snapshot, not an allocation history feed. Account/voucher handle transfer is
explicit and rights-attenuating; ambient numeric IDs never select a sponsor.

## 5. Allocation sponsorship across a server boundary

Provide a kernel-mediated, restricted allocation voucher, not a user-supplied
account ID. The account owner grants a bounded amount for one peer connection
incarnation and one permitted allocation class (e.g. CPU pixels). Only the
intended server can consume it; it does not expose general account control or
DMA/device rights. Both existing hardware authority and voucher funding must
pass. The kernel stamps the resulting object's stable sponsor account.

Reserve before server allocation; consume once; atomically convert reserved
capacity into actual allocation charges. Return unused capacity on failure or
voucher expiry/closure. Used capacity stays charged until actual object release.
Bound voucher count/metadata; closure/cancellation must use preallocated records.
Connection death revokes unconsumed authority but does not invalidate live memory.
A voucher may fund a bounded group of buffers up to its total, with all-or-nothing
reservation for that group. Replays and service replacement cannot spend twice.

Tapestry obtains per-session funding from its session owner; ordinary client
retention is still charged to that client's account. Halcyon spending within a
session is not allowed to debit another login session. Persistent display output,
trusted SAK scene and service bookkeeping use an explicitly configured operating
budget. They must not become an excuse to fund unlimited client-selected surfaces
from an exempt server account. Legacy calls may use their caller's own account;
no API can silently charge a named victim.

This adds resource delegation, not memory access authority. Mapping permissions,
Weft share grants, PCI ownership, W^X, non-transferable hardware handles and
trusted-display isolation remain independently enforced.

## 6. Complete holder accounting

Replace shared_map_pages' fixed-cap decision only after every retaining path is
covered: incoming VMA, Burrow/Weft handle or outstanding grant, registered Loom
buffer, queued backend command, CPU composition borrow and display/GPU pin.
A claim follows the reference that can prolong allocation lifetime. A borrower
inside a creator's operation may be covered by that already-live claim; when
ownership escapes it must acquire its own claim before the old one drops.
Two aliases of the same object in one account use one membership with refcount.
Two wrapper objects pointing to the same underlying DMA allocation use that
allocation's identity, not wrapper pointer identity.

For each insertion reserve all required ancestor/account/AddrSpace membership
and metadata before making the reference reachable. On failure restore every
counter and table, including PTE allocation and share-grant consumption state.
For removal perform the actual unmap/unpin/reference drop before releasing the
corresponding claim. A last-client-unmap does not release a backend fence pin.
Owner exit never moves cost to a magical uncharged orphan list.

Use a documented account-tree transaction lock for reserve/release in v1, with
no allocation, transport call, page-table work or sleeping inside it. Preallocate
candidate membership records, lock, validate current limits/incarnations, reserve,
unlock, perform resource work and either commit or roll back. Account destruction
waits on explicit refs/reservations. Account ancestry is immutable. Quota-unit arithmetic and actual-page arithmetic remain separate,
with checked 64-bit counters and no saturating refund that hides an underflow.
Never acquire account locks while holding Burrow/VMA/driver locks; take charge tokens first,
then mutate those structures, then settle outside their locks. Charge tokens
carry their own references, so races cannot free their ledgers beneath rollback.
A fixed maximum hierarchy depth 16 bounds one transaction; configuration rejects
a deeper tree rather than silently flattening it. This is a proposed metadata
bound, not a per-app memory ceiling. Measure contention before finer locking.

## 7. Physical and backend integration

Route user-induced weave/GPU BO allocations through capacity-aware allocation,
charging every actual buddy block including rounded tails. Partial multi-block
failure unwinds both backing and reservations. Do not count logical length alone
when a buddy block consumes more. Preserve scatter support and existing per-object
size limits until a separate backend-envelope change is qualified.

Physical capacity needs a race-safe reserve guard shared by raw and user
allocation paths: ordinary allocations cannot consume the recovery reserve just
because unrelated raw kernel allocations are absent from PG_USER totals. Track
ordinary and critical consumers separately, reconcile against free buddy pages,
and serialize admission with allocation/reserve accounting. The existing pool
formula is a policy starting point, not evidence that today's raw allocator
already enforces such an inviolable partition. Critical kernel/device allocations
may draw on their explicit reserve; client-induced work in a SYSTEM server may
not select that class. Audit plain DMA call sites before classifying them; neither
"all DMA exempt" nor "all DMA user" is a safe migration rule.

HOSTMEM/device-local/aperture storage has a separate class and limit: it is not
RAM returned by free_pages. Account committed device resource bytes, aperture
occupation and retention independently, using driver-issued immutable resource
identity and an audited create/destroy receipt. Aliases cannot count as different
allocations. Pi scanout contiguous/CMA-like requirements, alignment and supported
formats are backend constraints supplied by the driver; no QEMU resource ID or
virtio completion stands in for a release fence on another platform.

A driver/backend must expose completion or safe reset/quiescence before a pin
can be released. On an unresponsive device keep it charged/quarantined and refuse
new allocations if needed; never free memory the device could still access.
Warden's recovery path may later reset a device, but accounting alone is not
permission to reset it or kill an application. Hardware qualification is required
before claiming Pi 400/500 support.

## 8. Pressure and recovery

Expose pollable, coalesced pressure state through a read-only account handle:
NORMAL, WARNING, CRITICAL plus generation, limiting resource/class, available
headroom, current usage/reservations and a suggested reclaim target. Own/subtree
visibility only; global reports omit other users' identities/content. Consumer
slowness cannot create an event queue proportional to allocation activity.
Reading the current snapshot after registering interest closes the lost-wakeup
race. Level changes, not a busy 100Hz loop, drive reclamation.

Proposed initial tunable thresholds: WARNING when applicable headroom falls
below 1/8 of its limit; CRITICAL below 1/32. Leave each level only after an extra
1/64 of the limit becomes free. Evaluate physical ordinary headroom, account
allocation capacity and retention capacity separately; report the tightest.
Allocation failure triggers a pressure generation immediately, including buddy
fragmentation/device-class failure even if aggregate byte headroom is healthy.
These fractions are deployment heuristics, not security invariants or research
constants. Configuration and observed transitions must be visible in diagnostics.

Halcyon/Tapestry response order: suspend disposable hidden buffers; trim caches;
stop speculative/preallocated graphics work; attempt the requested visible buffer
once after actual reclaim progress. Never reclaim semantic terminal transcripts
as if they were pixel caches. No automatic retry storm or eager cache refill when
NORMAL returns. A cancelled async operation may still retain storage until local
retirement: only its actual accounting release counts as reclaimed progress.

The allocation path does not wait indefinitely for applications to cooperate.
Kernel reclaim of its own safe caches may be bounded; userspace reclaim is
notified asynchronously. If admission still fails, return a precise error and
preserve the existing workspace. Existing lazy private-page fault OOM policy is
unchanged in this arc; we do not claim fallible malloc handles every later page
fault. No swap, global OOM victim selection or implicit privilege elevation.

## 9. Graphics admission and error reporting

Before creating a new generation, calculate checked stride x height x buffers,
rounded backend backing, mapping metadata and temporary old+new overlap. Reserve
that actual peak; do not assume "two 64 MiB generations" describes every desktop.
A backend can negotiate a supported layout/buffering count, but silently lowering
image quality or changing presentation guarantees is not an allocator decision.
Keep the old displayed generation if a replacement cannot be funded. A confirmed
hidden generation can be retired before replacement, using TAPESTRY-STORAGE.

For scale: three tightly packed 1280x800 RGBA buffers are 11.72 MiB; three 3840x2160
buffers are 94.92 MiB, before stride/allocator overhead. These are calculations,
not runtime qualification. Today's 64 MiB-per-weave limit means a single such4K
triple-buffer weave is still inadmissible even after replacing 128 MiB aggregate
policy. Split/per-buffer objects or a larger backend envelope is a subsequent
explicit graphics contract, not a hidden consequence of this proposal.

New versioned fallible allocation/map APIs return a structured outcome with
request ID, resource class, rounded requested amount, available amount, limiting
account (scoped identifier) and one of: AccountLimit, RetentionLimit,
PhysicalPressure, ContiguousUnavailable, DeviceBudget, MetadataLimit,
UnsupportedLayout, PermissionDenied or StaleGrant. Legacy APIs retain their
return shape. Do not implement a race-prone per-Proc "last error" shared by threads.
Halcyon turns these into a concise notice and an explicit retry; no content or
cross-user usage leaks into diagnostics. Query includes retained-after-exit and
awaiting-device-release bytes so invisible retention is explainable.

## 10. Migration and verification

MM-0: ratify account/voucher/pressure contracts and ABI mirrors. Document the
whole holder inventory and allocation classes; record source tests for today's
aliases, partial map, orphan pins and DMA bypass before changing enforcement.
MM-1: durable accounts and exact physical allocation ledger in shadow/diagnostic
mode, retaining128 MiB enforcement. Reconcile actual rounded blocks against buddy
and PG_USER; validate account exit and rollback. No silent policy relaxation.
MM-2: retention claims, vouchers, spawn/RFMEM/exec inheritance and class-aware
allocation; validate shadow counters against an independent reference model.
Activate new accounting only as a coherent set for all affected share paths.
MM-3: pressure snapshots and Halcyon/Tapestry allocation receipts; actual reclaim,
failed reveal/retry, backend generation and physical F10 preservation.
MM-4: remove the 128 MiB constant from the general admission path only after the
new defence passes. Default ceiling derives from capacity; explicit constrained
profiles remain possible. Legacy operations enter the same accountant, never a
bypass. Before activation the old limit stays in force; rollback across a running
machine is not allowed to free existing mappings.

Required tests include aliases/subrange pins/wrapper aliases; mapper and sponsor
exit in both orders; last backend fence after both exit; stale vouchers and
cross-session attempts; duplicate consume; failed multi-block allocation; physical
rounding; concurrent sibling reservations and quota lowering; fork/RFMEM/exec;
full metadata/notification queues ; 1000 reallocate/cancel/reconnect rounds; physical
pool exhaustion versus fragmentation; and permission failure with no information
leak. Check every counter returns to baseline only when the real final holder
releases, and quota is never returned twice. Include model/source mutants for
refund-before-fence, uncharged orphan, alias-double-count and fork-budget reset.

Production gate: multiple users, adversarial retained old generations, resize/
hide/reveal storms, multiple resolutions and actual backend limits, SAK under
memory pressure, service crash/restart and long-run idle/peak footprint. Qualify
QEMU and later Pi separately. ASan/UBSan/SMP follow current repository policy;
host tests alone cannot establish hardware fence correctness or UI usability.

## 11. Ratified choices (October 4)

The operator approved the durable hierarchical accounts, restricted allocation vouchers,
whole-retention-unit charging, capacity-derived unconfined ceilings, explicit
reserve classification, pressure notifications and failure-preserving recovery.
The initial depth 16 and pressure fractions are tunable proposed policy, exposed
rather than disguised as hardware facts. No guaranteed per-user partition,
automatic process termination or broad new imperium capability is proposed.
This is a substantial resource-lifetime implementation, staged alongside the
async work; neither design is claimed implemented by the clipboard prerequisite.

## 12. MM-0 inventory: what holds and allocates today

Read from the code at f44b1d198 (November 3), every call site, by function so
the anchors survive edits. Nothing here is a decision until the operator ratifies
it, except where section 7 already rules. The last column of each table is a
proposal. `kernel/test/test_mm0_holders.c` pins today's four behaviours section 10
names; those tests must turn red when MM-2 changes them, and be changed with it.

Census method and its controls: `burrow_ref(`, `kobj_dma_ref(`, the mapping-count
increment, every struct field typed `struct Burrow *` or `struct KObj_DMA *`, and
every cast to those types. The casts found two holders the field search cannot see,
because their field is `const void *backing` (`struct loom_pool_extent`) or a
handle slot's `obj`. Local arrays such as `pins[]`, `old[]`, `orphans[]` and `drop[]`
carry an existing reference to its release outside a lock and hold nothing new.

### Holders of backing

| Holder | Site | Retains | Charged today | Proposed claim |
| --- | --- | --- | --- | --- |
| Shared-in VMA | `burrow_share_into` | whole object; a cut is refused | `shared_map_pages`, per VMA, so aliases count twice | one retention membership per account, refcounted by alias |
| Other VMA | `vma_alloc` to `burrow_acquire_mapping`; every `burrow_map`, a fork's clone, a cut's tail piece | whole Burrow by mapping count | payer's `page_count` through the charge record, for eager anonymous; lazy per resident page | the space's existing constraint; R claim when the backing is foreign |
| Burrow handle | `handle_acquire_obj` | whole Burrow | nothing | R claim on the holder's account |
| DMA handle | `handle_acquire_obj`; `sys_seat_import_for_proc` for an imported one | whole KObj_DMA | nothing | R claim, by object identity |
| DMA wrapper Burrow | `burrow_create_dma`, one per SYS_DMA_MAP | KObj_DMA | nothing; an owner's map is not shared-in | same membership as the object, never per wrapper |
| Weft share slot | `weft_share_register`, 64 slots machine-wide, owner death reclaims | whole Burrow | nothing | outstanding-grant claim on the sharer |
| Weft binding | `struct weft_binding`, the slot's pin moved at claim | whole Burrow until `dev9p_close` | the mapper's shared-in VMA only | follows the claimant; must exist before the slot's claim drops |
| Loom ring | `struct Loom.ring` | ring Burrow | charge record | ring metadata, requesting account |
| Legacy registered buffer | `struct loom_reg_buffer`, `loom_resolve_buf` | whole Burrow until re-register or `loom_unref` | nothing new | registered-buffer claim |
| Private fixed buffer | `struct loom_private_fixed` extent | whole Burrow until settled | nothing new | registered-buffer claim |
| Private pool member | `struct loom_pool_extent` | whole Burrow until settled | nothing new | registered-buffer claim |
| I-30 per-operation pin | `pinned_buf` at submit | whole Burrow for the operation | nothing | covered by its buffer's claim, section 6's borrower |
| Exec image cache | `image.c`, one handle ref per entry | FILE Burrow | clean-page policy, section 3 | bounded system cache account |
| vDSO | `g_vdso_burrow` | one Burrow for the boot | boot | boot account |
| Syscall and fault temporaries | JIT sync, `weft_share` registration gap, `freq->burrow` | across a lock gap in one call | nothing | none; inside a live claim, never escaping |

The kernel has no type for section 6's queued backend command, CPU composition
borrow or display/GPU pin. Each reaches it as a row above, held by a service. The
production driver is `usr/lictor`; `usr/virtio-gpu` is a probe.

- Display/GPU pin: lictor's `Pin` (`backend/import.rs`) is a DMA handle from
  SYS_SEAT_IMPORT, so it survives its owner's exit, and `allowance_handle_alloc`
  charges nobody for it. It goes on a successful detach, a proven unref that `reap`
  sees retired, an attach proven to have left nothing, or at `Device` drop after a
  GPU reset. Two cases have no way out short of that drop: an UNREF `drain_condemned`
  finds refused on a live engine, which it forgets on the reading that a second
  UNREF of a freed id is harmless, and an unref past the condemned list's overflow,
  which answers Ok and records nothing. This is section 6's "a last-client-unmap does
  not release a backend fence pin", today, with no account behind it.
- Retirement against a busy engine: `gpu_api::admit` parks it first, in the
  connection's one `pending` slot, until every chain has retired, quiescence becomes
  unreachable, or `PARK_BOUND_MS` (30 s, per request). `Device::execute` then refuses
  it on a non-quiescent engine and keeps the pins.
- Queued backend command: a BO's retirement waits on `fences_in_flight`, deferring to
  `warp_pump_retires`. Abandoned work poisons the context, and the backing parks in
  `warp_park_leaked` until vindicated. A surface's own retirement in `release_gen`,
  `retire` and `free_screen` issues its detach and unref, discards both results and
  closes its handle, relying on lictor's pin; nothing retries a refusal there.
  `gl_evict_res` unbinds a display binding and frees nothing itself. `wring_destroy`
  takes the slot and unshares, then a host3d ring goes through `retire_host3d_ring`,
  a guest ring through `resource_unref`, unmap and close. Lictor's pin outlives both
  until `reap`.
- Client gone: `retire_conn` calls `retire`. `reconcile` has no client-gone test and
  releases backing only by retiring a surface with a wedged event queue. Its
  `storage_sync` offers a suspend, and only to a hidden, unbound surface.
- CPU composition borrow: the source is tapestryd's own mapping of the weave it
  created or of a GL BO's backing, read synchronously on its main thread, in a present
  and also in repaint, the menu card and readback. Its own `retire` runs from the same
  loop, so it cannot land in a read, and it unmaps and closes last. A client's exit
  or unmap can land mid-read on another CPU, and tapestryd's mapping keeps the pages
  (I-7's rule, not separately traced here). So it is section 6's borrower inside a
  live claim, and that claim is tapestryd's.
- Displaced generation: parked by tapestryd while it is still scanned out.

The RPC to lictor blocks with no deadline: `Client::call` reads its reply on a plain
fd, and the kernel clears the connection's deadline after the handshake. A caller
keeps what it holds across the call: `attach_backing` holds one of Weft's 64 share
slots, and `release_gen` its mapping and DMA handle. A retirement in lictor's park
therefore stops tapestryd's main thread, which is GPU-DESIGN 4.5.15's stated price
during a stall. The seat worker has its own client and seat requests are always
admitted, so SAK is not behind it. An abandoned fence raises that price with no bound
on its count: OPEN-BUGS 2026-11-03T11:53Z.

Not yet traced here: the kernel half of the mapping-count rule for the CPU borrow, and
`wbo_retire`'s RPCs.

### Allocation classes

Pages charged to the capacity pool (`alloc_user_pages`, PG_USER): `burrow_create_anon`,
lazy commit and pagemap nodes, the COW and demand paths in `arch/arm64/fault.c`,
user page tables (`user_table_alloc`), JIT commit. They stay ordinary, with the
requesting account as sponsor; an exempt caller keeps its exemption until section 7's
reserve guard replaces it.

Raw `alloc_pages`, outside the pool:

| Site | Induced by | Proposed class |
| --- | --- | --- |
| `dma_create_body`, weave | Tapestry: each surface's create, reweave and resume, and each screen; a mode preflight, closed at once | ordinary, voucher-funded; a screen from the operating budget, section 5 |
| `dma_create_body`, GPU BO | Tapestry: client BOs and guest-blob rings; two probes | ordinary, voucher-funded; a probe from the operating budget |
| `dma_create_body`, plain: rings, event and packet pools | lictor, netd, nocturned, stratumd's virtio-blk (`bdev_thylacine.c`), at start-up; sizes fixed | critical device, each driver's operating account; no client can grow them |
| `dma_create_body`, plain: lictor framebuffer | one per screen, w x h x 4 | display operating budget, section 5 |
| `dma_create_body`, plain: lictor cursor | first cursor request, once, fixed size | display operating budget |
| `kstack_alloc` | every thread; bounded per Proc | metadata, requesting account; a thread that cannot pay is refused at create |
| `proc_pgtable_create` | every address space, one L0 page | metadata, requesting account |
| `mm/slub.c` slabs and large allocations | everything | metadata; existing bounds, then charged membership records, section 3 |
| `file_demand_page_cluster` stage | a file fault; freed before return, falls back to one page on failure | transient ordinary; must never draw on the reserve |
| `virtio_virtqueue_create`, `random.c` pull page | kernel virtio-rng at boot | boot account, critical |
| `gic_its.c` tables, `kpage_alloc` | ITS bring-up | boot account, critical |
| `mmu.c` L2 and L3 demotions | boot's `mmu_pagemap_directmap`. A thread's kstack guards reach the same walker from a user spawn, but the boot pass has already made every zone entry a table, so the walker returns early and allocates nothing. No other caller found. | boot account, critical |

HOSTMEM (`burrow_create_hostmem`) is a PCI BAR extent and allocates no RAM: its own
class, section 7.

### What the four pins showed against this document

Section 10's "partial map" is narrower than a client's pin. A shared-in cut is
refused, so a client holds and pays for the whole object. Only an owner's own DMA
map, placed inside the burrow window, can be cut down to one page while retaining
every page; outside the window a hardware map detaches whole. The charge today is
none in either case, which is the defect the retention unit closes.

## 13. The voted interface

Operator votes 47 and 48, November 3. Section 4's proposal stands except where this
says otherwise.

**One operation on a typed handle.** `SYS_ACCOUNT(handle, op, rec_va, rec_size)` is
one syscall number, taken when the layout below is reserved in code. It works on a
`KOBJ_ACCOUNT` handle or a self selector for the caller's own account, so an
application needs no handle to query or to be charged. A file tree was the other
candidate, and Plan 9's idiom. It lost because a path can name an account, which
section 4 forbids, and because all-or-nothing limit changes and structured outcomes
would become parsing, with the allocation path still needing a binary call.

A read-only `/proc/<pid>/account` gives the shell and diagnostics a text view, under
the /proc gates. It confers no authority, and nothing may parse it to decide policy.

**Pressure through poll.** `poll` on the handle replaces section 4's ACCOUNT_WATCH.
Section 8's rule, read the snapshot after registering interest, is then poll's own.

**A voucher is bound to one connection.** There is no voucher handle. The client
calls `VOUCHER_CREATE` naming its end of a /srv connection and receives a
non-repeating 64-bit id, which it sends in band on the service's protocol. The server
calls `ALLOCATE_FUNDED` naming its own end, and the kernel checks that the two ends
pair and the id has funds left. The id alone grants nothing, the server cannot pass it
on, and connection death returns what is unspent, as section 5 requires. Handles
cannot move between Procs today (`handle_transfer_via_9p` is named in comments and not
built), and this needs no such path. Genode's session funding has the same shape:
quota travels with the session.

The layout, to be pinned with `_Static_assert` and mirrored in libthyla-rs:

```c
struct account_rec_hdr { u32 version; u32 size; u32 flags; u32 pad; u64 incarnation; };
enum { ACCOUNT_QUERY=1, ACCOUNT_CHILD=2, ACCOUNT_SET_LIMITS=3,
       VOUCHER_CREATE=4, VOUCHER_CLOSE=5, ALLOCATE_FUNDED=6 };
enum { CLASS_ALLOC=0, CLASS_RETAIN=1, CLASS_META=2, CLASS_DEVICE=3, ACCOUNT_CLASSES=4 };
struct account_class { u64 used, reserved, limit; };
struct account_query {
    struct account_rec_hdr h;
    u64 generation; u32 level; u32 limiting;
    u64 headroom, reclaim_target, retained_after_exit, awaiting_device;
    struct account_class c[ACCOUNT_CLASSES];
};
struct account_outcome { u64 request_id; u32 cls; u32 reason; u64 requested, available, limiting_account; };
```

`flags` is zero and `incarnation` is the expected account or object, with 0 for self.
Amounts cross the boundary in bytes, with checked page rounding inside, per section 4.
`ACCOUNT_SET_LIMITS` carries the whole class vector, so all-or-nothing is the record's
shape. `reason` takes section 9's nine values, 1 to 9 in that section's order, so a
record never filled in names none. `level` is 0 NORMAL, 1 WARNING and 2 CRITICAL, and
`limiting` is the class that is tightest. The self selector is -1, since no handle is
negative.

`SYS_ACCOUNT` is 140, reserved at MM-0 and given its dispatch arm at MM-1. Linux's 140 is
setpriority, which has no row, so a Linux Proc takes ENOSYS before the native switch,
reaches no account operation and is charged without one. The kernel's `thylacine/account.h`, libt's
`thyla/account.h` and libthyla-rs's `account` are held to one byte record by
`tools/test-account-abi.py`. Records for the operations other than the query come with
those operations, and the version and size fields are what lets them.

**A child account's control stays with the Proc that made it.** Operator vote 58,
November 4. Any Proc may `ACCOUNT_CHILD` its own account through the self selector and
receives a `KOBJ_ACCOUNT` in its own handle table. The kind is non-transferable, like
Loom, and fork does not copy it. The Proc names that handle in a new `SPAWN_EXT_ACCOUNT`
tail on spawn's record, and the new Proc is born under that account. A Proc's account is
set only at birth and never changes. `ACCOUNT_SET_LIMITS` needs the handle, so nobody
adjusts their own account. Login makes the session's child and holds its control, and
joey makes the resident services'. Each level subdivides what it has, so authority only
narrows, and no handle crosses a Proc boundary. Genode's quota trading has this shape:
budget flows down the parent chain, with no ambient id. Each account pays META, and a
Proc's breadth is bounded by `PROC_HANDLE_MAX` while its handles hold them; depth is
section 6's sixteen.

If the creator exits, the limits it set stay and nobody can change them. Halcyon settings
that adjust a session go through login, or wait for a transfer path. Two other routes
lost. Adding the kind to spawn's descriptor array moves it to the transferable side of
I-4 and I-5, widens fork's copy unless it is special-cased, and changes what
`specs/handles.tla`'s partition means. Waiting for `handle_transfer_via_9p` would hold
MM-2 behind a large path nobody has built.

**A sandbox reads its own chain.** Operator vote 59, November 4. The query reports the
caller's account and what bounds it from above. Until sessions have child accounts that
chain is the root, so a Proc with no `/ctl` reads machine totals, and other sessions'
allocations and thread creations show as steps. It reaches no address, lock or per-key
data. Giving sessions children narrows this to the session's. No capability gates the
query and its figures are not coarsened, because a program should be able to ask its
budget rather than find it by allocating until refused. A sandbox that should see less
is given its own child, and then sees only itself.

Not settled here: whether `ALLOCATE_FUNDED` returns a `KOBJ_DMA`, a Burrow or either by
class; and whether a map needs its own operation or `burrow_share_into`'s callers charge
inside it, section 4's MAP_ACCOUNTED (MM-2).

## 14. MM-1 as built

Section 10's first step: durable accounts and an exact physical ledger, watching only.
The 128 MiB limit and the capacity pool decide everything they decided before.

**Written, not yet booted:** the account tree, every Proc's account, a sponsor on every
pool block and DMA block, thread pool pages as `CLASS_META`, and a test that the ledger
equals the pool to the page across every allocation since boot.

**The ledger takes no lock.** Section 6's transaction lock is for reservations, and its
rule is that no account lock is taken under Burrow, VMA or driver locks. An allocation
lands under `as->lock` in a fault, so a physical charge is a walk of relaxed atomics up
an immutable parent chain. MM-2 keeps that for the settled charge and adds the lock for
the reservation before it. A shadow counter can be read a moment out of step with the
pool; nothing decides on it yet, and MM-2 must not decide on a raw read of it.

**A sponsor rides the block's head page,** in a union with `slab_cache`, so `struct page`
stays 48 bytes. `free_pages` is the one way off, as it is for the pool. Each block holds
a reference to its account, which is what makes section 4's orphan rule true without
any list of orphans.

**The requesting account pays,** which is the current thread's Proc's. File pages and the
map nodes that index them take a static system cache account on both fault paths, section
3's rule. A thread's pool pages
are charged to its payer and remembered on the thread, because its reaper runs in another
context.

**A broken pairing does not stop the kernel,** because at this stage nothing depends on
the ledger. It is counted, and the test runner reddens the test it happened under. That
changes when a decision first reads the ledger.

**The query is the whole of the call, also not yet booted.** `ACCOUNT_QUERY` on the self
selector is served. Any other handle is `-EBADF`, since `KOBJ_ACCOUNT` does not exist,
and the other operations are `-EOPNOTSUPP`. A stale incarnation is `-EINVAL`, because the
error registry has no `ESTALE` and this is not worth a change to it.

`used` is the account's and its descendants', in bytes. Nothing charges `CLASS_RETAIN`
or `CLASS_DEVICE` yet. An account has no limits of its own until MM-1c, so `limit` is
section 4's default: the pool for a RAM class, and all ones for device storage, which
has no budget yet. `headroom` is the least of each class's room at each level up the
chain and the pool's own room, floored at zero, and `limiting` names the class of the
chain's tightest room even when the pool is tighter still, because the pool is no class.
`generation`, `level` and the three reclaim figures are zero, as no pressure machinery
exists. The header is read once and the whole record then written, so a peer thread
cannot steer a check, and a fault on the way out may leave a part-written record, which
`-EFAULT` tells the caller not to trust.

**`/proc/<pid>/account` takes `sched`'s posture:** 0400, owner or `CAP_HOSTOWNER`, and it
reads through a dump seal. Section 13 says only "the /proc gates". What a session holds
is another user's business once sessions have accounts, so the stricter of the two
postures was chosen, and it can be relaxed without breaking anything. It renders the same
fill the call returns. `/ctl/memory` gains `ledger:` and `unpaired:`.

**A wrong order is visible.** The ledger refunds the block's own order and the pool the
caller's, on purpose: if both took the caller's, a block freed at the wrong order would
leave them equal. It is also counted in `unpaired`, with a refund past zero and an account
dying charged. Section 10's reconciliation walks the frames in a test, so the ledger is
held to the blocks that exist and not only to the other counter.

**`headroom` can read low.** `CLASS_ALLOC` counts DMA blocks outside the pool, and its limit
is the pool, so a class's room can reach zero while the pool has room and allocations
succeed. That is honest for a figure that decides nothing. MM-2 must not admit on it as it
stands, and critical device DMA stays outside the pool even after section 7's routing.

**Until sessions have children, every Proc's account is the root,** so a query shows
machine totals. `/ctl/memory` already shows them to every process, but a sandboxed Proc
whose namespace has no `/ctl` learns them only through this call. MM-1c ends that for a
session and leaves a sandbox seeing its session's totals.
