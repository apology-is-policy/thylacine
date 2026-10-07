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
//   1. Tag uniqueness (I-10): alloc_tag returns the lowest inactive
//      entry of the tag table. It refuses to return an entry already
//      `active`. The outstanding bookkeeping ensures no two in-flight
//      ops share a tag.
//
//   2. Fid stability (I-11): fid_bind and fid_unbind are explicit;
//      SendClunk Send-time-unbinds the target fid; subsequent sends
//      targeting that fid fail the `fid_bound` precondition.
//
//   3. Out-of-order correctness: dispatch_rmsg looks up the
//      outstanding entry by TAG (`entry(s, tag)`), not by arrival
//      order. The op_id stored in the outstanding entry pairs the
//      Send with the correct Receive.
//
//   4. Flow control: alloc_tag returns -1 when the op share is full or
//      the table cannot grow (ARCH 21.11). Back-pressure surfaces as a
//      send-side -1, never as a silent overflow.

#include <thylacine/9p_session.h>
#include <thylacine/9p_wire.h>
#include <thylacine/errno.h>     // T_E_IO for the synthetic local-failure ecode
#include <thylacine/page.h>      // KP_ZERO
#include <thylacine/types.h>

#include "../mm/slub.h"

// =============================================================================
// Compile-time invariants.
// =============================================================================

_Static_assert(P9_TAG_LIMIT <= P9_NOTAG,
               "tags must leave room for NOTAG (0xFFFF)");
_Static_assert(P9_TAG_CHUNK >= 1u && P9_TAG_CHUNKS * P9_TAG_CHUNK >= P9_TAG_LIMIT,
               "the chunks must cover every tag");
_Static_assert(2u * P9_OPS_MAX <= P9_TAG_LIMIT,
               "a Tflush must always find a tag: flushes <= ops <= P9_OPS_MAX");
_Static_assert(P9_ASYNC_MAX < P9_OPS_MAX,
               "async ops must leave part of the op share to sync ops");
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
// Reserved fid slots (docs/FID-LIFECYCLE-DESIGN.md section 9). An outstanding
// op that may leave a fid bound when it ends holds a slot: a walk naming a new
// fid the slot that fid will bind, a Tclunk the slot of the fid its build
// unbound. Bound + reserved never exceeds P9_SESSION_MAX_FIDS, because only a
// new reservation needs room and it checks slot_available; a take-back or a
// walk's bind turns a reserved slot back into a bound one.
// =============================================================================

static bool slot_available(const struct p9_session *s) {
    return s->n_bound_fids + s->n_reserved_slots < P9_SESSION_MAX_FIDS;
}

static struct p9_outstanding *entry(struct p9_session *s, u32 t);

static void slot_reserve(struct p9_session *s, u16 t) {
    entry(s, t)->holds_slot = true;
    s->n_reserved_slots++;
}

static void slot_release(struct p9_session *s, u16 t) {
    struct p9_outstanding *e = entry(s, t);
    if (!e->holds_slot) return;
    e->holds_slot = false;
    s->n_reserved_slots--;
}

// Bind `fid` into op `t`'s reserved slot. The release comes first, so the
// capacity check inside fid_bind cannot refuse; its duplicate check is the
// only refusal left.
static int slot_bind(struct p9_session *s, u16 t, u32 fid) {
    slot_release(s, t);
    return fid_bind(s, fid);
}

// =============================================================================
// The tag table (ARCH 21.11); tag value == table index.
// =============================================================================

static struct p9_tag_chunk *chunk(struct p9_session *s, u32 k) {
    return k == 0 ? &s->tags0 : s->tag_dir[k];
}

static struct p9_outstanding *entry(struct p9_session *s, u32 t) {
    u32 k = t / P9_TAG_CHUNK;
    if (k >= s->n_chunks) return NULL;
    return &chunk(s, k)->e[t % P9_TAG_CHUNK];
}

static void entry_zero(struct p9_outstanding *e) {
    e->active         = false;
    e->kind           = 0;
    e->fid            = 0;
    e->new_fid        = 0;
    e->op_id          = 0;
    e->awaiting_flush = false;
    e->owner_waits    = false;
    e->abandoned      = false;
    e->holds_slot     = false;
    e->flush_oldtag   = 0;
    e->wga_nwname     = 0;
    e->flush_tag      = 0;
    e->async          = false;
    e->owner          = NULL;
}

// Add a chunk and return its first tag, or -1 at tag_limit or when an
// allocation fails. The caller holds the client's spinlock; kmalloc does not
// sleep. The directory is allocated with the first chunk past tags0.
static int grow(struct p9_session *s) {
    u32 k = s->n_chunks;
    if (k >= P9_TAG_CHUNKS || k * P9_TAG_CHUNK >= s->tag_limit) return -1;
    if (!s->tag_dir) {
        s->tag_dir = kmalloc(P9_TAG_CHUNKS * sizeof(*s->tag_dir), KP_ZERO);
        if (!s->tag_dir) return -1;
    }
    struct p9_tag_chunk *ch = kmalloc(sizeof(*ch), KP_ZERO);
    if (!ch) return -1;
    s->tag_dir[k] = ch;
    s->n_chunks   = k + 1;
    return (int)(k * P9_TAG_CHUNK);
}

// Allocate the lowest free tag, growing the table when every entry is held.
// An op needs room in the op share; a Tflush takes any free tag. Returns the
// tag, or -1 (the share is full, or the table cannot grow).
static int alloc_tag(struct p9_session *s, bool flush) {
    if (!flush && s->n_active - s->n_flush >= s->ops_max) return -1;
    for (u32 k = 0; k < s->n_chunks; k++) {
        struct p9_tag_chunk *ch = chunk(s, k);
        if (ch->n_active == P9_TAG_CHUNK) continue;
        for (u32 i = 0; i < P9_TAG_CHUNK; i++) {
            u32 t = k * P9_TAG_CHUNK + i;
            if (t >= s->tag_limit) return -1;
            if (!ch->e[i].active) return (int)t;
        }
    }
    return grow(s);
}

// Mark tag `t` active with the given op shape. Caller validates `t` is
// free.
static void mark_outstanding(struct p9_session *s, u16 t,
                              u8 kind, u32 fid, u32 new_fid) {
    struct p9_outstanding *e = entry(s, t);
    s->next_op_id++;
    entry_zero(e);
    e->active  = true;
    e->kind    = kind;
    e->fid     = fid;
    e->new_fid = new_fid;
    e->op_id   = s->next_op_id;
    chunk(s, t / P9_TAG_CHUNK)->n_active++;
    s->n_active++;
    if (kind == P9_TFLUSH) s->n_flush++;
    s->total_sent++;
}

// Clear tag `t`. Caller validates `t` was active.
static void clear_outstanding(struct p9_session *s, u16 t) {
    struct p9_outstanding *e = entry(s, t);
    slot_release(s, t);
    if (e->kind == P9_TFLUSH) s->n_flush--;
    if (e->async)             s->n_async--;
    entry_zero(e);
    chunk(s, t / P9_TAG_CHUNK)->n_active--;
    s->n_active--;
    s->total_completed++;
}

// The active entry at the lowest tag >= *t, or NULL; skips idle chunks.
static struct p9_outstanding *next_active(struct p9_session *s, u32 *t) {
    for (u32 k = *t / P9_TAG_CHUNK; k < s->n_chunks; k++) {
        struct p9_tag_chunk *ch = chunk(s, k);
        u32 i = (k == *t / P9_TAG_CHUNK) ? *t % P9_TAG_CHUNK : 0;
        if (ch->n_active == 0) continue;
        for (; i < P9_TAG_CHUNK; i++) {
            if (!ch->e[i].active) continue;
            *t = k * P9_TAG_CHUNK + i;
            return &ch->e[i];
        }
    }
    return NULL;
}

// Check whether any LIVE in-flight op targets `fid` (as either `fid` or
// `new_fid`). Used by SendClunk / SendWalk(new_fid) / SendLopen / SendLcreate /
// SendWalkgetattr / SendSetattr / SendRename (SEVEN callers -- keep this list
// current: a stale list narrows future audit scoping, R2-F2) to
// enforce the spec's "no other in-flight op on the same fid" discipline.
//
// A FLUSHED op whose owner is gone (awaiting_flush without owner_waits, #845)
// is EXCLUDED: it has been cancelled, its reply is discarded (the I-10
// ownerless-demux path), and it will not act on the fid, so it does not block a
// fid op. A flushed op whose owner still waits (flush(5)) stays LIVE: a reply
// that beats its Rflush is honoured in full. This makes Tflush-then-Tclunk -- the
// standard cancel-then-close pattern -- legal: #294's cancel-at-close abandons
// (Tflush) an outstanding readiness op then IMMEDIATELY clunks its fid, before any
// Rflush has cleared the tag; counting the awaiting_flush entry here would refuse
// the clunk (-> the netd `ready`-fd Tclunk never goes out -> the slot leaks --
// exactly the leak the cancel-at-close was meant to close). The tag itself stays
// reserved until its Rflush (the I-10 reuse guard) -- that is orthogonal to this
// precondition, which is about whether a LIVE op references the fid.
static bool any_outstanding_on_fid(struct p9_session *s, u32 fid) {
    struct p9_outstanding *e;
    for (u32 t = 0; (e = next_active(s, &t)) != NULL; t++) {
        // A Tflush acts on no fid; its entry holds root_fid only as a
        // placeholder, which would refuse a setattr of a raw attach root fd.
        if (e->kind == P9_TFLUSH) continue;
        if (e->awaiting_flush && !e->owner_waits) continue;   // cancelled -> not live
        if (e->abandoned) continue;        // rolled-back abandon (#53-F1)
        if (e->fid == fid) return true;
        if (e->new_fid == fid) return true;
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
    s->n_reserved_slots = 0;
    for (u32 i = 0; i < P9_TAG_CHUNK; i++) entry_zero(&s->tags0.e[i]);
    s->tags0.n_active   = 0;
    s->tag_dir          = NULL;
    s->n_chunks         = 1;
    s->n_active         = 0;
    s->n_flush          = 0;
    s->n_async          = 0;
    s->ops_max          = P9_OPS_MAX;
    s->async_max        = P9_ASYNC_MAX;
    s->tag_limit        = P9_TAG_LIMIT;
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
    s->n_reserved_slots = 0;
    for (u32 i = 0; i < P9_TAG_CHUNK; i++) entry_zero(&s->tags0.e[i]);
    s->tags0.n_active   = 0;
    if (s->tag_dir) {
        for (u32 k = 1; k < s->n_chunks; k++) kfree(s->tag_dir[k]);
        kfree(s->tag_dir);
        s->tag_dir = NULL;
    }
    s->n_chunks         = 1;
    s->n_active         = 0;
    s->n_flush          = 0;
    s->n_async          = 0;
}

int p9_session_close(struct p9_session *s) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    // Refuse close while ops are in flight (spec's CloseSession action
    // requires Inflight = {}).
    if (s->n_active != 0) return -1;
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
    // Tversion uses NOTAG and never enters the tag table: it is out of band
    // relative to the tag pool, and dispatch_rmsg takes Rversion specially
    // (valid only in state INIT).
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
    int t = alloc_tag(s, false);
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
    // RW-4 round-2 (R-B-F1): a full fid table fails the walk CLOSED here
    // (clean -1 -> caller -EIO, no server round-trip, no shared-session
    // death). The walk then reserves the slot new_fid will bind, so no peer
    // can take it while the walk waits for its Rwalk (FID-LIFECYCLE section
    // 9; the old dispatch-time capacity race failed the walk with EIO after
    // the server had bound new_fid).
    if (!slot_available(s)) return -1;
    int t = alloc_tag(s, false);
    if (t < 0) return -1;
    int rc = p9_build_twalk(out, cap, (u16)t,
                            src_fid, new_fid,
                            nwname, names, name_lens);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TWALK, src_fid, new_fid);
    slot_reserve(s, (u16)t);
    return rc;
}

// =============================================================================
// Send: Twalkgetattr (POUNCE, 140).
// =============================================================================

int p9_session_send_walkgetattr(struct p9_session *s,
                                u8 *out, size_t cap,
                                u32 src_fid, u32 new_fid,
                                u64 request_mask,
                                u16 nwname,
                                const u8 *const *names,
                                const size_t *name_lens) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, src_fid)) return -1;
    if (nwname > P9_MAX_WALK) return -1;
    if (new_fid != P9_NOFID) {
        // send_walk's destination gates apply only when a fid will bind;
        // the NOFID query names no destination (nothing binds, nothing
        // to reserve, no capacity to pre-check).
        if (fid_bound(s, new_fid)) return -1;
        if (new_fid == s->root_fid) return -1;
        if (any_outstanding_on_fid(s, new_fid)) return -1;
        if (!slot_available(s)) return -1;
    }
    int t = alloc_tag(s, false);
    if (t < 0) return -1;
    int rc = p9_build_twalkgetattr(out, cap, (u16)t,
                                   src_fid, new_fid, request_mask,
                                   nwname, names, name_lens);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TWALKGETATTR, src_fid, new_fid);
    entry(s, (u32)t)->wga_nwname = nwname;
    if (new_fid != P9_NOFID) slot_reserve(s, (u16)t);
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
    int t = alloc_tag(s, false);
    if (t < 0) return -1;
    int rc = p9_build_tclunk(out, cap, (u16)t, fid);
    if (rc < 0) return -1;
    // Send-time unbind (spec's client discipline: no further ops on
    // this fid even while Tclunk's Rmsg is in flight). The fid's slot stays
    // reserved until the Rclunk, so a take-back can always re-bind it.
    (void)fid_unbind(s, fid);
    mark_outstanding(s, (u16)t, P9_TCLUNK, fid, fid);
    slot_reserve(s, (u16)t);
    return rc;
}

// =============================================================================
// Send: Tflush -- cancel an in-flight request: its owner gone (#845), or still
// waiting for whichever answer comes first (flush(5), owner_waits).
// =============================================================================

int p9_session_send_flush(struct p9_session *s,
                          u8 *out, size_t cap,
                          u16 oldtag) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    // Valid wherever a real-tag op can be outstanding: steady-state ops are
    // OPEN, the handshake's Tattach is VERSIONED. Tversion uses NOTAG and is
    // never in the tag table, so it is never flushable.
    if (s->state != P9_SESS_OPEN && s->state != P9_SESS_VERSIONED) return -1;
    if (!out) return -1;
    struct p9_outstanding *victim = entry(s, oldtag);
    if (!victim || !victim->active) return -1;   // nothing in flight under oldtag
    if (victim->kind == P9_TFLUSH) return -1;    // never flush a flush
    if (victim->awaiting_flush) return -1;       // already being flushed
    int t = alloc_tag(s, true);
    if (t < 0) return -1;                        // no tag: the table cannot grow
    int rc = p9_build_tflush(out, cap, (u16)t, oldtag);
    if (rc < 0) return -1;
    // The flush op is fid-less; root_fid is a placeholder (matches
    // version/attach), which any_outstanding_on_fid skips. alloc_tag skipped
    // the active `oldtag`, so t != oldtag, and a growth moves no entry, so the
    // victim pointer survives. Record oldtag so the Rflush can free it, and
    // reserve oldtag against reuse until that Rflush (9P: oldtag not reusable
    // until Rflush -- the I-10 guard); the victim records the flush's tag, so
    // an unstage finds it without a search.
    mark_outstanding(s, (u16)t, P9_TFLUSH, s->root_fid, s->root_fid);
    entry(s, (u32)t)->flush_oldtag = oldtag;
    victim->awaiting_flush = true;
    victim->flush_tag      = (u16)t;
    return rc;
}

// #52: reclaim the tag of an op whose frame NEVER reached the wire (a
// never-sent abort). The transport send contract is all-or-nothing (zero
// bytes pushed on back-pressure; a genuine break latches the session dead
// elsewhere), so on the never-sent paths -- reply-buffer OOM before the
// send, a self-dying sender refusing to park, a spill-OOM under
// back-pressure -- the server has NEVER seen this tag. Clearing it is
// therefore I-10-safe: no late reply can ever arrive for a request that was
// never sent, so immediate reuse cannot be mis-attributed. Without this,
// each such abort leaks a tag of the op share on a LIVE shared session, and
// enough of them leave every op on the mount waiting for a tag forever.
//
// Fail-soft guards: an inactive tag is a no-op; an awaiting_flush tag is
// owned by the #845 flush protocol (freed only by its Rflush) and is left
// alone -- a never-sent op can never be a flush victim (flush targets
// in-flight ops), so hitting that guard means the caller is wrong and
// leaving the slot is the conservative disposition.
void p9_session_abort_unsent(struct p9_session *s, u16 tag) {
    if (!s) return;
    if (s->magic != P9_SESSION_MAGIC) return;
    struct p9_outstanding *e = entry(s, tag);
    if (!e || !e->active) return;
    if (e->awaiting_flush) return;
    if (e->abandoned) return;   // sent (rolled-back abandon) -- not ours
    clear_outstanding(s, tag);
}

// abort_unsent leaves a never-sent Tclunk's fid unbound, which is right only
// on a dead session: the fid died with it. On a live session the server still
// holds the fid, so its Tclunk must be resubmitted or handed to the closer
// (FID-LIFECYCLE section 9), and a take-back restores the fid too. That is
// also what 9p_client.tla says: it has no step for a send that never
// happened. The re-bind uses the slot the Tclunk kept reserved, so it cannot
// fail on capacity even when the caller dropped the lock to park after the
// build.
int p9_session_retract_unsent(struct p9_session *s, u16 tag) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    struct p9_outstanding *op = entry(s, tag);
    if (!op || !op->active || op->awaiting_flush || op->abandoned) return -1;
    int rc = 0;
    if (op->kind == P9_TCLUNK) rc = slot_bind(s, tag, op->fid);
    clear_outstanding(s, tag);
    return rc;
}

// #53: roll back a Tflush whose frame could NOT be pushed because the c2s
// ring is transiently FULL (P9_TRANSPORT_EAGAIN) -- back-pressure, not a
// break. The DIED-path / abandon-path senders must neither park (a dying
// thread is unwinding; an abandoner holds c->lock in a teardown) nor latch
// the WHOLE shared session dead (the #349 collapse on a different send
// path). Undo exactly what send_flush staged: free the flush op's own tag
// (never sent -> I-10-safe, as above) and clear the victim's
// awaiting_flush, restoring the pre-#845 ownerless reclaim -- the victim
// tag stays ACTIVE until its late original reply is drained ownerlessly by
// a survivor's reader, or session teardown reclaims it (the documented
// no-regression fallback the flush-BUILD-failure path already takes).
//
// The victim names its flush's tag (flush_tag): at most one flush per oldtag
// can exist (send_flush rejects an already-awaiting_flush victim), and a real
// flush entry is (active && kind==TFLUSH && flush_oldtag==oldtag). If the
// victim is not awaiting_flush, or its flush_tag names no such entry, the
// state did not come from send_flush -- leave everything untouched
// (fail-soft).
// #52/#53 R2-F1: the flush-BUILD-failure fallback (tag pool full at the
// abandon instant, or a non-OPEN state) stages NO flush, so flush_rollback
// (which requires awaiting_flush) cannot run -- yet the owner is exactly as
// gone as on the EAGAIN path. Mark the victim abandoned directly so the
// #294 cancel-then-close Tclunk is not refused on this rarer sibling path.
// Fail-soft: only an active, un-flushed, un-abandoned tag is marked; a tag
// with a flush in flight is owned by the flush protocol.
void p9_session_mark_abandoned(struct p9_session *s, u16 tag) {
    if (!s) return;
    if (s->magic != P9_SESSION_MAGIC) return;
    struct p9_outstanding *e = entry(s, tag);
    if (!e || !e->active) return;
    if (e->awaiting_flush) return;
    e->abandoned = true;
}

// Free the never-sent flush that send_flush staged for `oldtag` and clear
// the victim's awaiting_flush. True when the (victim, flush-slot) pair matched.
static bool flush_unstage(struct p9_session *s, u16 oldtag) {
    if (!s) return false;
    if (s->magic != P9_SESSION_MAGIC) return false;
    struct p9_outstanding *victim = entry(s, oldtag);
    if (!victim || !victim->active || !victim->awaiting_flush) return false;
    struct p9_outstanding *fl = entry(s, victim->flush_tag);
    if (!fl || !fl->active || fl->kind != P9_TFLUSH || fl->flush_oldtag != oldtag)
        return false;
    clear_outstanding(s, victim->flush_tag);
    victim->awaiting_flush = false;
    victim->owner_waits    = false;
    victim->flush_tag      = 0;
    return true;
}

void p9_session_flush_rollback(struct p9_session *s, u16 oldtag) {
    // #53-audit F1: the victim's owner is gone and no flush is in flight.
    // Without this bit the victim counts LIVE in any_outstanding_on_fid
    // and the #294 cancel-then-close Tclunk that dev9p_close issues NEXT
    // is refused with no retry -- re-opening the #294 netd slot leak (+
    // tag accumulation on deferred-reply servers) on the exact congestion
    // path #53 targets. The late original reply still frees the tag.
    if (flush_unstage(s, oldtag)) entry(s, oldtag)->abandoned = true;
}

// The owner still waits, so the victim goes back to being an ordinary
// in-flight op: its reply completes it and frees its tag.
void p9_session_flush_retract(struct p9_session *s, u16 oldtag) {
    (void)flush_unstage(s, oldtag);
}

void p9_session_flush_owner_waits(struct p9_session *s, u16 oldtag, bool waits) {
    if (!s) return;
    if (s->magic != P9_SESSION_MAGIC) return;
    struct p9_outstanding *e = entry(s, oldtag);
    if (!e || !e->active) return;
    if (!e->awaiting_flush) return;
    e->owner_waits = waits;
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
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
    int t = alloc_tag(s, false);
    if (t < 0) return -1;
    int rc = p9_build_tunlinkat(out, cap, (u16)t, dfid, name, name_len, flags);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TUNLINKAT, dfid, dfid);
    return rc;
}

// =============================================================================
// Send: Weft extension (Weft-6) -- request the per-flow zero-copy ring.
// =============================================================================

int p9_session_send_weft(struct p9_session *s,
                         u8 *out, size_t cap,
                         u32 fid) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    // Tweft is read-shaped (returns the flow's stable share_id + geometry;
    // idempotent on the netd side, no client-side fid mutation) -- concurrent
    // ops on fid permitted.
    int t = alloc_tag(s, false);
    if (t < 0) return -1;
    int rc = p9_build_tweft(out, cap, (u16)t, fid);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TWEFT, fid, fid);
    return rc;
}

int p9_session_send_weftio(struct p9_session *s,
                           u8 *out, size_t cap,
                           u32 fid, u32 off, u32 len, u32 dir) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (s->state != P9_SESS_OPEN) return -1;
    if (!out) return -1;
    if (!fid_bound(s, fid)) return -1;
    // Tweftio is read/write-shaped (the descriptor is kernel-validated; the
    // server acts on the ring in place + returns a count); no client-side fid
    // mutation -- concurrent ops on fid permitted, the elected reader demuxes
    // by tag.
    int t = alloc_tag(s, false);
    if (t < 0) return -1;
    int rc = p9_build_tweftio(out, cap, (u16)t, fid, off, len, dir);
    if (rc < 0) return -1;
    mark_outstanding(s, (u16)t, P9_TWEFTIO, fid, fid);
    return rc;
}

// =============================================================================
// Receive: dispatch by tag, apply state mutation.
// =============================================================================

static void zero_result(struct p9_dispatch_result *out) {
    out->kind          = 0;
    out->fid           = 0;
    out->new_fid       = 0;
    out->bound_new_fid = P9_NOFID;
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
    out->weft_geom.share_id     = 0;
    out->weft_geom.ring_size    = 0;
    out->weft_geom.ring_entries = 0;
    out->weftio_count           = 0;
}

// Special path for Rversion: tag is NOTAG; not from the tag table;
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

// flush(5): "If a response to the flushed request is received before the
// Rflush, the client must honor the response as if it had not been flushed."
// The only fid state a reply creates is a walk's new fid, so a late successful
// walk binds it -- into the slot the walk reserved -- and reports it in
// out->bound_new_fid for the closer, since its owner is gone
// (FID-LIFECYCLE section 9). The bind rule is the ordinary arms' (a TWALK
// binds on any Rwalk, a TWALKGETATTR only on a full walk). holds_slot doubles
// as "not yet honoured": a duplicate late reply finds it clear and binds
// nothing. The tag itself stays reserved until the Rflush, as before.
static void honour_late_walk(struct p9_session *s, struct p9_outstanding *op,
                             u16 tag, u8 type, const u8 *rmsg, size_t len,
                             struct p9_dispatch_result *out) {
    if (!op->holds_slot) return;
    if (type != (u8)(op->kind + 1)) return;          // an Rlerror binds nothing
    u16 tag_check;
    u16 nwqid;
    if (op->kind == P9_TWALK) {
        if (p9_parse_rwalk(rmsg, len, &tag_check, &nwqid,
                           out->qids, P9_MAX_WALK) < 0) return;
    } else if (op->kind == P9_TWALKGETATTR) {
        const u8 *body = NULL;
        if (p9_parse_rwalkgetattr(rmsg, len, &tag_check, &nwqid,
                                  out->qids, P9_MAX_WALK, &body) < 0) return;
        if (nwqid != op->wga_nwname) return;
    } else {
        return;
    }
    if (tag_check != tag) return;
    if (slot_bind(s, tag, op->new_fid) == 0) out->bound_new_fid = op->new_fid;
}

// Apply a reply to the op it answers: check its type, parse it, make its
// state mutation and fill *out. Whether the tag is freed is the caller's.
static int apply_rmsg(struct p9_session *s, struct p9_outstanding *op,
                      u16 tag, u8 type, const u8 *rmsg, size_t len,
                      struct p9_dispatch_result *out) {
    int rc;
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
        if (slot_bind(s, tag, op->new_fid) < 0) {
            // The bind goes into the slot send_walk reserved, so capacity
            // cannot refuse it; only a redundant already-bound fid can, a LOCAL
            // condition -- the server's Rwalk is conformant. Surface it as a
            // per-op error (a synthetic Rlerror) so this op completes with -EIO
            // + the common tail clears the tag, WITHOUT the -1 that R3-F1's
            // mark_dead-on-drc<0 reads as a protocol violation (round-2 R-B-F1:
            // a local fid-table condition must NOT latch the shared root-FS
            // session dead).
            out->is_error = true;
            out->ecode    = T_E_IO;   // EIO (POSIX-aligned 9P2000.L wire errno)
        } else {
            out->nwqid         = nwqid;
            out->bound_new_fid = op->new_fid;
        }
    } else if (op->kind == P9_TWALKGETATTR) {
        u16 tag_check;
        u16 nwqid;
        const u8 *body = NULL;
        rc = p9_parse_rwalkgetattr(rmsg, len, &tag_check, &nwqid,
                                   out->qids, P9_MAX_WALK, &body);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // Bind new_fid ONLY on a FULL walk with a real destination -- the
        // correct partial-walk semantics (a partial Rwalkgetattr binds
        // nothing server-side; the NOFID query never names a destination).
        // This is the nuance the TWALK arm defers (its callers send 0/1
        // names, where partial cannot exist); the multi-name pounce
        // requires it.
        if (op->new_fid != P9_NOFID && nwqid == op->wga_nwname) {
            if (slot_bind(s, tag, op->new_fid) < 0) {
                // Same LOCAL-failure posture as the TWALK arm: a redundant
                // bind completes THIS op with -EIO; it must not latch the
                // shared session dead.
                out->is_error = true;
                out->ecode    = T_E_IO;
            } else {
                out->nwqid         = nwqid;
                out->wga_data      = body;
                out->bound_new_fid = op->new_fid;
            }
        } else {
            out->nwqid    = nwqid;
            out->wga_data = body;
        }
    } else if (op->kind == P9_TCLUNK) {
        u16 tag_check;
        rc = p9_parse_rclunk(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // Send-time already unbound; the slot it kept is released with the
        // tag below.
    } else if (op->kind == P9_TFLUSH) {
        // Rflush (#845): the server guarantees it will not answer the
        // abandoned oldtag, so this Rflush is the SOLE authority that frees it
        // (the I-10 reuse-race guard). Clear the reserved original; the common
        // tail clears this flush's own tag. The guard is maximal: `awaiting_flush`
        // is set only on a reserved NORMAL op (a flush entry never carries it),
        // so this can never clear an unrelated in-flight flush, and the bounds +
        // active checks reject a malformed flush_oldtag. The one residual is a
        // NON-CONFORMANT server that sends a DUPLICATE Rflush after this flush
        // tag was freed + reused for a new flush: the duplicate is
        // indistinguishable on the wire (9P carries no per-tag generation) and
        // would free the new flush's reserved oldtag. That is the generic
        // "server sends exactly one reply per tag" assumption the whole client
        // rests on -- a duplicate same-type reply mis-attributes for ANY op kind
        // -- so it adds no class of hazard beyond what already exists; it does
        // not arise with the v1.0 trusted servers (stratumd / kernel dev9p), and
        // closing it for an untrusted/remote 9P server needs wire-level tag
        // generations (a v1.x ABI lift, the same seam as the n_uname trust-stamp).
        u16 tag_check;
        rc = p9_parse_rflush(rmsg, len, &tag_check);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        u16 oldtag = op->flush_oldtag;
        struct p9_outstanding *victim = entry(s, oldtag);
        if (victim && victim->active && victim->awaiting_flush) {
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
    } else if (op->kind == P9_TWEFT) {
        u16 tag_check;
        rc = p9_parse_rweft(rmsg, len, &tag_check, &out->weft_geom);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // No fid table mutation: the flow's data fid stays bound; Tweft
        // queries the per-flow ring's stable share_id + geometry.
    } else if (op->kind == P9_TWEFTIO) {
        u16 tag_check;
        rc = p9_parse_rweftio(rmsg, len, &tag_check, &out->weftio_count);
        if (rc < 0) return -1;
        if (tag_check != tag) return -1;
        // No fid table mutation: the flow's data fid stays bound; Tweftio
        // moves payload through the per-flow ring + returns the byte count.
    } else {
        // Unknown / unsupported kind.
        return -1;
    }

    // Echo back what we completed.
    out->kind    = op->kind;
    out->fid     = op->fid;
    out->new_fid = op->new_fid;
    out->op_id   = op->op_id;
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

    // Rversion is the only Rmsg that lives outside the tag table
    // bookkeeping (it uses NOTAG). Dispatch it specially.
    if (type == P9_RVERSION) {
        return dispatch_rversion(s, rmsg, len, out);
    }

    // For every other Rmsg, the tag must index a live outstanding entry.
    struct p9_outstanding *op = entry(s, tag);
    if (!op || !op->active) return -1;

    // A reply for a tag reserved by a pending Tflush (#845) is a LATE reply
    // for an abandoned op (its owner Proc died). Consume it WITHOUT freeing
    // the tag: per 9P, oldtag is reusable only after the Rflush, so freeing
    // here would let the tag be reused while a stray duplicate / twin reply
    // is still possible -> a future reply mis-attributed to the reused tag
    // (the I-10 violation the naive fix introduces). The flush's Rflush
    // (dispatched via its own tag, below) is the SOLE authority that frees an
    // awaiting_flush tag. The one fid mutation it makes is a walk's new fid,
    // which flush(5) says the client must honour (honour_late_walk).
    if (op->awaiting_flush) {
        // Absorb (do not complete): the tag stays reserved. Only the ownerless
        // demux path reaches an awaiting_flush tag here: an owner that still
        // waits on its flush has its reply applied by
        // p9_session_dispatch_flushed_rmsg instead. The ownerless caller reads
        // only out->bound_new_fid, which honour_late_walk sets when it binds.
        // A 0 return here means "ownerless late reply absorbed", NOT "op
        // completed"; no clear_outstanding.
        honour_late_walk(s, op, tag, type, rmsg, len, out);
        return 0;
    }

    if (apply_rmsg(s, op, tag, type, rmsg, len, out) < 0) return -1;
    clear_outstanding(s, tag);
    return 0;
}

int p9_session_dispatch_flushed_rmsg(struct p9_session *s,
                                     const u8 *rmsg, size_t len,
                                     struct p9_dispatch_result *out) {
    if (!s) return -1;
    if (s->magic != P9_SESSION_MAGIC) return -1;
    if (!out) return -1;
    if (!rmsg) return -1;
    zero_result(out);

    u32 size; u8 type; u16 tag;
    if (p9_peek_header(rmsg, len, &size, &type, &tag) < 0) return -1;
    struct p9_outstanding *op = entry(s, tag);
    if (!op || !op->active || !op->awaiting_flush) return -1;
    // No clear_outstanding: the tag stays reserved until the flush's Rflush
    // (the I-10 guard). The walk arms' slot_bind releases the slot, so a
    // duplicate reply reaching the ownerless arm above binds nothing. The op
    // has acted, so its fid is free for the owner's next op.
    int rc = apply_rmsg(s, op, tag, type, rmsg, len, out);
    op->owner_waits = false;
    return rc;
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
    return s->n_active;
}

// The client asks before a build, so a sync op waits for a tag (ARCH 21.11
// part 3) and the async clunk drains one instead of leaking its fid. The
// growth an alloc_tag here makes is the one the build's alloc_tag would make:
// the chunk stays, and the build under the same lock hold finds the tag.
bool p9_session_has_free_tag(struct p9_session *s) {
    if (!s) return false;
    if (s->magic != P9_SESSION_MAGIC) return false;
    return alloc_tag(s, false) >= 0;
}

bool p9_session_has_flush_tag(struct p9_session *s) {
    if (!s) return false;
    if (s->magic != P9_SESSION_MAGIC) return false;
    return alloc_tag(s, true) >= 0;
}

bool p9_session_async_room(const struct p9_session *s) {
    if (!s) return false;
    if (s->magic != P9_SESSION_MAGIC) return false;
    return s->n_async < s->async_max;
}

struct p9_outstanding *p9_session_entry(struct p9_session *s, u32 tag) {
    if (!s) return NULL;
    if (s->magic != P9_SESSION_MAGIC) return NULL;
    return entry(s, tag);
}

struct p9_outstanding *p9_session_next_active(struct p9_session *s, u32 *tag) {
    if (!s || !tag) return NULL;
    if (s->magic != P9_SESSION_MAGIC) return NULL;
    return next_active(s, tag);
}

void *p9_session_owner(struct p9_session *s, u32 tag) {
    struct p9_outstanding *e = p9_session_entry(s, tag);
    return (e && e->active) ? e->owner : NULL;
}

void p9_session_set_owner(struct p9_session *s, u32 tag, void *owner) {
    struct p9_outstanding *e = p9_session_entry(s, tag);
    if (e && e->active) e->owner = owner;
}

void p9_session_mark_async(struct p9_session *s, u16 tag) {
    struct p9_outstanding *e = p9_session_entry(s, tag);
    if (!e || !e->active || e->async || e->kind == P9_TFLUSH) return;
    e->async = true;
    s->n_async++;
}

size_t p9_session_n_bound_fids(const struct p9_session *s) {
    if (!s) return 0;
    if (s->magic != P9_SESSION_MAGIC) return 0;
    return s->n_bound_fids;
}

size_t p9_session_n_reserved_slots(const struct p9_session *s) {
    if (!s) return 0;
    if (s->magic != P9_SESSION_MAGIC) return 0;
    return s->n_reserved_slots;
}
