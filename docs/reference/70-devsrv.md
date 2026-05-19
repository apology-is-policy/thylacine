# 70 — devsrv: the `/srv` service registry

**Status**: as-built at P5-corvus-srv-impl-a2. The service registry, the
`devsrv` Dev, the `SYS_POST_SERVICE` syscall, the `proc_flags` post-gate,
and poster-exit tombstoning are landed. The per-connection layer (the
`devsrv` walk op, the kernel-minted transport, `SYS_SRV_ACCEPT`,
`SYS_SRV_PEER`) is P5-corvus-srv-impl-a3 — not yet built.

---

## Purpose

`/srv` is the kernel surface by which a userspace 9P **server** publishes
itself for per-connection client access. A server posts a name; the
kernel mediates each client connection, stamping it with the client's
kernel-attested identity. It is Plan 9's `#s` device, and the v1.0
consumer is `corvus`, the key agent (CORVUS-DESIGN.md §6).

`devsrv` is deliberately a *distinct* `Dev` from `dev9p` (the Dev backing
kernel-mounted 9P trees). The separation is structural, not cosmetic:
every Spoor walked out of `/srv` carries `devsrv`'s device character, so
a `/srv/<name>` connection Spoor is a `KObj_Srv` kernel object —
non-transferable (ARCH §18.2; `specs/handles.tla` `SrvKObjs`). A client
cannot 9P-transfer its connection to another Proc, which is what keeps
the kernel-stamped peer identity behind it unforgeable across a walk.

This layer sits above the Dev framework (`kernel/dev.c`) and the handle
table (`kernel/handle.c`), and is reached from userspace through one
syscall, `SYS_POST_SERVICE`.

---

## Public API

### Syscall

```
SYS_POST_SERVICE(name_va, name_len) → service_handle / -1
```

Registers the calling Proc as the 9P server for `/srv/<name>` and returns
a `KObj_Srv` service handle. `name_va` is a user-VA pointer to the name
bytes; `name_len` is `1..SRV_NAME_MAX` (32). Gated on the one-way
joey-stamped `PROC_FLAG_MAY_POST_SERVICE` bit. Defined in
`kernel/syscall.c` as the thin user-VA wrapper `sys_post_service_handler`
over the testable core:

```c
int sys_post_service_for_proc(struct Proc *p, const char *name,
                              size_t name_len);
```

`sys_post_service_for_proc` returns the service handle (`hidx ≥ 0`) on
success, `-1` on any failure (see Error paths).

### Registry API — `<thylacine/devsrv.h>`

```c
int  srv_reserve(const char *name, u8 name_len, struct Proc *poster,
                 struct SrvService **svc_out, enum srv_state *prior_out);
void srv_commit(struct SrvService *svc);
void srv_abort(struct SrvService *svc, enum srv_state prior);
struct SrvService *srv_lookup(const char *name, u8 name_len);
void srv_proc_exit_notify(struct Proc *p);
int  srv_registry_count(void);
```

`srv_reserve` / `srv_commit` / `srv_abort` are the reserve-then-commit
two-phase post: a registry slot is claimed (`RESERVING`), then either
committed to `LIVE` or rolled back. `srv_lookup` finds a service by name.
`srv_proc_exit_notify` is the poster-exit hook. `srv_registry_count`
returns the number of non-`FREE` entries (tests + diagnostics).

`srv_registry_reset()` exists in `kernel/devsrv.c` as **test support
only** — it is not declared in `devsrv.h` and has no production caller.

### Proc post-gate — `<thylacine/proc.h>`

```c
void proc_mark_may_post_service(struct Proc *p);
bool proc_may_post_service(const struct Proc *p);
```

`proc_mark_may_post_service` stamps `PROC_FLAG_MAY_POST_SERVICE`
(`proc_flags` bit 4) — one-way, never cleared, never propagated by
`rfork`. `proc_may_post_service` is the fail-closed query
`SYS_POST_SERVICE` gates on.

### The Dev

`extern struct Dev devsrv;` — `dc='s'`, `name="srv"`, registered by
`dev_init()`. Only `attach` is meaningful at a2 (yields the `/srv` root
QTDIR Spoor); the other 15 vtable slots are graceful-fail stubs.

---

## Implementation

### The registry

`kernel/devsrv.c` holds a single static `struct SrvRegistry`:

```c
struct SrvRegistry {
    spin_lock_t       lock;
    struct SrvService entries[SRV_MAX_SERVICES];   // SRV_MAX_SERVICES == 8
};
static struct SrvRegistry g_srv_registry;
```

Static storage zero-initializes every entry's `state` to `SRV_STATE_FREE`
and the lock to the all-zero `SPIN_LOCK_INIT` form, so no `init()` hook is
needed — `devsrv.init` is a no-op. The lock is a leaf lock: every registry
function takes and releases it without nesting another lock inside.

`corvus` is the only v1.0 service (`/srv/stratum-ctl` is a *mounted* 9P
tree, not a `devsrv` service); `SRV_MAX_SERVICES = 8` is headroom.

### Two-phase post

`SYS_POST_SERVICE` must roll back cleanly if `handle_alloc` fails after
the registry has been touched, so the post is reserve-then-commit
(`kernel/syscall.c:sys_post_service_for_proc`):

1. `srv_reserve` — claims a slot, sets it `SRV_STATE_RESERVING`, records
   the poster's `stripes`/`pid`, returns the prior state (`FREE` for a
   fresh post, `TOMBSTONED` for a rebind). A `RESERVING` entry is never
   observed as a connectable service.
2. `handle_alloc(p, KOBJ_SRV, …, svc)` — installs the service handle.
3. On success, `srv_commit` flips `RESERVING → LIVE`. On `handle_alloc`
   failure, `srv_abort` flips `RESERVING → prior` (a `FREE`-prior abort
   wipes the slot; a `TOMBSTONED`-prior abort restores the tombstone).

The `RESERVING` window is bounded by the single syscall — at v1.0 there
is no async kill, so a poster cannot exit mid-post.

### The post-gate

`SYS_POST_SERVICE` refuses any Proc that does not carry
`PROC_FLAG_MAY_POST_SERVICE`. The bit is stamped by
`proc_mark_may_post_service` — one-way, `ALIVE`-gated, idempotent, and
**not copied by `rfork`** (`rfork_internal` deliberately does not copy
`proc_flags`; `proc_alloc` zeroes it). joey is the sole stamper: it marks
the corvus Proc it spawns, so an ordinary Proc cannot post or hijack
`/srv/corvus`. The same one-way-marker discipline as
`PROC_FLAG_CONSOLE_ATTACHED` (§5.5 / reference 14).

The joey→corvus stamp itself is **deferred to P5-corvus-srv-impl-b**,
when corvus is wired to post `/srv/corvus` for real; until then the gate
ships fail-closed (no production Proc carries the bit) and is exercised
only by tests.

### Tombstoning

When a Proc exits, `exits()` (`kernel/proc.c`) calls
`srv_proc_exit_notify(p)` — before acquiring `g_proc_table_lock`, while
`p` is still `ALIVE` and valid — which scans the registry and flips any
`LIVE` service whose `poster_stripes` matches `proc_stripes(p)` to
`SRV_STATE_TOMBSTONED`, clearing the dead poster identity.

A tombstone keeps the name reserved: it is re-postable, but only by a
joey-marked Proc (the `srv_reserve` rebind path requires the post-gate to
have already passed). A malicious Proc therefore cannot race corvus's
restart to claim `/srv/corvus` — the marker *is* the rebind authority
(CORVUS-DESIGN.md §6.1; one mechanism, not two).

The poster identity is stored **by value** (`poster_stripes`,
`poster_pid` — never a raw `struct Proc *`), so a poster that exits and is
reaped never turns a registry read into a use-after-free (CORVUS-DESIGN.md
§6.3).

`exits()` is the sole termination path at v1.0 (a Proc reaches `ZOMBIE`
only through it); a future async-kill path must also call
`srv_proc_exit_notify`.

---

## Data structures

### `struct SrvService` — `<thylacine/devsrv.h>`

```c
struct SrvService {
    u64            magic;            // SRV_SERVICE_MAGIC at offset 0
    enum srv_state state;
    u8             name_len;         // 1..SRV_NAME_MAX
    char           name[SRV_NAME_MAX];   // not NUL-terminated; name_len authoritative
    u64            poster_stripes;   // poster's stripes tag (by value)
    int            poster_pid;       // poster's PID (by value; diagnostics)
};
```

`magic` (`SRV_SERVICE_MAGIC`) sits at offset 0 so the P5-corvus-srv-impl-
a3 handle-release path can discriminate a service object from a
connection object. The struct is purely kernel-internal — never on-disk,
on-wire, or ABI-exposed — so it carries no `_Static_assert` on size.

### `enum srv_state`

| State | Value | Meaning |
|---|---|---|
| `SRV_STATE_FREE` | 0 | slot unused (zero-init reads as this) |
| `SRV_STATE_RESERVING` | 1 | a `SYS_POST_SERVICE` is in flight |
| `SRV_STATE_LIVE` | 2 | posted; poster alive; the 9P server |
| `SRV_STATE_TOMBSTONED` | 3 | poster exited; name reserved for a marked rebind |

### `PROC_FLAG_MAY_POST_SERVICE`

`proc_flags` bit 4 (`<thylacine/proc.h>`). Kernel-stamped, one-way, never
`rfork`-propagated — joins bits 0-3 (`NODUMP` / `NOTRACE` / `MLOCKED` /
`CONSOLE_ATTACHED`).

---

## State machines

A registry slot's lifecycle:

```
            srv_reserve (fresh post)
   FREE ─────────────────────────────► RESERVING
    ▲                                   │   │
    │ srv_abort(prior=FREE)              │   │ srv_commit
    └───────────────────────────────────┘   ▼
                                           LIVE
   TOMBSTONED ◄──────────────────────────── │
    │   ▲   srv_proc_exit_notify (poster exits)
    │   │
    │   └── srv_abort(prior=TOMBSTONED)  ◄─┐
    │                                      │
    └──── srv_reserve (rebind) ──► RESERVING (prior=TOMBSTONED)
```

`PostService` / `ServiceTombstone` in `specs/corvus.tla` pin the
`{UNPOSTED, LIVE, TOMBSTONED}` view of this machine (the `RESERVING`
transient is an implementation detail of the rollback-safe post and is
not modeled — the spec's `PostService` is atomic).

---

## Spec cross-reference

`specs/corvus.tla` (connection layer, extended at P5-corvus-srv-impl-a2):

| Spec action / invariant | Code |
|---|---|
| `MarkMayPost(p)` | `proc_mark_may_post_service` (`kernel/proc.c`) |
| `PostService(p)` | `sys_post_service_for_proc` + `srv_reserve`/`srv_commit` |
| `ServiceTombstone` | `srv_proc_exit_notify`, called from `exits()` |
| `ServicePosterEverMarked` | the `proc_may_post_service` gate in `sys_post_service_for_proc` |
| `BuggyPostWithoutMarker` | `corvus_buggy_post_without_marker.cfg` counterexample |

`corvus.cfg` is TLC-clean (all 8 invariants); the buggy cfg drives a
`ServicePosterEverMarked` counterexample. The canonical action↔code map
is `specs/SPEC-TO-CODE.md`.

---

## Tests

`kernel/test/test_devsrv.c` — 6 tests:

- `devsrv.registered` — `devsrv` is in the bestiary (`dc='s'`); `attach`
  yields a QTDIR `/srv` root Spoor.
- `devsrv.post_gate` — post refused for an unmarked Proc and for a
  malformed name; accepted once marked.
- `devsrv.post_basic` — a post produces a `LIVE` entry stamped with the
  poster's `stripes`/`pid`; a live name is not re-postable; a distinct
  name posts independently.
- `devsrv.tombstone` — `srv_proc_exit_notify` tombstones the poster's
  service; a tombstone is re-postable only by a marked Proc; rebinding
  re-stamps the poster identity.
- `devsrv.registry_full` — the registry caps at `SRV_MAX_SERVICES`.
- `devsrv.post_rollback` — a `handle_alloc` failure after `srv_reserve`
  leaves no stale registry entry (the `srv_abort` rollback path).

Each test calls `srv_registry_reset()` first for isolation. Suite:
459/459 PASS × default + UBSan.

---

## Error paths

`sys_post_service_for_proc` / `SYS_POST_SERVICE` return `-1` on:

| Cause | Detail |
|---|---|
| caller not marked | no `PROC_FLAG_MAY_POST_SERVICE` (fail-closed) |
| bad name length | `name_len == 0` or `> SRV_NAME_MAX` |
| bad name byte | a byte outside `0x21..0x7e`, or `/` |
| bad user-VA | `name_va` outside the readable user range |
| name already served | a `LIVE` or `RESERVING` entry for that name exists |
| registry full | all `SRV_MAX_SERVICES` slots in use |
| handle table full | `handle_alloc` failed — the reservation is rolled back |

A `-1` return never leaves a stale or partial registry entry: a failure
before `srv_reserve` touches nothing, and a failure after it is undone by
`srv_abort`.

---

## Status

| Item | State |
|---|---|
| service registry + two-phase post | landed (P5-corvus-srv-impl-a2) |
| `SYS_POST_SERVICE` (syscall 26) | landed |
| `PROC_FLAG_MAY_POST_SERVICE` post-gate | landed |
| poster-exit tombstoning | landed |
| `devsrv` Dev (`attach` + 15 stubs) | landed |
| joey→corvus post-gate stamp | deferred to P5-corvus-srv-impl-b |
| `devsrv` walk op (connection mint) | deferred to P5-corvus-srv-impl-a3 |
| transport pair + `SYS_SRV_ACCEPT` / `SYS_SRV_PEER` | deferred to a3 |

---

## Known caveats / footguns

- `devsrv` registers as a Dev but has no walk op at a2 — `/srv` is
  attachable, but its children are not yet reachable. A client cannot
  open `/srv/corvus` until P5-corvus-srv-impl-a3.
- The post-gate ships fail-closed: between a2 and impl-b, *no* production
  Proc carries `PROC_FLAG_MAY_POST_SERVICE`, so `SYS_POST_SERVICE` always
  returns `-1` in production. This is correct — the syscall is inert but
  safe until impl-b wires joey's stamp.
- A `SrvService` handle's `obj` points into the static registry array.
  Closing the handle is a no-op (`handle_release_obj` `KOBJ_SRV` case) —
  the registry entry's lifetime is the poster Proc's (tombstoned on
  exit), never the handle's.
- `srv_lookup` returns a pointer that stays valid (the registry is
  static) but whose `state` may change after the call — callers
  re-validate. Sound at v1.0 (single-CPU, corvus single-threaded).
- `proc_flags` writers (`proc_mark_may_post_service` included) use a
  non-atomic `|=` — sound under the v1.0 single-thread-per-Proc
  invariant; the multi-threaded-Proc lift converts every writer to
  `__atomic_or_fetch`.
