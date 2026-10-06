// p9_attach — kernel-side machinery for the attach_9p syscall
// (P5-attach-create; the substantive piece of what will become the
// user-visible SYS_ATTACH_9P).
//
// Per ARCHITECTURE.md §9.6 (filesystem-as-Spoor) + §11.2's
// `attach_9p(transport_fd, aname, n_uname) → spoor_fd` syscall.
// The syscall's body — taking a byte-pipe transport, wrapping it in
// a p9_client, driving the handshake, returning a dev9p Spoor — lives
// here. The user-visible SVC handler (looking up transport_fd in the
// caller's handle table, allocating a new KOBJ_SPOOR slot for the
// returned Spoor) lands in a follow-up chunk once Thylacine has fd
// syscalls (open/close/dup) to populate the handle table with
// byte-pipe Spoors. At v1.0 the kernel-internal machinery is used by:
//   - Tests (this chunk; loopback-backed transports).
//   - The P5-stratumd boot path (when it lands).
//
// Layering:
//
//   sys_attach_9p_handler  (deferred; future chunk)
//      │
//   p9_attached_create   (this header)
//      │
//   p9_client (heap-allocated)
//      │
//   p9_transport + caller-provided transport_ops
//
// Lifecycle:
//
//   Create:
//     a = p9_attached_create(ops, recv_cap, root_fid, msize,
//                             uname, uname_len, aname, aname_len, n_uname)
//       1. kmalloc the p9_client (~12 KiB).
//       2. kmalloc the recv_buf (recv_cap bytes; sized to msize).
//       3. p9_client_init + p9_client_handshake.
//       4. Return a heap-managed struct p9_attached owning all of the
//          above. NULL on any failure (with cleanup).
//
//   Get a dev9p Spoor for the bound root:
//     root_spoor = p9_attached_root_spoor(a, root_fid)
//       — produces a Spoor whose dev9p_priv references a->client with
//         fid_owned=false (root_fid is owned by the attached struct).
//       — the caller owns the returned Spoor's lifecycle; spoor_clunk
//         when done.
//
//   Destroy:
//     p9_attached_destroy(a)
//       1. Caller must have released all dev9p Spoors that point at
//          a->client BEFORE calling destroy (including any walk-derived
//          Spoors). At v1.0 this is by convention; a future spec
//          extension formalizes.
//       2. Clunks root_fid via the client.
//       3. p9_client_close + p9_client_destroy.
//       4. kfree the client + recv_buf + the attached struct itself.
//
// Lifecycle ownership for byte-pipe-backed transports (future):
//
//   When the transport_ops's ctx is a kernel Spoor (P5-spoor-transport),
//   the attached struct also holds a reference to that Spoor — bumped
//   at create, dropped at destroy. The transport_ops.close hook in the
//   Spoor-backend will spoor_unref the underlying Spoor; create takes
//   the initial ref, destroy releases it via the same path.

#ifndef THYLACINE_9P_ATTACH_H
#define THYLACINE_9P_ATTACH_H

#include <thylacine/9p_transport.h>
#include <thylacine/types.h>

struct p9_client;
struct Spoor;
struct p9_spoor_transport;
struct p9_closer_entry;

#define P9_ATTACHED_MAGIC  0x50394154u  // "P9AT" little-endian

// P5-stratumd-stub-bringup audit close F2 (F236 deferred close): refcounted
// so the underlying p9_client + recv_buf + adapter + transport spoor refs
// stay alive until ALL derived dev9p_priv (root + every walked Spoor) have
// dropped their references. Pre-fix the root Spoor's close ran
// p9_attached_destroy immediately, leaving walked dev9p_priv pointers
// dangling and producing a UAF on subsequent walked Spoor closes (R15 F236).
//
// ref discipline:
//   - p9_attached_create returns ref = 1 (the caller's hold).
//   - p9_attached_ref bumps; every walked dev9p_priv allocated from a Spoor
//     whose priv carries attached_owner takes one such ref.
//   - p9_attached_unref drops; on the LAST ref the destroy logic runs
//     (clunk root_fid + p9_client_close + p9_client_destroy + free buffers
//     + spoor_clunk transport_tx/rx + kfree adapter + kfree(a)).
//
// Transport ownership (the SYS_ATTACH_9P path):
//   - sys_attach_9p_handler calls p9_attached_install_transport(a, adapter,
//     tx, rx) once, AFTER p9_attached_create succeeds, transferring ownership
//     of the kmalloc'd adapter + the syscall's transport-Spoor refs into the
//     attached. The last unref releases them.
//   - For test paths that use a loopback transport (no Spoor-backed adapter),
//     install_transport is never called and adapter/tx/rx stay NULL —
//     unref's destroy path skips them.
struct p9_attached {
    u32                          magic;
    int                          ref;          // F2: refcount; init 1; last-unref destroys
    struct p9_client            *client;       // heap-allocated by create
    u8                          *recv_buf;     // heap-allocated; sized to msize
    size_t                       recv_cap;
    u32                          root_fid;     // the bound fid from Tattach
    u32                          msize;        // negotiated
    bool                         handshake_ok; // true once create succeeds; gates destroy's clunk
    // F2 (SYS_ATTACH_9P-path transport ownership). NULL for test-loopback.
    struct p9_spoor_transport   *adapter;
    struct Spoor                *transport_tx;
    struct Spoor                *transport_rx;
    // #210: /ctl/9p-sessions registry linkage + identity. Linked at create
    // success, unlinked at the top of the last-unref destroy (the walker
    // holds the registry lock across its walk, pinning lifetimes). Label
    // defaults to the attach aname; srvconn_attach_dev9p_root relabels
    // with the conn's peer pid so a /srv session is attributable.
    struct p9_attached          *ctl_next;
    char                         ctl_label[12];
    int                          ctl_id;       // peer pid for /srv conns; -1 else
    // The session's two ends, who may read its counters (IMPERIUM-DESIGN 11.3
    // item 10): the attacher, and the server when the kernel knows it (a /srv
    // conn's poster). PRINCIPAL_INVALID until stamped, and for an unknown end.
    u32                          ctl_owner;
    u32                          ctl_server;
    // The closer (docs/FID-LIFECYCLE-DESIGN.md section 9): this session's
    // deferred Tclunks, oldest first, each entry holding one ref on this
    // struct. closer_queued = waiting on the closers' run-queue; closer_busy =
    // a closer is serving it -- never both, so one closer serves a session at
    // a time. All under the closer lock (9p_attach.c).
    struct p9_closer_entry      *closer_head;
    struct p9_closer_entry      *closer_tail;
    struct p9_attached          *closer_next;
    bool                         closer_queued;
    bool                         closer_busy;
};

// Create + handshake + return ownership. `transport_ops` is the byte-
// pipe backend (loopback for tests; Spoor-backed when P5-spoor-transport
// lands). `recv_cap` is the transport's frame buffer (typically equal
// to `msize`). Returns a heap-allocated struct on success (caller calls
// p9_attached_destroy when done), NULL on failure with all intermediate
// allocations released.
//
// `out_err` (A-3c / M6): when non-NULL and the call returns NULL, receives
// the negative POSIX errno that caused the failure -- crucially the Tattach
// `Rlerror` ecode the server sent (e.g. -T_E_ACCES for a dataset-scope
// refusal), so the SYS_ATTACH_9P* handlers surface it instead of a bare -1.
// On success it is set to 0. Pass NULL when the caller does not distinguish.
struct p9_attached *p9_attached_create(
    struct p9_transport_ops transport_ops,
    size_t                  recv_cap,
    u32                     root_fid,
    u32                     msize,
    const u8               *uname, size_t uname_len,
    const u8               *aname, size_t aname_len,
    u32                     n_uname,
    int                    *out_err);

// Allocate a fresh dev9p Spoor representing the attached's bound root.
// The returned Spoor has fid_owned=false (root_fid stays bound until
// p9_attached_destroy). The caller owns the Spoor's lifecycle; close
// (spoor_clunk) does NOT clunk root_fid or tear down the attached.
//
// At v1.0 each call returns a new Spoor wrapping the same root_fid.
// Callers needing multiple kernel handles to the root should walk
// (clone) and clunk derived fids in the normal way.
struct Spoor *p9_attached_root_spoor(struct p9_attached *a);

// Refcount API (P5-stratumd-stub-bringup audit close F2 / F236).
//
// p9_attached_ref: increment ref. Safe on NULL (no-op). Bumps the ref by 1.
// p9_attached_unref: decrement ref. Safe on NULL (no-op). On the LAST drop:
//   - clunks root_fid via the client (if handshake completed),
//   - p9_client_close + p9_client_destroy,
//   - kfree(recv_buf) + kfree(client),
//   - if a transport was installed: spoor_clunk(transport_tx [+ rx]) and
//     kfree(adapter),
//   - kfree(a) itself.
//
// Walked dev9p_priv carries an attached_owner pointer and holds one ref;
// each walked Spoor's dev9p_close drops it. The root dev9p_priv (created
// by SYS_ATTACH_9P) also holds one ref. The construction ref (the one
// returned by p9_attached_create) is the caller's; for SYS_ATTACH_9P it
// is transferred to the root dev9p_priv's attached_owner stash.
void p9_attached_ref(struct p9_attached *a);
void p9_attached_unref(struct p9_attached *a);

// Install transport ownership (P5-stratumd-stub-bringup audit close F2).
// Transfers ownership of `adapter` (which will be kfree'd on last unref)
// and the syscall's references on `tx` / `rx` (which will be spoor_clunk'd
// on last unref). `tx` and `rx` may alias (same Spoor → only one clunk).
// First-call-wins: returns 0 on the first install, -1 on subsequent calls
// or on bad args (NULL a, NULL adapter).
int p9_attached_install_transport(struct p9_attached *a,
                                   struct p9_spoor_transport *adapter,
                                   struct Spoor *tx,
                                   struct Spoor *rx);

// Legacy alias: equivalent to p9_attached_unref. Retained because callers
// who construct an attached + tear it down with no walks ever existing
// (tests, attach-create failure paths) read more naturally as "destroy."
// Drop in a future refactor once no caller depends on the name.
void p9_attached_destroy(struct p9_attached *a);

// Query: is this attached's session OPEN?
bool p9_attached_is_open(const struct p9_attached *a);

// =============================================================================
// The closer (docs/FID-LIFECYCLE-DESIGN.md section 9; dec-2026-09-28-tclunk-
// closer). A Tclunk its caller could not send on a live session -- the caller
// is dying, or a spill buffer could not be allocated -- is sent by a pool of
// kernel threads that never die: Plan 9's closeproc, one closer per session.
// =============================================================================

// Hand `fid` -- still bound, its Tclunk never sent -- to the closer. Queues it
// with a reference on `a` in a node from kmalloc (which never sleeps) and wakes
// an idle closer, or spawns one when every closer is busy (thread_create never
// sleeps either): it never blocks (I-24). The caller holds a reference on `a`.
// Returns 0 when queued; -1 when the node could not be allocated, and the fid
// then stays bound -- a leak on a live session, which the caller reports
// (p9_clunk_refused).
int p9_attached_defer_clunk(struct p9_attached *a, u32 fid);

// Print `9p: close: clunk of fid N refused rc R` and count it. Only for a fid
// that stays live on a live session: tools/test.sh fails on the line.
void p9_clunk_refused(u32 fid, int rc);

// Boot: start the pool with its first closer. -1 if it could not be created.
int p9_closer_start(void);

// Observability (tests; the counters are cumulative since boot).
struct p9_closer_stats {
    u32 threads;         // closers alive, idle or busy
    u32 idle;            // 0 or 1
    u32 idle_parked;     // 1 while the idle closer sleeps: nothing of the
                         // pool is runnable
    u32 retired;         // retired and not yet reaped
    u32 runq;            // sessions waiting for a closer
    u64 pending;         // entries queued and not yet finished
    u64 sent;            // Tclunks the closers put on the wire
    u64 dropped;         // entries that needed no Tclunk: the session died
                         // (its fids with it) or the fid was not bound
    u64 refused;         // entries left live on a live session (reported)
    u64 spawned;
    u64 spawn_failed;
    u64 reaped;
    u64 live_refusals;   // p9_clunk_refused lines, from every caller
};
void p9_closer_stats(struct p9_closer_stats *out);

// Tests: the next `n` closer spawns, or closer queue nodes, fail as an
// allocation failure would. Each returns how many were still to fail.
u32 p9_closer_fail_spawns_for_test(u32 n);
u32 p9_closer_fail_nodes_for_test(u32 n);

// Tests: hold the next spawn at its end -- before it readies its new closer,
// or before it takes its failure -- until released with false.
void p9_closer_hold_spawn_for_test(bool hold);
bool p9_closer_spawn_held_for_test(void);

// #210: relabel a registered attached for /ctl/9p-sessions (label is
// copied, truncated to the ctl_label field; id is free-form — the /srv
// path stamps the conn's peer pid). Safe any time between create and the
// last unref.
void p9_attached_set_ctl_ident(struct p9_attached *a, const char *label,
                               int id);

// Stamp the session's ends for /ctl/9p-sessions: `attacher` is the attaching
// Proc's principal, `server` the server's when the kernel knows it, else
// PRINCIPAL_INVALID. Until stamped the row's counters read "-" to every reader
// but the system principal and a hostowner.
void p9_attached_set_ctl_owners(struct p9_attached *a, u32 attacher, u32 server);

// #210: walk every live attached session for /ctl/9p-sessions. cb gets
// the label/id/msize plus a consistent client snapshot; the registry lock
// is held across the walk, so cb must not block or attach/destroy.
struct p9_client_ctl;
typedef bool (*p9_attached_ctl_cb)(const char *label, int id, u32 msize,
                                   u32 owner, u32 server,
                                   const struct p9_client_ctl *snap,
                                   void *arg);
void p9_attached_ctl_iterate(p9_attached_ctl_cb cb, void *arg);

// srvconn_attach_dev9p_root -- wrap a byte-transport SrvConn's CLIENT side into
// a mountable dev9p root Spoor (stalk-3b-β). kmalloc a p9_srvconn_transport
// adapter (it takes ONE srvconn_ref on `cn`), set kernel_attached + the handshake
// deadline, drive p9_attached_create (Tversion + Tattach over the byte rings),
// install the transport, build the root Spoor, and stamp its dev9p_priv->
// attached_owner so the root's clunk tears down the whole attach session. The
// shared core of SYS_ATTACH_9P_SRV (stratum-fs byte client) and devsrv_open's
// 9p-mode connect (corvus) -- the 9P-unification.
//
// On success: returns the dev9p root Spoor (one owned ref; *out_err = 0). The
// adapter's srvconn_ref keeps `cn` alive for the session; the construction ref on
// the internal p9_attached is dropped here (the root owns the session via its
// attached_owner). The CALLER's own ref(s) on `cn` are untouched.
//
// On failure: returns NULL, *out_err = a negative errno (a Tattach Rlerror ecode
// like -T_E_ACCES, or -T_E_IO / -T_E_NOMEM). All internal allocations are
// released. For failures AFTER the adapter's transport_init, the adapter's close
// path (srvconn_transport_close) has torn `cn` down (EOF both rings); for failures
// BEFORE it, `cn` is untouched (the caller decides whether to teardown). Either
// way the caller's own ref(s) on `cn` are NOT dropped here.
// `who` is the attaching Proc and `flags` the validated SYS_ATTACH_9P_SRV word,
// whose one bit, SYS_ATTACH_9P_LOOSE, opts the minted client into the B1
// per-attach loose mode (I-38 opt-in, docs/chase/B1-VOTE.md). The identity cape
// (IDENTITY-DESIGN 3.2) is decided by the conn alone, never by `flags`: a byte
// conn from a DMSRVCAPE service capes the session -- the Tattach names no user,
// and the client reports `who`'s principal + primary group as every file's
// owner. Without the cape the Tattach asserts `who`'s principal (A-3 M4). Both
// marks are stamped on the client BEFORE the root Spoor is returned, so the
// caller's handle publication orders them against every use. SYS_ATTACH_9P_SRV
// passes its validated flags; devsrv_open's 9p-mode connect passes 0 (strict,
// and a 9P-mode conn never carries the cape).
struct SrvConn;
struct Proc;
struct Spoor *srvconn_attach_dev9p_root(struct SrvConn *cn,
                                        const u8 *aname, size_t aname_len,
                                        const struct Proc *who, u32 flags,
                                        int *out_err);

#endif  // THYLACINE_9P_ATTACH_H
