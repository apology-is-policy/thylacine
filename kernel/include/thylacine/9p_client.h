// 9P client high-level API (P5-client).
//
// Consolidates the codec (P5-wire/-io/-meta/-mutation), state machine
// (P5-session), and transport (P5-transport) into a single callable
// surface. Each public method composes:
//
//   1. session.send_*  — build the Tmsg into an outbound buffer,
//                        allocate a tag, mark outstanding.
//   2. transport.exchange — write the Tmsg + read the Rmsg + dispatch
//                           through the session.
//   3. result extraction — pull the relevant fields out of the
//                          dispatch_result and surface them to the
//                          caller.
//   4. error mapping — Rlerror's Linux errno → negative return; any
//                      lower-layer failure → -EIO; bad args → -EINVAL.
//
// The caller-visible surface looks like a small filesystem RPC library.
// Callers above this layer (the future mount/Dev/syscall integration)
// no longer need to know about tag allocation, frame framing, or the
// outstanding-request table.
//
// Layering:
//
//   syscall / mount / Dev      ← future caller of this layer
//      │
//   p9_client  (this header)
//      │
//   p9_session  (state machine)   p9_transport  (byte pipe)
//      │                              │
//   p9_wire     (codec)           {loopback, Spoor-over-Unix-socket, ...}
//
// Concurrency: thread-safe at the public p9_client_* API surface. Each
// public op acquires `c->lock` (a spin_lock_t) for the entire duration
// of build + transport-exchange + dispatch + bookkeeping, then releases
// it. Cross-Proc sharing of a single client (typically via inherited
// dev9p Spoors after SYS_SPAWN_WITH_FDS) is correct under SMP. The lock
// also covers `c->next_fid`, the monotonic fid allocator. Direct callers
// of `kernel/9p_session.c` p9_session_* functions are responsible for
// their own serialization — those are below the client's locked surface
// and are typically only invoked by test code. R15-c F230 close.
//
// Buffer ownership:
//
//   - Each `struct p9_client` embeds its session + transport inline.
//     Cumulative size ≈ 4 KiB (dominated by the session's fid +
//     outstanding tables).
//   - The transport's recv buffer is caller-provided at init time.
//   - The outbound buffer for each Tmsg is INLINE in the client
//     (`out_buf[P9_CLIENT_OUT_BUF_MAX]`); sized to comfortably hold
//     any Tmsg shape up to one msize-bounded Twrite. Tests that don't
//     need huge writes can use a smaller cap; the default 8 KiB is
//     enough for handshake + meta + small reads/writes.

#ifndef THYLACINE_9P_CLIENT_H
#define THYLACINE_9P_CLIENT_H

#include <thylacine/9p_session.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

// Inline outbound Tmsg buffer. 8 KiB is plenty for handshake / meta /
// mutation messages; large writes should use a larger client cap.
#define P9_CLIENT_OUT_BUF_MAX  (8u * 1024u)

#define P9_CLIENT_MAGIC        0x50394354u   // "P9CT" little-endian

// One in-flight steady-state op (ARCH §21.3 "Request" / §21.10). The
// submitter allocates a p9_rpc on its OWN stack, registers it in
// c->inflight[tag] under c->lock, and blocks on its own `rendez` until
// the elected reader copies the matching reply frame into `reply_buf`
// and sets `done`. SINGLE-WAITER: exactly one thread (the submitter)
// ever sleeps on `rendez` -- the struct Rendez single-waiter convention
// holds because each rpc has its own. The three flags are mutated only
// under c->lock, and every mutation is followed by wakeup(&rpc->rendez)
// (the I-9 register-then-observe discipline; rpc_wait_cond reads ONLY
// these flags, never cross-lock client state).
struct p9_rpc {
    u16            tag;        // 9P tag (0..P9_SESSION_MAX_OUTSTANDING-1)
    bool           done;       // reply copied into reply_buf (reply_len valid)
    bool           dead;       // session torn down under me -> -P9_E_IO
    bool           be_reader;  // a departing reader handed me the reader role
    int            reply_len;  // bytes in reply_buf (valid iff done)
    u8            *reply_buf;  // kmalloc'd recv_cap bytes; reader copies frame here
    struct Rendez  rendez;     // the submitter sleeps here; reader wakes it
};

struct p9_client {
    u32                  magic;
    // Per-client lock. Protects session.outstanding[], session.bound_fids[],
    // out_buf, next_fid, total_ops/total_errors, the inflight[] table, and
    // reader_active / dead. Held across build + send + dispatch, but DROPPED
    // across the blocking reader recv and the per-rpc sleep (ARCH §21.10 --
    // the #841 elected-reader restoration; never held across a blocking wait).
    spin_lock_t          lock;
    struct p9_session    session;
    struct p9_transport  transport;
    // Shared outbound Tmsg buffer. Used only under c->lock during the
    // build+send of one op (serialized), so the elected-reader pipeline can
    // share it across ops without per-op allocation -- the frame is copied
    // into the c2s ring by the send before c->lock is released.
    u8                   out_buf[P9_CLIENT_OUT_BUF_MAX];
    size_t               recv_cap;     // transport recv-buf cap; per-rpc reply_buf size
    // Pipeline state (ARCH §21.10). inflight[tag] is the submitter's stack
    // p9_rpc for the op holding `tag`, or NULL (free / op died + unwound,
    // leaving outstanding[tag] active for stray-reply reclaim). reader_active
    // is the single-reader election flag; dead latches on transport EOF/error
    // (every op then returns -P9_E_IO). All under c->lock.
    struct p9_rpc       *inflight[P9_SESSION_MAX_OUTSTANDING];
    bool                 reader_active;
    bool                 dead;
    // Most-recently-completed op's reply buffer, kept alive past client_run's
    // return. The read/readdir/readlink dispatch results ZERO-COPY ALIAS into
    // it (out->read_data / readdir_data / readlink_target point inside the
    // dispatched frame), and the caller copies those out AFTER client_run
    // returns -- so the buffer must outlive the call. Freed at the NEXT
    // completion or at destroy, both under c->lock, by which point the prior
    // caller has copied out and dropped c->lock. Holds at most one buffer.
    u8                  *done_reply_buf;
    // Monotonic fid allocator. Starts at root_fid + 1; each
    // p9_client_alloc_fid returns next_fid then increments. Burned
    // fids are not reclaimed at v1.0 (32-bit space; plenty for any
    // realistic session lifetime). Used by dev9p when allocating
    // fresh fids for walk-derived Spoors. Mutated only under
    // c->lock.
    u32                  next_fid;
    // Diagnostics.
    u32                  total_ops;
    u32                  total_errors;
};

// =============================================================================
// Lifecycle.
// =============================================================================

// Initialize a client: configures the embedded session + transport.
// `root_fid` and `msize` configure the session; `transport_ops` +
// `recv_buf` / `recv_cap` configure the transport. Returns 0 on
// success, -EINVAL on arg violation.
int  p9_client_init(struct p9_client *c,
                     u32 root_fid, u32 msize,
                     struct p9_transport_ops transport_ops,
                     u8 *recv_buf, size_t recv_cap);

// Tear down: clobbers magic, destroys session + transport.
void p9_client_destroy(struct p9_client *c);

// Graceful close: closes the transport, then session.
int  p9_client_close(struct p9_client *c);

// =============================================================================
// Handshake.
// =============================================================================

// Drive the Tversion + Tattach handshake. After this returns 0 the
// session is OPEN and the root_fid is bound. Returns 0 on success,
// negative errno on failure. On Rlerror, returns -Rlerror.ecode.
int  p9_client_handshake(struct p9_client *c,
                          const u8 *uname, size_t uname_len,
                          const u8 *aname, size_t aname_len,
                          u32 n_uname);

// =============================================================================
// Path operations.
// =============================================================================

// Walk from `src_fid` to `new_fid` along `nwname` components. Each
// component is a (name_ptr, name_len) pair; `names` is `nwname`
// pointers, `name_lens` is `nwname` lengths. `nwname == 0` clones
// `src_fid` into `new_fid`. The output qids array is filled with up
// to P9_MAX_WALK entries; `*out_nwqid` is set to the actual count.
// Returns 0 on success.
int  p9_client_walk(struct p9_client *c,
                     u32 src_fid, u32 new_fid,
                     u16 nwname,
                     const u8 *const *names, const size_t *name_lens,
                     u16 *out_nwqid, struct p9_qid *out_qids);

// Convenience: walk a single component from `src_fid` to `new_fid`.
int  p9_client_walk_one(struct p9_client *c,
                         u32 src_fid, u32 new_fid,
                         const u8 *name, size_t name_len,
                         struct p9_qid *out_qid);

// Clunk (release) a fid.
int  p9_client_clunk(struct p9_client *c, u32 fid);

// =============================================================================
// IO operations.
// =============================================================================

// Open `fid` with Linux O_* flags. Returns iounit (server's
// recommended max single-IO count; 0 = no recommendation) in
// `*out_iounit`; the entry's qid in `*out_qid`.
int  p9_client_lopen(struct p9_client *c, u32 fid, u32 flags,
                      struct p9_qid *out_qid, u32 *out_iounit);

// Create `name` in directory `fid` with Linux flags/mode/gid. After
// this call, `fid` server-side refers to the new file (not the
// parent dir).
int  p9_client_lcreate(struct p9_client *c, u32 fid,
                        const u8 *name, size_t name_len,
                        u32 flags, u32 mode, u32 gid,
                        struct p9_qid *out_qid, u32 *out_iounit);

// Read up to `count` bytes from `fid` at `offset`. The returned data
// is copied into `out_data` (caller-provided); `*out_count` is the
// actual byte count read (may be < count at EOF).
// NB: this is a copy semantics. The lower-layer Rread is zero-copy
// (aliases the transport's recv buffer); the client copies it out so
// the caller doesn't need to know about the recv-buffer lifetime.
int  p9_client_read(struct p9_client *c, u32 fid, u64 offset,
                     u32 count, u8 *out_data, u32 *out_count);

// Write `count` bytes from `data` to `fid` at `offset`. `*out_accepted`
// is set to the server-reported accepted count (may be < count under
// back-pressure).
int  p9_client_write(struct p9_client *c, u32 fid, u64 offset,
                      u32 count, const u8 *data, u32 *out_accepted);

// =============================================================================
// Metadata operations.
// =============================================================================

int  p9_client_getattr(struct p9_client *c, u32 fid,
                        u64 request_mask, struct p9_attr *out_attr);
int  p9_client_setattr(struct p9_client *c, u32 fid,
                        const struct p9_setattr *attr);
int  p9_client_readdir(struct p9_client *c, u32 fid, u64 offset,
                        u32 count, u8 *out_data, u32 *out_count);
int  p9_client_statfs(struct p9_client *c, u32 fid,
                       struct p9_statfs *out_statfs);
int  p9_client_fsync(struct p9_client *c, u32 fid, u32 datasync);

// =============================================================================
// Mutation operations.
// =============================================================================

int  p9_client_symlink(struct p9_client *c, u32 fid,
                        const u8 *name, size_t name_len,
                        const u8 *symtgt, size_t symtgt_len,
                        u32 gid, struct p9_qid *out_qid);

int  p9_client_mknod(struct p9_client *c, u32 dfid,
                      const u8 *name, size_t name_len,
                      u32 mode, u32 major, u32 minor, u32 gid,
                      struct p9_qid *out_qid);

int  p9_client_rename(struct p9_client *c, u32 fid, u32 dfid,
                       const u8 *name, size_t name_len);

int  p9_client_readlink(struct p9_client *c, u32 fid,
                         u8 *out_target, u16 *out_target_len);

int  p9_client_link(struct p9_client *c, u32 dfid, u32 fid,
                     const u8 *name, size_t name_len);

int  p9_client_mkdir(struct p9_client *c, u32 dfid,
                      const u8 *name, size_t name_len,
                      u32 mode, u32 gid, struct p9_qid *out_qid);

int  p9_client_renameat(struct p9_client *c, u32 olddirfid,
                         const u8 *oldname, size_t oldname_len,
                         u32 newdirfid,
                         const u8 *newname, size_t newname_len);

int  p9_client_unlinkat(struct p9_client *c, u32 dfid,
                         const u8 *name, size_t name_len, u32 flags);

// =============================================================================
// Fid allocator (P5-attach-dev consumer).
// =============================================================================

// Allocate a fresh fid for a walk-derived Spoor. Returns a u32 fid
// value that has not been previously returned by this allocator on
// this client. Caller uses it as the `new_fid` arg to walk. v1.0
// monotonic; no reclaim. Returns P9_NOFID if the 32-bit space is
// exhausted (in practice, never).
u32 p9_client_alloc_fid(struct p9_client *c);

// =============================================================================
// Query helpers.
// =============================================================================

bool   p9_client_is_open(const struct p9_client *c);
size_t p9_client_inflight(const struct p9_client *c);

// =============================================================================
// Errno convention.
//
// All ops return:
//   0       — success
//   -errno  — failure
//
// Mapping:
//   -EINVAL — bad arguments at the client layer (NULL, magic mismatch,
//             arg too long).
//   -EBUSY  — session not OPEN (handshake hasn't run).
//   -EIO    — lower-layer failure: send/recv error, frame malformed,
//             tag pool full, fid bookkeeping conflict, etc.
//   -<n>    — Rlerror surface: n = the Linux errno the server returned.
// =============================================================================

#define P9_E_OK       0
#define P9_E_INVAL   22       // EINVAL
#define P9_E_BUSY    16       // EBUSY
#define P9_E_IO       5       // EIO

#endif  // THYLACINE_9P_CLIENT_H
