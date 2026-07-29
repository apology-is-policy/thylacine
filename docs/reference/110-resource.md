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

Charged as the *logical* page count (the VMA span / ring span). **Physical
commitment is ≤ 2×** the charged count because `burrow_create_anon` rounds the
allocation up to a buddy power-of-2 order (audit F2) — so the per-Proc *physical*
anon ceiling is up to `~2 * PROC_PAGE_MAX` (≈ 512 MiB). The cap bounds *logical*
attached anon; precise-RAM accounting is the SEAM's job. v1.0 has no mid-life
`vma_drain` (no in-place re-exec), so attach/detach (and the ring's
detach / `vma_drain`) balance the counter while the Proc lives; at exit the Proc
and its counter vanish together (`vma_drain` is the SEAM hook where a future
aggregate would uncharge).

### Thread cap — `sys_thread_spawn_handler`
(`kernel/syscall.c`). `proc_thread_cap_ok` is checked after argument validation
and before `thread_create_user`, refusing `-EAGAIN` (the POSIX `RLIMIT_NPROC`
convention). kproc is already excluded at the handler top. The thread cap is the
tightest of the three because each thread pins `THREAD_KSTACK_TOTAL_SIZE` (32
KiB) of **unswappable** kernel kstack (256 → 8 MiB).

The thread cap covers `SYS_THREAD_SPAWN` (the only EL0 thread-create path).
**Kernel-side kthreads spawned on a Proc's behalf** — at v1.0 only the Loom
SQPOLL poll-thread (`SYS_LOOM_SETUP | LOOM_SETUP_SQPOLL`, spawned against the
exempt `kproc`) — are **not** counted by the thread cap (audit F4). They are
bounded transitively: one per live SQPOLL Loom, and the number of Looms a Proc
can hold is now bounded by the page cap (the F1 ring charge) and the handle
table (`PROC_HANDLE_MAX` = 64), so the SQPOLL kstack footprint is bounded.

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

## Performance

Three counter reads/writes (atomic) plus one short lock-bounded predicate per
creation — negligible vs the allocation work each gate guards. No steady-state
cost (the counters are touched only at attach/detach/spawn/reap).

## Known caveats / footguns

- `proc_page_charge` / `proc_page_uncharge` **require `p->vma_lock` held** by the
  caller — they do not take it. (The two syscall call sites already hold it.)
- The thread/child caps carry a **bounded overshoot** (≤ ncpus−1). A consumer
  that needs an exact limit must not rely on the cap as a hard ceiling.
- The page cap counts **logical** attached anon pages; physical RAM committed is
  ≤ 2× (buddy order rounding). Precise-RAM accounting is the SEAM's job.
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
- **The TCB never consults a budget.** `PRINCIPAL_SYSTEM` is exempt via
  `proc_resource_exempt`, and `rfork` inherits the principal — so anything joey
  spawns (including the clade gate's `clang++`) is unbounded. That exemption is
  precisely why this collision stayed invisible until it was measured.
- The raise authority is a `SPAWN_PERM_*` bit, so like its siblings it is
  **not** propagated by `rfork` — but the budget it grants *is*. That asymmetry
  is deliberate: it is what lets a raise survive down through pouch programs
  that could never re-request one.
