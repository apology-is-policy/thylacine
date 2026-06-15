# 117 — allowance: the per-Proc hardware allowance (I-34)

**Status**: landed this chunk (the Menagerie build-arc step 3). The spec
(`specs/allowance.tla`) landed first, TLC-green, at `1602e37` — **spec-first
re-enabled for this surface** (user-voted 2026-06-15; CLAUDE.md's 4th
"RE-ENABLED for ..." entry); the impl (`kernel/allowance.{c,h}` + the three
create-gate insertions + the rfork/reap sites) follows, validated against the
model. This is the **one new kernel mechanism** the Menagerie driver framework
needs; it introduces **invariant I-34** (ARCHITECTURE.md §28) and is
audit-bearing (ARCH §25.4).

Scripture: `docs/MENAGERIE.md` §4 (the design rationale) + §11 (the warden's
grant policy — the one I-34 leg the warden owns) + ARCHITECTURE.md §28 (I-34)
+ `specs/allowance.tla` (the formal model).

---

## Purpose

The hardware allowance **scopes the coarse `CAP_HW_CREATE` to a bounded resource
set**. Before this chunk, `CAP_HW_CREATE` was flat: any holder could create a
`KObj_MMIO` / `KObj_IRQ` / `KObj_DMA` handle over *any* range the kernel had not
I-5-reserved (`docs/MENAGERIE.md` §4). That is acceptable for three trusted
system servers (stratumd, netd, kproc/joey); it is unacceptable for a fleet of
per-device drivers, where a GPIO driver must not be able to mint a handle over
the disk controller's registers.

The allowance is the **I-25 (legate scope) analog for hardware**: a driver's
hardware authority is *exactly* its warden-granted allowance — a subset of its
bound node's resources, declared in its manifest, granted only by the warden,
**never widened, fully revoked on unbind / removal / crash**. It is the missing
narrowing the warden confers when it spawns a per-device driver; it never
*grants* — the coarse `CAP_HW_CREATE` gate still runs first, so a Proc with no
cap creates nothing regardless of its allowance.

The mechanism is a per-Proc pointer (`struct Proc::allowance`,
`kernel/include/thylacine/proc.h:438`) plus the gate inserted at the three
create syscalls. The pointer being `NULL` is the **backward-compat hinge**:

```
p->allowance == NULL   ->  BROAD: the warden + the existing trusted servers,
                           bounded only by the I-5 kernel reservation -- the
                           as-built v1.0 behavior, unchanged.
p->allowance != NULL   ->  NARROWED: the driver may create KObj_MMIO/IRQ/DMA
                           only within the conferred set, and only while NOT
                           revoked.
```

"Everything not kernel-reserved" (the broad set, `docs/MENAGERIE.md` §4) is
exactly what the existing `kobj_*_create` reservation checks already enforce — a
broad (`NULL`) allowance defers to them. So nothing the warden / trusted servers
do changes; the allowance only ever *narrows*.

---

## Layering

The mechanism splits cleanly between the kernel structural enforcement and the
warden's policy:

- **`kernel/allowance.{c,h}` — the gate + lifecycle.** Owns the create gate
  (`allowance_permits` + `allowance_handle_alloc`), the confer / revoke / clone /
  free lifecycle, and the SMP-race-closing under-lock re-check. No knowledge of
  *which* resources a device has — it copies whatever the warden confers.
- **The warden (native `libthyla-rs` userspace) — the grant policy.** Computes
  the conferred set as `node INTERSECT manifest` (the bound node's `reg` /
  `interrupts` from `devhw`, intersected with the driver manifest's declared
  needs) and drives `proc_confer_allowance` at spawn / `proc_revoke_allowance`
  on `DeviceRemoved`. This is `docs/MENAGERIE.md` §11; it is the *step-5*
  consumer of this chunk (not yet wired — see Status).

This split is exactly the four I-34 legs (`§ Spec cross-reference`): the kernel
enforces three structurally; the warden enforces the fourth (`ConferredWithinNode`)
as policy, prose-audited.

---

## Public API (`kernel/include/thylacine/allowance.h`)

```c
#define ALLOWANCE_MMIO_MAX  8   // permitted MMIO PA windows
#define ALLOWANCE_IRQ_MAX   8   // permitted IRQ INTIDs (SPIs >= 32)

struct hw_window { u64 base; u64 size; };   // a permitted PA window [base, base+size)

// The (a, b) pair carried into allowance_permits, per resource kind:
//   HW_RES_MMIO: a = PA base, b = byte size  -> [a, a+b) within one window.
//   HW_RES_IRQ:  a = INTID,   b = 0          -> a in the irq set.
//   HW_RES_DMA:  a = byte size, b = 0        -> a in (0, dma_max].
enum hw_res_kind { HW_RES_MMIO, HW_RES_IRQ, HW_RES_DMA };
```

### `allowance_permits` — CreateBegin (the lock-free gate check)

```c
bool allowance_permits(struct Proc *p, enum hw_res_kind kind, u64 a, u64 b);
```

True iff `p` may create the requested resource. A `NULL` allowance is **BROAD**
→ `true` (subject to the caller's `CAP_HW_CREATE` gate + the `kobj_*_create`
I-5 reservation). A non-`NULL` allowance permits **only** its conferred set AND
**only while NOT revoked**. This is the **lock-free fast-path read** — the
windows are immutable post-confer; `revoked` is read `__ATOMIC_ACQUIRE`. The
authoritative re-check is `allowance_handle_alloc` at commit. Fails closed: a
`NULL` Proc, a revoked allowance, a zero-size MMIO/DMA request, an
overflowing `[base, base+size)`, or an unknown kind all → `false`
(`kernel/allowance.c:27`).

### `allowance_handle_alloc` — CreateCommit (the under-lock install)

```c
hidx_t allowance_handle_alloc(struct Proc *p, enum kobj_kind kind,
                              rights_t rights, void *obj);
```

Install a hardware handle, **re-checking the allowance UNDER its lock** so a
concurrent `proc_revoke_allowance` is observed. A `NULL` allowance bypasses the
re-check (broad) and calls `handle_alloc` directly. A non-`NULL`, non-revoked
allowance installs under `al->lock`; a revoked allowance returns `-1` (the
in-flight create lost the race → the caller rolls back the kobj exactly as for a
`handle_alloc` failure). `handle_alloc` is spinlock-only (never sleeps), so
holding `al->lock` across it is sound (`kernel/allowance.c:69`).

### `proc_confer_allowance` — Confer (the warden's narrowing, at spawn)

```c
int proc_confer_allowance(struct Proc *p,
                          const struct hw_window *mmio, u32 mmio_count,
                          const u32 *irq, u32 irq_count, u64 dma_max);
```

Confer a narrowed allowance on a freshly-spawned driver Proc. Deep-copies the
descriptor into a heap `Allowance`. The caller (the warden's spawn path)
guarantees a fresh child (`p->allowance == NULL` and **no concurrent reader** —
the driver has not yet entered EL0). Returns `0` on success, `-1` on OOM or a bad
descriptor (`mmio_count > ALLOWANCE_MMIO_MAX`, `irq_count > ALLOWANCE_IRQ_MAX`,
or a positive count with a `NULL` pointer). The warden ensures the conferred set
is a subset of the bound node (`ConferredWithinNode` — the warden's grant policy,
`docs/MENAGERIE.md` §11). `mmio[]` / `irq[]` / `dma_max` become **immutable**
here (`kernel/allowance.c:89`).

### `proc_revoke_allowance` — Revoke (DeviceRemoved)

```c
void proc_revoke_allowance(struct Proc *p);
```

Revoke a driver's allowance on `DeviceRemoved`. Sets `revoked` under `al->lock`
— closing the gate for in-flight **and** future creates. The **caller** then
`proc_group_terminate`s the driver; the #809/#811 cascade drops the live handles
at reap (the handle-axis teardown). A `NULL` allowance is a no-op
(`kernel/allowance.c:117`).

### `allowance_clone_into` — the rfork inherit

```c
int allowance_clone_into(struct Proc *child, struct Proc *parent);
```

Deep-copy a parent's allowance into a forked child. A narrowed parent's child is
**equally narrowed** — the hardware-axis analog of caps' monotonic reduction
(I-2): a child can never reach a *broader* hardware authority than its parent. A
broad parent (`allowance == NULL`) → child stays `NULL`. The `revoked` flag is
copied (`__ATOMIC_ACQUIRE`), so a child forked **after** its parent's revocation
is **born revoked** (permits nothing). The counts are clamped to the caps
defensively, so a corrupt source count can never leave a garbage tail. Returns
`0` on success (including the `NULL`-parent no-op), `-1` on OOM
(`kernel/allowance.c:133`).

### `allowance_free` — the reap release

```c
void allowance_free(struct Proc *p);
```

Free a Proc's allowance at reap (`proc_free`) and NULL the pointer.
NULL-tolerant (`kernel/allowance.c:152`).

---

## The create gate, in situ

The gate is **two-step at each of the three create syscalls** — the structure
that closes the revoke-vs-create SMP race. Each handler does
`CAP_HW_CREATE` → rights validation → `allowance_permits` (CreateBegin) →
`kobj_*_create` (the I-5-reserving, exclusivity-enforcing constructor) →
`allowance_handle_alloc` (CreateCommit) → rollback-on-`-1`. The exact insertion
points (`docs/MENAGERIE.md` §4 named them as the confirmed sites):

| Syscall | Handler | CreateBegin | CreateCommit |
|---|---|---|---|
| `SYS_MMIO_CREATE` | `sys_mmio_create_handler` | `kernel/syscall.c:228` | `kernel/syscall.c:238` |
| `SYS_IRQ_CREATE` | `sys_irq_create_handler` | `kernel/syscall.c:287` | `kernel/syscall.c:296` |
| `SYS_DMA_CREATE` | `sys_dma_create_handler` | `kernel/syscall.c:494` | `kernel/syscall.c:500` |

The ordering is load-bearing: `kobj_*_create` runs **between** the begin and the
commit (it allocates the kobj, claims the PA range / INTID / page chunk). If the
commit aborts (`-1`, the revoke won the race), the handler must release that kobj
(`kobj_mmio_unref` / `kobj_irq_unref` / `kobj_dma_unref`) so the claim is freed
and a retry — or another driver's create — can succeed (`kernel/syscall.c:239`,
`:297`, `:501`). The allowance gate sits **after** the `CAP_HW_CREATE` gate, so
the allowance never grants: a Proc with no cap is rejected before the allowance
is ever consulted.

---

## Implementation notes

- **The MMIO containment check** (`kernel/allowance.c:36`) is full-window
  containment, not mere base membership: a request `[base, base+size)` permits
  only if it lies *entirely* within one conferred window
  (`base >= wb && end <= wb + ws`). It is overflow-hardened on **both** sides —
  the request's own `base + size` wrap and a corrupt window's `wb + ws` wrap each
  fail-closed (`continue` / `return false`), so a wrapped span can never produce a
  spurious permit. A zero-size request is denied.
- **The IRQ check** (`kernel/allowance.c:49`) is plain set membership over the
  conferred `irq[]`. The syscall layer already rejects `intid < 32` (SGI/PPI are
  kernel-only) and `intid > 0xFFFFFFFF` *before* the allowance is consulted, so
  the allowance only ever sees SPI INTIDs.
- **The DMA check** (`kernel/allowance.c:55`) is a per-buffer size cap:
  `0 < size <= dma_max`. `dma_max == 0` means *no DMA permitted* (a driver that
  declared no DMA need). This is a **per-buffer** bound, not a cumulative pool
  budget — see Known caveats.
- **The revoked read is the first thing `allowance_permits` checks** after the
  `NULL` test (`kernel/allowance.c:33`): a revoked allowance permits *nothing*,
  the spec's `allowance[d] = {}`. The `__ATOMIC_ACQUIRE` load pairs with the
  `__ATOMIC_RELEASE` store in `proc_revoke_allowance`.
- **The confer is set-once at spawn** (`kernel/allowance.c:108`): the swap
  (`old = p->allowance; p->allowance = al`) + free-old is race-free *only*
  because the caller guarantees no concurrent reader (the driver has not entered
  EL0). After confer, `mmio[]` / `irq[]` / `dma_max` are immutable — no path
  mutates them — which is what makes the spec's `AllowanceWithinConferred` hold
  by construction (only `revoked` ever flips). This is documented as a hard
  contract; see Known caveats.
- **The clone clamps the counts defensively** (`kernel/allowance.c:139`):
  `dst->mmio_count = src->mmio_count > ALLOWANCE_MMIO_MAX ? ALLOWANCE_MMIO_MAX :
  src->mmio_count` (and likewise for IRQ), mirroring the A-1a `supp_gid_count`
  clamp, so a corrupt source can never make the child copy read past its arrays.

### Lifecycle wiring

- **rfork inherit** (`kernel/proc.c:875`): `rfork_internal` calls
  `allowance_clone_into(child, parent)`; on its `-1` (OOM) the whole rfork
  unwinds (`child->state = PROC_STATE_ZOMBIE; proc_free(child); return -1`). The
  clone leaves `child->allowance == NULL` on its own failure, so the `proc_free`
  rollback's `allowance_free` is a clean no-op there; a *later* rfork failure
  (territory_clone / thread_create) frees the just-cloned allowance via the same
  `proc_free` path. Note the broad-parent path is unreachable for a plain-rfork
  child (it holds `CAP_NONE`, so even a `NULL`-inherited allowance creates
  nothing) — only the warden/TCB spawns broad hw-capable children, and it does so
  via the *confer* path, not inheritance.
- **reap release** (`kernel/proc.c:533`): `proc_free` calls `allowance_free(p)`.
  The allowance is a plain heap struct, independent of the handle / notes / VMA
  frees around it.

---

## Data structures

### `struct Allowance` (`kernel/include/thylacine/allowance.h:51`)

The conferred resource set plus the revoke flag plus the serializing lock. The
windows / INTIDs / `dma_max` are **immutable after confer**; only `revoked` ever
flips.

| Field | Type | Meaning |
|---|---|---|
| `mmio[ALLOWANCE_MMIO_MAX]` | `struct hw_window[8]` | permitted MMIO PA windows `[base, base+size)` |
| `mmio_count` | `u32` | live count of `mmio[]` (≤ `ALLOWANCE_MMIO_MAX`) |
| `irq[ALLOWANCE_IRQ_MAX]` | `u32[8]` | permitted IRQ INTIDs (SPIs ≥ 32) |
| `irq_count` | `u32` | live count of `irq[]` (≤ `ALLOWANCE_IRQ_MAX`) |
| `dma_max` | `u64` | max bytes per `KObj_DMA`; `0` = no DMA permitted |
| `revoked` | `u32` (0/1) | set on DeviceRemoved; written `__ATOMIC_RELEASE` under `lock`, read `__ATOMIC_ACQUIRE` lock-free (CreateBegin) and under `lock` (CreateCommit) |
| `lock` | `spin_lock_t` | serializes the CreateCommit re-check against `proc_revoke_allowance` |

`struct hw_window { u64 base; u64 size; }` is the per-window PA range.

`revoked` is a `u32` (not a `bool`) for the established `__atomic` flag idiom
(cf. `proc_flags`).

### Caps

```c
#define ALLOWANCE_MMIO_MAX  8   // permitted MMIO PA windows
#define ALLOWANCE_IRQ_MAX   8   // permitted IRQ INTIDs (SPIs >= 32)
```

Eight of each is generous for a single device (a typical platform node has one
or two `reg` ranges and one `interrupts` entry; a PCI function its claimed BARs).
DMA has no count cap — it is a single per-buffer-size ceiling (`dma_max`), not a
list of pools.

### The `struct Proc` field

```c
struct Allowance  *allowance;   // kernel/include/thylacine/proc.h:438
```

`KP_ZERO` from `proc_alloc` inits it `NULL` (broad/none). It appends after the
#65 resource-floor block, pinned by `_Static_assert`s:

- `__builtin_offsetof(struct Proc, allowance) == 272`
  (`kernel/include/thylacine/proc.h:494`) — appends after `child_count`@268,
  8-byte-aligned; existing offsets stay stable.
- `sizeof(struct Proc) == 280` (`kernel/include/thylacine/proc.h:483`) — the #65
  272 baseline + the 8-byte allowance pointer.

---

## State machine

A Proc's allowance lifecycle has three states, mapped to the spec's
`state[d] ∈ {"idle", "running", "revoked"}` and to the live pointer:

```
                proc_confer_allowance              proc_revoke_allowance
  NULL (broad/idle) ------------------> conferred ----------------------> revoked
   p->allowance==NULL    [Confer]    (narrowed, running)   [Revoke]    (permits nothing)
                                       p->allowance!=NULL              al->revoked==1
                                       al->revoked==0
```

| State | Pointer / flag | Spec action that enters it | Gate behavior |
|---|---|---|---|
| **broad** | `allowance == NULL` | — (the `proc_alloc` / KP_ZERO default; the broad warden + trusted servers stay here for life) | `allowance_permits` → `true` (deferred to `CAP_HW_CREATE` + the I-5 reservation) |
| **conferred / narrowed-running** | `allowance != NULL`, `revoked == 0` | `Confer` (`proc_confer_allowance`) | `allowance_permits` checks the conferred set; `allowance_handle_alloc` installs under the lock |
| **revoked** | `allowance != NULL`, `revoked == 1` | `Revoke` (`proc_revoke_allowance`) | `allowance_permits` → `false`; `allowance_handle_alloc` → `-1` (in-flight create aborts) |

Notes on the transitions:

- **broad → conferred** (`Confer`) is **set-once at spawn**: a fresh child has no
  concurrent reader, so the install is race-free without a lock. There is no
  spec action that re-confers a running driver (the warden spawns a fresh Proc per
  device); `proc_confer_allowance` *can* swap-and-free an existing allowance, but
  the warden never drives that path on a running driver.
- **conferred → revoked** (`Revoke`) is the only post-confer mutation, and it
  touches only the `revoked` flag (under `lock`). The conferred *set* is never
  mutated — `AllowanceWithinConferred` holds by construction.
- There is **no revoked → anything** transition. A revoked driver is
  `proc_group_terminate`d by the caller; its allowance is freed at reap
  (`allowance_free`). The "fully revoked" guarantee is the spec's
  `RevokedFullyCleared`: a revoked driver holds no allowance *and* no handle (the
  #809/#811 cascade sweeps the handles).
- The **rfork inherit** copies the *current* state into the child: a broad parent
  → broad child; a conferred parent → an equally-conferred (deep-copied) child; a
  revoked parent → a **born-revoked** child (the `revoked` flag is copied).

---

## Spec cross-reference

The formal module is `specs/allowance.tla` (clean + 4 buggy cfgs), landed
TLC-green at `1602e37` **before** the impl. It models the **kernel mechanism**,
not the warden's policy: the warden is the implicit privileged actor that drives
`Confer` / `Revoke`; `Resources` is the broad universe (everything not
I-5-reserved is simply *in* `Resources`).

### Actions ↔ impl sites

| Spec action | Impl site | What it models |
|---|---|---|
| `Confer(d, N, A)` | `proc_confer_allowance` (`kernel/allowance.c:89`) — driven by the warden's spawn path; the `struct Proc::allowance` set at driver spawn | DeviceAdded: the warden confers `A = node INTERSECT manifest`, `A ⊆ N`; `idle → running` |
| `CreateBegin(d, r)` | `allowance_permits` at the three create gates (`kernel/syscall.c:228 / 287 / 494`) | the `SYS_*_CREATE` in-allowance check; on pass the create is "in flight" |
| `CreateCommit(d)` | `allowance_handle_alloc` (`kernel/allowance.c:69`) — the `handle_alloc` install, re-validating the allowance under the same lock `Revoke` takes (`kernel/syscall.c:238 / 296 / 500`) | the SAFE install: commit only if still running AND `r` still in the allowance; else abort |
| `Revoke(d)` | `proc_revoke_allowance` (`kernel/allowance.c:117`) + the caller's `proc_group_terminate` | DeviceRemoved: empty the allowance + drop every handle (the #809/#811 cascade); `running → revoked` |

The central guard the spec captures: a create is a **two-step under the
protecting lock**, and `Revoke` takes the **same lock**. A `SYS_*_CREATE` that
passes CreateBegin does not yet hold the handle; CreateCommit re-checks the live
allowance under `al->lock`. So an in-flight create concurrent with a revoke
serializes one of two ways — (a) commit-before-revoke: the handle lands, then
revoke's handle-sweep drops it; or (b) revoke-before-commit: the commit re-checks,
sees the emptied allowance, aborts. Either way no live handle survives over a
revoked allowance. This is the I-25-teardown × I-30-submit-pin discipline
(resolve-and-act under one lock, never re-trust a pre-check across a yield).

### The four invariants (the four legs of I-34)

| Spec invariant | Statement | Who enforces it |
|---|---|---|
| `HandlesWithinAllowance` | `∀ d : handles[d] ⊆ allowance[d]` — every live handle is within the LIVE allowance | **kernel** — the two-step gate + the under-lock re-check. **The crux** (covers gate soundness, the revoke race, and revoked⇒no-handle) |
| `AllowanceWithinConferred` | `∀ d : allowance[d] ⊆ conferred[d]` — never widened past the grant | **kernel** — the conferred set is immutable post-confer (only `revoked` flips) |
| `ConferredWithinNode` | `∀ d : conferred[d] ⊆ node[d]` — the grant never exceeds the bound node | **warden** — the grant policy computes `node INTERSECT manifest`; prose-audited (`docs/MENAGERIE.md` §11). The kernel copies whatever the warden confers |
| `RevokedFullyCleared` | revoked ⇒ no handle AND no allowance | **kernel** — `proc_revoke_allowance` empties the allowance; the caller's `proc_group_terminate` (the #809/#811 cascade) sweeps the handles |

Three of the four are kernel-structural; the one the kernel cannot enforce
(`ConferredWithinNode`) is the warden's job — the kernel cannot know a device's
real resources, so it trusts the warden's `node INTERSECT manifest` computation.
That trust boundary is the warden's prosecution surface (`docs/MENAGERIE.md` §11),
landing with the warden in build-arc step 5.

Liveness witness (clean cfg): `EventuallyResolves` — `∀ d : pending[d] # {} ~>
pending[d] = {}`. Every in-flight create eventually resolves (commits or aborts);
the re-check-under-lock gate cannot wedge a `SYS_*_CREATE` against a concurrent
revoke. The runtime analog is that `allowance_handle_alloc` never blocks
indefinitely (it takes `al->lock`, does a bounded `handle_alloc`, returns).

### The buggy cfgs (executable counterexamples)

Each buggy flag enables a `Buggy*` action that violates one leg; the cfg is a
durable regression that the corresponding *correct* action closes.

| Buggy cfg | Flag | Models | Caught by |
|---|---|---|---|
| `allowance_buggy_revoke_race.cfg` | `BUGGY_COMMIT_NO_RECHECK` | **THE central counterexample** — the create commit installs the handle UNCONDITIONALLY (no re-check). A revoke interleaving between CreateBegin and the commit leaves a live handle over the emptied allowance — the revoke-vs-create SMP race | `HandlesWithinAllowance` (Confer → CreateBegin → Revoke → BuggyCreateCommit) |
| `allowance_buggy_revoke_leak.cfg` | `BUGGY_REVOKE_LEAVES_HANDLES` | Revoke empties the allowance but FAILS to drop the handles (an incomplete I-25 teardown — the group-terminate didn't sweep the hw handles) | `RevokedFullyCleared` + `HandlesWithinAllowance` |
| `allowance_buggy_confer_widen.cfg` | `BUGGY_CONFER_WIDEN` | The warden's `node INTERSECT manifest` is buggy and confers an allowance NOT a subset of the node (a grant past the device) | `ConferredWithinNode` |
| `allowance_buggy_self_widen.cfg` | `BUGGY_SELF_WIDEN` | The live allowance grows past the conferred grant (a kernel bug where the allowance set is mutable after confer, or a stale/widened copy) | `AllowanceWithinConferred` |

The headline `BUGGY_COMMIT_NO_RECHECK` cfg is the executable form of the
revoke-vs-create race the scripture names; its runtime regression is
`allowance.handle_alloc_revoked_aborts` (below).

---

## Tests (`kernel/test/test_allowance.c`, 10 cases)

| Test | Covers |
|---|---|
| `allowance.null_is_broad` | a fresh Proc is broad (`allowance == NULL`) → permits any MMIO/IRQ/DMA (the as-built v1.0 path) |
| `allowance.mmio_containment` | full-window containment: exact + sub-window permit; straddle / outside / zero-size deny; the `base+size` overflow guard; a narrowed allowance denies a never-conferred kind |
| `allowance.irq_membership` | set membership over `irq[]` (conferred INTIDs permit; non-conferred + a never-conferred kind deny) |
| `allowance.dma_cap` | `0 < size <= dma_max` (at / under cap permit; over cap + zero deny); `dma_max == 0` → no DMA permitted |
| `allowance.revoked_permits_nothing` | post-`proc_revoke_allowance`, MMIO/IRQ/DMA all deny (the spec's `allowance[d] = {}`) |
| `allowance.confer_rejects_overcap` | `mmio_count` / `irq_count` over the cap → `-1`; a positive count with a NULL pointer → `-1`; **no allowance installed on any reject** |
| `allowance.handle_alloc_broad` | a NULL allowance → `allowance_handle_alloc` behaves like `handle_alloc` (install succeeds) |
| **`allowance.handle_alloc_revoked_aborts`** | **THE revoke-vs-create race regression** (the spec's `BUGGY_COMMIT_NO_RECHECK`): a narrowed non-revoked install succeeds; after `proc_revoke_allowance`, the commit aborts (`-1`). The buggy variant (no re-check) would install here and violate `HandlesWithinAllowance` |
| `allowance.clone_inherit` | broad parent → child NULL; narrowed parent → child gets an equally-narrow **own** deep copy (not the same pointer) inheriting the conferred set but never broader; revoked parent → child **born revoked** (permits nothing) |
| `allowance.free_null_tolerant` | `allowance_free` on a NULL allowance → no-op; confer-then-free → NULLs the pointer |

The tests use the `test_handle.c` Proc make/drop idiom: `proc_alloc` gives a
fresh Proc with an empty handle table + a NULL (broad) allowance; the drop
ZOMBIEs + `proc_free`s it (which `allowance_free`s any conferred allowance). The
race regression is *deterministic* (it sequences `revoke` then `commit` on one
thread); the SMP interleaving it stands in for is the spec's province — the
TLA+ model is the durable witness that the *concurrent* race is closed, exactly
the spec-first division of labor.

---

## Error paths

| Return | Trigger | Caller expectation |
|---|---|---|
| `allowance_permits` → `false` | NULL Proc; revoked allowance; MMIO request outside / straddling / zero-size / `base+size` overflow; IRQ not in the set; DMA `0` or `> dma_max`; unknown kind | the create handler returns `-1` *before* allocating the kobj (no rollback needed) |
| `allowance_handle_alloc` → `-1` | NULL Proc; **revoked allowance** (the in-flight create lost the race); the underlying `handle_alloc` failed (table full) | the create handler **rolls back** the just-created kobj (`kobj_*_unref`) and returns `-1` |
| `proc_confer_allowance` → `-1` | `mmio_count > ALLOWANCE_MMIO_MAX`; `irq_count > ALLOWANCE_IRQ_MAX`; a positive count with a NULL pointer; OOM (`kmalloc` failed) | the warden's spawn path aborts the driver spawn; **no allowance is installed** (the existing `p->allowance` is untouched on a reject) |
| `allowance_clone_into` → `-1` | OOM (`kmalloc` of the child copy failed) | `rfork_internal` unwinds the whole fork (`proc_free(child); return -1`) |

`proc_revoke_allowance` and `allowance_free` have no error return (both
NULL-tolerant no-ops on a broad Proc).

---

## Performance

- **CreateBegin is lock-free.** `allowance_permits` reads the immutable conferred
  set + one `__ATOMIC_ACQUIRE` load of `revoked`; no lock, no allocation. MMIO is
  O(`mmio_count`) ≤ 8 window comparisons; IRQ O(`irq_count`) ≤ 8; DMA O(1). On the
  common broad path it is a single NULL test → `true`.
- **CreateCommit takes `al->lock` briefly** — one `__ATOMIC_ACQUIRE` re-read of
  `revoked` + the bounded `handle_alloc` (itself spinlock-only) under the lock,
  then unlock. The lock is held only across the commit window; `handle_alloc`
  never sleeps, so the hold is short and bounded. On the broad path the lock is
  skipped entirely.
- **Revoke takes `al->lock`** for a single `__ATOMIC_RELEASE` store. The
  subsequent `proc_group_terminate` (the expensive part — the handle sweep) runs
  *outside* the allowance lock (the caller drives it after `proc_revoke_allowance`
  returns).
- **Confer / clone** each do one `kmalloc(KP_ZERO)` + the small fixed-size copy
  (≤ 8 windows + ≤ 8 INTIDs); confer additionally frees the old allowance if one
  existed. These run at driver spawn / rfork, not on any hot path.

Lock order: `al->lock → handle-table lock` (CreateCommit holds `al->lock` across
`handle_alloc`, which takes the handle-table lock). Nothing acquires the
handle-table lock then `al->lock`, so the order is acyclic.
`proc_revoke_allowance` takes `al->lock` **alone**, releasing it *before* the
caller's `proc_group_terminate` — no nesting with `g_proc_table_lock`.

---

## Status

- **Landed this chunk** (Menagerie build-arc step 3): `kernel/allowance.{c,h}`,
  the three create-gate insertions (`kernel/syscall.c`), the rfork inherit
  (`kernel/proc.c:875`) + reap release (`kernel/proc.c:533`), the
  `struct Proc::allowance` field + offset asserts
  (`kernel/include/thylacine/proc.h`), and the 10 tests
  (`kernel/test/test_allowance.c`). The spec landed first at `1602e37`,
  TLC-green.
- **Backward-compat is total.** Every existing trusted server (kproc, joey,
  stratumd, netd) stays broad (`allowance == NULL`) and is unaffected — the gate
  on a NULL allowance is `true`, deferring to the unchanged `CAP_HW_CREATE` +
  I-5-reservation checks. No existing boot path confers a narrowed allowance.
- **The warden's confer-at-spawn wiring is the step-5 consumer.** The mechanism
  is complete and tested in isolation; the warden (the native-userspace device
  manager) that actually *drives* `proc_confer_allowance` at driver spawn and
  `proc_revoke_allowance` on `DeviceRemoved` lands later in the Menagerie
  build-arc (`docs/MENAGERIE.md` §5/§11). Until then, the allowance is dormant
  (every Proc is broad) but ready.

---

## Known caveats / footguns

- **Confer is set-once-at-spawn — the no-concurrent-reader contract is load-
  bearing.** `proc_confer_allowance` swaps `p->allowance` and frees the old one
  with **no lock** (`kernel/allowance.c:108`). This is sound *only* because the
  caller (the warden's spawn path) guarantees the conferred-upon Proc has not yet
  entered EL0, so nothing reads `p->allowance` concurrently. Calling
  `proc_confer_allowance` on a *running* driver (one whose threads are executing
  the create gate) would be a UAF — a CreateBegin reader could dereference the
  just-`kfree`d old allowance. The warden never does this (it spawns a fresh Proc
  per device); the contract is the discipline that keeps the swap lock-free. A
  future "re-confer a running driver" need would require routing the swap through
  `al->lock` (and an RCU-style grace for the old struct), a deliberate addition.
- **The forked-child scope-teardown is a v1.x seam.** Today `allowance_clone_into`
  makes a child equally narrowed, but the child's allowance is an *independent*
  copy — revoking the parent's device (`proc_revoke_allowance(parent)`) does **not**
  revoke the child's. For v1.0 this does not arise: drivers do not `rfork`
  hw-capable children (the warden spawns one driver Proc per device; the
  CAP_HW_CREATE child of a plain rfork holds `CAP_NONE` and creates nothing
  anyway). If a driver framework ever forks worker children that inherit a live
  allowance, a DeviceRemoved would need to cascade the revoke across the
  fork-tree — a scope-teardown analog of the legate scope (I-25). The hook point
  is `proc_revoke_allowance` (it would walk the children); recorded, not built.
- **DMA is a per-buffer cap, not a cumulative pool budget.** `dma_max` bounds the
  size of *each individual* `KObj_DMA`; it does not bound the *sum* of a driver's
  live DMA allocations. A driver could create many buffers each at `dma_max`. This
  is deliberate at v1.0: the cumulative DMA-pool budget is the **resource-DoS
  axis**, which composes with the #65 per-Proc resource floor
  (`PROC_PAGE_MAX` — DMA buffers are the driver's *own* kernel memory) — **not**
  the I-34 cross-device-authority axis (a DMA buffer is never another device's
  registers). The two axes are orthogonal; the cumulative budget is a documented
  v1.x refinement that lands on the #65 side (`kernel/syscall.c:491`).
- **The kernel trusts the warden's grant.** `ConferredWithinNode` is the one I-34
  leg the kernel cannot enforce — it copies whatever the warden confers and has no
  way to know a device's real resources. A buggy warden that confers a window past
  the device's `reg` range would violate I-34, and the kernel would not catch it.
  This is the warden's prosecution surface (`docs/MENAGERIE.md` §11, the
  discovery-source-trust question); the `BUGGY_CONFER_WIDEN` cfg is the spec-level
  reminder that this leg exists and lives above the kernel line.
- **Counts are clamped defensively on clone, but confer trusts the count.**
  `allowance_clone_into` clamps `src->mmio_count` / `irq_count` to the caps
  (against a corrupt source), but `proc_confer_allowance` *rejects* an over-cap
  count rather than clamping (`-1`). The asymmetry is intentional: confer is the
  warden asserting a fresh descriptor (reject a malformed one); clone is copying
  trusted-but-possibly-corrupted live state (clamp so a torn count can never read
  past the array).
