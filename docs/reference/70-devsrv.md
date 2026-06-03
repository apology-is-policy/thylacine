# 70 — devsrv: the `/srv` service registry

**Status**: as-built at A-5b-0 **stalk-3c** (syscall retirement). The service
registry, the `devsrv` Dev, the now-retired `SYS_POST_SERVICE` syscall, the
`proc_flags` post-gate, and poster-exit tombstoning landed at P5-corvus-srv-impl-a2; the
**per-connection layer** — the `devsrv` walk op, the client-connect path
(`srv_conn_open_for_proc`), the bounded accept backlog, `SYS_SRV_ACCEPT`,
and the connection-Spoor read/write/close — landed at a3b; `SYS_SRV_PEER`
(the peer-identity read) landed at a3c. **stalk-3a** made the registry
**namespace-resident**: the single static `g_srv_registry` became a
heap-allocated, refcounted `SrvRegistry` reached *through the mounted devsrv
root Spoor* (the root's `aux`), and boot mounts one immortal registry on the
kproc `/srv` synthetic dir. **stalk-3b-α** added the
**create=post** path: a `SYS_WALK_CREATE` against a `/srv` directory mints a
`KObj_Srv` listener (`devsrv_post_listener`), the Plan-9-true symmetric
sibling of the open=connect — `perm & DMSRVBYTE` selects byte-mode,
else 9P-mode. **stalk-3b-β (this revision)** lands the **open=connect** core
(`devsrv_open_connect`): an `open` of a `/srv/<name>` service-ref Spoor mints a
`SrvConn`, enqueues it on the poster's accept backlog, and returns the
connection ENDPOINT as a `KOBJ_SPOOR` Spoor — a **dev9p root** for a 9p-mode
service (the two-step attach: `srvconn_attach_dev9p_root` drives Tversion +
Tattach, the **9P-unification** shared with `SYS_ATTACH_9P_SRV`) or a
**CLIENT-direction byte-conn Spoor** (`CSRVCLIENT`) for a byte-mode one. The
3b-β-C migrated the connect-side clients (joey/login/legate -> the two-step
`SYS_OPEN`) + retargeted `SYS_ATTACH_9P_SRV` to a `KOBJ_SPOOR` CSRVCLIENT conn
Spoor + removed the per-Proc cap; **3b-β-D retired the embedded
`srvconn_client_*` 9P client** (the 9p-mode connect now drives the SHARED kernel
client only), made `SYS_SRV_CONNECT` **byte-only** (it fail-closed-rejects a 9P
service — 9P is open=connect), and collapsed `srv_conn_count` to a reserved pad
(struct Proc stays 264). **3b-β-E (F1)** added the `kernel_attached`
no-direct-I/O guard to `devsrv_read`/`devsrv_write`'s CSRVCLIENT branches (a
userspace read/write on a conn endpoint already wrapped by the kernel 9P client
would corrupt its wire — the same guard the `KOBJ_SRV` r/w arms carry, now
following the endpoint to `KOBJ_SPOOR`). **stalk-3c (this revision)** RETIRED the
three name-only syscalls -- `SYS_POST_SERVICE` (26), `SYS_SRV_CONNECT` (30),
`SYS_POST_SERVICE_BYTE` (43); numbers reserved, no reuse, no compat shim. Posting
is now `SYS_WALK_CREATE` on a `/srv` dir (`devsrv_post_listener`); connecting is
`SYS_OPEN("/srv/<name>")` (`devsrv_open_connect`); the read/write handle resolver
collapsed to KOBJ_SPOOR-only (a `/srv` conn endpoint is itself a `KOBJ_SPOOR` conn
Spoor). corvus's POST migrated to create=post (post-before-chroot) and the pouch
AF_UNIX seam to bind=create / connect=open. The boot registry is still resolved
through the mounted `/srv` root that the create=post / open=connect path walks.

> **stalk-3a delta in one place.** The registry was a single static
> `struct SrvRegistry g_srv_registry`; it is now heap-allocated +
> refcounted (`srv_registry_create` / `srv_registry_ref` /
> `srv_registry_unref`), reached through a mounted devsrv root Spoor whose
> `aux` *is* the registry (`SRV_REGISTRY_MAGIC` at offset 0).
> **Registry-ref discipline** (mirrors dev9p's `attached_owner`): every
> devsrv Spoor instance carrying `aux = reg` holds exactly ONE registry ref
> — the mounted root, each `clone_walk_zero` cross-clone of it, and each
> `/srv/<name>` service-ref Spoor — dropped at `devsrv_close` (which fires
> only on the Spoor's last `spoor_clunk`). `spoor_ref` (same instance) adds
> NO registry ref; only a new instance does. The name-based core API is the
> internal `_in(reg, …)` variant (`srv_reserve_in` / `srv_lookup_in` /
> `srv_proc_exit_notify_in`); the boot path + the in-kernel tests bind the
> boot registry (`srv_boot_registry()`) explicitly. (The EL0-reachable
> name-only syscall wrappers `srv_reserve` / `srv_lookup` /
> `srv_conn_open_for_proc` were RETIRED at stalk-3c -- posting is now
> create=post, connecting open=connect.) The svc-based
> API (`srv_commit` / `srv_abort` / `srv_accept_blocking` /
> `srv_backlog_depth`) reaches the registry lock through a new `svc->reg`
> back-pointer. Boot (`kernel/joey.c`, in kproc's bringup after the
> devramfs chroot) `stalk`s `/srv` (`STALK_MOUNT`), mints
> `devsrv_attach_registry(srv_boot_registry())`, and `mount`s it (MREPL) so
> joey + every descendant shares the one registry via `territory_clone`
> mount inheritance; the boot registry is immortal because kproc's mount
> holds a ref forever. STALK-DESIGN.md §5.1.

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
table (`kernel/handle.c`), and is reached from userspace through the
namespace: a server POSTS with `SYS_WALK_CREATE` on a `/srv` directory and a
client CONNECTS with `SYS_OPEN("/srv/<name>")`. (The name-only
`SYS_POST_SERVICE` / `SYS_SRV_CONNECT` syscalls were retired at stalk-3c.)

---

## Public API

### Syscalls

```
SYS_WALK_CREATE(/srv-dir, name, perm)      → service_handle / -1   (post)
SYS_OPEN("/srv/<name>", omode)             → conn endpoint   / -1   (connect)
SYS_SRV_ACCEPT(service_handle)             → connection_handle / -1
SYS_SRV_PEER(connection_handle, out_va)    → 0 / -1
```

A service POST is a `SYS_WALK_CREATE` against a `/srv` directory Spoor
(`devsrv_post_listener`): it registers the calling Proc as the 9P server for
`/srv/<name>` and returns a `KObj_Srv` service handle. `perm & DMSRVBYTE`
selects byte-mode, else 9P-mode; `name` is `1..SRV_NAME_MAX` (32). Gated on
the one-way joey-stamped `PROC_FLAG_MAY_POST_SERVICE` bit. A CONNECT is a
`SYS_OPEN("/srv/<name>")` (`devsrv_open_connect`; see "open=connect" below).

> The name-only `SYS_POST_SERVICE` (syscall 26) -- the `sys_post_service_for_proc`
> core + the `sys_post_service_handler` wrapper -- was RETIRED at stalk-3c. The
> number stays reserved (no reuse, no compat shim).

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

`SYS_SRV_PEER` (syscall 28) reads a `/srv` connection's
kernel-stamped peer identity (CORVUS-DESIGN.md §6.3; invariant C-22).
`connection_handle` is the `KObj_Spoor` connection endpoint from
`SYS_SRV_ACCEPT`; `out_va` is a user-VA pointer to a 24-byte
`struct srv_peer_info` (see Data structures) the kernel fills on success.
The peer identity is stamped by the kernel, never supplied by the client
and never cached on corvus's fid state — corvus calls `SYS_SRV_PEER` per
request to learn who is on the other end. Gated to the service's poster:
the caller's `stripes` must match the connection's recorded poster.
Defined in `kernel/syscall.c` as the thin wrapper `sys_srv_peer_handler`
over the testable core:

```c
int sys_srv_peer_for_proc(struct Proc *p, hidx_t conn_h,
                          struct srv_peer_info *out);
```

`sys_srv_peer_for_proc` returns `0` on success (`*out` filled), `-1` on
any failure. Both `_for_proc` cores above return the handle (`hidx ≥ 0`)
on success, `-1` on any failure (see Error paths).

### Registry API — `<thylacine/devsrv.h>`

```c
// Per-territory registry lifecycle (stalk-3a).
struct SrvRegistry *srv_registry_create(void);
void                srv_registry_ref(struct SrvRegistry *reg);
void                srv_registry_unref(struct SrvRegistry *reg);
struct Spoor       *devsrv_attach_registry(struct SrvRegistry *reg);
struct SrvRegistry *srv_boot_registry(void);
u64                 srv_registry_total_created(void);
u64                 srv_registry_total_destroyed(void);

// Name-based registry API (the retained SYS_POST_SERVICE / SYS_SRV_CONNECT
// path) — thin wrappers bound to the boot registry over internal _in(reg).
int  srv_reserve(const char *name, u8 name_len, struct Proc *poster,
                 enum srv_mode mode,
                 struct SrvService **svc_out, enum srv_state *prior_out);
void srv_commit(struct SrvService *svc);          // reaches svc->reg->lock
void srv_abort(struct SrvService *svc, enum srv_state prior);
struct SrvService *srv_lookup(const char *name, u8 name_len);
void srv_proc_exit_notify(struct Proc *p);
int  srv_registry_count(void);
```

`srv_registry_create` allocates a fresh refcounted registry (ref = 1;
stamps each entry's permanent magic + poll_list + `reg` back-pointer);
`srv_registry_ref` / `srv_registry_unref` are the refcount (the last unref
drains every pending connection then frees). `devsrv_attach_registry` mints
a `/srv` root Spoor whose `aux` is the registry (taking one ref).
`srv_boot_registry` returns the one immortal boot registry the retained
syscall path resolves. See "Per-territory registry (stalk-3a)" below.

`srv_reserve` / `srv_commit` / `srv_abort` are the reserve-then-commit
two-phase post: a registry slot is claimed (`RESERVING`), then either
committed to `LIVE` or rolled back. `srv_lookup` finds a service by name.
`srv_proc_exit_notify` is the poster-exit hook. `srv_registry_count`
returns the number of non-`FREE` entries (tests + diagnostics). Each is a
thin wrapper over an internal `*_in(reg, …)` variant bound to
`srv_boot_registry()`; `srv_commit` / `srv_abort` reach the registry lock
through the entry's `reg` back-pointer.

`srv_registry_reset()` exists in `kernel/devsrv.c` as **test support
only** — it is not declared in `devsrv.h` and has no production caller; it
drains the boot registry.

### Per-connection API — `<thylacine/devsrv.h>`

```c
int  srv_conn_open_for_proc(struct Proc *p, const char *name, u8 name_len);
struct SrvConn *srv_accept_blocking(struct SrvService *svc);
struct Spoor *devsrv_make_conn_spoor(struct SrvConn *conn);
struct SrvConn *devsrv_conn_of(struct Spoor *c);
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

`devsrv_conn_of` is the inverse: it resolves a `devsrv` connection Spoor
back to its `SrvConn`, returning `NULL` for a `NULL` / non-`devsrv` Spoor
or for a `devsrv` *root* / *service* Spoor (discriminated by `dc` and the
`aux`'s first `u64`). It was made non-static at a3c — `SYS_SRV_PEER`
resolves corvus's accepted endpoint handle to its `SrvConn` through it.

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
`dev_init()`. The live vtable slots are `init` (stamps the type
tag on every registry entry), `attach` (yields the `/srv` root QTDIR
Spoor), `walk` (the `/srv` root, one component deep, to a service
Spoor), `open` (stalk-3b-β = **connect**: a `/srv/<name>` service-ref Spoor
mints a `SrvConn` and returns the connection endpoint — a dev9p root Spoor for
9p-mode, a `CSRVCLIENT` byte-conn Spoor for byte-mode; the slot resolves the
connecting Proc from `current_thread()->proc` and calls `devsrv_open_connect`),
and `read` / `write` / `close` (real for a connection Spoor — `read`/`write`
mirror by the `CSRVCLIENT` direction, `close` honors
`srvconn_is_kernel_attached`; see Implementation). The remaining slots
(`stat`, `create`, `bread`, `bwrite`, `remove`, `wstat`, `power`) are
graceful-fail stubs (`create`=post rides the dedicated `devsrv_post_listener`
handler branch, not the vtable slot).

---

## Implementation

### The registry

`kernel/devsrv.c` holds the `struct SrvRegistry` — **heap-allocated +
refcounted** since stalk-3a (a single static `g_srv_registry` pre-3a):

```c
struct SrvRegistry {
    u64               magic;    // SRV_REGISTRY_MAGIC at offset 0; 0 once freed
    int               ref;      // instance refcount (atomic)
    spin_lock_t       lock;
    struct SrvService entries[SRV_MAX_SERVICES];   // SRV_MAX_SERVICES == 8
};
static struct SrvRegistry *g_boot_srv_registry;    // the one immortal boot registry
```

`srv_registry_create` `kmalloc`s one with `KP_ZERO` (every entry's `state`
reads `SRV_STATE_FREE`, each accept `Rendez` `{unlocked, no waiter}`),
stamps `magic` + `ref = 1` + `spin_lock_init(&lock)`, and stamps each
entry's permanent `SRV_SERVICE_MAGIC` + `reg` back-pointer +
`poll_waiter_list_init`. The `devsrv.init` hook (`devsrv_init`, run once by
`dev_init`) creates the one boot registry into `g_boot_srv_registry`. The
lock is a leaf lock: every registry function takes and releases it without
nesting another lock inside. `magic` (`SRV_REGISTRY_MAGIC`) at offset 0
both discriminates a devsrv root Spoor's `aux` and fast-fails a
stale-pointer read (cleared at free, the `spoor_free_internal` UAF-defense
pattern).

`corvus` is the only v1.0 service in the boot registry (`/srv/stratum-ctl`
is a *mounted* 9P tree, not a `devsrv` service); `SRV_MAX_SERVICES = 8` is
headroom. A future login session gets its OWN registry (A-5b-body), so a
second user's coordinator is unnameable from another session (I-1).

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

### Posting via `create` (stalk-3b-α)

`SYS_WALK_CREATE` against a `/srv` directory — a devsrv root Spoor (`dc =
's'`, `aux` = a `SrvRegistry`, `SRV_REGISTRY_MAGIC` at offset 0) — is a
service **post**, the Plan-9-true symmetric sibling of the open=connect to
come (STALK-DESIGN.md §5.3 / D2). `sys_walk_create_handler` detects the
devsrv root *after* validating the name and *before* the generic clone-walk,
and routes to `devsrv_post_listener` (`kernel/devsrv.c`):

```c
int devsrv_post_listener(struct Proc *p, struct Spoor *root,
                         const char *name, size_t name_len, enum srv_mode mode);
```

It mirrors `sys_post_service_core` exactly — the same `PROC_FLAG_MAY_POST_-
SERVICE` gate, the same printable-ASCII name hygiene, the same
reserve → `handle_alloc(KOBJ_SRV)` → commit two-phase (rolled back via
`srv_abort` on a full handle table) — but bound to the registry behind
`root` (resolved from `root->aux`, re-validated against `SRV_REGISTRY_MAGIC`)
rather than the boot registry, and returns the listener `hidx` directly.

**Why a dedicated entry, not the `Dev.create` vtable slot.** A listener is a
`KObj_Srv` handle whose `obj` is a `SrvService`; the generic
`sys_walk_create_handler` installs the returned Spoor as a `KObj_Spoor`. The
post yields a *different handle kind*, so it cannot ride the Spoor-returning
create path. `devsrv_create` (the vtable slot) stays a graceful-fail stub;
the handler branch is the post path. A non-root devsrv Spoor used as a create
parent (a connection or service-ref Spoor) falls through to the generic path,
where `devsrv_walk` with `nname == 0` rejects non-roots — so a create in a
connection cleanly returns `-1` with no leak.

**`DMSRVBYTE`.** `perm & SYS_WALK_CREATE_DMSRVBYTE` (`0x02000000`, bit 25 —
unused by Plan 9's standard `DM*` set, so collision-free; pinned by a
`_Static_assert`) posts byte-mode; its absence posts 9P-mode — the Plan 9
`DM*` perm-bit idiom (the mode is a service attribute, not an open intent).
The bit is admitted through the create-perm validation so it reaches the
devsrv branch; a regular (non-`/srv`) create **rejects** it, so it can never
leak into a dev9p `Tlcreate` perm.

No client posts via `create` yet — corvus still uses `SYS_POST_SERVICE`; the
migration (post-before-chroot) lands with the open=connect client migration
in stalk-3b-β.

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

`devsrv_walk` is real. Only the `/srv` root walks — a root Spoor is the
one whose `aux` is its `SrvRegistry` (`SRV_REGISTRY_MAGIC` at offset 0;
pre-stalk-3a the root's `aux` was `NULL`) — and only one component deep.

**The aux-normalize discipline (stalk-3a).** `nc` arrives as a
`spoor_clone` of the root, so `spoor_clone` shallow-copied `nc->aux = reg`
— but a clone holds NO registry ref. `devsrv_walk` therefore **normalizes
`nc->aux = NULL` on entry** and sets `nc->aux` (plus takes the matching
registry ref) only on a success path. A walk failure then leaves
`nc->aux == NULL`, so `devsrv_close(nc)` is a clean no-op — never a phantom
`srv_registry_unref` of a ref the clone never took. (This matches the
dev9p discipline: a clone's `aux` is unowned until the walk takes
ownership; `clone_walk_zero` / stalk's failure paths detach `aux` before
`spoor_unref`.)

- `nname == 0` is a clone (the `cross_mounts` cross): `nc` becomes a FRESH
  `/srv` root instance over the SAME registry — it takes its own registry
  ref (mirroring dev9p's `attached_owner` clone-bump), dropped at
  `devsrv_close(nc)`.
- `nname == 1` of a `LIVE` posted service (resolved in the root's registry
  via `srv_lookup_in(reg, …)` + a locked `state` check) yields a **service
  Spoor** (`QTFILE`) whose `aux` is a `kmalloc`'d `struct devsrv_svc_ref`
  carrying a registry ref.
- `nname > 1` fails — there is no `/srv/<name>/<path>` nesting yet.

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
offset 0: a **root Spoor (`SRV_REGISTRY_MAGIC`)** `srv_registry_unref`s the
registry (stalk-3a — the last unref drains + frees; the boot registry
never reaches that, its mount holding a ref forever); a service Spoor
(`DEVSRV_SVC_MAGIC`) `kfree`s the `devsrv_svc_ref` (clearing its magic
first) then `srv_registry_unref`s its registry ref; a connection Spoor
(`SRV_CONN_MAGIC`) tears down + `srvconn_unref`s. An
`aux == NULL` Spoor (a failed/transient walk clone, normalized) is a clean
no-op. Closing a connection Spoor *is* a connection close (CORVUS-DESIGN.md
§6.2: teardown EOFs both rings so the peer wakes).

The teardown is **skipped only for the kernel-attached CLIENT endpoint**
(`(c->flag & CSRVCLIENT) && srvconn_is_kernel_attached(cn)`): when the kernel 9P
client wraps a conn's rings, those rings are load-bearing for that client and a
userspace close of the redundant CLIENT endpoint must not EOF them (teardown
migrates to the adapter's `transport.close` at `p9_attached_destroy`). The
**SERVER endpoint** (corvus's accepted Spoor — no `CSRVCLIENT`) is the other side
of the same shared SrvConn and carries the flag the CLIENT's attach set, but its
close means the 9P server is GONE and MUST EOF the rings so the kernel client's
blocked recv wakes with EOF (#841: the no-timeout client observes connection
death via EOF; honoring `kernel_attached` on the server side suppressed the EOF
and hung joey's Tclunk forever — the boot-hang root cause). So the skip is
gated on the CLIENT direction; the regression is `devsrv.kernel_attached_server_close_eofs`.

### `SYS_SRV_PEER` — the peer-identity read

`SYS_SRV_PEER` (a3c, `kernel/syscall.c`) is corvus's read of a `/srv`
connection's kernel-stamped peer identity (CORVUS-DESIGN.md §6.3;
invariant C-22). It is the kernel's unforgeable answer to "who is on the
other end of this connection" — corvus never trusts a client's
self-report and never caches the identity on its fid state; it calls
`SYS_SRV_PEER` per request. The spec contract is `specs/corvus.tla`
`SrvPeerOp` (resolve the peer *fresh* every op) plus `ConnOpPeerWasLive`
(a dead peer fail-closes).

`sys_srv_peer_for_proc(p, conn_h, out)` is the testable core:

1. `sys_lookup_spoor(p, conn_h, RIGHT_READ)` — resolve `conn_h` to a
   `KObj_Spoor` handle the caller holds. `RIGHT_READ` is
   defense-in-depth: `SYS_SRV_ACCEPT` installs `READ | WRITE` on the
   endpoint, so the check is redundant with that path but pins the
   contract — `SYS_SRV_PEER` reads a connection, and the endpoint a
   reader holds is a `READ`-righted handle.
2. `devsrv_conn_of(sp)` — the `SrvConn` behind the Spoor. It returns
   `NULL` for a pipe, a `dev9p` Spoor, or a `devsrv` *root* / *service*
   Spoor, so a handle that is a Spoor but not a `/srv` *connection*
   endpoint fails here.
3. **Poster gate** — `proc_stripes(p)` must be non-zero **and** equal
   `srvconn_server_stripes(cn)`. The `SrvConn` captured the service
   poster's `stripes` by value at mint; only that poster (corvus) may
   query the connection's peer. A `stripes` of `0` (an unstamped or
   torn-read caller) fails the gate fail-closed.
4. **Immutable identity** — `srvconn_peer_stripes(cn)` and
   `srvconn_peer_console(cn)`. These come straight off the `SrvConn`,
   where they were captured by value at mint. No Proc lookup — so they
   are knowable even after the peer Proc exits and is reaped, with no
   use-after-free.
5. **Live caps + the dead-Proc guard** — `proc_caps_by_stripes(
   peer_stripes, &peer_caps)` (reference 14) re-finds the peer by
   `stripes` under `g_proc_table_lock` and snapshots its live `caps`. If
   no `ALIVE` Proc carries that `stripes` (the peer exited / is a zombie
   / was reaped), it returns `false` and `out->caps` / `out->alive` both
   fail-close to `0` — never a stale capability snapshot. This is the
   kernel half of `corvus.tla` `ConnOpPeerWasLive`.

On success the core fills `*out` (`stripes`, `caps`, `console`, `alive`)
and returns `0`. A **dead peer is not an error**: the syscall still
returns `0`, with `alive` = `0` and `caps` = `0` — the immutable
`stripes` / `console` are still knowable; only the mutable caps
fail-close.

`sys_srv_peer_handler(conn_h_raw, out_va)` is the user-VA wrapper. It
validates `out_va` with `sys_validate_user_buf(out_va, sizeof(struct
srv_peer_info))` **before** any store (`uaccess_store_u8` does not
range-check), calls the core into a zero-initialized stack `struct
srv_peer_info`, then stores the 24-byte struct to `out_va` per-byte via
`uaccess_store_u8` with fault fixup. On a partial-write fault it scrubs
the bytes already written back to `0` before returning `-1` — so
userspace can never read a torn peer identity (the
`sys_wait_pid_handler` torn-write discipline). `case SYS_SRV_PEER` in
`syscall_dispatch` routes `x0` / `x1` into it.

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
    struct poll_waiter_list poll_list;   // listener pollers (P5-poll-b)
    struct SrvRegistry *reg;         // owning registry (stalk-3a; permanent)
};
```

The `reg` back-pointer (stalk-3a) is stamped once at `srv_registry_create`
and never cleared — it lets the svc-taking API reach `svc->reg->lock`
without threading a `reg` argument through every signature.

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
    struct SrvRegistry *reg;         // the registry this service-ref names into (stalk-3a)
};
```

The `aux` of a *service Spoor* — the product of a `/srv` root walk
(`devsrv_walk`, `nname == 1`). The `reg` field (stalk-3a) carries ONE
registry ref (taken in `devsrv_walk`, dropped in `devsrv_close`) and is the
registry the service is resolved fresh in — per-territory, never a global. It names a posted service **by value**: the
connect path resolves the name fresh via `srv_lookup`, never caching a raw
`SrvService *` (a tombstone-then-rebind reuses the registry slot, so a
cached pointer could name a different service). `DEVSRV_SVC_MAGIC`
(`0x5352564E4F444500`, `"SRVNODE\0"`) at offset 0 distinguishes it from a
connection Spoor's `aux` (a `SrvConn *`, `SRV_CONN_MAGIC`) — that is how
`devsrv_read` / `devsrv_write` / `devsrv_close` tell a service Spoor from
a connection Spoor. `kmalloc`'d by `devsrv_walk`, `kfree`'d by
`devsrv_close`.

### `struct srv_peer_info` — `<thylacine/syscall.h>`

```c
struct srv_peer_info {
    u64 stripes;     // peer Proc's identity tag (0 → unidentifiable peer)
    u64 caps;        // peer's live capability set; 0 when alive == 0
    u32 console;     // 1 iff the peer is console-attached, else 0
    u32 alive;       // 1 iff an ALIVE Proc still carries `stripes`, else 0
};
```

The `SYS_SRV_PEER` result — the kernel writes one of these to the
syscall's `out_va` buffer. Unlike `struct SrvService` / `struct
devsrv_svc_ref` (kernel-internal), this is a **syscall-ABI type**: a
userspace consumer (corvus, at P5-corvus-srv-impl-b) decodes a fixed
24-byte record, so the layout is pinned by `_Static_assert`s on the
total size (24) and every field offset (`stripes` 0, `caps` 8, `console`
16, `alive` 20 — naturally aligned, no implicit padding). `stripes` /
`console` are the peer's **immutable** identity (captured by value on
the `SrvConn` at mint); `caps` / `alive` are the **mutable** part — read
live, fail-closed to `0` when the peer is no longer an `ALIVE` Proc.

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
was needed for a3b or a3c. The action↔code map:

| Spec action / invariant | Code |
|---|---|
| `MarkMayPost(p)` | `proc_mark_may_post_service` (`kernel/proc.c`) |
| `PostService(p)` | `sys_post_service_for_proc` + `srv_reserve`/`srv_commit` |
| `ServiceTombstone` | `srv_proc_exit_notify`, called from `exits()` |
| `SrvBind` | `srv_conn_open_for_proc` (mint + enqueue the connection) |
| `SrvAccept` | `srv_accept_blocking` / `sys_srv_accept_for_proc` |
| `SrvPeerOp` | `sys_srv_peer_for_proc` / `sys_srv_peer_handler` |
| `ProcExit` | the accept-backlog drain in `srv_proc_exit_notify` |
| `ServicePosterEverMarked` | the `proc_may_post_service` gate in `sys_post_service_for_proc` |
| `ConnOpIdentityIsKernelTruth` | `SYS_SRV_PEER` reads the `SrvConn`'s by-value `peer_stripes` / `peer_console`, never a client report |
| `ConnOpPeerWasLive` | `proc_caps_by_stripes`' dead-Proc guard (`alive` / `caps` fail-close to `0`) |
| `BuggyPostWithoutMarker` | `corvus_buggy_post_without_marker.cfg` counterexample |

`SrvPeerOp` (the peer-identity read) is **as-built at a3c** — `SYS_SRV_PEER`,
`kernel/syscall.c`. The two invariants `ConnOpIdentityIsKernelTruth` (the
peer identity is the kernel's record, not the client's word) and
`ConnOpPeerWasLive` (a dead peer authorizes nothing) are implemented by
the immutable-by-value read and the `proc_caps_by_stripes` dead-Proc
guard respectively. `specs/handles.tla`'s `WalkDerive` /
`WalkChildIsSrv` / `SrvHandlesAtOrigin` pin the non-transferability of
the walked-out `KObj_Srv` connection handle.

`corvus.cfg` is TLC-clean (all 8 invariants); the buggy cfg drives a
`ServicePosterEverMarked` counterexample. The canonical action↔code map
is `specs/SPEC-TO-CODE.md`.

---

## Tests

`kernel/test/test_devsrv.c` — 7 tests (the a2 registry + the stalk-3b-α
create=post):

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
- `devsrv.post_listener` (stalk-3b-α) — the create=post MECHANISM:
  `devsrv_post_listener` on a `/srv` root mints a `LIVE` `KObj_Srv` listener
  in that root's registry; the selected mode (9P / byte) is recorded; the
  `MAY_POST_SERVICE` gate holds on this path too (unmarked Proc → `-1`); and
  the parent must be a registry ROOT (a service-ref Spoor is rejected).

`kernel/test/test_devsrv_conn.c` — 11 tests (the a3b per-connection
layer + the a3c `SYS_SRV_PEER` tests):

- `devsrv.walk_service` — a `/srv` root walk of a posted name yields a
  `QTFILE` service Spoor with a `devsrv_svc_ref` aux; a walk of an
  unposted name or two components deep fails; a clone (`nname == 0`)
  yields a fresh `/srv` root instance over the **same registry**
  (stalk-3a: `nc4->aux == root->aux`, `SRV_REGISTRY_MAGIC`).
- `devsrv.registry_lifecycle` (stalk-3a) — the registry-ref crux: a heap
  registry's ref counts the devsrv Spoor INSTANCES carrying `aux=reg` (the
  attached root + each clone-walk-zero), each dropped at `devsrv_close`;
  the last unref drains + frees (`srv_registry_total_destroyed` bumps), and
  no Spoor is leaked (allocated-delta == freed-delta). Proves the
  normalize-aux discipline (no phantom unref).
- `devsrv.svc_ref_holds_registry` (stalk-3a) — a `/srv/<name>` service-ref
  Spoor + a 2nd root over the BOOT registry each take + drop a registry ref
  via `devsrv_walk` / `devsrv_close`; the boot registry is never freed by
  this churn (immortal) and still resolves the posted service.
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
- `devsrv.srv_peer_identity` — `SYS_SRV_PEER` on an accepted connection
  returns the kernel-stamped peer identity: `stripes`, `console`, `caps`,
  and `alive` are all correct. The `caps` read is **live** — a mutation
  of the peer's capability set shows on the next `SYS_SRV_PEER` call.
- `devsrv.srv_peer_dead_peer` — the dead-Proc guard: against a `ZOMBIE`
  peer, then a reaped peer, `SYS_SRV_PEER` returns `0` with `alive` = `0`
  and `caps` = `0` (fail-closed) while the immutable `stripes` still
  survives.
- `devsrv.srv_peer_gate` — only the service poster may query a
  connection's peer: a non-poster — even one holding the connection
  endpoint Spoor — is refused; a `KObj_Srv` handle is rejected as the
  wrong kind.
- `devsrv.srv_peer_bad_args` — argument validation: a NULL Proc, a NULL
  `out`, or a bad handle each return `-1`.

Each test calls `srv_registry_reset()` first for isolation; the
`SYS_SRV_PEER` tests additionally splice their bare-allocated peer Procs
into the process table with `proc_test_link` (so `proc_caps_by_stripes`
can see them) and `proc_test_unlink` before freeing. Suite: 708/708 PASS
× default (smp4) + UBSan + smp8 at stalk-3a.

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

`sys_srv_peer_for_proc` returns `-1` on:

| Cause | Detail |
|---|---|
| bad args | `p == NULL` or `out == NULL` |
| bad connection handle | `conn_h` is not a `KObj_Spoor` handle the caller holds with `RIGHT_READ` |
| not a `/srv` connection Spoor | `devsrv_conn_of` returned `NULL` — the Spoor is a pipe, a `dev9p` Spoor, or a `devsrv` root / service Spoor, not a connection endpoint |
| caller not the poster | the caller's `stripes` is `0`, or it does not equal the connection's `server_stripes` |

`sys_srv_peer_handler` / `SYS_SRV_PEER` returns `-1` additionally on:

| Cause | Detail |
|---|---|
| bad user-VA | `out_va` is outside the readable user range (`sys_validate_user_buf`) |
| store fault | a per-byte `uaccess_store_u8` faulted — the bytes already written are scrubbed to `0` first |

A **dead peer is not an error**: `SYS_SRV_PEER` returns `0` with `alive`
= `0` and `caps` = `0` — the immutable `stripes` / `console` are still
filled. Only the dead-Proc guard fail-closes the mutable fields.

---

## poll (P5-poll-b)

`devsrv` is the second `.poll` implementor in the kernel (after
`devpipe` at P5-poll-a). Two distinct surfaces are pollable:

**The listener** (a KObj_Srv handle whose obj is a SrvService — the
handle `SYS_POST_SERVICE` returned). corvus polls it to learn when a
client has connected. The KObj_Srv handle kind is NOT a Spoor — the
Dev `.poll` vtable can't reach it. `kernel/poll.c::poll_scan_one`
recognizes `slot->kind == KOBJ_SRV` and routes through
`srv_handle_poll(obj, events, pw)` — a parallel-handle-kind dispatch
that reads the obj's first u64 (its magic) and forwards to
`svc_listener_poll` (SrvService) or `srvconn_poll` (SrvConn).

The listener-readiness signal is `backlog_count > 0` (POLLIN) and
`state != LIVE` (POLLHUP). `svc_listener_poll` samples both under the
registry lock + registers the `poll_waiter` under the same lock — the
register-then-observe step. The producer side wakes the listener
`poll_list` at:
- `srv_conn_open_for_proc` after a successful `srv_backlog_push`
  (backlog grew → POLLIN edge);
- `srv_proc_exit_notify` after the poster's services are tombstoned
  (POLLHUP edge);
- `srv_registry_reset` after the registry is wiped (test-only).

Every wake site fires `poll_waiter_list_wake(&svc->poll_list)` AFTER
the registry lock is released, mirroring the existing
`wakeup(&svc->accept_rendez)` discipline (the lock-then-mutate /
release / wake pattern from `kernel/pipe.c`).

**The connection Spoor** (the KObj_Spoor handle corvus gets from
`SYS_SRV_ACCEPT`). corvus polls it for POLLIN (bytes to read) and
POLLOUT (room to write); a teardown surfaces POLLHUP + POLLERR.
`devsrv_poll` (the Dev vtable slot) dispatches by aux:
- aux == NULL → a failed/transient walk clone (normalized): returns 0.
- aux's first u64 == SRV_REGISTRY_MAGIC → `/srv` root Spoor: returns 0
  (no readiness; a directory poll — stalk-3a, was the aux==NULL case).
- aux's first u64 == DEVSRV_SVC_MAGIC → service-ref Spoor (the result
  of walking `/srv/<name>`): returns 0 (not pollable as transport).
- aux's first u64 == SRV_CONN_MAGIC → connection Spoor: delegates to
  `srvconn_poll` in `kernel/srvconn.c`.

The connection-Spoor `.poll` mechanics live in
`docs/reference/71-srvconn.md` (the SrvConn owns the transport state).

**Lock ordering**. The poll-list lock is internal to
`struct poll_waiter_list` and last in the order: for the listener,
the chain is `g_srv_registry.lock` → `svc->poll_list.lock` →
`pw->rendez->lock`; for the connection, the chain is
`cn->c2s.lock` → `cn->s2c.lock` → `cn->poll_list.lock` →
`pw->rendez->lock`. Producers acquire and release the readiness-state
lock(s) BEFORE calling `poll_waiter_list_wake`, so the wake's
list-lock acquisition cannot deadlock with the producer's state lock.

---

## Status

| Item | State |
|---|---|
| heap+refcounted per-territory `SrvRegistry` + boot `/srv` mount | landed (A-5b-0 stalk-3a) |
| `srv_registry_create`/`_ref`/`_unref` + `devsrv_attach_registry` + `srv_boot_registry` | landed (stalk-3a) |
| create=post (`devsrv_post_listener` + `SYS_WALK_CREATE` `/srv` branch + `DMSRVBYTE`) | landed (stalk-3b-α) |
| `devsrv_open`=connect (two-step 9P-unification) + native client migration | landed (stalk-3b-β A/B/C1/C2) |
| retire the embedded per-SrvConn `srvconn_client_*` 9P client + the per-Proc cap (`SRV_CONN_PER_PROC_MAX`/`srv_conn_count`) | landed (stalk-3b-β-D) |
| `SYS_SRV_CONNECT` byte-only (fail-closed-rejects a 9P service) | landed (stalk-3b-β-D) |
| `kernel_attached` no-direct-I/O guard on `devsrv_read`/`devsrv_write` CSRVCLIENT branches | landed (stalk-3b-β-E F1) |
| `devsrv_create` Dev vtable slot (still a graceful-fail stub; post rides the handler branch) | by design |
| retire `SYS_SRV_CONNECT` / `SYS_POST_SERVICE` (subsumed by `SYS_OPEN` / `SYS_WALK_CREATE`) | deferred to stalk-3c |
| service registry + two-phase post | landed (P5-corvus-srv-impl-a2) |
| `SYS_POST_SERVICE` (syscall 26) | landed |
| `PROC_FLAG_MAY_POST_SERVICE` post-gate | landed |
| poster-exit tombstoning | landed |
| `devsrv` Dev `attach` | landed (a2) |
| accept backlog + `devsrv` walk op + connection-Spoor I/O | landed (P5-corvus-srv-impl-a3b) |
| client-connect path (`srv_conn_open_for_proc`) | landed (a3b) |
| `SYS_SRV_ACCEPT` (syscall 27) + `handle_release_obj` `KOBJ_SRV` | landed (a3b) |
| poster-exit backlog drain | landed (a3b) |
| `SYS_SRV_PEER` (syscall 28) peer-identity read + poster gate | landed (a3c) |
| `devsrv_poll` Dev vtable slot + connection-Spoor `.poll` | landed (P5-poll-b) |
| `srv_handle_poll` KObj_Srv dispatch + listener `.poll` | landed (P5-poll-b) |
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
