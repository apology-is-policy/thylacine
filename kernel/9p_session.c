// 9P2000.L session state machine — P5-session.
//
// Per `kernel/include/thylacine/9p_session.h`. Composes the wire codec
// at `kernel/9p_wire.c` into the spec-described state machine at
// `specs/9p_client.tla`.
//
// Implementation map:
//
//   Spec action          | Impl                            | Wire helpers
//   ─────────────────────┼─────────────────────────────────┼────────────────────────────
//   OpenSession (vers)   | p9_session_send_version         | p9_build_tversion
//   OpenSession (attach) | p9_session_send_attach          | p9_build_tattach
//   CloseSession         | p9_session_close                | (no wire op)
//   SendWalk             | p9_session_send_walk            | p9_build_twalk
//   SendClunk            | p9_session_send_clunk           | p9_build_tclunk
//                        |                                 |   + Send-time fid_unbind
//   ReceiveOp (kind: walk) | p9_session_dispatch_rmsg     | p9_parse_rwalk
//                        |                                 |   + fid_bind(new_fid)
//   ReceiveOp (kind: clunk) | p9_session_dispatch_rmsg   | p9_parse_rclunk
//   ReceiveOp (Rlerror)  | p9_session_dispatch_rmsg       | p9_parse_rlerror
//                        |                                 |   (no fid mutation)
//
// State-machine guarantees:
//
//   1. Tag uniqueness (I-10): alloc_tag scans outstanding[] for the
//      first inactive slot. It refuses to return a slot already
//      `active`. The outstanding bookkeeping ensures no two in-flight
//      ops share a tag.
//
//   2. Fid stability (I-11): fid_bind and fid_unbind are explicit;
//      SendClunk Send-time-unbinds the target fid; subsequent sends
//      targeting that fid fail the `fid_bound` precondition.
//
//   3. Out-of-order correctness: dispatch_rmsg looks up the
//      outstanding entry by TAG (`outstanding[tag]`), not by arrival
//      order. The op_id stored in the outstanding entry pairs the
//      Send with the correct Receive.
//
//   4. Flow control: alloc_tag returns -1 when no slot is available.
//      Back-pressure surfaces as a send-side -1, never as a silent
//      overflow.

#include <thylacine/9p_session.h>
#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

// =============================================================================
// Compile-time invariants.
// =============================================================================

_Static_assert(P9_SESSION_MAX_OUTSTANDING >= 1u,
               "session must support at least 1 outstanding op");
_Static_assert(P9_SESSION_MAX_OUTSTANDING <= 0xFFFEu,
               "tag range must leave room for NOTAG (0xFFFF)");
_Static_assert(P9_SESSION_MAX_FIDS >= 1u,
               "session must support at least 1 bound fid (the root)");

// Default dialect version Thylacine speaks.
static const u8 P9_DEFAULT_VERSION[] = {'9', 'P', '2', '0', '0', '0', '.', 'L'};

// =============================================================================
// Fid table — linear array, swap-with-last on unbind.
// =============================================================================

static bool fid_bound(const struct p9_session *s, u32 fid) {
    for (size_t i = 0; i < s->n_bound_fids; i++) {
        if (s->bound_fids[i] == fid) return true;
    }
    return false;
}

// Insert `fid` into bound_fids. Returns 0 on success, -1 if already
// bound or capacity exhausted. Caller is expected to gate on
// `!fid_bound(...)` before calling; the duplicate check is defense in
// depth.
static int fid_bind(struct p9_session *s, u32 fid) {
    if (fid_bound(s, fid)) return -1;
    if (s->n_bound_fids >= P9_SESSION_MAX_FIDS) return -1;
    s->bound_fids[s->n_bound_fids++] = fid;
    return 0;
}

// Remove `fid` from bound_fids. Returns 0 on success, -1 if not bound.
// Compacts via swap-with-last (order doesn't matter per spec).
static int fid_unbind(struct p9_session *s, u32 fid) {
    for (size_t i = 0; i < s->n_bound_fids; i++) {
        if (s->bound_fids[i] == fid) {
            s->bound_fids[i] = s->bound_fids[s->n_bound_fids - 1];
            s->bound_fids[s->n_bound_fids - 1] = 0;
            s->n_bound_fids--;
            return 0;
        }
    }
    return -1;
}

// =============================================================================
// Tag pool — bitmap-like; tag value == outstanding-table index.
// =============================================================================

// Allocate the lowest free tag. Returns the tag value (0..MAX-1), or
// -1 if the table is full (flow-control trip).
static int alloc_tag(const struct p9_session *s) {
    for (size_t t = 0; t < P9_SESSION_MAX_OUTSTANDING; t++) {
        if (!s->outstanding[t].active) return (int)t;
    }
    return -1;
}

// Mark tag `t` active with the given op shape. Caller validates `t` is
// free.
static void mark_outstanding(struct p9_session *s, u16 t,
                              u8 kind, u32 fid, u32 new_fid) {
    s->next_op_id++;
    s->outstanding[t].active        = true;
    s->outstanding[t].kind          = kind;
    s->outstanding[t].fid           = fid;
    s->outstanding[t].new_fid       = new_fid;
    s->outstanding[t].op_id         = s->next_op_id;
    s->outstanding[t].awaiting_flush = false;
    s->outstanding[t].flush_oldtag  = 0;
    s->total_sent++;
}

// Clear tag `t`. Caller validates `t` was active.
static void clear_outstanding(struct p9_session *s, u16 t) {
    s->outstanding[t].active        = false;
    s->outstanding[t].kind          = 0;
    s->outstanding[t].fid           = 0;
    s->outstanding[t].new_fid       = 0;
    s->outstanding[t].op_id         = 0;
    s->outstanding[t].awaiting_flush = false;
    s->outstanding[t].flush_oldtag  = 0;
    s->total_completed++;
}

// Check whether any in-flight op targets `fid` (as either `fid` or
// `new_fid`). Used by SendClunk to enforce the spec's "no other
// in-flight op on the same fid" discipline.
static bool any_outstanding_on_fid(const struct p9_session *s, u32 fid) {
    for (size_t t = 0; t < P9_SESSION_MAX_OUTSTANDING; t++) {
        if (!s->outstanding[t].active) continue;
        if (s->outstanding[t].fid == fid) return true;
        if (s->outstanding[t].new_fid == fid) return true;
    }
    return false;
}

// =============================================================================
// Lifecycle.
// =============================================================================

int p9_session_init(struct p9_session *s, u32 root_fid, u32 msize) {
    if (!s) return -1;
    if (root_fid == P9_NOFID) return -1;
    if (msize == 0) return -1;
    s->magic            = P9_SESSION_MAGIC;
    s->state            = P9_SESS_INIT;
    s->root_fid         = root_fid;
    s->msize            = msize;
    s->negotiated_msize = 0;
    for (size_t i = 0; i < P9_SESSION_MAX_FIDS; i++) s->bound_fids[i] = 0;
    s->n_bound_fids     = 0;
    for (size_t i = 0; i < P9_SESSION_MAX_OUTSTANDING; i++) {
        s->outstanding[i].active        = false;
        s->outstanding[i].kind          = 0;
        s->outstanding[i].fid           = 0;
        s->outstanding[i].new_fid       = 0;
        s->outstanding[i].op_id         = 0;
        s->outstanding[i].awaiting_flush = false;
        s->outstanding[i].flush_oldtag  = 0;
    }
    s->next_op_id       = 0;
    s->total_sent       = 0;
    s->total_completed  = 0;
    return 0;
}

void p9_session_destroy(struct p9_session *s) {
    if (!s) return;
    if (s->magic != P9_SESSION_MAGIC) return;     // defensive: not ours
    // Clobber magic FIRST so subsequent calls into a freed/destroyed
    // session fast-fail (R9 F148 mirror — see docs/reference/39-hw-handles.md
    // caveat #2 for the kobj_*_unref pattern).
    s->magic            = 0;
    s->state            = P9_SESS_CLOSED;
    s->n_bound_fids     = 0;
    for (size_t i = 0; i < P9_SESSION_MAX_OUTSTANDING; i++) {
        s->outstanding[i].active = false;
    }
}

int p9_session_close(struct p9_session *s) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    // Refuse close while ops are in flight (spec's CloseSession action
    // requires Inflight = {}).
    for (size_t t = 0; t < P9_SESSION_MAX_OUTSTANDING; t++) {
        if (s->outstanding[t].active) return -1;
    }
    s->state            = P9_SESS_CLOSED;
    s->n_bound_fids     = 0;
    return 0;
}

// =============================================================================
// Send: Tversion.
// =============================================================================

int p9_session_send_version(struct p9_session *s,
                            u8 *out, size_t cap,
                            const u8 *version, size_t version_len) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_INIT) return -1;
    if (!out) return -1;
    // Tversion uses NOTAG; never allocates from the tag pool. We still
    // insert into outstanding to keep dispatch_rmsg's tag-lookup happy —
    // but since the index range is 0..MAX-1 and NOTAG=0xFFFF, we'd
    // overflow. Workaround: we use tag 0 as the version-in-flight slot.
    //
    // Hmm wait — that conflicts with normal tag allocation. Cleaner:
    // version uses a dedicated outstanding entry tracked outside the
    // bitmap. For simplicity at this chunk, we treat Tversion as
    // exiting on Rversion without entering outstanding[] (the wire's
    // NOTAG semantic — version is "out of band" relative to the
    // normal tag pool).
    //
    // dispatch_rmsg handles Rversion specially: if state is INIT, it
    // expects Rversion (not from outstanding[]).
    const u8 *ver = (version != NULL) ? version : P9_DEFAULT_VERSION;
    size_t    ver_len = (version != NULL) ? version_len : sizeof(P9_DEFAULT_VERSION);
    int rc = p9_build_tversion(out, cap, P9_NOTAG, s->msize, ver, ver_len);
    if (rc < 0) return -1;
    // No outstanding slot taken; Tversion is out-of-band.
    s->total_sent++;
    return rc;
}

// =============================================================================
// Send: Tattach.
// =============================================================================

int p9_session_send_attach(struct p9_session *s,
                           u8 *out, size_t cap,
                           const u8 *uname, size_t uname_len,
                           const u8 *aname, size_t aname_len,
                           u32 n_uname) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_VERSIONED) return -1;
    if (!out) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tattach(out, cap, (u16)t,
                              s->root_fid, P9_NOFID,
                              uname, uname_len,
                              aname, aname_len,
                              n_uname);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TATTACH, s->root_fid, s->root_fid);
    return rc;
}

// =============================================================================
// Send: Twalk.
// =============================================================================

int p9_session_send_walk(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 src_fid, u32 new_fid,
                         u16 nwname,
                         const u8 *const *names,
                         const size_t *name_lens) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, src_fid)) return -1;
    if (fid_bound(s, new_fid)) return -1;
    if (new_fid == P9_NOFID) return -1;
    if (new_fid == s->root_fid) return -1;
    if (nwname > P9_MAX_WALK) return -1;
    // Per spec's SendWalk precondition: no other in-flight op targets
    // new_fid as either src or destination.
    if (any_outstanding_on_fid(s, new_fid)) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_twalk(out, cap, (u16)t,
                            src_fid, new_fid,
                            nwname, names, name_lens);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TWALK, src_fid, new_fid);
    return rc;
}

// =============================================================================
// Send: Tclunk.
// =============================================================================

int p9_session_send_clunk(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    if (fid == s->root_fid) return -1;
    // Spec's SendClunk precondition: no other in-flight op targets fid.
    if (any_outstanding_on_fid(s, fid)) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tclunk(out, cap, (u16)t, fid);
    if (rc < 0) return -1;
    // Send-time unbind (spec's client discipline: no further ops on
    // this fid even while Tclunk's Rmsg is in flight).
    (void)fid_unbind(s, fid);
    mark_outstanding(s, (u16)t, P9_TCLUNK, fid, fid);
    return rc;
}

// =============================================================================
// Send: Tflush (#845) -- abandon an in-flight request whose owner is gone.
// =============================================================================

int p9_session_send_flush(struct p9_session *s,
                          u8 *out, size_t cap,
                          u16 oldtag) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    // Valid wherever a real-tag op can be outstanding: steady-state ops are
    // OPEN, the handshake's Tattach is VERSIONED. Tversion uses NOTAG and is
    // never tracked in outstanding[], so it is never flushable.
    if (s->state != P9_SESS_OPEN && s->state != P9_SESS_VERSIONED) return -1;
    if (!out) return -1;
    if (oldtag >= P9_SESSION_MAX_OUTSTANDING) return -1;
    struct p9_outstanding *victim = &s->outstanding[oldtag];
    if (!victim->active) return -1;              // nothing in flight under oldtag
    if (victim->kind == P9_TFLUSH) return -1;    // never flush a flush
    if (victim->awaiting_flush) return -1;       // already being flushed
    int t = alloc_tag(s);
    if (t < 0) return -1;                        // pool full -> caller falls back
    int rc = p9_build_tflush(out, cap, (u16)t, oldtag);
    if (rc < 0) return -1;
    // The flush op is fid-less; root_fid is a harmless placeholder (matches
    // version/attach). alloc_tag skipped the active `oldtag`, so t != oldtag
    // and the victim pointer survives mark_outstanding's write to t. Record
    // oldtag so the Rflush can free it, and reserve oldtag against reuse until
    // that Rflush (9P: oldtag not reusable until Rflush -- the I-10 guard).
    mark_outstanding(s, (u16)t, P9_TFLUSH, s->root_fid, s->root_fid);
    s->outstanding[t].flush_oldtag = oldtag;
    victim->awaiting_flush = true;
    return rc;
}

// =============================================================================
// Send: IO family (Tlopen / Tlcreate / Tread / Twrite). Each shares the
// OPEN-state + fid-bound preconditions; mutation-shaped ops (lopen,
// lcreate) additionally require no other in-flight op on fid.
// =============================================================================

int p9_session_send_lopen(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid, u32 flags) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    // Tlopen mutates server-side fid state; refuse concurrent ops on fid.
    if (any_outstanding_on_fid(s, fid)) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tlopen(out, cap, (u16)t, fid, flags);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TLOPEN, fid, fid);
    return rc;
}

int p9_session_send_lcreate(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid,
                            const u8 *name, size_t name_len,
                            u32 flags, u32 mode, u32 gid) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    if (name_len == 0 || name_len > P9_NAME_MAX) return -1;
    if (!name) return -1;
    // Tlcreate rebinds fid to the new file at server-side; refuse
    // concurrent ops on fid (the binding is observable as soon as the
    // server processes the request).
    if (any_outstanding_on_fid(s, fid)) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tlcreate(out, cap, (u16)t, fid,
                               name, name_len, flags, mode, gid);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TLCREATE, fid, fid);
    return rc;
}

int p9_session_send_read(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 fid, u64 offset, u32 count) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    // Tread permits concurrent ops on fid (offset is explicit on the wire).
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tread(out, cap, (u16)t, fid, offset, count);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TREAD, fid, fid);
    return rc;
}

int p9_session_send_write(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid, u64 offset,
                          u32 count, const u8 *data) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    if (count > 0 && !data) return -1;
    // Twrite permits concurrent ops on fid (offset is explicit on the wire).
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_twrite(out, cap, (u16)t, fid, offset, count, data);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TWRITE, fid, fid);
    return rc;
}

// =============================================================================
// Send: metadata family (Tgetattr / Tsetattr / Treaddir / Tstatfs / Tfsync).
// Read-shaped ops permit concurrent fids; setattr is mutation-shaped and
// requires fid-exclusion.
// =============================================================================

int p9_session_send_getattr(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid, u64 request_mask) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    // Tgetattr is read-shaped — concurrent ops on fid permitted.
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tgetattr(out, cap, (u16)t, fid, request_mask);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TGETATTR, fid, fid);
    return rc;
}

int p9_session_send_setattr(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid, const struct p9_setattr *attr) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    if (!attr) return -1;
    // Tsetattr mutates server-side metadata; refuse concurrent ops on fid.
    if (any_outstanding_on_fid(s, fid)) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tsetattr(out, cap, (u16)t, fid, attr);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TSETATTR, fid, fid);
    return rc;
}

int p9_session_send_readdir(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid, u64 offset, u32 count) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    // Treaddir permits concurrent ops on fid (offset is explicit on the wire).
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_treaddir(out, cap, (u16)t, fid, offset, count);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TREADDIR, fid, fid);
    return rc;
}

int p9_session_send_statfs(struct p9_session *s,
                           u8 *out, size_t cap,
                           u32 fid) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    // Tstatfs is read-only at the fid — concurrent permitted.
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tstatfs(out, cap, (u16)t, fid);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TSTATFS, fid, fid);
    return rc;
}

int p9_session_send_fsync(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 fid, u32 datasync) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    // Tfsync is a barrier; concurrent calls on the same fid are wasteful
    // but not undefined (idempotent). Permitted.
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tfsync(out, cap, (u16)t, fid, datasync);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TFSYNC, fid, fid);
    return rc;
}

// =============================================================================
// Send: mutation family. All ops require state OPEN + fid_bound on the
// targeting fid. Trename is fid-exclusive (server-side identity mutation);
// other mutation ops permit concurrent ops on the same fid (server
// serializes per directory entry internally).
// =============================================================================

int p9_session_send_symlink(struct p9_session *s,
                            u8 *out, size_t cap,
                            u32 fid,
                            const u8 *name, size_t name_len,
                            const u8 *symtgt, size_t symtgt_len,
                            u32 gid) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    if (name_len == 0 || name_len > P9_NAME_MAX) return -1;
    if (!name) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tsymlink(out, cap, (u16)t, fid,
                               name, name_len, symtgt, symtgt_len, gid);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TSYMLINK, fid, fid);
    return rc;
}

int p9_session_send_mknod(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 dfid,
                          const u8 *name, size_t name_len,
                          u32 mode, u32 major, u32 minor, u32 gid) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, dfid)) return -1;
    if (name_len == 0 || name_len > P9_NAME_MAX) return -1;
    if (!name) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tmknod(out, cap, (u16)t, dfid,
                             name, name_len, mode, major, minor, gid);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TMKNOD, dfid, dfid);
    return rc;
}

int p9_session_send_rename(struct p9_session *s,
                           u8 *out, size_t cap,
                           u32 fid, u32 dfid,
                           const u8 *name, size_t name_len) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    if (!fid_bound(s, dfid)) return -1;
    if (name_len == 0 || name_len > P9_NAME_MAX) return -1;
    if (!name) return -1;
    // Trename mutates server-side identity of fid; refuse concurrent ops.
    if (any_outstanding_on_fid(s, fid)) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_trename(out, cap, (u16)t, fid, dfid, name, name_len);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TRENAME, fid, fid);
    return rc;
}

int p9_session_send_readlink(struct p9_session *s,
                             u8 *out, size_t cap,
                             u32 fid) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_treadlink(out, cap, (u16)t, fid);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TREADLINK, fid, fid);
    return rc;
}

int p9_session_send_link(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 dfid, u32 fid,
                         const u8 *name, size_t name_len) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, dfid)) return -1;
    if (!fid_bound(s, fid)) return -1;
    if (name_len == 0 || name_len > P9_NAME_MAX) return -1;
    if (!name) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tlink(out, cap, (u16)t, dfid, fid, name, name_len);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TLINK, dfid, fid);
    return rc;
}

int p9_session_send_mkdir(struct p9_session *s,
                          u8 *out, size_t cap,
                          u32 dfid,
                          const u8 *name, size_t name_len,
                          u32 mode, u32 gid) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, dfid)) return -1;
    if (name_len == 0 || name_len > P9_NAME_MAX) return -1;
    if (!name) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tmkdir(out, cap, (u16)t, dfid, name, name_len, mode, gid);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TMKDIR, dfid, dfid);
    return rc;
}

int p9_session_send_renameat(struct p9_session *s,
                             u8 *out, size_t cap,
                             u32 olddirfid,
                             const u8 *oldname, size_t oldname_len,
                             u32 newdirfid,
                             const u8 *newname, size_t newname_len) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, olddirfid)) return -1;
    if (!fid_bound(s, newdirfid)) return -1;
    if (oldname_len == 0 || oldname_len > P9_NAME_MAX) return -1;
    if (newname_len == 0 || newname_len > P9_NAME_MAX) return -1;
    if (!oldname || !newname) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_trenameat(out, cap, (u16)t,
                                olddirfid, oldname, oldname_len,
                                newdirfid, newname, newname_len);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TRENAMEAT, olddirfid, newdirfid);
    return rc;
}

int p9_session_send_unlinkat(struct p9_session *s,
                             u8 *out, size_t cap,
                             u32 dfid,
                             const u8 *name, size_t name_len,
                             u32 flags) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, dfid)) return -1;
    if (name_len == 0 || name_len > P9_NAME_MAX) return -1;
    if (!name) return -1;
    int t = alloc_tag(s);
    if (t < 0) return -1;
    int rc = p9_build_tunlinkat(out, cap, (u16)t, dfid, name, name_len, flags);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TUNLINKAT, dfid, dfid);
    return rc;
}

// =============================================================================
// Receive: dispatch by tag, apply state mutation.
// =============================================================================

static void zero_result(struct p9_dispatch_result *out) {
    out->kind          = 0;
    out->fid           = 0;
    out->new_fid       = 0;
    out->op_id         = 0;
    out->is_error      = false;
    out->ecode         = 0;
    out->nwqid         = 0;
    out->version_msize = 0;
    out->version_len   = 0;
    out->version_ptr   = NULL;
    out->attach_qid.type    = 0;
    out->attach_qid.version = 0;
    out->attach_qid.path    = 0;
    for (size_t i = 0; i < P9_MAX_WALK; i++) {
        out->qids[i].type    = 0;
        out->qids[i].version = 0;
        out->qids[i].path    = 0;
    }
    out->open_qid.type    = 0;
    out->open_qid.version = 0;
    out->open_qid.path    = 0;
    out->open_iounit      = 0;
    out->read_count       = 0;
    out->read_data        = NULL;
    out->write_count      = 0;
    // Metadata family zero-init.
    out->attr.valid       = 0;
    out->attr.qid.type    = 0;
    out->attr.qid.version = 0;
    out->attr.qid.path    = 0;
    out->attr.mode        = 0;
    out->attr.uid         = 0;
    out->attr.gid         = 0;
    out->attr.nlink       = 0;
    out->attr.rdev        = 0;
    out->attr.size        = 0;
    out->attr.blksize     = 0;
    out->attr.blocks      = 0;
    out->attr.atime_sec   = 0;
    out->attr.atime_nsec  = 0;
    out->attr.mtime_sec   = 0;
    out->attr.mtime_nsec  = 0;
    out->attr.ctime_sec   = 0;
    out->attr.ctime_nsec  = 0;
    out->attr.btime_sec   = 0;
    out->attr.btime_nsec  = 0;
    out->attr.gen         = 0;
    out->attr.data_version = 0;
    out->statfs.type      = 0;
    out->statfs.bsize     = 0;
    out->statfs.blocks    = 0;
    out->statfs.bfree     = 0;
    out->statfs.bavail    = 0;
    out->statfs.files     = 0;
    out->statfs.ffree     = 0;
    out->statfs.fsid      = 0;
    out->statfs.namelen   = 0;
    out->readdir_count    = 0;
    out->readdir_data     = NULL;
    out->created_qid.type    = 0;
    out->created_qid.version = 0;
    out->created_qid.path    = 0;
    out->readlink_target     = NULL;
    out->readlink_target_len = 0;
}

// Special path for Rversion: tag is NOTAG; not from outstanding[];
// only valid in state INIT.
static int dispatch_rversion(struct p9_session *s,
                              const u8 *rmsg, size_t len,
                              struct p9_dispatch_result *out) {
    if (s->state != P9_SESS_INIT) return -1;
    u16 tag;
    u32 msize;
    const u8 *version_ptr;
    u16 version_len;
    int rc = p9_parse_rversion(rmsg, len, &tag, &msize, &version_ptr, &version_len);
    if (rc < 0) return -1;
    if (tag != P9_NOTAG) return -1;
    // Negotiate down: per spec the server's msize is the final value.
    s->negotiated_msize  = (msize <= s->msize) ? msize : s->msize;
    s->state             = P9_SESS_VERSIONED;
    s->total_completed++;
    out->kind            = P9_TVERSION;
    out->fid             = s->root_fid;
    out->new_fid         = s->root_fid;
    out->op_id           = 0;
    out->version_msize   = s->negotiated_msize;
    out->version_len     = version_len;
    out->version_ptr     = version_ptr;
    return 0;
}

int p9_session_dispatch_rmsg(struct p9_session *s,
                             const u8 *rmsg, size_t len,
                             struct p9_dispatch_result *out) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (!out) return -1;
    if (!rmsg) return -1;
    zero_result(out);

    u32 size; u8 type; u16 tag;
    int rc = p9_peek_header(rmsg, len, &size, &type, &tag);
    if (rc < 0) return -1;

    // Rversion is the only Rmsg that lives outside the outstanding[]
    // bookkeeping (it uses NOTAG). Dispatch it specially.
    if (type == P9_RVERSION) {
        return dispatch_rversion(s, rmsg, len, out);
    }

    // For every other Rmsg, the tag must index a live outstanding entry.
    if (tag >= P9_SESSION_MAX_OUTSTANDING) return -1;
    struct p9_outstanding *op = &s->outstanding[tag];
    if (!op->active) return -1;

    // A reply for a tag reserved by a pending Tflush (#845) is a LATE reply
    // for an abandoned op (its owner Proc died). Consume it WITHOUT freeing
    // the tag: per 9P, oldtag is reusable only after the Rflush, so freeing
    // here would let the tag be reused while a stray duplicate / twin reply
    // is still possible -> a future reply mis-attributed to the reused tag
    // (the I-10 violation the naive fix introduces). The flush's Rflush
    // (dispatched via its own tag, below) is the SOLE authority that frees an
    // awaiting_flush tag. No fid mutation either: the abandoned caller is gone.
    if (op->awaiting_flush) {
        out->kind = op->kind;       // diagnostics only; this reply is ownerless
        return 0;
    }

    // Type must match (or be Rlerror). The R-msg of T-msg `kind` is
    // numerically kind + 1.
    u8 expected_r = (u8)(op->kind + 1);
    if (type != expected_r && type != P9_RLERROR) {
        return -1;
    }

    // Apply state mutation based on the actual response type.
    if (type == P9_RLERROR) {
        u16 tag_check;
        u32 ecode;
        rc = p9_parse_rlerror(rmsg, len, &tag_check, &ecode);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // No fid mutation on Rlerror (note: clunk's Send-time unbind
        // STAYS — the client already treated the fid as gone).
        out->is_error = true;
        out->ecode    = ecode;
    } else if (op->kind == P9_TATTACH) {
        u16 tag_check;
        struct p9_qid qid;
        rc = p9_parse_rattach(rmsg, len, &tag_check, &qid);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // Bind the root fid.
        if (fid_bind(s, s->root_fid) < 0) return -1;
        s->state          = P9_SESS_OPEN;
        out->attach_qid   = qid;
    } else if (op->kind == P9_TWALK) {
        u16 tag_check;
        u16 nwqid;
        rc = p9_parse_rwalk(rmsg, len, &tag_check, &nwqid, out->qids, P9_MAX_WALK);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // Per 9P2000.L: bind new_fid into the fid table at Rwalk time
        // (server-side WalkBindsWithCurrentGen — see
        // `stratum/v2/docs/reference/20-9p.md` §"fid.tla composition").
        // At this bring-up subset we bind unconditionally; nuanced
        // partial-walk semantics (when nwqid < requested nwname) land
        // in P5-session-walk-partial.
        if (fid_bind(s, op->new_fid) < 0) return -1;
        out->nwqid        = nwqid;
    } else if (op->kind == P9_TCLUNK) {
        u16 tag_check;
        rc = p9_parse_rclunk(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // Send-time already unbound; no further action.
    } else if (op->kind == P9_TFLUSH) {
        // Rflush (#845): the server guarantees it will not answer the
        // abandoned oldtag. This Rflush is therefore the SOLE authority that
        // frees oldtag (the I-10 reuse-race guard). Clear the reserved
        // original here; the common tail below clears this flush's own tag.
        u16 tag_check;
        rc = p9_parse_rflush(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        u16 oldtag = op->flush_oldtag;
        if (oldtag < P9_SESSION_MAX_OUTSTANDING &&
            s->outstanding[oldtag].active &&
            s->outstanding[oldtag].awaiting_flush) {
            clear_outstanding(s, oldtag);
        }
    } else if (op->kind == P9_TLOPEN) {
        u16 tag_check;
        struct p9_qid qid;
        u32 iounit;
        rc = p9_parse_rlopen(rmsg, len, &tag_check, &qid, &iounit);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // No fid table mutation: fid stays bound; server's view shifts
        // from "walked (closed)" to "opened-with-mode".
        out->open_qid    = qid;
        out->open_iounit = iounit;
    } else if (op->kind == P9_TLCREATE) {
        u16 tag_check;
        struct p9_qid qid;
        u32 iounit;
        rc = p9_parse_rlcreate(rmsg, len, &tag_check, &qid, &iounit);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // No fid table mutation: fid stays bound; server's view shifts
        // from "parent dir" to "newly opened file". The client-app caller
        // is responsible for understanding the semantic rebind.
        out->open_qid    = qid;
        out->open_iounit = iounit;
    } else if (op->kind == P9_TREAD) {
        u16 tag_check;
        u32 count;
        const u8 *data;
        // Use the session's negotiated_msize as the upper bound on the
        // server-supplied count (R111 doctrine). msize - 11 is the
        // theoretical max single-read count (msize - header - count
        // field), but the parser only needs to refuse oversize claims;
        // strict-equality below catches the rest.
        u32 data_cap = (s->negotiated_msize > P9_HDR_LEN + 4)
            ? (s->negotiated_msize - P9_HDR_LEN - 4)
            : 0;
        rc = p9_parse_rread(rmsg, len, &tag_check, &count, &data, data_cap);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        out->read_count = count;
        out->read_data  = data;
    } else if (op->kind == P9_TWRITE) {
        u16 tag_check;
        u32 count;
        rc = p9_parse_rwrite(rmsg, len, &tag_check, &count);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        out->write_count = count;
    } else if (op->kind == P9_TGETATTR) {
        u16 tag_check;
        rc = p9_parse_rgetattr(rmsg, len, &tag_check, &out->attr);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
    } else if (op->kind == P9_TSETATTR) {
        u16 tag_check;
        rc = p9_parse_rsetattr(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
    } else if (op->kind == P9_TREADDIR) {
        u16 tag_check;
        u32 count;
        const u8 *data;
        // Same R111 cap-derivation as Tread: max single-message readdir
        // count is negotiated_msize - 11 (header + count field).
        u32 data_cap = (s->negotiated_msize > P9_HDR_LEN + 4)
            ? (s->negotiated_msize - P9_HDR_LEN - 4)
            : 0;
        rc = p9_parse_rreaddir(rmsg, len, &tag_check, &count, &data, data_cap);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        out->readdir_count = count;
        out->readdir_data  = data;
    } else if (op->kind == P9_TSTATFS) {
        u16 tag_check;
        rc = p9_parse_rstatfs(rmsg, len, &tag_check, &out->statfs);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
    } else if (op->kind == P9_TFSYNC) {
        u16 tag_check;
        rc = p9_parse_rfsync(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
    } else if (op->kind == P9_TSYMLINK) {
        u16 tag_check;
        struct p9_qid qid;
        rc = p9_parse_rsymlink(rmsg, len, &tag_check, &qid);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        out->created_qid = qid;
    } else if (op->kind == P9_TMKNOD) {
        u16 tag_check;
        struct p9_qid qid;
        rc = p9_parse_rmknod(rmsg, len, &tag_check, &qid);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        out->created_qid = qid;
    } else if (op->kind == P9_TMKDIR) {
        u16 tag_check;
        struct p9_qid qid;
        rc = p9_parse_rmkdir(rmsg, len, &tag_check, &qid);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        out->created_qid = qid;
    } else if (op->kind == P9_TRENAME) {
        u16 tag_check;
        rc = p9_parse_rrename(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
    } else if (op->kind == P9_TRENAMEAT) {
        u16 tag_check;
        rc = p9_parse_rrenameat(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
    } else if (op->kind == P9_TLINK) {
        u16 tag_check;
        rc = p9_parse_rlink(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
    } else if (op->kind == P9_TUNLINKAT) {
        u16 tag_check;
        rc = p9_parse_runlinkat(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
    } else if (op->kind == P9_TREADLINK) {
        u16 tag_check;
        const u8 *target;
        u16 target_len;
        rc = p9_parse_rreadlink(rmsg, len, &tag_check, &target, &target_len);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        out->readlink_target     = target;
        out->readlink_target_len = target_len;
    } else {
        // Unknown / unsupported kind.
        return -1;
    }

    // Echo back what we completed.
    out->kind    = op->kind;
    out->fid     = op->fid;
    out->new_fid = op->new_fid;
    out->op_id   = op->op_id;
    clear_outstanding(s, tag);
    return 0;
}

// =============================================================================
// Query helpers.
// =============================================================================

bool p9_session_is_open(const struct p9_session *s) {
    if (!s) return false;
    if (s->magic != P9_SESSION_MAGIC) return false;
    return s->state == P9_SESS_OPEN;
}

bool p9_session_fid_bound(const struct p9_session *s, u32 fid) {
    if (!s) return false;
    if (s->magic != P9_SESSION_MAGIC) return false;
    return fid_bound(s, fid);
}

size_t p9_session_inflight(const struct p9_session *s) {
    if (!s) return 0;
    if (s->magic != P9_SESSION_MAGIC) return 0;
    size_t n = 0;
    for (size_t t = 0; t < P9_SESSION_MAX_OUTSTANDING; t++) {
        if (s->outstanding[t].active) n++;
    }
    return n;
}

size_t p9_session_n_bound_fids(const struct p9_session *s) {
    if (!s) return 0;
    if (s->magic != P9_SESSION_MAGIC) return 0;
    return s->n_bound_fids;
}
