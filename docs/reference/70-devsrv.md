# 70 — devsrv: the `/srv` service registry

**Status**: as-built at P5-corvus-srv-impl-a3b. The service registry, the
`devsrv` Dev, the `SYS_POST_SERVICE` syscall, the `proc_flags` post-gate,
and poster-exit tombstoning landed at a2; the **per-connection layer** —
the `devsrv` walk op, the client-connect path (`srv_conn_open_for_proc`),
the bounded accept backlog, `SYS_SRV_ACCEPT`, and the connection-Spoor
read/write/close — landed at a3b. What remains: `SYS_SRV_PEER` (the
peer-identity read; P5-corvus-srv-impl-a3c) and the corvus 9P server with
the §6.2 step-5 handshake drive plus the production client-open syscall
(P5-corvus-srv-impl-b).

---

## Purpose

`/srv` is the kernel surface by which a userspace 9P **server** publishes
itself for per-connection client access. A server posts a name; the
kernel mediates each client connection, stamping it with the client's
kernel-attested identity. It is Plan 9's `#s` device, and the v1.0
consumer is `corvus`, the key agent (CORVUS-DESIGN.md §6).

The registry (a2) is the publish side; the per-connection layer (a3b) is
the mediation side — together they carry a client from a `/srv` name to a
kernel-minted, peer-identity-stamped connection end to end. A client
connect mints a `SrvConn` (reference 71) and enqueues it on the named
service's accept backlog; the poster's `SYS_SRV_ACCEPT` dequeues one and
receives the server endpoint of that connection.

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

### Syscalls

```
SYS_POST_SERVICE(name_va, name_len)  → service_handle / -1
SYS_SRV_ACCEPT(service_handle)       → connection_handle / -1
```

`SYS_POST_SERVICE` (syscall 26) registers the calling Proc as the 9P
server for `/srv/<name>` and returns a `KObj_Srv` service handle.
`name_va` is a user-VA pointer to the name bytes; `name_len` is
`1..SRV_NAME_MAX` (32). Gated on the one-way joey-stamped
`PROC_FLAG_MAY_POST_SERVICE` bit. Defined in `kernel/syscall.c` as the
thin user-VA wrapper `sys_post_service_handler` over the testable core:

```c
int sys_post_service_for_proc(struct Proc *p, const char *name,
                              size_t name_len);
```

`SYS_SRV_ACCEPT` (syscall 27) is the poster's accept. The poster of a
`/srv` service **blocks** here until a client opens the service, then
receives the server endpoint of one kernel-minted connection as a
`KObj_Spoor` handle. Gated to the poster: the caller must hold the
service's `KObj_Srv` handle (with `RIGHT_READ`) **and** its `stripes`
must match the registry entry's `poster_stripes` — the stripes match
rejects a stale handle into a service that was tombstoned and rebound by
a different poster. Defined in `kernel/syscall.c` as the thin wrapper
`sys_srv_accept_handler` over the testable core:

```c
int sys_srv_accept_for_proc(struct Proc *p, hidx_t service_h);
```

corvus reads client→server 9P frame bytes off the accepted endpoint with
`SYS_READ`, writes server→client bytes with `SYS_WRITE`, and `SYS_CLOSE`
tears the connection down — no syscall-surface addition for the byte I/O.

Both `_for_proc` cores return the handle (`hidx ≥ 0`) on success, `-1` on
any failure (see Error paths).

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

### Per-connection API — `<thylacine/devsrv.h>`

```c
int  srv_conn_open_for_proc(struct Proc *p, const char *name, u8 name_len);
struct SrvConn *srv_accept_blocking(struct SrvService *svc);
struct Spoor *devsrv_make_conn_spoor(struct SrvConn *conn);
int  srv_backlog_depth(struct SrvService *svc);
```

`srv_conn_open_for_proc` is the client-connect core: it mints a `SrvConn`
for `p` to a `LIVE` service named `name`, enqueues it on the service's
accept backlog, installs a non-transferable `KObj_Srv` connection handle
(obj = the `SrvConn`) in `p`'s handle table, and wakes a blocked
accepter. Returns the connection handle (`hidx ≥ 0`) or `-1`.

`srv_accept_blocking` blocks until a connection is on `svc`'s backlog,
dequeues it, and returns it — ownership of the backlog reference passes
to the caller. Returns `NULL` if `svc` stopped being `LIVE` while
blocked. It is the blocking body behind `sys_srv_accept_for_proc`, and is
single-waiter (corvus is single-threaded).

`devsrv_make_conn_spoor` wraps an accepted `SrvConn` in a `devsrv`
connection Spoor (`dc='s'`, pre-opened) — corvus's server endpoint; the
Spoor's read/write route to the `SrvConn` server side, its close drops
the `SrvConn` reference.

`srv_backlog_depth` returns the current accept-backlog depth (tests +
diagnostics).

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
`dev_init()`. At a3b the live vtable slots are `init` (stamps the type
tag on every registry entry), `attach` (yields the `/srv` root QTDIR
Spoor), `walk` (the `/srv` root, one component deep, to a service
Spoor), and `read` / `write` / `close` (real for a connection Spoor; see
Implementation). `open` remains a deliberate graceful-fail stub (see
Known caveats); the remaining slots (`stat`, `create`, `bread`,
`bwrite`, `remove`, `wstat`, `power`) are graceful-fail stubs.

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

Static storage zero-initializes every entry's `state` to `SRV_STATE_FREE`,
the lock to the all-zero `SPIN_LOCK_INIT` form, and each accept `Rendez`
to `{unlocked, no waiter}`. The `devsrv.init` hook (`devsrv_init`, run
once by `dev_init`) does one job: it stamps `SRV_SERVICE_MAGIC` into every
entry's `magic` field as a permanent struct type tag (see Data structures
— the `magic` field). The lock is a leaf lock: every registry function
takes and releases it without nesting another lock inside.

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

### The accept backlog

`P5-corvus-srv-impl-a3b` gives each `struct SrvService` a bounded FIFO of
kernel-minted-but-not-yet-accepted connections — `backlog[]`,
`backlog_head` / `backlog_tail` / `backlog_count`, all mutated under the
registry lock. `srv_backlog_push_locked` enqueues, `srv_backlog_pop_locked`
dequeues; both are caller-holds-the-lock helpers. The depth is capped at
`SRV_ACCEPT_BACKLOG` (16) — a client connect past a full backlog fails
fast rather than queueing unboundedly (CORVUS-DESIGN.md §6.2). A second,
global cap `SRV_MAX_CONNS` (64) bounds the live `SrvConn` count across all
services (each `SrvConn` pins ~32 KiB of kernel heap).

`srv_backlog_push_locked` re-checks `SRV_STATE_LIVE` before it writes the
slot — so the LIVE re-check and the enqueue are atomic against the
registry lock, closing the window where a service is tombstoned between a
client's pre-check and its push. The push transfers one `srvconn_ref` to
the backlog slot; the matching pop transfers it to the accepter.

### The client-connect path

`srv_conn_open_for_proc(p, name, name_len)` (`kernel/devsrv.c`) is the
client-connect core:

1. Enforce the global cap — `srvconn_total_created() -
   srvconn_total_freed() ≥ SRV_MAX_CONNS` fails fast. The read is racy
   against concurrent create/free, which is fine for a *soft* cap; the
   hard, per-service bound is the accept backlog enforced under the lock.
2. `srv_lookup` + a locked `state == LIVE` check — a missing / `RESERVING`
   / `TOMBSTONED` name fails fast, sparing the heavy mint. This is an
   optimization; the push's LIVE re-check is the correctness gate.
3. `srvconn_create(proc_stripes(p), p->pid,
   proc_is_console_attached(p))` — mint the connection, capturing `p`'s
   identity **by value** (CORVUS-DESIGN.md §6.3 — no raw `Proc *`).
4. `handle_alloc(p, KOBJ_SRV, RIGHT_READ | RIGHT_WRITE, cn)` — install
   the client's connection handle. `handle_alloc` does not take a
   reference, so the `SrvConn`'s create-reference *is* this handle's
   reference; `handle_release_obj`'s `KOBJ_SRV` case `srvconn_unref`s it
   on close.
5. `srvconn_ref(cn)` — a second reference for the backlog slot — then
   `srv_backlog_push_locked` under the registry lock.
6. `wakeup(&svc->accept_rendez)` — wake a poster blocked in
   `SYS_SRV_ACCEPT`, outside the registry lock.

Every failure path unwinds cleanly: a failed `handle_alloc` `srvconn_unref`s
the create-reference (`→ 0 →` teardown + free); a failed push (service
died, or backlog full) drops the backlog reference then `handle_close`s
the handle (whose release drops the create-reference).

### The accept path

`srv_accept_blocking(svc)` is the blocking body; `sys_srv_accept_for_proc`
is the syscall core that gates the caller and wraps the result. The
accept loop: pop the backlog under the registry lock; a non-`NULL` pop
returns at once with ownership; an empty backlog on a still-`LIVE`
service blocks via an **indefinite `sleep`** on `svc->accept_rendez`
(corvus *wants* to wait for clients — there is no deadline here, unlike
the `tsleep`-bounded client recv inside `SrvConn`); an empty backlog on a
no-longer-`LIVE` service returns `NULL`. The wait predicate
`accept_cond_is_ready` reads `backlog_count` / `state` without the
registry lock — `sleep` evaluates it under the `Rendez` lock, and every
producer (the client-connect push, the tombstone drain) mutates these
fields under the registry lock then calls `wakeup`, whose `Rendez`-lock
acquisition is the happens-before. This is `devsrv`'s own restatement of
the `chan_cond_readable` discipline from `srvconn.c` / `pipe.c`. The
accept `Rendez` is single-waiter (corvus is single-threaded — §6.2).

`sys_srv_accept_for_proc` then wraps the accepted `SrvConn` with
`devsrv_make_conn_spoor` — corvus's server endpoint — and installs it as
a `KObj_Spoor` handle. The backlog's reference passes to the connection
Spoor. If the endpoint cannot be built (`devsrv_make_conn_spoor` OOM, or
`handle_alloc` failure) the connection is torn down so the client wakes
with EOF rather than waiting on a server it will never reach.

### The `devsrv` walk

`devsrv_walk` is real at a3b. Only the `/srv` root walks — a root Spoor
is the one whose `aux` is `NULL` — and only one component deep:

- `nname == 0` is a clone: the result is the caller's shallow copy of the
  root, which stays a `/srv` root Spoor (its `aux` is `NULL` too).
- `nname == 1` of a `LIVE` posted service yields a **service Spoor**
  (`QTFILE`) whose `aux` is a `kmalloc`'d `struct devsrv_svc_ref` naming
  that service. Only a `LIVE` service is walkable (`srv_lookup` + a
  locked `state` check).
- `nname > 1` fails — there is no `/srv/<name>/<path>` nesting yet; the
  client-side connection-tree walk is P5-corvus-srv-impl-b.

### Connection-Spoor read / write / close

`devsrv_read` / `devsrv_write` are real for a *connection Spoor* — a
`devsrv` Spoor whose `aux` carries `SRV_CONN_MAGIC`, recognized by the
`devsrv_conn_of` helper. They route straight to `srvconn_server_recv` /
`srvconn_server_send` (the a3a `SrvConn` API). A connection-Spoor read
returns `> 0` bytes, `0` (the ring is empty but the connection is live —
corvus polls again), or `-1` (EOF — the connection is torn down). The
`/srv` root and service Spoors are not readable or writable (`-1`). `off`
is ignored — a connection is a byte stream.

`devsrv_close` discriminates the Spoor's `aux` by the magic word at
offset 0: a root Spoor (`aux == NULL`) is a no-op; a service Spoor
(`DEVSRV_SVC_MAGIC`) `kfree`s the `devsrv_svc_ref` (clearing its magic
first); a connection Spoor (`SRV_CONN_MAGIC`) does `srvconn_teardown`
then `srvconn_unref`. Closing a connection Spoor *is* a connection close
(CORVUS-DESIGN.md §6.2: teardown EOFs both rings so the peer wakes).

### `handle_release_obj` `KOBJ_SRV` magic discriminator

A `KObj_Srv` handle's `obj` is one of two `/srv` kernel objects, and
`handle.c`'s `handle_release_obj` discriminates them by the magic word at
offset 0:

- `SRV_SERVICE_MAGIC` — a service registry entry (from
  `SYS_POST_SERVICE`). Its lifetime is the poster Proc's (tombstoned by
  `exits()`, never freed), not the handle's — closing the handle is a
  **no-op**.
- `SRV_CONN_MAGIC` — a `SrvConn` (the client side of a connection, from
  `srv_conn_open_for_proc`). Closing the handle is a connection close:
  `srvconn_teardown` (idempotent — EOFs both rings so corvus wakes) then
  `srvconn_unref`; the last unref frees.

Neither magic at offset 0 is an `extinction` — a `KObj_Srv` handle's obj
must be one or the other.

### The poster-exit backlog drain

a3b extends `srv_proc_exit_notify`: when it tombstones a dead poster's
`LIVE` service it now also **drains that service's accept backlog**. Each
pending `SrvConn` is collected under the registry lock, then — outside it
— gets `srvconn_teardown` + `srvconn_unref`, so a client blocked on a
connection to a now-dead server wakes with EOF instead of hanging. The
teardown work runs outside the registry lock because `srvconn_teardown` /
`srvconn_unref` take the `SrvConn`'s own locks and `wakeup` takes a
`Rendez` lock — keeping them off any path that re-enters the registry
lock. `srv_proc_exit_notify` also `wakeup`s each tombstoned service's
accept `Rendez` (defensive — the exiting poster is normally its
service's only accepter, but a woken accepter then sees `state != LIVE`
and `srv_accept_blocking` returns `NULL`).

### `SrvService.magic` — a permanent type tag

`SrvService.magic` is a **permanent struct type tag**, not a liveness
bit. It is stamped once for every registry entry by `devsrv_init` (the
Dev's `init` hook) and is **never cleared** — not even by
`srv_clear_locked` when it wipes a slot back to `FREE`. Liveness is
tracked by `state` (`FREE` / `RESERVING` / `LIVE` / `TOMBSTONED`), never
by `magic`. The reason: a `KObj_Srv` service handle may outlive its
entry's `LIVE` state, and `handle_release_obj` must reliably read
`SRV_SERVICE_MAGIC` at offset 0 to discriminate a service object from a
connection object. (Pre-a3b `srv_clear_locked` zeroed the magic — sound
only because a2's `KOBJ_SRV` release was a no-op that never read it;
a3b's release does.)

### Handle-kind asymmetry — a deliberate design point

A connection is reached through two handles of *different* kinds, and the
asymmetry is intentional:

- The **client's** connection handle is `KObj_Srv` — its `obj` is the
  `SrvConn` directly, and it is **non-transferable**. §6.1 requires the
  peer-identity-bearing handle be pinned to the opening Proc; `handle_dup`
  rejects `KObj_Srv` via the existing `NoSrvDup` rule (`specs/handles.tla`
  `SrvHandlesAtOrigin`). A client cannot 9P-transfer its connection to
  another Proc, which is what keeps the kernel-stamped peer identity
  behind it unforgeable.
- corvus's **accepted server-endpoint** handle is `KObj_Spoor` — a
  `devsrv` connection Spoor whose `aux` is the `SrvConn`. corvus does its
  byte I/O through the *existing* `SYS_READ` / `SYS_WRITE` / `SYS_CLOSE`
  with zero syscall-surface change.

The client side is the peer-identity-bearing connection that must not be
transferable; corvus's side is a server I/O endpoint — a byte-I/O Spoor
like a pipe.

### Deferred to P5-corvus-srv-impl-b

a3b is a tracked split — the items below are deferred, not gaps:

- **The §6.2 step-5 handshake drive** — the kernel driving
  `Tversion` / `Tattach` / `Twalk` on the `SrvConn`'s `p9_client`. It
  needs a real corvus 9P server to answer; at a3b corvus is still the
  joey-pipe harness.
- **The production client-open syscall + the `/srv` mount into a
  client's Territory (joey).** At a3b `srv_conn_open_for_proc` is
  exercised directly by tests, not by a syscall.
- **The per-Proc-one-connection cap** (§6.2 "one connection per Proc";
  the `specs/corvus.tla` `SrvBind` precondition). It needs a `struct
  Proc` connection back-pointer with teardown wiring, which lands cleanly
  with the production client-open path. a3b's DoS bounds are the
  per-service accept backlog (`SRV_ACCEPT_BACKLOG`) plus the global
  `SRV_MAX_CONNS` cap — those *are* the §6.2-named "bounded; fails fast"
  hazard; the per-Proc cap is resource hygiene, not a safety invariant.
- **The joey→corvus post-gate stamp** — already deferred from a2.

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

    struct SrvConn *backlog[SRV_ACCEPT_BACKLOG];   // accept FIFO
    u32            backlog_head;     // next-push index; mod SRV_ACCEPT_BACKLOG
    u32            backlog_tail;     // next-pop  index; mod SRV_ACCEPT_BACKLOG
    u32            backlog_count;    // entries buffered; 0..SRV_ACCEPT_BACKLOG
    struct Rendez  accept_rendez;    // the poster blocks here in SYS_SRV_ACCEPT
};
```

`magic` (`SRV_SERVICE_MAGIC`) sits at offset 0 — a **permanent struct
type tag**, stamped once per entry by `devsrv_init` and never cleared
(not even by `srv_clear_locked`); `state`, not `magic`, tracks liveness.
`handle_release_obj` reads it to discriminate a service object from a
connection object, and a `KObj_Srv` service handle may outlive its
entry's `LIVE` state, so the tag must survive the slot returning to
`FREE`. The backlog fields (a3b) are the bounded accept FIFO — mutated
under the registry lock; the create-side reference of each enqueued
`SrvConn` is held by its backlog slot until `SYS_SRV_ACCEPT` pops it. The
struct is purely kernel-internal — never on-disk, on-wire, or
ABI-exposed — so it carries no `_Static_assert` on size.

### `struct devsrv_svc_ref` — `<thylacine/devsrv.h>`

```c
struct devsrv_svc_ref {
    u64  magic;                      // DEVSRV_SVC_MAGIC at offset 0
    u8   name_len;                   // 1..SRV_NAME_MAX
    char name[SRV_NAME_MAX];
};
```

The `aux` of a *service Spoor* — the product of a `/srv` root walk
(`devsrv_walk`, `nname == 1`). It names a posted service **by value**: the
connect path resolves the name fresh via `srv_lookup`, never caching a raw
`SrvService *` (a tombstone-then-rebind reuses the registry slot, so a
cached pointer could name a different service). `DEVSRV_SVC_MAGIC`
(`0x5352564E4F444500`, `"SRVNODE\0"`) at offset 0 distinguishes it from a
connection Spoor's `aux` (a `SrvConn *`, `SRV_CONN_MAGIC`) — that is how
`devsrv_read` / `devsrv_write` / `devsrv_close` tell a service Spoor from
a connection Spoor. `kmalloc`'d by `devsrv_walk`, `kfree`'d by
`devsrv_close`.

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

The per-connection `SrvConn` has its own one-way `LIVE → TORN` lifecycle
machine; it is documented in reference 71 (the `SrvConn` transport). A
registry slot's backlog holds `SrvConn`s between a client connect and the
poster's accept; an accept or a poster-exit drain empties it.

---

## Spec cross-reference

`specs/corvus.tla` already models the connection layer — no spec change
was needed for a3b. The action↔code map:

| Spec action / invariant | Code |
|---|---|
| `MarkMayPost(p)` | `proc_mark_may_post_service` (`kernel/proc.c`) |
| `PostService(p)` | `sys_post_service_for_proc` + `srv_reserve`/`srv_commit` |
| `ServiceTombstone` | `srv_proc_exit_notify`, called from `exits()` |
| `SrvBind` | `srv_conn_open_for_proc` (mint + enqueue the connection) |
| `SrvAccept` | `srv_accept_blocking` / `sys_srv_accept_for_proc` |
| `ProcExit` | the accept-backlog drain in `srv_proc_exit_notify` |
| `ServicePosterEverMarked` | the `proc_may_post_service` gate in `sys_post_service_for_proc` |
| `BuggyPostWithoutMarker` | `corvus_buggy_post_without_marker.cfg` counterexample |

`SrvPeerOp` (the peer-identity read) is **not yet implemented** — it is
P5-corvus-srv-impl-a3c (`SYS_SRV_PEER`). `specs/handles.tla`'s
`WalkDerive` / `WalkChildIsSrv` / `SrvHandlesAtOrigin` pin the
non-transferability of the walked-out `KObj_Srv` connection handle.

`corvus.cfg` is TLC-clean (all 8 invariants); the buggy cfg drives a
`ServicePosterEverMarked` counterexample. The canonical action↔code map
is `specs/SPEC-TO-CODE.md`.

---

## Tests

`kernel/test/test_devsrv.c` — 6 tests (the a2 registry):

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

`kernel/test/test_devsrv_conn.c` — 7 tests (the a3b per-connection
layer):

- `devsrv.walk_service` — a `/srv` root walk of a posted name yields a
  `QTFILE` service Spoor with a `devsrv_svc_ref` aux; a walk of an
  unposted name or two components deep fails; a clone (`nname == 0`)
  stays a `/srv` root.
- `devsrv.conn_open` — `srv_conn_open_for_proc` mints a connection,
  enqueues it, and installs a `KObj_Srv` handle; `handle_dup` refuses the
  handle (`NoSrvDup`); a connect to an unposted or tombstoned service
  fails.
- `devsrv.accept_immediate` — with a connection already backlogged,
  `SYS_SRV_ACCEPT` returns at once with a `KObj_Spoor` server endpoint
  and drains the backlog; the endpoint and the client's handle name the
  same `SrvConn`; a non-poster cannot accept.
- `devsrv.accept_blocks_then_wakes` — `SYS_SRV_ACCEPT` on an empty
  backlog blocks (`THREAD_SLEEPING`, the accept-rendez waiter); a client
  connect wakes it.
- `devsrv.conn_io` — corvus reads `c2s` bytes off its accepted endpoint
  via `SYS_READ` and writes `s2c` bytes via `SYS_WRITE`; an empty live
  connection reads `0`; closing the endpoint tears the connection down.
- `devsrv.conn_release` — a live accepted connection has refcount 2
  (client handle + corvus's endpoint); closing one end tears the
  transport down but does not free; closing the last reference frees the
  `SrvConn`.
- `devsrv.poster_exit_drains_backlog` — `srv_proc_exit_notify` on the
  poster tombstones the service **and** tears down + drains every pending
  backlog connection.

Each test calls `srv_registry_reset()` first for isolation. Suite:
473/473 PASS × default + UBSan.

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

`srv_conn_open_for_proc` returns `-1` on:

| Cause | Detail |
|---|---|
| bad args | `p == NULL`, `name == NULL`, or `name_len` outside `1..SRV_NAME_MAX` |
| no LIVE service | `srv_lookup` miss, or the named entry is `RESERVING` / `TOMBSTONED` |
| global cap reached | live `SrvConn` count ≥ `SRV_MAX_CONNS` |
| backlog full | the service's accept backlog is at `SRV_ACCEPT_BACKLOG` |
| service raced to non-LIVE | tombstoned between the pre-check and the locked push |
| `kmalloc` OOM | `srvconn_create` could not mint the `SrvConn` |
| handle table full | `handle_alloc` failed for the `KObj_Srv` handle |

Every `-1` path unwinds fully — no leaked `SrvConn` reference, no
half-installed handle.

`sys_srv_accept_for_proc` / `SYS_SRV_ACCEPT` return `-1` on:

| Cause | Detail |
|---|---|
| bad service handle | not a `KObj_Srv` handle, missing `RIGHT_READ`, or `obj == NULL` |
| wrong object kind | the `obj`'s magic is not `SRV_SERVICE_MAGIC` (a connection handle, not a service handle) |
| caller not the poster | the caller's `stripes` is `0`, or it does not match `poster_stripes` |
| service stopped being LIVE | `srv_accept_blocking` woke to find the service tombstoned (poster exited / registry reset) |
| endpoint build failed | `devsrv_make_conn_spoor` OOM or `handle_alloc` failure — the accepted connection is torn down |

---

## Status

| Item | State |
|---|---|
| service registry + two-phase post | landed (P5-corvus-srv-impl-a2) |
| `SYS_POST_SERVICE` (syscall 26) | landed |
| `PROC_FLAG_MAY_POST_SERVICE` post-gate | landed |
| poster-exit tombstoning | landed |
| `devsrv` Dev `attach` | landed (a2) |
| accept backlog + `devsrv` walk op + connection-Spoor I/O | landed (P5-corvus-srv-impl-a3b) |
| client-connect path (`srv_conn_open_for_proc`) | landed (a3b) |
| `SYS_SRV_ACCEPT` (syscall 27) + `handle_release_obj` `KOBJ_SRV` | landed (a3b) |
| poster-exit backlog drain | landed (a3b) |
| `SYS_SRV_PEER` (peer-identity read) | deferred to P5-corvus-srv-impl-a3c |
| corvus 9P server + §6.2 step-5 handshake drive | deferred to P5-corvus-srv-impl-b |
| production client-open syscall + joey `/srv` mount | deferred to P5-corvus-srv-impl-b |
| per-Proc-one-connection cap | deferred to P5-corvus-srv-impl-b |
| joey→corvus post-gate stamp | deferred to P5-corvus-srv-impl-b |

---

## Known caveats / footguns

- **`devsrv_open` is a deliberate graceful-fail stub.** A `/srv`
  client-connect yields a `KObj_Srv` handle whose `obj` is a `SrvConn` —
  *not* a Spoor — and the Dev `open` vtable returns a Spoor, so `open`
  structurally cannot host the client-connect. The client-connect core is
  `srv_conn_open_for_proc`; the production client-open syscall that routes
  a namespace open into it lands at P5-corvus-srv-impl-b with the joey
  `/srv` mount.
- **The handle-kind asymmetry is intentional.** The client's connection
  handle is `KObj_Srv` (non-transferable — pins the peer identity);
  corvus's accepted server-endpoint handle is `KObj_Spoor` (a byte-I/O
  Spoor). See Implementation — "Handle-kind asymmetry".
- **No per-Proc connection cap yet.** §6.2's "one connection per Proc" is
  deferred to P5-corvus-srv-impl-b (it needs a `struct Proc` connection
  back-pointer). At a3b the DoS bounds are the per-service accept backlog
  (`SRV_ACCEPT_BACKLOG`) plus the global `SRV_MAX_CONNS` cap — those are
  the §6.2-named safety bounds; the per-Proc cap is resource hygiene.
- **The §6.2 step-5 handshake is not driven yet.** a3b wires the
  transport and the accept path, but the kernel does not yet drive
  `Tversion` / `Tattach` / `Twalk` on a `SrvConn`'s `p9_client` — that
  needs a real corvus 9P server to answer, deferred to
  P5-corvus-srv-impl-b.
- **A connection-Spoor `SYS_READ` is slightly non-POSIX.** It returns `0`
  for an empty-but-live connection (corvus polls again) and `-1` for EOF
  (the connection is torn down) — the inverse of the POSIX convention
  where `0` is EOF. corvus is transport-aware and reads it correctly; a
  generic reader would not.
- The post-gate ships fail-closed: between a2 and impl-b, *no* production
  Proc carries `PROC_FLAG_MAY_POST_SERVICE`, so `SYS_POST_SERVICE` always
  returns `-1` in production. This is correct — the syscall is inert but
  safe until impl-b wires joey's stamp.
- A `SrvService` handle's `obj` points into the static registry array.
  Closing the handle is a no-op (`handle_release_obj` `KOBJ_SRV` case,
  the `SRV_SERVICE_MAGIC` branch) — the registry entry's lifetime is the
  poster Proc's (tombstoned on exit), never the handle's. A connection
  handle (`SRV_CONN_MAGIC` branch) is the opposite — closing it is a
  connection close.
- `srv_lookup` returns a pointer that stays valid (the registry is
  static) but whose `state` may change after the call — callers
  re-validate. `srv_conn_open_for_proc`'s push and `srv_accept_blocking`'s
  pop both re-check `state == LIVE` under the registry lock for exactly
  this reason. Sound at v1.0 (single-CPU, corvus single-threaded).
- `proc_flags` writers (`proc_mark_may_post_service` included) use a
  non-atomic `|=` — sound under the v1.0 single-thread-per-Proc
  invariant; the multi-threaded-Proc lift converts every writer to
  `__atomic_or_fetch`.
