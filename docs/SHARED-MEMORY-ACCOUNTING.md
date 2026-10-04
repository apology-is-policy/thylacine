# Capacity-backed shared memory and retention accounts

Status: PROPOSED for operator ratification, October 4, 2026. Design only.
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
rights. Reserve the concrete layout and C/Rust mirrors at MM-0.

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

## 11. Binding choices requested

Approve the durable hierarchical accounts, restricted allocation vouchers,
whole-retention-unit charging, capacity-derived unconfined ceilings, explicit
reserve classification, pressure notifications and failure-preserving recovery.
The initial depth 16 and pressure fractions are tunable proposed policy, exposed
rather than disguised as hardware facts. No guaranteed per-user partition,
automatic process termination or broad new imperium capability is proposed.
This is a substantial resource-lifetime implementation, staged alongside the
async work; neither design is claimed implemented by the clipboard prerequisite.
