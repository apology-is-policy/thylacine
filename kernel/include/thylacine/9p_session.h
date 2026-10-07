// 9P2000.L session state machine (P5-session).
//
// Composes `kernel/9p_wire.{c,h}` (P5-wire codec) into the per-session
// tag pool + fid table + outstanding-request bookkeeping that
// `specs/9p_client.tla` describes. The spec's actions map to this
// module's send/recv functions:
//
//   Spec action                    | Symbol
//   ───────────────────────────────┼──────────────────────────────────────
//   OpenSession                    | p9_session_send_version +
//                                  | p9_session_send_attach
//                                  | (post-Rversion + Rattach dispatch)
//   CloseSession                   | p9_session_close
//   SendIO   (open / create)       | p9_session_send_lopen
//                                  | p9_session_send_lcreate
//   SendIO   (read / write)        | p9_session_send_read
//                                  | p9_session_send_write
//   SendIO   (metadata)            | p9_session_send_getattr
//                                  | p9_session_send_setattr
//                                  | p9_session_send_readdir
//                                  | p9_session_send_statfs
//                                  | p9_session_send_fsync
//   SendIO   (mutation)            | p9_session_send_symlink
//                                  | p9_session_send_mknod
//                                  | p9_session_send_rename
//                                  | p9_session_send_readlink
//                                  | p9_session_send_link
//                                  | p9_session_send_mkdir
//                                  | p9_session_send_renameat
//                                  | p9_session_send_unlinkat
//   SendWalk                       | p9_session_send_walk
//   SendClunk                      | p9_session_send_clunk
//   ReceiveOp                      | p9_session_dispatch_rmsg
//
// Invariants this module upholds (TLC-pinned per `specs/9p_client.tla`):
//
//   I-10  Per-session tag uniqueness — the allocator over the tag table
//         (ARCH 21.11) refuses to return a tag whose entry is currently
//         active.
//
//   I-11  Per-session fid identity stable across open lifetime —
//         fid_bind/fid_unbind are explicit; SendClunk Send-time-unbinds
//         per spec's client discipline.
//
//   OutOfOrderCorrectness — dispatch_rmsg uses tag-indexed lookup
//         (p9_session_entry), not arrival-order, to apply the state
//         mutation. The bookkeeping pairs Send with the correct
//         Receive regardless of Rmsg arrival order.
//
//   FlowControl — alloc_tag returns -1 when the op share is full
//         (P9_OPS_MAX ops in flight) or the table cannot grow. Back-
//         pressure surfaces as send-side refusal, never as silent
//         overflow; the client waits for a tag (ARCH 21.11 part 3).
//
// State machine:
//
//   INIT       — fresh session; only p9_session_send_version accepted.
//                Transitions to VERSIONED on Rversion.
//   VERSIONED  — msize negotiated; only p9_session_send_attach accepted.
//                Transitions to OPEN on Rattach (root_fid bound).
//   OPEN       — full surface available; send_walk/send_clunk OK.
//                Closing requires draining outstanding first; tries to
//                close while in-flight ops exist return -1.
//   CLOSED     — terminal; no further sends. Set by p9_session_close.
//
// Audit posture: spec-bearing. The kernel/9p_*.c surface joins the
// CLAUDE.md trigger list when this chunk lands and composes the wire
// codec. Per `specs/9p_client.tla`, bugs here are exactly the bug
// classes the spec's buggy cfgs surface (tag collision / fid after
// clunk / out-of-order match / unbounded outstanding).
//
// Concurrency: this module is NOT thread-safe internally. Callers
// must serialize externally. The wrapping `struct p9_client`
// (kernel/9p_client.c) provides the v1.0 discipline — a per-client
// spin_lock_t held across every send/dispatch sequence (R15-c F230
// close). Direct callers of p9_session_* below the client layer
// (typically test code) are responsible for their own serialization.
// This matches `stratum/v2/docs/reference/23-9p_client.md`
// §"Concurrency model" — "One client = one connection = one fid namespace."

#ifndef THYLACINE_9P_SESSION_H
#define THYLACINE_9P_SESSION_H

#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

// =============================================================================
// Sizing constants.
// =============================================================================

// The tag table (ARCH 21.11). Tags run 0..P9_TAG_LIMIT-1; NOTAG (0xFFFF)
// is Tversion's. The first P9_TAG_CHUNK entries live in the session; more
// are allocated a chunk at a time when every entry is held, and kept until
// p9_session_destroy. A chunk that cannot be allocated means no free tag.
#define P9_TAG_CHUNK                64u
#define P9_TAG_LIMIT                0xFFFFu
#define P9_TAG_CHUNKS               ((P9_TAG_LIMIT + P9_TAG_CHUNK - 1u) / P9_TAG_CHUNK)

// The shares. An op (any T-message but Tflush) takes a tag only while fewer
// than P9_OPS_MAX ops hold one; a Tflush takes any free tag. Flushes never
// outnumber ops, so with 2 * P9_OPS_MAX <= P9_TAG_LIMIT a Tflush always finds
// a tag. Async ops, which no thread waits on, hold at most P9_ASYNC_MAX of
// the op share, so the rest stays open to the ops a thread waits on.
#define P9_OPS_MAX                  32767u
#define P9_ASYNC_MAX                16384u

// Max bound fids per session. 256 was comfortable for FS workloads but
// bound the #198 GL client: a warp program holds one fid per live BO
// mapping on its /srv/warp session, so Quake-class texture sets blew
// past 256 (silently -- the refusal at 9p_session.c is kernel-side,
// invisible to both the client's and the server's diagnostics; that
// silence cost the hunt a full instrumented round). 1024 matches
// PROC_HANDLE_MAX, since each bound fid wires a kernel-side handle
// anyway; Stratum's server caps at 4096 (STM_9P_MAX_FIDS). Cost: the
// bound_fids array grows 1 KiB -> 4 KiB per session.
#define P9_SESSION_MAX_FIDS         1024u

// Magic for struct lifetime discipline (R9 F148 mirror — see
// `docs/reference/39-hw-handles.md` caveat #2). Clobbered on destroy.
#define P9_SESSION_MAGIC            0x50395345u   // "P9SE" little-endian

// =============================================================================
// State machine.
// =============================================================================

enum p9_session_state {
    P9_SESS_INIT      = 0,   // before Tversion
    P9_SESS_VERSIONED = 1,   // post-Rversion, pre-Rattach
    P9_SESS_OPEN      = 2,   // post-Rattach, full surface available
    P9_SESS_CLOSED    = 3,   // terminal
};

// =============================================================================
// Per-tag outstanding entry. Active iff `kind` is set; cleared at
// dispatch_rmsg time.
// =============================================================================

struct p9_outstanding {
    bool active;
    u8   kind;       // P9_TVERSION / P9_TATTACH / P9_TWALK / P9_TCLUNK /
                     // P9_TLOPEN / P9_TLCREATE / P9_TREAD / P9_TWRITE /
                     // P9_TGETATTR / P9_TSETATTR / P9_TREADDIR /
                     // P9_TSTATFS / P9_TFSYNC /
                     // P9_TSYMLINK / P9_TMKNOD / P9_TRENAME /
                     // P9_TREADLINK / P9_TLINK / P9_TMKDIR /
                     // P9_TRENAMEAT / P9_TUNLINKAT
    u32  fid;        // primary target fid; equals root_fid for version/attach
    u32  new_fid;    // walk's destination; equals fid otherwise
    u32  op_id;      // monotonic spec-side identifier (for diagnostics)
    // Tflush bookkeeping (#845). `awaiting_flush` marks an op for which a
    // Tflush is in flight -- its owner gone (#845) or, with owner_waits, still
    // waiting for the first answer (flush(5)): the tag stays active (reserved)
    // but is freed ONLY by the flush's Rflush, never by a late original reply
    // -- 9P forbids reusing oldtag until Rflush, so this is the I-10
    // reuse-race guard. `flush_oldtag` is meaningful only on a TFLUSH entry:
    // the original tag this flush cancels.
    bool awaiting_flush;
    // flush(5): the flushed op's owner still waits, so a reply that beats the
    // Rflush is honoured in full and the op may yet act on its fid: it counts
    // LIVE in any_outstanding_on_fid until that reply is applied, the Rflush
    // lands, or the owner dies. Meaningful only with awaiting_flush.
    bool owner_waits;
    // #53-audit F1: a rolled-back or flush-less abandon (the flush-EAGAIN
    // path via flush_rollback; the flush-BUILD-failure path via
    // mark_abandoned). The owner is gone but NO Tflush is in flight, so --
    // unlike awaiting_flush -- the tag is freed by its late original reply
    // (the pre-#845 ownerless reclaim) OR by session teardown (a
    // deferred-reply server that cancels a parked op without replying, the
    // task-#56 netd tail). Like awaiting_flush it is EXCLUDED from
    // any_outstanding_on_fid: the op is cancelled and will not act on its
    // fid, so it must not refuse the #294 cancel-then-close Tclunk. Its late
    // reply's only reachable fid mutation is a walk-family fid_bind on a
    // FRESH monotonic new_fid -- conflict-free with any post-exclusion fid
    // op (the Rattach arm also binds, but an abandoned attach exists only on
    // a private pre-publish client with no survivor to dispatch it).
    bool abandoned;
    // FID-LIFECYCLE section 9: this op holds a fid-table slot -- a Tclunk
    // the slot of the fid its build unbound (until its reply or a take-back),
    // a walk naming a new fid the slot that fid will bind. Counted in
    // p9_session.n_reserved_slots; released by clear_outstanding, or turned
    // into a binding by slot_bind.
    bool holds_slot;
    u16  flush_oldtag;
    // Twalkgetattr bookkeeping (POUNCE): the REQUESTED nwname, so the
    // dispatch can bind new_fid ONLY on a full walk (nwqid == wga_nwname).
    // Meaningful only on a TWALKGETATTR entry. (The TWALK arm predates
    // this and still binds unconditionally -- safe because every TWALK
    // caller sends 0/1 names, where a partial walk cannot exist; the
    // pounce sends multi-name walks, where it can.)
    u16  wga_nwname;
    // The tag of the Tflush in flight for this op; meaningful only with
    // awaiting_flush, so a flush is unstaged without a search.
    u16  flush_tag;
    // An async op (p9_session_mark_async): counted against the async share.
    bool async;
    // The client's registration for the op's reply (its struct p9_rpc), or
    // NULL. Set only on an active entry; cleared with the entry.
    void *owner;
};

// A chunk of the tag table: P9_TAG_CHUNK consecutive tags.
struct p9_tag_chunk {
    struct p9_outstanding e[P9_TAG_CHUNK];
    u32                   n_active;        // active entries in this chunk
};

// =============================================================================
// Session struct. Caller-allocated; lifetime managed by the caller.
// =============================================================================

struct p9_session {
    u32                   magic;
    enum p9_session_state state;

    // Connection parameters.
    u32                   root_fid;         // caller-supplied at init
    u32                   msize;            // proposed at init, negotiated at Rversion
    u32                   negotiated_msize; // 0 until Rversion arrives

    // Fid table — linear array of bound fid values; new entries
    // appended at n_bound_fids; unbind compacts by swap-with-last.
    // Per `specs/9p_client.tla`, bound_fids is a SUBSET; the order
    // doesn't matter.
    u32                   bound_fids[P9_SESSION_MAX_FIDS];
    size_t                n_bound_fids;
    // Slots held by outstanding ops (p9_outstanding.holds_slot). A new
    // reservation needs n_bound_fids + n_reserved_slots below
    // P9_SESSION_MAX_FIDS, so a take-back or a walk's bind always finds room.
    size_t                n_reserved_slots;

    // The tag table, indexed by tag. An entry is active iff a Tmsg was
    // built under that tag and its Rmsg hasn't been dispatched yet.
    // tags0 is chunk 0; tag_dir[k] is chunk k for 1 <= k < n_chunks, and
    // tag_dir stays NULL until the first growth.
    struct p9_tag_chunk   tags0;
    struct p9_tag_chunk **tag_dir;
    u32                   n_chunks;
    u32                   n_active;         // active entries: ops + flushes
    u32                   n_flush;          // active Tflush entries
    u32                   n_async;          // active async ops
    // The op share, the async share, and how many tags the table may grow
    // to: P9_OPS_MAX, P9_ASYNC_MAX and P9_TAG_LIMIT from init. Tests lower
    // them; a tag_limit below 2 * ops_max stands in for a chunk allocation
    // that fails.
    u32                   ops_max;
    u32                   async_max;
    u32                   tag_limit;

    // Counters (spec's op_seq + sent_ops + completed_ops; the impl
    // keeps cardinalities + a monotonic id).
    u32                   next_op_id;
    u32                   total_sent;
    u32                   total_completed;
};

// =============================================================================
// Lifecycle.
// =============================================================================

// Initialize a session. `root_fid` is the caller-managed fid value that
// Tattach will bind. `msize` is the client's proposal — Rversion will
// negotiate down if the server's cap is smaller. Returns 0 on success,
// -1 on arg violation.
int  p9_session_init(struct p9_session *s, u32 root_fid, u32 msize);

// Tear down: clears all state, frees the grown chunks, clobbers magic.
// NULL-safe.
void p9_session_destroy(struct p9_session *s);

// Close the session — requires no outstanding ops. Returns 0 on
// success, -1 if drain not complete. Transitions to CLOSED.
int  p9_session_close(struct p9_session *s);

// =============================================================================
// Send-side API. Each function:
//   - validates state machine + args,
//   - allocates a tag,
//   - calls the corresponding p9_build_T* into the caller's buffer,
//   - inserts the operation into outstanding,
//   - on SendClunk additionally Send-time-unbinds the target fid,
//   - returns total Tmsg byte length on success, -1 on any failure.
// =============================================================================

// Build a Tversion using NOTAG. `version` defaults to "9P2000.L" if
// version == NULL; otherwise caller supplies. Only valid in state INIT.
int p9_session_send_version(struct p9_session *s,
                            u8 *out, size_t cap,
                            const u8 *version, size_t version_len);

// Build a Tattach binding the session's root_fid. Only valid in state
// VERSIONED. afid is hard-coded to P9_NOFID (auth deferred).
int p9_session_send_attach(struct p9_session *s,
                           u8 *out, size_t cap,
                           const u8 *uname, size_t uname_len,
                           const u8 *aname, size_t aname_len,
                           u32 n_uname);

// Build a Twalk that clones / walks `src_fid` into `new_fid`. Only
// valid in state OPEN. Preconditions:
//   - src_fid is bound.
//   - new_fid is NOT bound (about to be bound on Rwalk).
//   - new_fid is NOT the root fid.
//   - No other in-flight op targets new_fid.
//   - nwname <= P9_MAX_WALK.
//   - bound + reserved fid slots are below P9_SESSION_MAX_FIDS; the walk
//     reserves the slot new_fid will bind, so its Rwalk always finds room.
// `names` is an array of pointers (nwname elements); `name_lens` is
// the matching length array. nwname == 0 is a fid clone.
int p9_session_send_walk(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 src_fid, u32 new_fid,
                         u16 nwname,
                         const u8 *const *names,
                         const size_t *name_lens);

// Build a Twalkgetattr (POUNCE, 140): send_walk's preconditions when
// new_fid is real; new_fid == P9_NOFID is the walk-QUERY form (no fid
// gates on the destination, nothing binds at dispatch). Dispatch binds
// a real new_fid ONLY on a full walk (nwqid == nwname) -- the correct
// partial-walk semantics the multi-name pounce requires.
int p9_session_send_walkgetattr(struct p9_session *s,
                                u8 *out, size_t cap,
                                u32 src_fid, u32 new_fid,
                                u64 request_mask,
                                u16 nwname,
                                const u8 *const *names,
                                const size_t *name_lens);

// Build a Tclunk on `fid`. Only valid in state OPEN. Preconditions:
//   - fid is bound.
//   - fid is NOT the root fid (root released only at session close).
//   - No other in-flight op targets fid.
// Send-time unbinds fid (the spec's canonical client-discipline shape),
// and keeps its slot reserved until the Rclunk, so a take-back of a
// never-sent Tclunk can always re-bind it.
int p9_session_send_clunk(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid);

// Build a Tflush abandoning the in-flight request bearing `oldtag` (#845).
// Valid in state VERSIONED or OPEN. Preconditions:
//   - oldtag's entry is active and is NOT itself a Tflush.
//   - oldtag is not already awaiting a flush.
// Allocates a fresh tag for the flush -- any free tag, outside the op share
// -- marks it outstanding (kind TFLUSH, remembering oldtag), and RESERVES oldtag (`awaiting_flush`): from here
// oldtag is freed only by this flush's Rflush, never by a late original
// reply -- the 9P "oldtag not reusable until Rflush" rule, which is the
// I-10 reuse-race guard. Returns the Tflush byte length, or -1.
int p9_session_send_flush(struct p9_session *s,
                          u8 *out, size_t cap,
                          u16 oldtag);

// #52: reclaim the tag of an op whose frame never reached the wire (all-or-
// nothing send contract -> the server never saw it -> I-10-safe immediate
// reuse). No-op on an inactive or awaiting_flush tag (fail-soft).
void p9_session_abort_unsent(struct p9_session *s, u16 tag);

// Take back an op whose frame never reached the wire, so that it can be
// resubmitted or handed to the closer: free the tag as abort_unsent does,
// AND re-bind the fid a Tclunk unbound at build, since the server still
// holds it. The session is then exactly as it was before the build. The
// Tclunk's reserved slot makes the re-bind total, even after the caller
// dropped the lock to park (FID-LIFECYCLE section 9). Returns 0 when the
// op was taken back, -1 on a guard (inactive, flushed or abandoned tag) or
// a failed re-bind; the tag is freed on a failed re-bind, never left active.
int p9_session_retract_unsent(struct p9_session *s, u16 tag);

// #53: undo send_flush after its frame hit c2s back-pressure (EAGAIN): free
// the never-sent flush tag + clear the victim's awaiting_flush, restoring
// the pre-#845 ownerless reclaim. No-op unless the (victim, flush-slot)
// pair matches what send_flush staged (fail-soft).
void p9_session_flush_rollback(struct p9_session *s, u16 oldtag);

// flush(5), for an owner that still waits: undo send_flush when its frame
// never reached the wire. Frees the flush tag and clears the victim's
// awaiting_flush, leaving the victim an ordinary in-flight op whose reply
// completes it. Unlike flush_rollback it does not mark the victim abandoned,
// because its owner is still there. Fail-soft like flush_rollback.
void p9_session_flush_retract(struct p9_session *s, u16 oldtag);

// flush(5): record whether the owner of the flushed op under `oldtag` still
// waits for its answer (see owner_waits). The client sets it when it stages a
// living owner's Tflush and clears it when that owner dies in the flush wait,
// handing the op's fid to the closer. Fail-soft: only an active op with a
// flush in flight is marked.
void p9_session_flush_owner_waits(struct p9_session *s, u16 oldtag, bool waits);

// #52/#53 R2-F1: mark an owner-gone op abandoned when NO flush could even be
// staged (pool-full / build failure) -- the flush-less sibling of
// flush_rollback. Fail-soft on inactive / awaiting_flush tags.
void p9_session_mark_abandoned(struct p9_session *s, u16 tag);

// =============================================================================
// IO send-side API (P5-wire-io extension; spec's SendIO action).
//
// Preconditions shared by all four:
//   - state == OPEN
//   - fid is bound
//
// Fid-exclusivity rules:
//   - send_lopen + send_lcreate REQUIRE no other in-flight op on fid
//     (Tlopen mutates server-side fid state from "walked" to "opened";
//     Tlcreate rebinds fid to the new file; concurrent ops are undefined).
//   - send_read + send_write PERMIT concurrent in-flight ops on fid (the
//     wire passes offset explicitly, so the server is stateless wrt
//     position; client-app callers serialize logically).
//
// None of these mutate the client-side fid table — fid_bound stays true
// across all four operations + their R-message dispatch.
// =============================================================================

// Tlopen: open the file currently bound to `fid` with Linux O_* flags.
int p9_session_send_lopen(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid, u32 flags);

// Tlcreate: create `name` in directory `fid` and rebind fid to the new
// file. flags / mode / gid carry the standard Linux semantics.
int p9_session_send_lcreate(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid,
                            const u8 *name, size_t name_len,
                            u32 flags, u32 mode, u32 gid);

// Tread: read `count` bytes at `offset` from `fid`. Concurrent reads on
// the same fid with different offsets are permitted.
int p9_session_send_read(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 fid, u64 offset, u32 count);

// Twrite: write `count` bytes from `data` at `offset` to `fid`. Concurrent
// writes on the same fid are permitted.
int p9_session_send_write(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid, u64 offset,
                          u32 count, const u8 *data);

// =============================================================================
// Metadata-family send APIs (P5-wire-meta).
//
// Fid-exclusivity rules:
//   - send_getattr / send_statfs / send_readdir / send_fsync: read-shaped
//     (no server-side fid state mutation on the canonical fid identity).
//     Concurrent ops on same fid are permitted at the wire layer; client-
//     app callers serialize logically when needed.
//   - send_setattr: mutates server-side metadata (mode / uid / size /
//     atime / mtime). Requires no other in-flight op on fid (mutation-
//     shaped; concurrent ops are undefined behavior).
//
// None mutate the client-side fid table.
// =============================================================================

// Tgetattr: query attributes for `fid`. `request_mask` is a hint; the
// server's response carries the authoritative valid mask.
int p9_session_send_getattr(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid, u64 request_mask);

// Tsetattr: set attributes for `fid`. `attr->valid` says which fields are
// being set; only those are honored by the server. Fid-exclusive.
int p9_session_send_setattr(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid, const struct p9_setattr *attr);

// Treaddir: read `count` bytes of dirent data from `fid` at `offset`. Same
// concurrency profile as Tread (concurrent permitted; offset explicit).
int p9_session_send_readdir(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid, u64 offset, u32 count);

// Tstatfs: filesystem statistics for `fid`.
int p9_session_send_statfs(struct p9_session *s,
                           u8 *out, size_t cap,
                           u32 fid);

// Tfsync: barrier — block until prior writes on `fid` are durable.
// `datasync` per Linux fdatasync(2): 0 = sync data + metadata, 1 = data only.
int p9_session_send_fsync(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid, u32 datasync);

// =============================================================================
// Mutation-family send APIs (P5-wire-mutation).
//
// Fid-exclusivity rules:
//   - send_rename mutates the server-side identity of `fid` (the named
//     binding moves). Requires no other in-flight op on fid (mutation-
//     exclusive, mirroring setattr from P5-wire-meta).
//   - send_symlink / send_mknod / send_link / send_mkdir / send_unlinkat /
//     send_renameat operate on dfid (parent dir) as a target slot; the
//     server serializes concurrent ops on the same dfid internally.
//     The client permits concurrent ops at the wire layer.
//   - send_readlink reads the target of a symlink fid; concurrent
//     readlinks permitted (read-shaped).
//
// None mutate the client-side fid table. (Trename mutates server-side
// path binding; the fid stays bound to the same inode at the client
// level. Trenameat doesn't touch any fid.)
// =============================================================================

// Tsymlink: create symlink `name` in directory `fid`, target `symtgt`.
int p9_session_send_symlink(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid,
                            const u8 *name, size_t name_len,
                            const u8 *symtgt, size_t symtgt_len,
                            u32 gid);

// Tmknod: create device-node / fifo / socket `name` in directory `dfid`.
int p9_session_send_mknod(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 dfid,
                          const u8 *name, size_t name_len,
                          u32 mode, u32 major, u32 minor, u32 gid);

// Trename: rename the file at `fid` to `name` in directory `dfid`.
// Fid-exclusive on `fid` (server-side identity mutation).
int p9_session_send_rename(struct p9_session *s,
                           u8 *out, size_t cap,
                           u32 fid, u32 dfid,
                           const u8 *name, size_t name_len);

// Treadlink: read the symlink target of `fid`.
int p9_session_send_readlink(struct p9_session *s,
                             u8 *out, size_t cap,
                             u32 fid);

// Tlink: hard-link `fid` as `name` in directory `dfid`.
int p9_session_send_link(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 dfid, u32 fid,
                         const u8 *name, size_t name_len);

// Tmkdir: create directory `name` in directory `dfid`.
int p9_session_send_mkdir(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 dfid,
                          const u8 *name, size_t name_len,
                          u32 mode, u32 gid);

// Trenameat: pure path-based rename across directories.
int p9_session_send_renameat(struct p9_session *s,
                             u8 *out, size_t cap,
                             u32 olddirfid,
                             const u8 *oldname, size_t oldname_len,
                             u32 newdirfid,
                             const u8 *newname, size_t newname_len);

// Tunlinkat: unlink `name` from directory `dfid`. `flags` may include
// P9_UNLINK_AT_REMOVEDIR for rmdir semantics.
int p9_session_send_unlinkat(struct p9_session *s,
                             u8 *out, size_t cap,
                             u32 dfid,
                             const u8 *name, size_t name_len,
                             u32 flags);

// =============================================================================
// Weft send-side API (Weft-6; NET-THROUGHPUT.md section 6).
// =============================================================================

// Tweft: request the per-flow zero-copy ring for the flow bound to `fid`.
// Read-shaped -- returns the flow's stable share_id + ring geometry; idempotent
// on the netd side, no client-side fid-table mutation -- so concurrent ops on
// the same fid are permitted at the wire layer. Only valid in state OPEN; fid
// must be bound.
int p9_session_send_weft(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 fid);

// Tweftio (Weft-6b-2): drive `len` payload bytes at ring offset `off` for the
// flow bound to `fid`, in direction `dir` (WEFT_DIR_WRITE / WEFT_DIR_READ).
// The descriptor is already kernel-validated; netd acts on the ring in place +
// replies the moved-byte count. Only valid in state OPEN; fid must be bound.
int p9_session_send_weftio(struct p9_session *s,
                           u8 *out, size_t cap,
                           u32 fid, u32 off, u32 len, u32 dir);

// =============================================================================
// Receive-side API.
// =============================================================================

// Outcome of dispatch_rmsg. The Rmsg may be the normal R-pair (which
// applies a state mutation) OR an Rlerror (which surfaces the server-
// supplied Linux ecode without mutating fid state, except for clunk
// where the Send-time unbind already happened).
struct p9_dispatch_result {
    u8   kind;       // op kind that was completed (echoes the original Tmsg's type)
    u32  fid;        // primary fid of the completed op
    u32  new_fid;    // walk's destination; equals fid for non-walk
    u32  op_id;      // monotonic op-id (for diagnostics)
    bool is_error;   // TRUE iff Rmsg was Rlerror
    u32  ecode;      // valid iff is_error; Linux errno
    // The new fid this dispatch bound (a walk's), or P9_NOFID. An ownerless
    // dispatch -- a flushed or abandoned walk's late reply -- hands it to the
    // closer: the server holds it and nobody else will clunk it.
    u32            bound_new_fid;
    // For walk, the parsed qids (capacity P9_MAX_WALK).
    u16            nwqid;
    struct p9_qid  qids[P9_MAX_WALK];
    // For attach + version, the parsed result.
    struct p9_qid  attach_qid;
    u32            version_msize;  // valid iff kind == P9_TVERSION
    u16            version_len;
    const u8      *version_ptr;    // aliases the input buffer
    // For lopen + lcreate, the parsed qid + iounit.
    struct p9_qid  open_qid;
    u32            open_iounit;
    // For read, the parsed count + zero-copy data pointer aliasing rmsg.
    u32            read_count;
    const u8      *read_data;      // aliases the input buffer; caller must
                                   // not free rmsg while consuming
    // For write, the parsed accepted count.
    u32            write_count;
    // For getattr, the parsed Linux statx-shaped record.
    struct p9_attr attr;
    // For statfs, the parsed filesystem statistics.
    struct p9_statfs statfs;
    // For readdir, the parsed count + zero-copy data pointer (dirent stream
    // — consumer parses entries via p9_unpack_dirent).
    u32            readdir_count;
    const u8      *readdir_data;
    // For mutation-create ops (Tsymlink / Tmknod / Tmkdir), the qid of
    // the newly-created entry. Kept distinct from `open_qid` (which
    // surfaces Tlopen / Tlcreate's open-side qid) to avoid semantic
    // confusion at the consumer.
    struct p9_qid  created_qid;
    // For Treadlink, the parsed target string (zero-copy pointer into
    // the input rmsg buffer; caller must not free rmsg while consuming).
    const u8      *readlink_target;
    u16            readlink_target_len;
    // For Tweft, the parsed per-flow ring registration token + geometry
    // (Weft-6). Plain scalars -- no alias into rmsg, safe past the call.
    struct p9_weft_geom weft_geom;
    // For Tweftio, the count of payload bytes the consumer moved (Weft-6b-2).
    u32 weftio_count;
    // For Twalkgetattr (POUNCE), the per-component attr elements: nwqid
    // fixed-stride (P9_WGA_BODY_LEN) Rgetattr bodies aliasing the input
    // rmsg (frame validated by p9_parse_rwalkgetattr; the qids land in
    // `qids` above). The caller extracts each element via
    // p9_parse_getattr_body while rmsg stays alive (done_reply_buf).
    const u8      *wga_data;
};

// Dispatch one received Rmsg. The Rmsg's tag is looked up in the tag
// table; if the tag is in use and the type matches (or is
// Rlerror), the state mutation is applied and the op is marked
// complete. Returns 0 on success, -1 on malformed / unmatched / wrong
// type.
//
// The caller must keep `rmsg` alive until after this call returns
// (parse_str returns pointers INTO `rmsg`; *out captures them).
int p9_session_dispatch_rmsg(struct p9_session *s,
                             const u8 *rmsg, size_t len,
                             struct p9_dispatch_result *out);

// flush(5): "If a response to the flushed request is received before the
// Rflush, the client must honor the response as if it had not been flushed."
// Dispatch a reply on an awaiting_flush tag whose owner still waits: the
// state mutation is applied and *out filled exactly as dispatch_rmsg does,
// but the tag stays reserved until the flush's Rflush frees it (the I-10
// guard). A duplicate reply after this is absorbed as an ownerless late
// reply and binds nothing. Returns 0, or -1 on malformed / unmatched /
// wrong-type replies and when the tag is not awaiting a flush.
int p9_session_dispatch_flushed_rmsg(struct p9_session *s,
                                     const u8 *rmsg, size_t len,
                                     struct p9_dispatch_result *out);

// =============================================================================
// Query helpers (read-only; used by tests + audit + caller bookkeeping).
// =============================================================================

bool   p9_session_is_open(const struct p9_session *s);   // state == OPEN
bool   p9_session_fid_bound(const struct p9_session *s, u32 fid);
size_t p9_session_inflight(const struct p9_session *s);  // outstanding count
// An op would get a tag now: the op share has room and an entry is free,
// growing the table if every entry is held. A grown chunk stays, so this
// may allocate; a failed allocation reads as no free tag.
bool   p9_session_has_free_tag(struct p9_session *s);
// A Tflush would get a tag now (any free entry, growing as above).
bool   p9_session_has_flush_tag(struct p9_session *s);
// The async share has room (P9_ASYNC_MAX less the async ops in flight).
bool   p9_session_async_room(const struct p9_session *s);

// =============================================================================
// The tag table, for the client (ARCH 21.11).
// =============================================================================

// The entry for `tag`, or NULL past the table's current size.
struct p9_outstanding *p9_session_entry(struct p9_session *s, u32 tag);

// The active entry at the lowest tag >= *tag, its tag stored back in *tag,
// or NULL. Chunks with nothing active are skipped, so a walk over a grown
// table costs what is in flight.
struct p9_outstanding *p9_session_next_active(struct p9_session *s, u32 *tag);

// The owner registered on an active tag, or NULL.
void *p9_session_owner(struct p9_session *s, u32 tag);

// Register `owner` on an active tag; NULL drops it. Fail-soft: an inactive
// or out-of-table tag is left alone, so an owner never outlives its tag.
void  p9_session_set_owner(struct p9_session *s, u32 tag, void *owner);

// Count the active op under `tag` against the async share. Fail-soft on an
// inactive, already-async or Tflush entry.
void  p9_session_mark_async(struct p9_session *s, u16 tag);
size_t p9_session_n_bound_fids(const struct p9_session *s);
size_t p9_session_n_reserved_slots(const struct p9_session *s);

#endif  // THYLACINE_9P_SESSION_H
