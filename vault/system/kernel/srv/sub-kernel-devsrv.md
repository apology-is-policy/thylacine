---
id: sub-kernel-devsrv
type: sub
title: "devsrv — the /srv service registry, Dev, and accept/peer syscalls"
parent: moc-kernel-srv
code: [kernel/devsrv.c, kernel/include/thylacine/devsrv.h]
audit: hard
guarded-by: [inv-i1]
validated-by: [spec-corvus, gate-smp]
locks: [lock-srv-registry-lock]
hazards: []
abis: []
design: ["docs/STALK-DESIGN.md", "docs/CORVUS-DESIGN.md"]
created: 2026-07-31
updated: 2026-07-31
---
## Purpose

`/srv` is the kernel surface by which a userspace server publishes
itself for per-connection client access — Plan 9's `#s`. A server POSTS
a name with `SYS_WALK_CREATE` on a `/srv` directory; a client CONNECTS
with `SYS_OPEN("/srv/<name>")`; the poster ACCEPTS each kernel-minted
connection and reads its kernel-stamped peer identity with
`SYS_SRV_PEER`. The name-only syscalls (`SYS_POST_SERVICE` 26,
`SYS_SRV_CONNECT` 30, `SYS_POST_SERVICE_BYTE` 43) were RETIRED at
stalk-3c — numbers reserved, no reuse, no compat shim; the namespace IS
the API.

`devsrv` is deliberately a distinct Dev from dev9p: a Spoor walked out
of `/srv` carries `dc='s'`, making the listener a non-transferable
KObj_Srv and the conn endpoints non-dup-able — which is what keeps the
kernel-stamped peer identity behind a connection unforgeable. The
transport those connections ride is [[sub-kernel-srvconn]]; this dossier
owns the registry, the state machine, the Dev, and the syscall layer.

## Contract

**create=post** — `devsrv_post_listener(p, root, name, len, mode, bulk)`
(reached from `sys_walk_create_handler`'s devsrv branch; a dedicated
entry, not the `Dev.create` vtable slot, because a post yields a
KObj_Srv listener HANDLE, not a Spoor). Gated on the one-way
joey-stamped `PROC_FLAG_MAY_POST_SERVICE`; name 1..`SRV_NAME_MAX` of
printable non-`/` ASCII; the parent must be a devsrv root whose aux
re-validates as `SRV_REGISTRY_MAGIC`. `perm & DMSRVBYTE` (bit 25)
selects byte-mode; `perm & DMSRVBULK` (bit 24, CF-3 B) selects the
128 KiB ring class. Returns the listener hidx (obj = the registry entry;
`RIGHT_READ|WRITE`; `handle_dup` refuses it) or −1.

**open=connect** — `devsrv_open_connect(p, c, omode)` (the `Dev.open`
slot resolves `p` from the current Thread). `c` is a `/srv/<name>`
service-ref Spoor; the return is the connection ENDPOINT as a KOBJ_SPOOR
Spoor: a **dev9p root** for a 9P-mode service (the two-step
`srvconn_attach_dev9p_root` Tversion+Tattach — the 9P-unification shared
with `SYS_ATTACH_9P_SRV`) or a **CSRVCLIENT byte-conn Spoor** for a
byte-mode one. Opening the registry ROOT is a plain directory open
(#957 — `cd /srv` / `ls /srv` work; `devsrv_stat_native` reports QTDIR
0555 SYSTEM / a service leaf as QTFILE 0444; devsrv is NOT
`perm_enforced` — per-territory mount visibility, not rwx, is the
boundary).

**`SYS_SRV_ACCEPT`** (`sys_srv_accept_for_proc`) — the poster's blocking
accept. Gates: a KObj_Srv handle with `RIGHT_READ` whose obj magic is
`SRV_SERVICE_MAGIC` (a #844 by-value handle snapshot; the borrow is
released immediately — the registry entry is registry-owned and outlives
the handle), PLUS the stripes match `svc->poster_stripes == proc_stripes(p)`
(rejects a stale handle into a tombstoned-and-rebound service). Blocks
in `srv_accept_blocking`; wraps the dequeued SrvConn in a pre-opened
server-endpoint conn Spoor (`devsrv_make_conn_spoor`) installed as a
KOBJ_SPOOR handle; on any endpoint-build failure the connection is torn
down so the client wakes with EOF rather than waiting on a server it
will never reach.

**`SYS_SRV_PEER`** (`sys_srv_peer_for_proc`) — the kernel's unforgeable
"who is on the other end". Resolves the conn Spoor (RIGHT_READ), rejects
a CSRVCLIENT endpoint (SO_PEERCRED is a SERVER-side query; a client-side
read would report the caller's own identity, and in a same-Proc
client+server the poster gate could not tell them apart — pouch
`getsockopt(SO_PEERCRED)` on the client fd is ENOTSOCK), **captures
every cn-derived value while the Spoor borrow still pins the SrvConn,
then clunks** (the #844-F2 hoist — reading `sp->aux` after the clunk is
a UAF window against a sibling close), then applies the poster gate
(`server_stripes == caller`). The result is the 40-byte append-only
`struct srv_peer_info`: immutable `stripes`/`console` off the SrvConn;
`caps`/`principal_id`/`primary_gid`/`flags`/`pid` resolved FRESH in one
alive-gated `g_proc_table_lock` walk (`proc_peer_snapshot_by_stripes`) —
a dead/reaped peer fail-closes them all to 0/NONE, never a stale
snapshot or a pid a reused table entry now owns. `flags` bit 0 is
`SRV_PEER_FLAG_CONSOLE_RENDERER` (cfg-3 — the tapestryd apply-authority
gate's live single-holder role stamp); `pid` @36 is the V-4a-0b
append-in-place. A dead peer is NOT an error (returns 0 with
`alive == 0`). The handler validates the user VA before the per-byte
store and scrubs on a partial-write fault (no torn identity readable).

**Registry API** — `srv_registry_create` (ref 1; stamps each entry's
permanent magic + `reg` back-pointer + poll list) ·
`srv_registry_ref`/`_unref` (last unref drains every pending connection
then frees; magic cleared first) · `devsrv_attach_registry` (mints a
root Spoor whose aux is the registry, +1 ref) · `srv_boot_registry` (the
one immortal boot registry; kproc's `/srv` mount holds it forever) ·
`srv_lookup_in`/`srv_commit`/`srv_abort`/`srv_proc_exit_notify` ·
counters. `srv_registry_reset` is test-only (undeclared in the header).

## Mechanism

**The registry**: a heap `SrvRegistry` — magic @0, atomic ref, one
irqsave lock, `entries[SRV_MAX_SERVICES]`. Entry magics + `reg`
back-pointers are PERMANENT type tags stamped once at create, never
cleared (a KObj_Srv listener handle outlives its entry's LIVE state and
`handle_release_obj` must still discriminate it); `state` alone tracks
liveness. `srv_clear_locked` wipes a slot back to FREE but leaves magic,
`reg`, and the accept Rendez untouched (clobbering a Rendez would strand
a sleeper).

**Two-phase post**: `srv_reserve_in` (claim slot/name → RESERVING;
capture poster stripes/pid, mode, ring class) → `handle_alloc(KOBJ_SRV)`
→ `srv_commit` (RESERVING→LIVE) or `srv_abort` (→ prior; a FREE-prior
abort wipes, a TOMBSTONED-prior abort restores the tombstone with the
dead-poster identity cleared). A RESERVING entry is never connectable;
the window is bounded by one syscall. **Rebind identity**: a TOMBSTONED
name is re-postable only through the same MAY_POST_SERVICE gate, and
`mode` AND `ring_msize` are part of the service identity — a rebind that
flips either is refused (a client mid-connect captured both atomically
with LIVE; a flip would land a wrong-mode/wrong-geometry connection in
the new poster's backlog). A LIVE or RESERVING name is never displaced.

**Tombstoning**: `exits()` → `srv_proc_exit_notify` → every LIVE entry
whose `poster_stripes` matches flips to TOMBSTONED (identity cleared),
its accept backlog is drained — each pending SrvConn collected under the
lock, then torn down + unref'd OUTSIDE it so the blocked client wakes
with EOF — and the accept rendez + listener poll list are woken (a
tombstone is the listener's POLLHUP edge). The name stays reserved
forever: the marker is the rebind authority (a malicious Proc cannot
race corvus's restart to claim `/srv/corvus`), and the entry pinning is
the stale-handle defense — with the accumulation cost
[[seam-srv-registry-lifecycle]] carries.

**The walk** (`devsrv_walk`): only a root walks, one component deep.
The aux-normalize discipline: `nc` arrives as a shallow clone carrying
an UNOWNED `aux = reg`, so the walk normalizes `nc->aux = NULL` on entry
and commits aux + takes the matching registry ref only on success —
every failure leaves `nc->aux == NULL` and `devsrv_close(nc)` a clean
no-op (no phantom unref). `nname == 0` (the mount-cross clone) mints a
fresh root instance over the SAME registry (+1 ref); `nname == 1` of a
LIVE service yields a QTFILE service Spoor whose aux is a kmalloc'd
`devsrv_svc_ref` — the name BY VALUE plus one registry ref (never a raw
`SrvService *`: a tombstone-rebind reuses the slot, so the connect
resolves the name fresh). Roots carry a per-instance `devno`
(stalk-3a F1 — [[fnd-stalk3a-r1-f1]]) so two registry roots have
distinct mount-key identity.

**open=connect** (`devsrv_open_connect`): global soft cap
(`created − freed ≥ SRV_MAX_CONNS` fails fast; the hard bound is the
per-service backlog under the lock) → resolve the service and capture
`poster_stripes` + `mode` + `ring_msize` under the registry lock
ATOMICALLY with the LIVE check → `srvconn_create` (identity by value;
create ref 1) → byte-mode flag if selected (before publication) → +1 ref
for the backlog slot → `srv_backlog_push_locked` (re-checks LIVE
atomically with the enqueue — the tombstone-between-check-and-push
window is closed here, the pre-check being only an optimization) → wake
the accept rendez + listener pollers after release. Then the endpoint
split: byte-mode wraps the create ref into a CSRVCLIENT conn Spoor;
9P-mode drives the blocking attach handshake LOCK-FREE (the poster,
woken, accepts + answers concurrently), and on success the create ref
drops — the dev9p root owns the session, the poster the backlog ref. On
ANY failure past the push, the conn is torn down first so the poster's
accept sees a dead conn, never a half-connected client.

**Conn-Spoor I/O** (`devsrv_read`/`devsrv_write`): dispatch by the
CSRVCLIENT direction flag. CLIENT endpoint: kernel-attached → −1 (the
stalk-3b F1 guard — [[fnd-stalk3b-r1-f1]]: after `SYS_ATTACH_9P_SRV`
wraps the rings, a userspace read would drain Rread bytes meant for the
kernel client); else blocking client recv / blocking client send.
SERVER endpoint: byte-mode (acquire-load paired with the mint-time
release) → blocking server recv; 9P-mode → NON-blocking recv (the
corvus poll-then-read shape); writes → the blocking server send
unconditionally. `off` ignored (streams).

**Close** (`devsrv_close`) — the first-u64 discriminator over the aux:
a ROOT drops its registry ref (the last drop drains + frees — the boot
registry never gets there); a SVC-REF frees the struct + drops its ref;
a CONNECTION is a connection close — teardown + unref — with teardown
SKIPPED only for the kernel-attached CLIENT endpoint
(`CSRVCLIENT && kernel_attached`: the rings are load-bearing for the
kernel client; teardown migrates to the transport adapter's close). The
SERVER endpoint always tears down even when kernel_attached — the
server closing means the 9P server is GONE and the no-timeout kernel
client observes death ONLY via EOF; honoring the flag there suppressed
the EOF and hung joey's Tclunk forever (the #841 boot-hang root cause;
regression `devsrv.kernel_attached_server_close_eofs`). Unknown magic
extincts.

**Poll**: the listener (a KObj_Srv, not a Spoor) routes via poll.c's
kind dispatch → `srv_handle_poll` → `svc_listener_poll` (POLLIN ↔
backlog non-empty; POLLHUP ↔ not LIVE; sample+register atomic under the
registry lock; producers wake after release). A SrvConn-flavored
KObj_Srv handle is POLLNVAL fail-closed (see [[sub-kernel-srvconn]]
Caveats). The Dev `.poll` slot dispatches conn Spoors to `srvconn_poll`;
roots and svc-refs report no events.

## Data structures

`struct SrvRegistry` — magic @0 (`SRV_REGISTRY_MAGIC`), atomic ref,
irqsave lock, 16 entries. `struct SrvService` — magic @0
(`SRV_SERVICE_MAGIC`, permanent), state, name (not NUL-terminated;
`name_len` authoritative), poster stripes/pid by value, `mode`
(9P/byte) + `ring_msize` (the CF-3 B class — both rebind-identity), the
bounded accept FIFO (`backlog[16]` + head/tail/count), `accept_rendez`
(single-waiter), listener `poll_list`, the permanent `reg`
back-pointer. `struct devsrv_svc_ref` — magic @0 (`DEVSRV_SVC_MAGIC`),
name by value, `reg` (+1 ref). `struct srv_peer_info` — the 40-byte
append-only syscall ABI (stripes@0, caps@8, console@16, alive@20,
principal_id@24, primary_gid@28, flags@32, pid@36; size + every offset
`_Static_assert`-pinned; consumers scan flags by bit, unknown-clear =
absent — its `abi-` note lands with the ABI registry pass). All three
magic-at-offset-0 pins are `_Static_assert`s ([[fnd-p5srv-r1-f2]]); the
four magics (`SRVSVC`/`SRVCONN`/`SRVNODE`/`SRVREGI`) are pairwise
distinct — the whole aux/obj discrimination scheme rests on the first
u64.

Constants: `SRV_NAME_MAX` 32 · `SRV_MAX_SERVICES` 16 (raised 8→16 at
#30 when permanent tombstones filled the registry at the login prompt;
the raise discipline + the ~2 KiB drain-stack cost live in the header
comment) · `SRV_ACCEPT_BACKLOG` 16 (a connect past a full backlog fails
fast) · `SRV_MAX_CONNS` 64 (the global soft cap bounding worst-case
ring memory at ≈32 MiB all-bulk).

## Concurrency

[[lock-srv-registry-lock]] serializes every entry + backlog mutation;
heavy work (SrvConn teardown/unref, all wakes) runs outside it; the
accept cond reads locklessly under the rendez-lock happens-before. The
accept rendez is single-waiter — at most one thread accepts per service
(the poster is its service's single accepter; corvus/stratumd/netd are
single-threaded on the accept path — a documented precondition, not a
guard). Registry-ref discipline: every devsrv Spoor INSTANCE carrying
`aux = reg` holds exactly one ref (the mounted root, each cross-clone,
each svc-ref), dropped at `devsrv_close`; `spoor_ref` on the same
instance adds none. The mortal-registry ordering obligation
([[fnd-stalk3a-r1-f2]]) is the standing constraint on any future
non-immortal registry.

## Invariants enforced

![[inv-i1#Statement]]

On this surface: the registry is reached ONLY through the mounted root's
aux (both post and connect re-validate `SRV_REGISTRY_MAGIC`; stalk-3c
removed the last EL0-reachable global-registry binding — prosecuted and
HELD at [[fnd-stalk3c-r1-f3]]); KObj_Srv non-transferability + the
`dc='s'` dup guard pin the peer identity to the opening Proc
(`specs/handles.tla` SrvHandlesAtOrigin); the kernel-truth peer read is
[[spec-corvus]]'s `ConnOpIdentityIsKernelTruth`/`ConnOpPeerWasLive`,
enforced by the by-value capture + the alive-gated fresh walk.

## Error paths

Everything returns −1/NULL fail-closed with full unwind: post (unmarked
Proc, bad name byte/length, non-root parent, LIVE/RESERVING name
collision, mode/class rebind flip, registry full, handle-table full →
`srv_abort` rollback — no stale entry survives any failure); connect
(dead/missing service, raced tombstone at the push, global cap, backlog
full [double-unref: backlog + create refs], OOM, handshake failure
[teardown-then-unref so the poster sees a dead conn]); accept (wrong
kind/rights/magic, stripes mismatch, service died while blocked,
endpoint-build failure [teardown so the client EOFs]); peer (bad handle,
non-conn Spoor, CSRVCLIENT, poster mismatch, bad user VA, store fault
[scrubbed]). A dead peer is success-with-zeros, not an error.

## Performance

Cold-path throughout: posts and connects are boot/session-rate events;
lookups scan ≤16 entries under a spinlock. The accept path adds one
rendez sleep per idle wait. The costs that matter are memory bounds
(the constants above) and the drain-stack note on
[[lock-srv-registry-lock]].

## Prosecution

What an auditor attacks here:

- **The post gate**: `MAY_POST_SERVICE` checked on the create=post path
  (an unmarked Proc must never post or rebind — corvus.tla
  ServicePosterEverMarked); the name hygiene (a name is a future path
  component — a `/` or control byte that survives becomes a resolver
  ambiguity).
- **Rebind identity**: mode + ring class immutable across a tombstone
  rebind — a flip lands a wrong-geometry conn in the new poster's
  backlog (the F2 discipline; `srv_client.byte_mode_mode_change_rebind_refused`).
- **Capture-atomic-with-LIVE**: the connect's
  stripes/mode/class capture and the push's LIVE re-check both under the
  registry lock; a capture outside it races the tombstone.
- **The ref dances**: open_connect's create/backlog/adapter refs across
  every failure branch (leak-free, no double-unref, teardown-before-
  abandon so no party strands); the walk's aux-normalize (a failed walk
  must leave `nc->aux == NULL` — the phantom-unref class); accept's
  endpoint-fail teardown.
- **The close discriminator**: the kernel-attached skip stays
  CLIENT-only (suppressing the server-side EOF re-opens the #841 boot
  hang; tearing down the client side EOFs a load-bearing mount — the 16c
  smoking gun); unknown magic stays an extinction.
- **The poster gates**: accept's stripes match (a stale handle into a
  rebound service must not accept the new poster's clients); peer's
  poster gate + the CSRVCLIENT reject + the #844 capture-before-clunk
  hoist (re-reading `sp->aux` after the clunk is the UAF).
- **The peer read's fail-closure**: every mutable field rides ONE
  alive-gated walk — a dead peer must never yield stale caps, a stale
  renderer grant, or a reused pid.
- **The mortal-registry obligation**: any future non-immortal registry
  must order its last unref after every handle into `entries[]` closes,
  or give those holders covering refs ([[seam-srv-registry-lifecycle]]).
- **I-1**: no EL0-reachable path may bind a registry other than through
  a mounted root's aux.

## Seams

- [[seam-srv-registry-lifecycle]] — tombstone accumulation + the shared
  boot registry + the mortal-registry ordering + per-registry fairness.
- [[seam-srv-9p-connect-unit]] — the 9p-mode connect's missing unit
  harness (boot E2E is the regression).

## Caveats

- **As-built registry sharing (recorded design drift)**: the per-session
  registry the A-5b note intended was never built — every login session
  shares the ONE boot registry via mount inheritance, which is exactly
  why per-user tombstones accumulate (#30) and cross-session NAMES are
  visible. Isolation is downstream (single-session proxy, dataset scope,
  per-user DEK) until the seam closes.
- **Global-cap fairness** ([[fnd-stalk3b-r1-f3]], accepted): one
  multi-thread Proc can hold all 64 conn slots, starving other Procs'
  connects — memory stays bounded; fairness caps arrive with per-session
  registries.
- **The 9P-mode server read is non-POSIX by design**: 0 = empty-but-live
  (poll again), −1 = EOF — the inverse convention; byte-mode is POSIX
  (blocking, 0 = EOF). A generic reader on a 9P-mode endpoint would
  misread it.
- `handle_release_obj`'s KOBJ_SRV `SRV_CONN_MAGIC` arm is defensive dead
  code post-stalk-3c (the only KObj_Srv obj is a listener; conn
  endpoints are Spoors) — retained as a corruption canary
  ([[fnd-stalk3c-r1-f1]]).
- The listener handle's obj points INTO `entries[]`; closing it is a
  no-op (the entry's lifetime is the poster's, tombstoned never freed).
- `srv_lookup_in` returns a pointer whose `state` may change after the
  call — every consumer re-validates under the lock (the push and the
  accept both do).

## Provenance

(generated from incoming `touched` edges — shaped by
[[chg-2026-05-19-srv-birth]] (registry + two-phase post + conn layer +
peer identity), [[chg-2026-06-02-stalk3a-registry]] (namespace-resident
refcounted registries), [[chg-2026-06-03-stalk3b-open-connect]]
(open=connect + the 9P-unification), [[chg-2026-06-03-stalk3c-retire]]
(the syscall retirement), [[chg-2026-06-24-348-s2c-blocking]] +
[[chg-2026-07-08-cf3b-bulk-ring]] (the blocking-write arms + ring
classes on the I/O dispatch). The A-1a identity fields, the cfg-3
renderer flag, and the V-4a-0b pid append on `srv_peer_info` are
recorded at their own arcs' sweeps.)

## Tests

Roster verified against `kernel/test/test.c` — 26 `devsrv.*`:
`registered` · `post_gate` · `post_basic` · `tombstone` ·
`registry_full` · `registry_full_tombstone_rebinds` (#30's at-capacity
asymmetry) · `post_rollback` · `post_listener` · `walk_service` ·
`registry_lifecycle` · `svc_ref_holds_registry` · `open_connect_byte` ·
`open_root_dir` (#957) · `stat_native_root` · `accept_immediate` ·
`accept_blocks_then_wakes` · `conn_io` · `conn_release` ·
`poster_exit_drains_backlog` · `kernel_attached_io_refused` (stalk-3b
F1) · `kernel_attached_server_close_eofs` (#841) · `srv_peer_identity` ·
`srv_peer_dead_peer` · `srv_peer_gate` · `srv_peer_bad_args` ·
`srv_peer_renderer_flag` (cfg-3). Plus 5 `srv_client.*` driving the
production create=post/open=connect cores end-to-end
(`byte_mode_conn_dispatch` · `byte_mode_propagates_to_conn` ·
`byte_mode_mode_change_rebind_refused` ·
`byte_mode_server_recv_blocking_eof` · `no_per_proc_cap`). The 9p-mode
connect has NO unit case ([[seam-srv-9p-connect-unit]]); the boot E2E
(joey/login/legate → corvus + stratumd) is its regression.
