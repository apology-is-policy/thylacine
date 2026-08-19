# 110 — Per-Proc resource floor (the DoS bound)

**Status**: implemented (#65, RW-12 D4 pre-rc). Invariant **I-32**. Scripture:
`ARCHITECTURE.md §28` (I-32) + `§25.4` (audit-trigger row), `IDENTITY-DESIGN.md
§3.8`.

## Purpose

The resource floor caps a **non-TCB** Proc's resource use so a fork / thread /
memory bomb hits a clean per-Proc limit instead of stressing the kernel
allocator toward the box-killing cliff. It is a **resource axis, not a privilege
axis** — it confers and bypasses no capability, so it is orthogonal to I-22 and
the §3.7.1 privilege model. It is the policy floor over an already-present
*graceful-OOM backstop*; together they give "a bomb is bounded, not
box-extincting."

The floor lives at the per-Proc layer; the global / per-user **aggregate** quota
(cgroup-equivalent) that sums these counters is a recorded SEAM, not built at
v1.0.

## The two layers

1. **Graceful-OOM (the backstop).** Every user-reachable creation path fails
   gracefully — `proc_alloc → NULL` (rfork `-1`), `thread_create → NULL`,
   `territory_clone → NULL`, `burrow_create_anon → NULL`, and a
   pgtable-on-fault allocation failure → `mmu_install_user_pte` `-1` →
   `userland_demand_page` `FAULT_UNHANDLED_USER` → `proc_fault_terminate` (a
   *per-Proc* kill, not box extinction). This is the property that bounds a
   *recursive* cross-Proc fork bomb at the physical-memory cliff. #65 did not
   create it — it verifies and preserves it; the audit prosecutes that no path
   on these chains extincts on user-triggerable exhaustion.

2. **Per-Proc caps (the floor).** A single misbehaving Proc hits a clean limit
   *early*, and the SEAM quota layer reads the maintained counters.

## Public API (`kernel/include/thylacine/proc.h`, impl `kernel/proc.c`)

```c
// The cap constants (tunable floor; generous for any v1.0 user workload).
#define PROC_PAGE_MAX   65536u   // 256 MiB at 4-KiB pages
#define PROC_THREAD_MAX 256
#define PROC_CHILD_MAX  256

// The TCB exemption. True iff principal_id == PRINCIPAL_SYSTEM. NULL -> false
// (fail-closed). Unforgeable: a post-login Proc cannot acquire PRINCIPAL_SYSTEM
// (CAP_SET_IDENTITY rejects it) and principal_id is immutable on a running Proc.
bool proc_resource_exempt(const struct Proc *p);

// The anon-page counter. CALLER MUST HOLD p->vma_lock. charge returns true (and
// adds npages) if exempt OR the new total fits PROC_PAGE_MAX; false (charging
// nothing) if over cap or the sum would overflow u32. uncharge clamp-subtracts.
bool proc_page_charge(struct Proc *p, u32 npages);
void proc_page_uncharge(struct Proc *p, u32 npages);

// The spawn-gate predicates. Exempt -> true. Else read the counter under
// g_proc_table_lock and compare to the cap. NULL -> false.
bool proc_thread_cap_ok(struct Proc *p);   // thread_count < PROC_THREAD_MAX
bool proc_child_cap_ok(struct Proc *p);    // child_count  < PROC_CHILD_MAX
```

## Data structures

Two counters appended to `struct Proc` (`thread_count` is the pre-existing
third). The struct grew 264 → **272** bytes (deliberate `_Static_assert` bump +
offset asserts at 264 / 268):

| Field | Type | Write domain | Meaning |
|---|---|---|---|
| `page_count` | `u32` | `p->vma_lock` | live anon pages via `SYS_BURROW_ATTACH` **and** the `SYS_LOOM_SETUP` ring (audit F1) |
| `child_count` | `u32` | `g_proc_table_lock` | live direct children == `children` list length |
| `thread_count` | `int` | `g_proc_table_lock` | live threads (pre-existing) |

All three are read by a cross-Proc `/proc` / `/ctl` stat reader **without** the
per-Proc lock, so every read is `__atomic_load_n(_, __ATOMIC_ACQUIRE)` and every
write is `__atomic_*(_, __ATOMIC_RELEASE)`. Neither new counter is propagated by
`rfork` (KP_ZERO at `proc_alloc`).

## Implementation

### Exemption — `proc_resource_exempt`
The TCB (`PRINCIPAL_SYSTEM`: kproc + the boot/service chain — joey, corvus,
stratumd, pre-login) is unbounded so the floor cannot pinch the FS server, the
orphan-adopter, or the kthread root. A bomb is untrusted *post-login* code; the
exemption boundary is exactly the login boundary, and it is unforgeable.

### Page cap — `sys_burrow_attach_for_proc` / `sys_burrow_detach_for_proc`
(`kernel/syscall.c`). Anon is **eager** at v1.0 (`burrow_create_anon`
`alloc_pages` up front), so `SYS_BURROW_ATTACH` is the single commit point.
Under `vma_lock`, after the gap is found and `npages` is known, `proc_page_charge`
runs *before* `burrow_create_anon` — so an over-cap request is refused with
`-ENOMEM` and **allocates nothing**. Because the check + charge happen under the
same `vma_lock` that serializes sibling attaches, the page cap is **exact** (no
TOCTOU overshoot). Every failure path after the charge (create-fail, map-fail)
and a successful `SYS_BURROW_DETACH` (`rc == 0`) uncharge the same rounded
`npages`.

Page-cap **scope**: every user-controllable, repeatable anon-page commit.
**Counted**: `SYS_BURROW_ATTACH` regions, **and** the `SYS_LOOM_SETUP` ring
(audit F1 — `sys_loom_setup_for_proc` charges `ring_size/PAGE_SIZE` at setup,
because the ring is EL0-reachable, repeatable, and the handle slot is reused on
close while `mapping_count` keeps the ring VMA alive; without the charge a
non-TCB Proc accumulated uncharged anon to the physical cliff). **Not counted**
(each separately bounded, none a repeatable bomb): pgtable sub-tables
(transitively bounded by mapped VA ≤ `page_count`), kstacks (bounded by the
thread cap), the exec image / user stack (one-shot at spawn, bounded by the
binary + `EXEC_USER_STACK_SIZE`, transitively bounded by the child cap across
children).

**The charge unit is the buddy's occupancy, not the request (#106).** Every eager
charger bills `burrow_backing_pages(size)` — `1 << order_for_pages(page_count)`,
the count `alloc_pages` actually takes out of the buddy — rather than
`size / PAGE_SIZE`. The four eager chargers are `SYS_BURROW_ATTACH`,
`SYS_JIT_CREATE` (and its destroy-side refund, recomputed from the VMA span), the
`SYS_LOOM_SETUP` ring (whose `ring_size` is page-rounded but *not* power-of-two
rounded — it is the sum of four 64-aligned regions), and the detach ANON arm. The
lazy-anon fault arm is the one charger that must **not** use the helper: it
allocates order 0, so its per-page charge is already exact.

Until #106 this section documented the gap as an accepted property — "charged as
the logical page count; physical commitment is ≤ 2×; precise-RAM accounting is
the SEAM's job". That reading was wrong about what I-32 is for. The invariant
bounds *physical occupancy* (it is a DoS floor, not a VA accountant), and a Proc
attaching 2049-page regions occupied 4096 pages each while being billed 2049 —
so `page_count` could read at most `PROC_PAGE_MAX` while real occupancy
approached twice that. Bounded at 2× (the next order is never more than double),
which made it an understated floor rather than an unbounded hole — and which is
presumably why it read as tolerable. It was not: the number the floor reports has
to be the number the machine gave away.

The **waste** the rounding causes is still deliberate and stays. A Burrow's
backing must be one physically contiguous run — exec's direct-map alias,
`loom_create`'s `ring_kva`, and the weft ring view all index `v->pages` as a
single chunk — and a buddy allocator buys that contiguity with power-of-two
rounding. Accounting for the waste is the fix available; eliminating it would
mean giving up contiguity. `burrow_backing_pages` shares `order_for_pages` with
`burrow_create_anon` / `burrow_create_code`, so the charge and the allocation
cannot drift apart, and `burrow.backing_pages_matches_alloc` pins the agreement
against a real Burrow's recorded `v->order`.

**The refund is a positive allowlist (#122).** `SYS_BURROW_DETACH` refunds
`page_count` only for the two VMA shapes that ever charged it — a non-`SHARED_IN`
`BURROW_TYPE_ANON` (the rounded occupancy) or `BURROW_TYPE_ANON_LAZY` (the
resident count) — and zero for everything else. The previous shape ("everything
except ANON_LAZY gets `length / PAGE_SIZE`") refunded two classes that were
charged elsewhere or not at all: a `SHARED_IN` VMA from `SYS_WEFT_MAP`
(`burrow_share_into` charges the client's `shared_map_pages` and deliberately
leaves `page_count` alone, yet places the VMA in the burrow-attach window, and a
shared Burrow's type is ANON), and an MMIO/DMA map (both take a caller-supplied
`vaddr`, so a `CAP_HW_CREATE` driver can place one in the window).
`proc_page_uncharge` clamps at 0 so it never wrapped, but a Proc could loop
map/detach to drive its own `page_count` to zero while its real occupancy was
unchanged — the same "drift, not wrap" reasoning that made the CODE alias a
finding. Listing what *did* charge, rather than subtracting what didn't, also
means a future Burrow type is uncharged by default.

**The refund is ATTRIBUTED, not inferred (#131/#132).** The allowlist above
reasons about a region's *shape* to decide whether it was charged. That works
only while shape determines payer, and it stopped doing so the moment a region
could be reached from two Procs. `struct Burrow` therefore records who paid:

```c
int charge_pid;    // the paying Proc's pid; 0 with charge_pages == 0 == released
u32 charge_pages;  // what it paid (the buddy-rounded count)
bool shared_out;   // burrow_share_into has mapped this into a SECOND Proc
```

`burrow_charge_record` stamps it at each eager charge; `burrow_charge_claim`
read-and-CLEARS it (so exactly one settler ever refunds — two racing paths cannot
both win, and a double refund is an under-count, the direction that inflates a
budget); `burrow_charge_restore` puts a claim back when the caller decides not to
settle. Callers claim **before** the drop, because a freeing drop takes the record
with it, and they snapshot the *Burrow* pointer rather than the VMA —
`burrow_unmap_reporting` frees the `Vma` struct, so `vma->burrow` is dangling the
moment it returns.

`charge_pages`, not `charge_pid`, is the "held" sentinel: `proc_alloc` stamps pid
0 and `rfork_internal` assigns the real one later, so pid 0 is a legitimate
identity and using it as the released marker would conflate identity with state.

Two defects motivated this, and they are the same gap seen from opposite sides:

- **#132** (introduced by #130's own fix): `loom_register_buffers` and
  `loom_free` refunded a displaced registered-buffer pin to the Loom's owner,
  justified by "registering requires a loom fd from p's own table, and KObj_Loom
  is neither transferable nor dup-able". That argument proves p owns the *Loom*
  and says nothing about who paid for the *buffer* — and `loom_resolve_buf`
  admits any writable ANON VMA, which a weft ring netd allocated and
  `burrow_share_into`'d satisfies exactly (Weft-6c-1's shipped path registers the
  whole ring). So a consumer could be refunded for the sharer's pages: an
  under-count on a non-exempt Proc, repeatable via the public `WeftFlow`/`Ring`
  API. The claim returns 0 for a region the caller did not pay for, which closes
  it at the mechanism rather than at each site.
- **#131**: nothing settled the sharer's charge at all. netd detaches its ring at
  flow close while the guest's mapping and the binding pin live on, so the last
  drop is the *guest's* `vma_drain` — generic vma code, in another Proc, under
  that Proc's `vma_lock`, with no way to name netd. 64 pages leaked per closed
  zero-copy flow.

**The release rule (user-voted 2026-08-03): the charge follows the sharer's own
claim, not the pages.** `SYS_BURROW_DETACH` settles when `freed || shared_out`.
`freed` is *sufficient* (if nothing holds the region, this Proc certainly does
not) but not *necessary*: once the region is shared out and this Proc has
unmapped it, the Proc can no longer reach those pages, and charging it for
memory it cannot touch caps it for nothing. From that point the region is bounded
by the consumer's `shared_map_pages` — the fifth axis exists for exactly this.

`shared_out` rather than "does anything still hold it" is load-bearing: the
Proc's **own** other claim (a Loom registered-buffer pin on its own buffer) also
keeps the region alive, and there the charge must stay until that claim drops.
The two cases are indistinguishable by refcount and distinguishable by this flag.

The never-claimed share settles at `weft_share_unregister` /
`weft_share_release_owner` — the sharer's registration pin is its last claim, and
both know the owner. `weft_binding_release` settles nothing: that pin belongs to
the consumer, which never paid. Precedent: Linux memcg keeps the charge with the
allocator (with reparenting on death); seL4 lets it follow the capability holder;
Zircon counts shared pages in every mapper. Thylacine's dual axis (`page_count` =
the allocator's commit, `shared_map_pages` = the mapper's pin) took the seL4
answer for the sharer half.

Known window: a share landing between the `shared_out` read and the drop leaves
the sharer charged until its next release point — an over-charge, never a wrong
refund, so the race degrades safely.

v1.0 has no mid-life `vma_drain` (no in-place re-exec), so attach/detach (and the
ring's detach / `vma_drain`) balance the counter while the Proc lives; at exit the
Proc and its counter vanish together (`vma_drain` is the SEAM hook where a future
aggregate would uncharge).

`shared_map_pages` (the fifth axis) is deliberately **not** rounded: it bounds a
client's cross-Proc *mapping* and pin, and the pages are the sharer's commit —
already rounded on the sharer's side. Charging the client the rounding too would
double-count it against a Proc that did not cause it.

### Thread cap — `sys_thread_spawn_handler`
(`kernel/syscall.c`). `proc_thread_cap_ok` is checked after argument validation
and before `thread_create_user`, refusing `-EAGAIN` (the POSIX `RLIMIT_NPROC`
convention). kproc is already excluded at the handler top. The thread cap is the
tightest of the three because each thread pins `THREAD_KSTACK_TOTAL_SIZE` (32
KiB) of **unswappable** kernel kstack (256 → 8 MiB).

The thread cap covers `SYS_THREAD_SPAWN` (the only EL0 thread-create path)
**and** kernel-side kthreads spawned on a Proc's behalf — at v1.0 only the Loom
SQPOLL poll-thread (`SYS_LOOM_SETUP | LOOM_SETUP_SQPOLL`, which runs under the
exempt `kproc`). The SQPOLL kthread is charged to the **creator** via
`proc_sqpoll_charge` (`Proc.loom_sqpoll_count`, settled exactly once by
`loom_free` under the `Loom.sqpoll_charged` latch), and `proc_thread_cap_ok`
sums `thread_count + loom_sqpoll_count` — the Proc's workers, wherever they
run (fid-lift audit F1). History: the original F4 disposition left SQPOLL
kthreads uncounted because they were *transitively* bounded by the handle
table at `PROC_HANDLE_MAX` = 64; the #198 lift to 1024 quadrupled that
transitive bound past the thread cap itself (1024 rings × 32 KiB kstack = 32
MiB of unswappable kstack), which is what converted the disposition into a
real charge. Regression: `loom.sqpoll_charges_thread_budget`.

### Child cap — `rfork_internal`
(`kernel/proc.c`). `proc_child_cap_ok` is checked **early** — right after the
parent is captured, before the heavy `proc_alloc` / `territory_clone` /
`thread_create` — and refuses `-1` (rfork's convention). `rfork_internal` is the
*single* Proc-creation chokepoint (every `SYS_SPAWN_*` variant routes through it
via `rfork_with_caps`), so no spawn variant escapes the cap. `child_count` is the
length of the `children` list, maintained at `proc_link_child` (++),
`proc_unlink_child` (--), and rebased in `proc_reparent_children` (adopter += N).

### The bounded TOCTOU overshoot (thread + child only)
The thread/child checks read the counter under the lock, release it, then the
counter is incremented at a *later* lock hold (`thread_link_into_proc` /
`proc_link_child`). So N concurrent creators can each pass the check and overshoot
the cap by ≤ ncpus−1. This is acceptable for a *floor* (a bound, not an exact
accountant) and is documented at each site. The **page** cap has no overshoot —
its check + charge are under one `vma_lock` hold.

## State / control flow

```
SYS_BURROW_ATTACH(len)        SYS_THREAD_SPAWN              rfork/SYS_SPAWN_*
  vma_lock                      validate args                 capture parent
  vma_find_gap                  proc_thread_cap_ok? ──no──▶   proc_child_cap_ok? ─no─▶ -1
  proc_page_charge? ─no─▶ -ENOMEM  │ -EAGAIN                       │
  burrow_create_anon            thread_create_user            proc_alloc/clone/thread
  burrow_map                    ready()                       proc_link_child (++)
  (fail ⇒ uncharge)                                           ready()
SYS_BURROW_DETACH(rc==0)
  proc_page_uncharge
```

## Observability

`/proc/<pid>/status` gains `pages:` and `children:` lines (next to `threads:`);
the `/ctl` procs listing gains two trailing columns (`PID STATE THREADS PAGES
CHILDREN`). Both read the counters with `__atomic_load_n`. These are the SEAM
counters a future aggregate quota reads, and they reconcile the RW-12 W5-F8
finding (the memory-accounting seam was recorded against a nonexistent
`/ctl/mm/` node — the real v1.0 surface is this per-Proc stat).

## Error paths

| Return | Site | Trigger |
|---|---|---|
| `-T_E_NOMEM` (−12) | `sys_burrow_attach_for_proc` | non-exempt, `page_count + npages > PROC_PAGE_MAX` |
| `-T_E_AGAIN` (−11) | `sys_thread_spawn_handler` | non-exempt, `thread_count >= PROC_THREAD_MAX` |
| `-1` | `rfork_internal` | non-exempt, `child_count >= PROC_CHILD_MAX` |

A capped Proc receives a clean errno and the box stays up; the graceful-OOM
backstop catches anything the caps don't (a recursive bomb that exhausts RAM
before any single Proc hits its cap).

## Tests

`kernel/test/test_resource.c` (6 tests, registered in `kernel/test/test.c`):
`resource.exempt_only_system` (the unforgeable exemption), `page_charge_caps`
(charge/uncharge/clamp/overflow + exempt bypass), `thread_cap_ok`,
`child_cap_ok`, `child_count_tracks_list` (counter == list length via the
test-only link/unlink), `child_count_rfork_reap` (the **production**
`proc_link_child` ++ at rfork + `proc_unlink_child` -- at the reap, via a real
rfork + `wait_pid_for`), and `page_cap_attach_enforced` (the **real**
`sys_burrow_attach_for_proc` path: over-cap → `-ENOMEM` allocating nothing,
boundary-fit → success + charge, detach → uncharge, exempt → bypass). The
integration test pre-sets `page_count` near the cap so it exercises the boundary
without a 256-MiB allocation. SMP-gated (the counters are SMP-shared state). The
thread-cap and child-cap *reject* paths are predicate-tested only (a real reject
needs a non-exempt EL0 context — an owed E2E, since the in-kernel harness runs as
exempt kproc).

Three more landed with #106/#122, all revert-probed:

- `resource.attach_charges_buddy_rounded` — a **3-page** attach must charge 4,
  and detach must refund exactly 4. Three pages is the smallest request whose
  buddy order rounds up, and the size choice is the point: every pre-#106 test on
  this path used `PAGE_SIZE` or `2 * PAGE_SIZE`, both exact powers of two, so the
  whole suite was structurally blind to the rounding — the charge and the
  occupancy agree for precisely the sizes it exercised.
- `resource.detach_shared_in_keeps_page_count` — shares an ANON Burrow into the
  Proc via `burrow_share_into`, then detaches it, and asserts `page_count` is
  **unchanged** while `shared_map_pages` returns to 0. The nonzero pre-charge is
  load-bearing rather than scene-setting: `proc_page_uncharge` clamps at 0, so on
  a Proc already at 0 the spurious refund is invisible and the test would pass
  against the broken code.
- `burrow.backing_pages_matches_alloc` (`test_burrow.c`) — the anti-drift pin.
  It hard-codes no expected order table; it creates a **real** Burrow per size and
  compares `burrow_backing_pages(size)` against `1 << v->order`, the value the
  Burrow itself hands to `free_pages`. So it fails if either side changes without
  the other, which is the only failure mode that silently re-opens the undercount
  — a charge computed from a stale rounding rule looks perfectly reasonable at its
  call site. The size set mixes fixed points (already-power-of-two requests, where
  the helper must not inflate) with worst cases (2^k + 1 pages, where it must
  nearly double).

## Performance

Three counter reads/writes (atomic) plus one short lock-bounded predicate per
creation — negligible vs the allocation work each gate guards. No steady-state
cost (the counters are touched only at attach/detach/spawn/reap).

## Known caveats / footguns

- `proc_page_charge` / `proc_page_uncharge` **require `p->vma_lock` held** by the
  caller — they do not take it. (The two syscall call sites already hold it.)
- The thread/child caps carry a **bounded overshoot** (≤ ncpus−1). A consumer
  that needs an exact limit must not rely on the cap as a hard ceiling.
- The page cap counts the **buddy occupancy** (#106) — `burrow_backing_pages`,
  not the requested span. A **new eager charger must use the helper**, and a new
  order-0 charger (the lazy fault arm's shape) must **not**: mixing them up
  silently re-opens the undercount in one direction or over-bills in the other.
- The detach refund is a **positive allowlist** (#122). Adding a Burrow type that
  charges `page_count` means adding it to that arm; a type that charges a
  *different* axis (as `SHARED_IN` charges `shared_map_pages`) must stay out of
  it. Getting this backwards is silent: `proc_page_uncharge` clamps at 0, so a
  spurious refund never faults — it just lowers the floor.
- Exempt (`PRINCIPAL_SYSTEM`) Procs are **unbounded** by design — a compromised
  TCB component is not rate-limited here (it is already inside the TCB). Bounding
  even the TCB (with measured stratumd ceilings) is a future hardening.
- The aggregate / per-user quota (cgroup-equivalent) is a **recorded SEAM**, not
  built. It reads these per-Proc counters; `vma_drain` is its uncharge hook.

---

## CL-5: the per-Proc page BUDGET (the Clade F4 lift)

Since CL-5 the page axis caps against a **per-Proc budget** rather than the
`PROC_PAGE_MAX` constant. `proc_page_charge` reads `p->page_budget`; everything
else about the axis is unchanged.

### Why

Measured, not assumed (`docs/LLVM-DESIGN.md` §7.1): a 1959-byte template-heavy
C++ TU costs **64066 pages (250 MiB) — 97.8% of the 256 MiB default** through
cc1 on-device, and a real project TU projects to **500–650 MiB**. The default
does not fit real compilation. Raising the default instead would have weakened
the fork-bomb floor ~8× for every Proc, which is why F4 chose a per-Proc budget.

### The fields

- **`Proc.page_budget`** (u32 pages, @392) — what the charge caps against.
  Seeded to `PROC_PAGE_MAX` in `proc_init_fields`, the one chokepoint every Proc
  passes through (`proc_alloc` for user Procs, `proc_init` for kproc). It is
  never 0 on a live Proc — a 0 budget would refuse every charge, i.e. a Proc
  that cannot fault in a single anon page.
- **`Proc.page_peak`** (u32 pages, @284, in existing padding) — the anon
  high-water, the Linux `VmHWM` analog. Stamped under the same `vma_lock` that
  makes `page_count` exact, so it is an *exact* peak, not a sampled one. Never
  decremented; not moved by a refused charge. Pure telemetry: **no policy reads
  it.** Both are surfaced as `peak:` / `budget:` in `/proc/<pid>/status`.

### The rules

| request (`sys_spawn_args.page_budget`) | outcome |
|---|---|
| `0` | inherit the spawner's budget — the compatible default |
| `<=` the spawner's own | granted, **no authority needed** (monotonic reduction, the I-2 shape) |
| `>` the spawner's own | requires `SPAWN_PERM_MAY_RAISE_PAGE_BUDGET` |
| `>` `PROC_PAGE_HARD_MAX` (4 GiB) | **refused for everyone**, authority or not |

Refused means **the spawn fails with -1** — never a silent clamp, which would
hand back a budget the caller did not ask for and hide the misconfiguration
until it resurfaced as an opaque OOM. The single decision lives in
`proc_spawn_budget_resolve`.

### Inheritance is load-bearing

The budget is copied to the child in `rfork_internal`. This is not incidental
convenience: the toolchain chain is `ut → make → clang → cc1`, and `make` and
`clang` are **pouch ports calling `posix_spawn`** with no notion of a Thylacine
budget. A spawn-time-only budget could never reach cc1 — the process that
actually needs the memory. One raise at the build root covers the whole tree.
(Linux rlimits are inherited across fork/exec for the same reason.)

`rfork` alone still never *widens* a budget; only an authority-gated spawn does.

### Known caveats / footguns

- **The stamp window.** `rfork` creates the child carrying the *inherited*
  budget; the spawn thunk stamps the resolved one. Between the two the child
  runs only `apply_spawn_perms` (flag RMWs — no charge, no mapping), and the
  three chargers (`SYS_BURROW_ATTACH`, the Loom ring, the lazy-anon fault arm)
  are all post-`userland_enter`. **A future charger placed earlier would break
  this and must move the stamp.** An observer reading `/proc/<pid>/budget`
  immediately after spawn can legitimately catch the inherited value — wait for
  the observable, do not sample it.
- **Exec charges nothing — so a reduced budget does not bound exec-image anon.**
  `kernel/exec.c` calls `proc_page_charge` zero times: exec's segments and stack
  are eager `burrow_create_anon`, which the #65 posture deliberately leaves
  uncharged ("exec-image one-shot bounded"). Do not read the reduced-budget
  sandboxing primitive as covering it. Post-REVENANT ("a binary of any size
  execs") that #65-era justification is weaker than when it was written, and
  what actually holds the line is graceful OOM (an `alloc_pages` failure fails
  the exec cleanly, never a box extinction). Closing it is the recorded REVENANT
  **per-page I-32 charge** seam; the stamp is deliberately placed before exec so
  that seam lands bounded without revisiting this ordering. *(CL-5 audit F1 —
  the pre-fix comments claimed exec was the first charger. It is not.)*

  **#131 narrowed this finding to its accounting half.** The #106 round filed it
  as "uncharged but *refundable*" — a crafted ELF places an eager ANON segment in
  the burrow window, the Proc detaches it, and the old shape-based refund paid out
  for pages nobody had charged (a `page_count` under-count = budget inflation).
  Since #131 the refund is *attributed*: `SYS_BURROW_DETACH` pays `paid =
  burrow_charge_claim(dv, p)`, which is 0 unless a payer was recorded, and exec
  records none. So the refundable half is closed **by construction** — a new
  uncharged region is uncharged on both sides, which is the fail-safe direction
  and the #122 rule enforced by attribution rather than by enumerating shapes.

  What remains is purely that `page_count` / `page_peak` under-track true
  occupancy by exec's segments plus the 1 MiB stack. No bound is breached (they
  were never counted, so nothing double-counts and nothing over-refunds); the
  cost is that **`page_peak` is what CL-5 budget-sizing reads**, so a measured
  peak omits them. Charging them is a deliberate scope change to I-32 — the cap
  is documented as *"live anon pages via `SYS_BURROW_ATTACH`"*, and widening it
  to all anon occupancy re-baselines every CL-5 measurement — so it belongs with
  the per-page seam and a re-measure, not in a cleanup pass.
- **The TCB never consults a budget.** `PRINCIPAL_SYSTEM` is exempt via
  `proc_resource_exempt`, and `rfork` inherits the principal — so anything joey
  spawns (including the clade gate's `clang++`) is unbounded. That exemption is
  precisely why this collision stayed invisible until it was measured.
- The raise authority is a `SPAWN_PERM_*` bit, so like its siblings it is
  **not** propagated by `rfork` — but the budget it grants *is*. That asymmetry
  is deliberate: it is what lets a raise survive down through pouch programs
  that could never re-request one.
