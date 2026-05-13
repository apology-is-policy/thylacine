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
    s->outstanding[t].active   = true;
    s->outstanding[t].kind     = kind;
    s->outstanding[t].fid      = fid;
    s->outstanding[t].new_fid  = new_fid;
    s->outstanding[t].op_id    = s->next_op_id;
    s->total_sent++;
}

// Clear tag `t`. Caller validates `t` was active.
static void clear_outstanding(struct p9_session *s, u16 t) {
    s->outstanding[t].active   = false;
    s->outstanding[t].kind     = 0;
    s->outstanding[t].fid      = 0;
    s->outstanding[t].new_fid  = 0;
    s->outstanding[t].op_id    = 0;
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
        s->outstanding[i].active  = false;
        s->outstanding[i].kind    = 0;
        s->outstanding[i].fid     = 0;
        s->outstanding[i].new_fid = 0;
        s->outstanding[i].op_id   = 0;
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
