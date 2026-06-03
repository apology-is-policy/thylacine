// devsrv — the /srv service registry Dev (P5-corvus-srv-impl-a2).
//
// Per ARCHITECTURE.md §9.4 + §11.2c + CORVUS-DESIGN.md §6.1. `/srv` is a
// kernel Dev — `devsrv`, Plan 9's `#s` heritage — deliberately distinct
// from `dev9p` (the Dev backing kernel-mounted 9P trees). The separation
// is structural: every Spoor walked out of `/srv` is a KObj_Srv kernel
// object (non-transferable; ARCH §18.2, specs/handles.tla SrvKObjs), so a
// connection's kernel-stamped peer identity (§6.3) cannot be forged by
// transferring the handle.
//
// A userspace 9P server registers a name with SYS_POST_SERVICE; the
// kernel mediates per-connection client access. `corvus` (CORVUS-DESIGN
// §6) is the v1.0 consumer.
//
// At P5-corvus-srv-impl-a2 (this chunk):
//   - The service registry: a fixed-size table of named services, each
//     keyed to its poster Proc by `stripes` (CORVUS-DESIGN.md §6.3 — by
//     value, never a raw Proc*, so a poster's exit is never a UAF).
//   - SYS_POST_SERVICE registers the calling Proc, gated on the one-way
//     joey-stamped PROC_FLAG_MAY_POST_SERVICE bit.
//   - On poster exit the kernel TOMBSTONES the name (srv_proc_exit_notify
//     from exits()): the name stays reserved, re-postable only by a
//     joey-marked Proc — so a malicious Proc cannot race corvus's restart
//     to claim /srv/corvus.
//   - The devsrv Dev itself attaches a /srv root directory Spoor.
//
// Deferred to P5-corvus-srv-impl-a3: the devsrv walk op (a walk of /srv/
// <name> mints a per-connection KObj_Srv Spoor — specs/handles.tla
// WalkDerive), the transport pair, SYS_SRV_ACCEPT, SYS_SRV_PEER.
//
// Spec: specs/corvus.tla — MarkMayPost / PostService / ServiceTombstone,
// invariant ServicePosterEverMarked.

#ifndef THYLACINE_DEVSRV_H
#define THYLACINE_DEVSRV_H

#include <thylacine/poll.h>
#include <thylacine/rendez.h>
#include <thylacine/types.h>

struct Dev;
struct Proc;
struct Spoor;
struct SrvConn;
struct SrvRegistry;
struct poll_waiter;

// Maximum service-name length. Service names are short identifiers
// ("corvus"); 32 is generous and bounds the registry entry + the
// SYS_POST_SERVICE kernel-stack name scratch.
#define SRV_NAME_MAX  32u

// Service registry capacity. corvus is the sole v1.0 service (ARCH §9.4);
// 8 is headroom. A post past the cap fails fast.
#define SRV_MAX_SERVICES  8u

// SRV_SERVICE_MAGIC — sentinel at offset 0 of struct SrvService. Lets the
// KObj_Srv handle-release path discriminate a service object from a
// connection object (SRV_CONN_MAGIC, <thylacine/srvconn.h>) by the magic
// word: a KObj_Srv handle's obj is a SrvService for a SYS_POST_SERVICE
// handle, a SrvConn for a client connection handle.
#define SRV_SERVICE_MAGIC  0x53525653564300ULL    // 'SRVSVC' || 0x00

// SRV_REGISTRY_MAGIC — sentinel at offset 0 of struct SrvRegistry
// (stalk-3a). A devsrv root Spoor's aux is its SrvRegistry; this magic
// lets devsrv_close / devsrv_poll / devsrv_conn_of discriminate the root
// (a SrvRegistry) from a service-ref (DEVSRV_SVC_MAGIC) and a connection
// (SRV_CONN_MAGIC) by the aux's first u64 — the same first-u64 dispatch
// the service/connection objects already use. Cleared at free (UAF
// defense), so a stale root-Spoor aux read fast-fails.
#define SRV_REGISTRY_MAGIC  0x5352565245474900ULL  // 'SRVREGI' || 0x00

// Per-service accept backlog depth — the count of kernel-minted-but-not-
// yet-accepted /srv connections held for the poster. A client open past
// the backlog fails fast rather than queueing unboundedly (CORVUS-DESIGN
// §6.2). corvus accepts promptly, so a short backlog suffices.
#define SRV_ACCEPT_BACKLOG  16u

// Global cap on live /srv connections. Each SrvConn pins ~32 KiB of
// kernel heap (two rings + the dedicated p9_client); the cap bounds the
// worst-case exposure. CORVUS-DESIGN §6.2 sizes this at corvus's
// MAX_USERS order (~256); v1.0 caps at 64 (~2 MiB worst case) — a
// tunable, raised when a multi-user workload needs the headroom.
#define SRV_MAX_CONNS  64u

// DEVSRV_SVC_MAGIC — sentinel at offset 0 of struct devsrv_svc_ref, the
// aux of a service Spoor (a /srv root walked to a service name). Lets
// devsrv's read/write/close ops discriminate a service Spoor's aux from
// a connection Spoor's aux (a SrvConn *, SRV_CONN_MAGIC) by the first u64.
#define DEVSRV_SVC_MAGIC  0x5352564E4F444500ULL   // 'SRVNODE' || 0x00

// devsrv_svc_ref — the aux of a service Spoor (devsrv_walk's product). It
// names a posted service by value; devsrv_open / srv_conn_open resolve it
// fresh via srv_lookup (no raw SrvService * — a tombstone-then-rebind
// reuses the registry slot, so a cached pointer could name a different
// service).
struct devsrv_svc_ref {
    u64  magic;                      // DEVSRV_SVC_MAGIC
    u8   name_len;                   // 1..SRV_NAME_MAX
    char name[SRV_NAME_MAX];
    // stalk-3a: the registry this service-ref names into. Carries ONE
    // registry ref (taken in devsrv_walk when the ref is minted, dropped
    // in devsrv_close). The service is resolved fresh in THIS registry
    // (devsrv_open in 3b), never a global — per-territory isolation.
    struct SrvRegistry *reg;
};

// F2 close (P5-corvus-srv-impl audit): pin the magic to offset 0. Read
// by `devsrv_close` / `devsrv_poll` to discriminate the aux's owning
// object kind; a field reorder that buries `magic` elsewhere would
// silently misread garbage at offset 0 — undetected at build time,
// extinction at runtime.
_Static_assert(__builtin_offsetof(struct devsrv_svc_ref, magic) == 0,
               "magic at offset 0 — devsrv_close / devsrv_poll discriminate "
               "by the aux's first u64");

// Service-entry lifecycle.
//
// FREE       — slot unused. Zero-initialized storage reads as FREE.
// RESERVING  — a SYS_POST_SERVICE is in flight: the slot + name are
//              claimed but not yet committed (the handle has not been
//              allocated). Transient — bounded by one syscall; never
//              observed as a connectable service.
// LIVE       — posted; the poster Proc is alive and is the 9P server.
// TOMBSTONED — the poster exited. The name stays reserved (not freed):
//              only a joey-marked Proc may rebind it via SYS_POST_SERVICE.
enum srv_state {
    SRV_STATE_FREE       = 0,
    SRV_STATE_RESERVING  = 1,
    SRV_STATE_LIVE       = 2,
    SRV_STATE_TOMBSTONED = 3,
};

// Service transport mode — selected by the poster at registration.
//
// SRV_MODE_9P    — the 9P-shaped channel (CORVUS-DESIGN.md §6.2). The
//                  client SYS_srv_connect drives a kernel-internal 9P
//                  handshake (Tversion+Tattach+Tlopen) over the SrvConn
//                  before returning; subsequent read/write on the client
//                  KObj_Srv handle become Tread/Twrite frames via the
//                  kernel-owned p9_client. The server endpoint Spoor
//                  reads RAW T-frame bytes from c2s — the userspace
//                  server is a 9P responder (corvus, future stratumd).
//                  This is the default / pre-existing mode.
//
// SRV_MODE_BYTE  — raw byte stream (P6-pouch-sockets, sub-chunk 12).
//                  SYS_srv_connect SKIPS the 9P handshake and refuses a
//                  non-empty path. Client-side read/write route through
//                  srvconn_client_send/recv — raw chan_produce / chan_
//                  consume_nonblock against the SrvConn's c2s/s2c rings.
//                  Server endpoint Spoor reads raw c2s as before. The
//                  net effect: a POSIX AF_UNIX SOCK_STREAM byte-channel
//                  with no 9P framing in the data path.
//
// The mode is captured by value on the SrvConn at mint (srv_conn_open_
// for_proc reads the service mode under the registry lock alongside
// poster_stripes), so a tombstone-then-rebind cannot change the mode
// of an already-minted connection.
enum srv_mode {
    SRV_MODE_9P   = 0,
    SRV_MODE_BYTE = 1,
};

// A registered (or tombstoned) service. The poster identity is captured
// BY VALUE (`poster_stripes`, `poster_pid`) — CORVUS-DESIGN.md §6.3: the
// registry holds no raw Proc*, so a poster Proc that exits and is reaped
// never turns a registry read into a use-after-free.
struct SrvService {
    u64            magic;            // SRV_SERVICE_MAGIC
    enum srv_state state;
    u8             name_len;         // 1..SRV_NAME_MAX; bytes valid in name[]
    char           name[SRV_NAME_MAX];
    u64            poster_stripes;   // poster Proc's stripes tag (by value)
    int            poster_pid;       // poster Proc's PID (by value; diagnostics)
    enum srv_mode  mode;             // transport: 9P (default) or byte stream
                                     // (P6-pouch-sockets, sub-chunk 12). Set at
                                     // srv_reserve, immutable through LIVE.

    // Accept backlog — a bounded FIFO ring of kernel-minted connections
    // awaiting SYS_SRV_ACCEPT. A client open enqueues one (holding a
    // srvconn_ref); the poster's accept dequeues it (the ref transfers to
    // the accepted connection Spoor). Mutated under the registry lock.
    struct SrvConn *backlog[SRV_ACCEPT_BACKLOG];
    u32            backlog_head;     // next-push index;  mod SRV_ACCEPT_BACKLOG
    u32            backlog_tail;     // next-pop  index;  mod SRV_ACCEPT_BACKLOG
    u32            backlog_count;    // entries buffered; 0..SRV_ACCEPT_BACKLOG
    struct Rendez  accept_rendez;    // the poster blocks here in SYS_SRV_ACCEPT

    // Pollers registered on the listener KObj_Srv handle (P5-poll-b). The
    // listener-ready signal is POLLIN when backlog_count > 0 (corvus has
    // a client waiting to accept); a tombstone surfaces POLLHUP.
    // Producers (srv_backlog_push_locked on a successful enqueue,
    // srv_proc_exit_notify on the tombstone, srv_registry_reset on a
    // drain) wake this list AFTER releasing the registry lock — the
    // accept_rendez wake's discipline (specs/poll.tla MakeReady).
    struct poll_waiter_list poll_list;

    // stalk-3a: back-pointer to the owning registry. Stamped once at
    // srv_registry_create (alongside `magic`), never cleared. Lets the
    // svc-taking API (srv_commit / srv_abort / srv_accept_blocking /
    // srv_backlog_depth / svc_listener_poll) reach the registry lock
    // without threading a separate `reg` argument through every signature.
    struct SrvRegistry *reg;
};

// F2 close (P5-corvus-srv-impl audit): pin the magic to offset 0. Read
// by `handle_release_obj` / `srv_handle_poll` / `devsrv_poll` /
// `sys_lookup_rw_handle` / `sys_srv_accept_for_proc` / `handle_close`
// to discriminate SrvService from SrvConn at a KObj_Srv handle's obj
// pointer. SrvConn's matching pin lives in `srvconn.h`.
_Static_assert(__builtin_offsetof(struct SrvService, magic) == 0,
               "magic at offset 0 — KObj_Srv handle-release / poll / accept "
               "paths read the first u64 of the obj to discriminate "
               "SrvService vs SrvConn");

// The devsrv Dev. dc='s' (Plan 9 #s); registered by dev_init().
extern struct Dev devsrv;

// =============================================================================
// Per-territory service registry (stalk-3a).
// =============================================================================
//
// STALK-DESIGN.md §5.1. `/srv` is namespace-resident: the registry is a
// heap-allocated, refcounted SrvRegistry reached THROUGH the mounted
// devsrv root Spoor (the root's `aux`), not a global or a Territory field
// (Plan-9-true — named through the namespace). Boot mounts one immortal
// registry on the kproc `/srv` synthetic dir; login (A-5b-body) mounts a
// fresh per-session registry, so a second user's coordinator is
// structurally unnameable (I-1).
//
// Registry-ref discipline (mirrors dev9p's attached_owner): EVERY devsrv
// Spoor instance carrying `aux = reg` holds exactly ONE registry ref — the
// mounted root, each clone_walk_zero cross-clone of it, and each
// /srv/<name> service-ref Spoor. devsrv_close drops exactly one. spoor_ref
// (same instance) adds NO registry ref; only a new instance (spoor_clone +
// walk0, or a fresh service-ref) does. The registry outlives any single
// Spoor; it is freed at the last srv_registry_unref.

// srv_registry_create — allocate a fresh registry (ref = 1; stamps each
// entry's permanent magic + poll_list + reg back-pointer). Returns NULL on
// OOM. The caller owns the initial reference.
struct SrvRegistry *srv_registry_create(void);

// srv_registry_ref / srv_registry_unref — refcount. The last unref drains
// every pending connection (the srv_registry_reset teardown discipline)
// then frees. NULL-safe (unref). Extincts on corrupt magic / underflow.
void srv_registry_ref(struct SrvRegistry *reg);
void srv_registry_unref(struct SrvRegistry *reg);

// devsrv_attach_registry — mint a /srv root directory Spoor whose `aux` is
// `reg` (taking ONE registry ref). The mount source at boot + per session.
// Returns NULL on OOM (no ref taken). spoor_clunk of the returned root
// (on its last holder) runs devsrv_close, which drops the registry ref.
//
// CLONE CONTRACT (the dev9p-aux discipline; audit F3): a `spoor_clone` of
// a devsrv root carries a SHALLOW copy of `aux = reg` but holds NO registry
// ref. The clone's `aux` is UNOWNED until `devsrv.walk` takes ownership
// (its normalize-then-ref discipline). A caller that clones a devsrv root
// and then `spoor_clunk`s it WITHOUT an intervening `devsrv.walk` would
// make devsrv_close phantom-`srv_registry_unref` a ref the clone never
// took -- so a devsrv-root clone MUST pass through `devsrv.walk` (which
// every kernel path does: `stalk`'s clone_walk_zero + main-loop walk), or
// have its `aux` detached before clunk.
struct Spoor *devsrv_attach_registry(struct SrvRegistry *reg);

// srv_boot_registry — the one immortal registry mounted on kproc's /srv at
// boot. The create=post / open=connect path resolves it through the mounted
// /srv root; the in-kernel test harness uses it via the `_in` name-based
// cores + srv_registry_reset. NULL before devsrv_init.
struct SrvRegistry *srv_boot_registry(void);

// Cumulative registry lifecycle counters (tests verify drain-on-last-unref
// without dereferencing a freed registry). (created - destroyed) == live.
u64 srv_registry_total_created(void);
u64 srv_registry_total_destroyed(void);

// =============================================================================
// Service registry.
// =============================================================================
//
// Posting (create=post -> devsrv_post_listener) is a reserve-then-commit
// two-phase post so a failing handle_alloc rolls back cleanly (the entry is
// never observably LIVE until the handle exists):
//
//   srv_reserve()  — claim a slot + name; the entry goes RESERVING.
//   srv_commit()   — RESERVING -> LIVE (handle allocated; post succeeded).
//   srv_abort()    — RESERVING -> prior (handle_alloc failed; rolled back).

// srv_commit — phase 2 (success). RESERVING -> LIVE. Takes the registry
// lock. `svc` must be a reservation from the reserve phase (srv_reserve_in,
// the internal slot claim devsrv_post_listener drives).
void srv_commit(struct SrvService *svc);

// srv_abort — phase 2 (failure). RESERVING -> `prior` (the value the
// reserve phase returned in *prior_out). Takes the registry lock.
void srv_abort(struct SrvService *svc, enum srv_state prior);

// srv_lookup_in — find a service by name in an explicit `reg` (the devsrv
// open=connect / walk internal lookup). The in-kernel test harness uses it
// against srv_boot_registry() now that the name-only SYS_POST_SERVICE /
// SYS_SRV_CONNECT path is retired (stalk-3c). Takes the registry lock.
struct SrvService *srv_lookup_in(struct SrvRegistry *reg,
                                 const char *name, u8 name_len);

// srv_proc_exit_notify — called from exits() for every exiting Proc. Any
// LIVE service whose poster is `p` (matched by stripes) is TOMBSTONED:
// the name stays reserved, awaiting a joey-marked rebind. A Proc that
// never posted is a cheap no-op scan. Maps to specs/corvus.tla
// ServiceTombstone.
void srv_proc_exit_notify(struct Proc *p);

// srv_registry_count — number of non-FREE registry entries. Tests +
// diagnostics; takes the registry lock.
int srv_registry_count(void);

// devsrv_post_listener — the create=post path (stalk-3b, STALK-DESIGN.md §5.3
// / D2). A SYS_WALK_CREATE against a /srv directory (a devsrv root Spoor) is a
// service post: mint a KObj_Srv listener bound to `p` for service `name` in
// `root`'s registry and return its handle index. `mode` (derived by the
// caller from the create perm's DMSRVBYTE bit) selects byte- vs 9P-mode.
//
// Gated on PROC_FLAG_MAY_POST_SERVICE — the SAME one-way joey-stamped gate
// SYS_POST_SERVICE checks (CORVUS-DESIGN.md §6.1); an unmarked Proc gets -1.
// `root` MUST be a devsrv root (dc='s', aux = a SrvRegistry, SRV_REGISTRY_
// MAGIC) -- the caller (sys_walk_create_handler's devsrv branch) verifies that
// before calling; this re-reads the aux defensively. The post is the same
// reserve -> handle_alloc(KObj_Srv) -> commit two-phase as SYS_POST_SERVICE,
// rolled back via srv_abort on a full handle table. Returns hidx >= 0 or -1
// (bad args / unmarked Proc / oversized-or-non-printable name / name already
// LIVE-or-RESERVING / registry full / handle table full).
//
// Why a dedicated entry, not the Dev `.create` vtable slot: a listener is a
// KObj_Srv handle whose obj is a SrvService, but the generic
// sys_walk_create_handler installs the returned Spoor as a KOBJ_SPOOR. The
// post yields a different handle KIND, so it cannot ride the Spoor-returning
// create path; the handler branches to here and returns the hidx directly.
int devsrv_post_listener(struct Proc *p, struct Spoor *root,
                         const char *name, size_t name_len, enum srv_mode mode);

// =============================================================================
// Per-connection layer (P5-corvus-srv-impl-a3b).
// =============================================================================
//
// A client Proc reaches corvus over its own kernel-minted connection (a
// SrvConn, <thylacine/srvconn.h>). The kernel mints + owns all transport
// (invariant C-23). CORVUS-DESIGN.md §6.2.

// devsrv_open_connect — the open=connect core (stalk-3b-β; STALK-DESIGN.md §5.2).
// `c` is a /srv/<name> service-ref Spoor (devsrv_walk's product); mint a SrvConn
// for `p`, enqueue it on the poster's accept backlog, and return the connection
// ENDPOINT as a KOBJ_SPOOR Spoor: a dev9p root (9p-mode, via the two-step
// p9_attached wrap) or a CLIENT-direction byte-conn Spoor (byte-mode, CSRVCLIENT).
// Returns NULL on bad-Spoor / no-LIVE-service / cap / OOM / handshake failure.
//
// The Dev.open vtable slot (devsrv_open) calls this with current_thread()->proc;
// it is non-static so the test harness can drive it with an explicit Proc.
struct Spoor *devsrv_open_connect(struct Proc *p, struct Spoor *c, int omode);

// srv_accept_blocking — the poster's accept: block until a connection is
// on `svc`'s accept backlog, then dequeue and return it. The returned
// SrvConn carries the one reference the backlog held — ownership passes
// to the caller. Returns NULL if `svc` ceased to be LIVE while blocked
// (the poster exited, or a test reset the registry).
//
// PRECONDITION: at most one thread accepts on a given service at a time
// (corvus is single-threaded; the accept Rendez is single-waiter).
struct SrvConn *srv_accept_blocking(struct SrvService *svc);

// devsrv_make_conn_spoor — wrap an accepted SrvConn in a devsrv connection
// Spoor (dc='s', pre-opened). The Spoor's read/write route to the SrvConn
// server side; its close drops the SrvConn reference the caller passed.
// Returns NULL on OOM (the caller still owns `conn`'s reference).
struct Spoor *devsrv_make_conn_spoor(struct SrvConn *conn);

// devsrv_conn_of — the SrvConn behind a connection Spoor, or NULL if `c`
// is not a devsrv connection Spoor (a NULL / non-devsrv Spoor, or a devsrv
// root / service Spoor — discriminated by dc and the aux's first u64).
// SYS_SRV_PEER (P5-corvus-srv-impl-a3c) resolves corvus's accepted
// endpoint handle to its SrvConn through here.
struct SrvConn *devsrv_conn_of(struct Spoor *c);

// srv_backlog_depth — current accept-backlog depth of `svc`. Tests +
// diagnostics; takes the registry lock.
int srv_backlog_depth(struct SrvService *svc);

// =============================================================================
// poll — readiness probe on a KObj_Srv handle (P5-poll-b).
// =============================================================================
//
// poll.c routes any KObj_Srv handle to this entry point. The KObj_Srv obj
// is either a SrvService (the listener — the handle SYS_POST_SERVICE
// returned) or a SrvConn (the client-side connection handle); the magic
// at offset 0 discriminates. SrvService is the corvus accept-listener
// case at v1.0; SrvConn handling is fail-closed POLLNVAL until the client
// connection poll story lands (no v1.0 caller polls its client handle —
// the client either drives 9P synchronously or, in joey's case, is the
// kernel-9P-client itself, neither of which uses poll on the KObj_Srv).
//
// Returns the masked revents. POLLNVAL is reported for a NULL / unknown-
// magic obj — a memory corruption or an unsupported KObj_Srv flavor.

short srv_handle_poll(void *obj, short events, struct poll_waiter *pw);

#endif  // THYLACINE_DEVSRV_H
