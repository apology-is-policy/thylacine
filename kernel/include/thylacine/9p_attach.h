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

#endif  // THYLACINE_9P_ATTACH_H
