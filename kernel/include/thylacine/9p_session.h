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
//   SendWalk                       | p9_session_send_walk
//   SendClunk                      | p9_session_send_clunk
//   ReceiveOp                      | p9_session_dispatch_rmsg
//
// Invariants this module upholds (TLC-pinned per `specs/9p_client.tla`):
//
//   I-10  Per-session tag uniqueness — bitmap allocator over a fixed
//         outstanding-table; alloc_tag refuses to return a tag whose
//         entry is currently active.
//
//   I-11  Per-session fid identity stable across open lifetime —
//         fid_bind/fid_unbind are explicit; SendClunk Send-time-unbinds
//         per spec's client discipline.
//
//   OutOfOrderCorrectness — dispatch_rmsg uses tag-indexed lookup
//         (`outstanding[tag]`), not arrival-order, to apply the state
//         mutation. The bookkeeping pairs Send with the correct
//         Receive regardless of Rmsg arrival order.
//
//   FlowControl — alloc_tag returns -1 when |Inflight| ==
//         P9_SESSION_MAX_OUTSTANDING. Back-pressure surfaces as
//         send-side refusal, never as silent overflow.
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
// holding multiple references to the same session must coordinate.
// At v1.0 the kernel 9P client owns sessions per-Proc and serializes
// access via the Proc lock; this matches `stratum/v2/docs/reference/23-9p_client.md`
// §"Concurrency model" — "One client = one connection = one fid namespace."

#ifndef THYLACINE_9P_SESSION_H
#define THYLACINE_9P_SESSION_H

#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

// =============================================================================
// Sizing constants.
// =============================================================================

// Max outstanding tags per session. Sized to comfortably cover
// pipelined-32 workloads (VISION §4.5 throughput target) with margin.
// Conservative at v1.0; bumpable. Tags allocated from 0..MAX-1; tag
// values past MAX are reserved (Tversion uses NOTAG = 0xFFFF).
#define P9_SESSION_MAX_OUTSTANDING  64u

// Max bound fids per session. 256 is comfortable for general workloads;
// Stratum's server caps at 4096 (STM_9P_MAX_FIDS). The client-side cap
// is typically smaller because each fid wires a kernel-side handle.
#define P9_SESSION_MAX_FIDS         256u

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
                     // P9_TLOPEN / P9_TLCREATE / P9_TREAD / P9_TWRITE
    u32  fid;        // primary target fid; equals root_fid for version/attach
    u32  new_fid;    // walk's destination; equals fid otherwise
    u32  op_id;      // monotonic spec-side identifier (for diagnostics)
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

    // Outstanding table — indexed by tag (0..MAX-1). Each entry is
    // active iff a Tmsg was sent under that tag and the corresponding
    // Rmsg hasn't been dispatched yet.
    struct p9_outstanding outstanding[P9_SESSION_MAX_OUTSTANDING];

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

// Tear down: clears all state, clobbers magic. NULL-safe.
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
// `names` is an array of pointers (nwname elements); `name_lens` is
// the matching length array. nwname == 0 is a fid clone.
int p9_session_send_walk(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 src_fid, u32 new_fid,
                         u16 nwname,
                         const u8 *const *names,
                         const size_t *name_lens);

// Build a Tclunk on `fid`. Only valid in state OPEN. Preconditions:
//   - fid is bound.
//   - fid is NOT the root fid (root released only at session close).
//   - No other in-flight op targets fid.
// Send-time unbinds fid (the spec's canonical client-discipline shape).
int p9_session_send_clunk(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid);

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
};

// Dispatch one received Rmsg. The Rmsg's tag is looked up in
// outstanding[]; if the tag is in use and the type matches (or is
// Rlerror), the state mutation is applied and the op is marked
// complete. Returns 0 on success, -1 on malformed / unmatched / wrong
// type.
//
// The caller must keep `rmsg` alive until after this call returns
// (parse_str returns pointers INTO `rmsg`; *out captures them).
int p9_session_dispatch_rmsg(struct p9_session *s,
                             const u8 *rmsg, size_t len,
                             struct p9_dispatch_result *out);

// =============================================================================
// Query helpers (read-only; used by tests + audit + caller bookkeeping).
// =============================================================================

bool   p9_session_is_open(const struct p9_session *s);   // state == OPEN
bool   p9_session_fid_bound(const struct p9_session *s, u32 fid);
size_t p9_session_inflight(const struct p9_session *s);  // outstanding count
size_t p9_session_n_bound_fids(const struct p9_session *s);

#endif  // THYLACINE_9P_SESSION_H
