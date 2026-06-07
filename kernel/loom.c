// Loom -- the KObj_Loom ring substrate (Loom-2a).
//
// Per kernel/include/thylacine/loom.h + docs/LOOM.md. This file owns the ring
// memory + geometry + the registered-handle table + the kobj refcount. The
// syscall inners that map the ring into a Proc + install the handle live in
// kernel/syscall.c (the sys_burrow_attach_for_proc factoring); the engine
// pluggable-completion seam + the SQE dispatch + the CQE post are Loom-2b /
// Loom-3.
//
// The ring lives in one anonymous Burrow: a 64-byte loom_ring_hdr at offset 0,
// then the SQ index ring (u32[sq_entries]), the SQE array, and the CQE array,
// each 64-aligned, the whole page-rounded. The kernel reaches the bytes via the
// Burrow's direct-map alias (exec.c precedent); userspace sees the same pages
// through its mapping. The dual-refcount discipline (#847) keeps the pages
// alive while EITHER the Loom (handle_count) OR the user mapping (mapping_count)
// holds a reference.

#include <thylacine/loom.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/burrow.h>
#include <thylacine/dev9p.h>
#include <thylacine/errno.h>
#include <thylacine/handle.h>
#include <thylacine/page.h>
#include <thylacine/poll.h>
#include <thylacine/rendez.h>
#include <thylacine/spoor.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

// =============================================================================
// One in-flight async (Loom) op. Heap-allocated per dispatched SQE; owned by the
// Loom (linked into l->inflight_ops). `rpc` is at OFFSET 0 so the engine's
// completion callback recovers the container with a single cast from the
// struct p9_rpc * it is handed (the same offset-0 idiom the seam tests use).
//
// Lifetime (Loom-3): the op holds an INDEPENDENT spoor_ref on `pinned` -- the
// I-30 submit-time pin, held for the op's whole lifetime so a concurrent
// re-register / clunk of the registered-handle slot cannot make completion act
// against a different object (the io_uring credential-vs-work CVE class). The op
// does NOT hold a loom ref: the loom outlives all its ops via the handle ref
// during SYS_LOOM_ENTER + the loom_free quiesce (which abandons any still in
// flight before freeing -- #898). `client` is borrowed (valid while `pinned` is
// held: a live dev9p Spoor implies a live client). `terminal` is set by
// loom_async_complete under l->lock and read by the reap sweep under l->lock.
// =============================================================================
struct loom_async_op {
    struct p9_rpc          rpc;        // OFFSET 0 -- on_complete casts (loom_async_op *)rpc
    struct Loom           *loom;       // owning ring (no ref held; see lifetime note)
    struct p9_client      *client;     // engine this op rides (valid while `pinned` held)
    struct Spoor          *pinned;     // the I-30 submit-time pin (independent spoor_ref)
    u64                    user_data;  // echoed verbatim into the CQE
    u32                    op_fid;      // resolved 9P fid (from `pinned`, at submit)
    u32                    op_arg;      // opcode-specific scalar (FSYNC: datasync 0/1)
    u8                     opcode;      // LOOM_OP_*
    bool                   terminal;    // CQE posted (set under l->lock by completion)
    struct loom_async_op  *next;        // l->inflight_ops chain (under l->lock)
};

_Static_assert(__builtin_offsetof(struct loom_async_op, rpc) == 0,
               "p9_rpc must be at offset 0 -- loom_async_complete recovers the "
               "container by casting the struct p9_rpc * the engine hands it");

_Static_assert(LOOM_MAGIC == 0x4C4F4F4D52494E47ULL, "loom magic drift");
_Static_assert((LOOM_MAX_ENTRIES & (LOOM_MAX_ENTRIES - 1u)) == 0,
               "LOOM_MAX_ENTRIES must be a power of two");

// Cumulative diagnostics (tests assert on the deltas). Atomic so the SMP
// matrix's concurrent create/destroy don't tear the counts.
static u64 g_loom_created;
static u64 g_loom_destroyed;

u64 loom_total_created(void)   { return __atomic_load_n(&g_loom_created, __ATOMIC_RELAXED); }
u64 loom_total_destroyed(void) { return __atomic_load_n(&g_loom_destroyed, __ATOMIC_RELAXED); }

static u32 align_up_u32(u32 x, u32 a) { return (x + (a - 1u)) & ~(a - 1u); }

static bool is_pow2_u32(u32 x) { return x != 0u && (x & (x - 1u)) == 0u; }

struct Loom *loom_create(u32 sq_entries, u32 cq_entries) {
    if (!is_pow2_u32(sq_entries) || sq_entries > LOOM_MAX_ENTRIES)  return NULL;
    if (!is_pow2_u32(cq_entries))                                   return NULL;
    if (cq_entries < sq_entries || cq_entries > 2u * LOOM_MAX_ENTRIES) return NULL;

    // Geometry. Each region 64-aligned (cache line); the whole ring
    // page-rounded. All sizes are bounded (sq/cq <= 2*LOOM_MAX_ENTRIES, each
    // entry <= 64 B) so the u32 arithmetic cannot overflow: the largest ring is
    // ~ 16 KiB (sq index) + 256 KiB (sqes) + 256 KiB (cqes) < 1 MiB.
    u32 hdr_off       = 0;
    u32 sq_array_off  = align_up_u32(hdr_off + (u32)sizeof(struct loom_ring_hdr), 64u);
    u32 sq_array_size = sq_entries * (u32)sizeof(u32);
    u32 sqe_off       = align_up_u32(sq_array_off + sq_array_size, 64u);
    u32 sqe_size      = sq_entries * (u32)sizeof(struct loom_sqe);
    u32 cqe_off       = align_up_u32(sqe_off + sqe_size, 64u);
    u32 cqe_size      = cq_entries * (u32)sizeof(struct loom_cqe);
    u32 ring_end      = cqe_off + cqe_size;
    u32 ring_size     = (ring_end + (PAGE_SIZE - 1u)) & ~((u32)PAGE_SIZE - 1u);

    struct Loom *l = kmalloc(sizeof(struct Loom), KP_ZERO);
    if (!l) return NULL;
    struct Burrow *r = burrow_create_anon((size_t)ring_size);
    if (!r) { kfree(l); return NULL; }

    l->magic    = LOOM_MAGIC;
    l->refcount = 1;
    spin_lock_init(&l->lock);
    poll_waiter_list_init(&l->cq_waiters);   // Loom-4 CQ wait-list (its own lock)
    l->ring     = r;
    // The ring Burrow is anonymous + physically contiguous (alloc_pages chunk);
    // its direct-map base is stable for the Burrow's lifetime (the Loom holds a
    // handle_count ref so the pages are never freed under us). exec.c sets the
    // precedent for kernel writes through a Burrow's direct-map alias.
    l->ring_kva = (u8 *)pa_to_kva(page_to_pa(r->pages));
    l->sq_entries    = sq_entries;
    l->cq_entries    = cq_entries;
    l->hdr_off       = hdr_off;
    l->sq_array_off  = sq_array_off;
    l->sqe_off       = sqe_off;
    l->cqe_off       = cqe_off;
    l->sq_array_size = sq_array_size;
    l->sqe_size      = sqe_size;
    l->cqe_size      = cqe_size;
    l->ring_size     = ring_size;
    l->cq_tail       = 0;   // kernel-private authoritative CQ tail (the shared mirror starts 0 too)

    // Stamp the immutable geometry into the shared ring header. The Burrow pages
    // are KP_ZERO, so the head/tail/flags/diagnostics start at 0; only the masks
    // + entry counts are written here. A dsb ish publishes the stores to the
    // inner-shareable domain so a secondary CPU that maps + reads the ring sees
    // them (burrow_create_anon already dsb'd its own zeroing).
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + hdr_off);
    h->sq_mask    = sq_entries - 1u;
    h->sq_entries = sq_entries;
    h->cq_mask    = cq_entries - 1u;
    h->cq_entries = cq_entries;
    __asm__ __volatile__("dsb ish" ::: "memory");

    __atomic_fetch_add(&g_loom_created, 1, __ATOMIC_RELAXED);
    return l;
}

// Last-ref teardown. No concurrent access (refcount hit 0), so no lock is
// taken. Clunking a registered Spoor may sleep (its Dev close hook) -- safe
// here because handle_release_obj runs OUTSIDE the handle-table lock (#844) and
// we hold none. burrow_unref drops the ring's handle_count; if the user mapping
// is already gone (mapping_count == 0) the pages free here, else the VMA
// teardown frees them later (dual-refcount, #847).
static void loom_free(struct Loom *l) {
    // #898: quiesce any in-flight async op BEFORE freeing the ring. refcount hit
    // 0 -> no SYS_LOOM_ENTER is in progress (an enter holds a loom ref via
    // handle_get), so the inflight_ops list is not being mutated here (only
    // submit/reap mutate it, both inside an enter). on_complete (from a demux /
    // mark_dead on a SHARED client, driven by another Proc) does NOT touch the
    // list -- it only sets terminal + posts a CQE under l->lock. So reading +
    // clearing the list lock-free is safe.
    //
    // For each op: p9_client_abandon_async runs UNDER the client's c->lock, so it
    // is mutually exclusive with that demux/mark_dead. If the reply has not been
    // demuxed it clears inflight[tag] (no future on_complete can fire) + Tflushes
    // (#845; a late original reply is discarded ownerless). If it already
    // completed, the abandon is a no-op. A racing demux that wins c->lock first
    // posts at-most-one CQE into the still-allocated ring (we free it only AFTER
    // this loop), then this abandon sees the slot cleared -- no double, no stale,
    // no UAF (the loom is alive throughout; I-29). The pin is released + the
    // container freed only after abandon severs the engine link.
    //
    // F3: snapshot + clear the list under l->lock. refcount is 0 so this Loom's
    // lock is uncontended for any SUBMIT/REAP (those only run inside a live enter,
    // which holds a ref), but the lock SERIALIZES against a concurrent
    // loom_async_complete on a SHARED client (which mutates async_inflight + posts
    // a CQE under l->lock) -- removing the data race the lock-free clear would
    // otherwise have on the shared-client path (it was UAF-safe but TSan-dirty).
    spin_lock(&l->lock);
    struct loom_async_op *op = l->inflight_ops;
    l->inflight_ops = NULL;
    l->async_inflight = 0;
    spin_unlock(&l->lock);
    while (op) {
        struct loom_async_op *next = op->next;
        p9_client_abandon_async(op->client, &op->rpc);
        spoor_clunk(op->pinned);
        kfree(op);
        op = next;
    }

    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++) {
        if (l->reg[i].spoor) {
            spoor_clunk(l->reg[i].spoor);
            l->reg[i].spoor = NULL;
        }
    }
    if (l->ring) {
        burrow_unref(l->ring);
        l->ring = NULL;
    }
    l->magic = 0;   // clobber before free (UAF defense, mirrors burrow_free_internal)
    kfree(l);
    __atomic_fetch_add(&g_loom_destroyed, 1, __ATOMIC_RELAXED);
}

void loom_ref(struct Loom *l) {
    if (!l || l->magic != LOOM_MAGIC) return;
    __atomic_fetch_add(&l->refcount, 1, __ATOMIC_ACQUIRE);
}

void loom_unref(struct Loom *l) {
    if (!l || l->magic != LOOM_MAGIC) return;
    if (__atomic_fetch_sub(&l->refcount, 1, __ATOMIC_RELEASE) == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        loom_free(l);
    }
}

int loom_register_handles(struct Loom *l, struct Spoor **spoors,
                          const rights_t *rights, u32 n) {
    if (!l || l->magic != LOOM_MAGIC)  return -1;
    if (n > LOOM_MAX_REG_HANDLES)      return -1;
    if (n > 0 && (!spoors || !rights)) return -1;

    // Replace the whole table (IORING_REGISTER_FILES semantics). Snapshot the
    // old Spoors + install the new under the lock, then clunk the old OUTSIDE
    // the lock (spoor_clunk may sleep -- it cannot run under the spin_lock).
    struct Spoor *old[LOOM_MAX_REG_HANDLES];
    spin_lock(&l->lock);
    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++) {
        old[i] = l->reg[i].spoor;
        l->reg[i].spoor  = NULL;
        l->reg[i].rights = 0;
    }
    for (u32 i = 0; i < n; i++) {
        l->reg[i].spoor  = spoors[i];   // adopt the caller's ref
        l->reg[i].rights = rights[i];
    }
    spin_unlock(&l->lock);

    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++) {
        if (old[i]) spoor_clunk(old[i]);
    }
    return 0;
}

int loom_post_cqe(struct Loom *l, u64 user_data, s32 result, u32 flags) {
    if (!l || l->magic != LOOM_MAGIC) return -1;

    spin_lock(&l->lock);
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    // The write position comes ONLY from kernel-private state -- never from the
    // shared header, which userspace can corrupt. Reading `h->cq_tail` / `h->cq_mask`
    // (both userspace-writable) to compute the index would let a hostile Proc
    // (under Loom-3, when the ring is userspace-controlled) drive an out-of-bounds
    // kernel write: e.g. cq_mask = 0xFFFFFFFF + cq_tail = 0x10000 -> idx far past
    // the cqe array. cq_entries is a power of two (loom_create), so its mask is
    // exact. (The ring-TOCTOU discipline, docs/LOOM.md §6.1, applied to the CQ.)
    u32 tail = l->cq_tail;                                       // kernel-private
    u32 head = __atomic_load_n(&h->cq_head, __ATOMIC_ACQUIRE);   // user-owned (advisory)

    // (tail - head) is the count of posted-unreaped CQEs (free-running u32 --
    // the subtraction is correct across wrap). A hostile `head` can only make
    // userspace overwrite its OWN claimed-reaped CQE (the ring is the Proc's own
    // Burrow) -- the masked index below is always in-bounds, so never an OOB.
    if ((u32)(tail - head) >= l->cq_entries) {
        // CQ full: never overwrite an unreaped CQE (CqNeverOverfull, I-29).
        // Count the dropped post; Loom-3's submit-time admission makes this
        // unreachable in production (then `overflow` is a pure diagnostic).
        __atomic_store_n(&h->overflow, h->overflow + 1u, __ATOMIC_RELEASE);
        spin_unlock(&l->lock);
        return -1;
    }

    struct loom_cqe *cqe = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    u32 idx = tail & (l->cq_entries - 1u);   // kernel-private mask -- NEVER h->cq_mask
    cqe[idx].user_data = user_data;
    cqe[idx].result    = result;
    cqe[idx].flags     = flags;
    // Advance the private tail, then publish it to the shared mirror with a
    // release store (so a user-side load-acquire of cq_tail sees a fully-written
    // slot). The mirror also overwrites any hostile value userspace wrote.
    l->cq_tail = tail + 1u;
    __atomic_store_n(&h->cq_tail, l->cq_tail, __ATOMIC_RELEASE);
    spin_unlock(&l->lock);

    // Loom-4 (specs/loom.tla PostCqe-wake): the CQ is now non-empty -- wake any
    // SYS_LOOM_ENTER thread sleeping for min_complete on the CQ wait-list. Wake
    // AFTER releasing l->lock (the pipe.c producer precedent): the cq_tail bump
    // above happened-before this under l->lock, so a concurrent CqWaitRegister's
    // sample (also under l->lock) either observes the bumped CQ (and does not
    // sleep) or is found by this list walk (and is woken) -- no missed wake
    // (CqFlagTracksCq + NoMissedCqWake, I-9). poll_waiter_list_wake takes only the
    // list + rendez locks (below l->lock in the global order) and does NOT sleep
    // or re-enter p9_client_*, so it is safe under the c->lock loom_async_complete
    // holds (the seam contract).
    poll_waiter_list_wake(&l->cq_waiters);
    return 0;
}

// =============================================================================
// Loom-3: SQE dispatch + the submit-time pin (I-30) + SYS_LOOM_ENTER.
// =============================================================================

// Map an engine completion to the CQE result for a scalar-result op (NOP /
// FSYNC). `status` from the engine is already 0 (success) or -errno (Rlerror /
// transport). Payload opcodes (Loom-6) will read dr->read_count etc. when
// status == 0; for the scalar ops dr is unused.
static s32 loom_scalar_result(int status) {
    if (status > 0) return 0;          // defensive: scalar ops carry no positive payload
    return (s32)status;
}

// The async completion callback (the POST_CQE front-end). Runs UNDER the 9P
// client's c->lock (invoked from demux_frame_locked / client_mark_dead_locked),
// so it MUST NOT sleep or re-enter the p9_client_* API (the seam contract in
// 9p_client.h). It posts exactly one CQE (loom_post_cqe -- a leaf lock, the
// c->lock -> l->lock order) and marks the op terminal + drops the in-flight
// count under l->lock. It does NOT free the container or clunk the pin (both may
// sleep); the reap sweep (loom_reap_terminal, run outside any lock from the
// SYS_LOOM_ENTER caller) does that.
static void loom_async_complete(struct p9_rpc *rpc, int status,
                                struct p9_dispatch_result *dr) {
    (void)dr;
    struct loom_async_op *op = (struct loom_async_op *)rpc;   // rpc at offset 0
    (void)loom_post_cqe(op->loom, op->user_data, loom_scalar_result(status), 0);
    // Mark terminal + decrement the in-flight count under l->lock so the reap
    // sweep (also under l->lock) observes a consistent (terminal, count) pair.
    // The CQE was already posted above, so a reaper that sees terminal will not
    // lose the completion.
    spin_lock(&op->loom->lock);
    op->terminal = true;
    if (op->loom->async_inflight > 0) op->loom->async_inflight--;
    spin_unlock(&op->loom->lock);
}

// Tmsg builder thunk for LOOM_OP_FSYNC. Invoked by p9_client_submit_async under
// the client's c->lock; allocates the 9P tag + marks outstanding. `ctx` is the
// loom_async_op (it carries the resolved fid + datasync).
static int loom_build_fsync(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_fsync(s, out, cap, op->op_fid, op->op_arg);
}

// Post a terminal CQE inline (no engine round-trip): the NOP success (result 0)
// and every submit-time-gate failure (bad opcode / handle / rights / OOM ->
// -errno). The SQE was consumed; userspace learns the result via the CQE, not a
// dropped SQE (io_uring semantics).
static void loom_complete_inline(struct Loom *l, u64 user_data, s32 result) {
    (void)loom_post_cqe(l, user_data, result, 0);
}

// Dispatch ONE already-copied-to-kernel SQE (`sqe` is a kernel-private snapshot,
// NEVER the shared ring slot -- ring TOCTOU, LOOM.md 6.1). Either completes
// inline (NOP / a rejected op posts a CQE) or submits an async 9P op whose reply
// drives loom_async_complete later. Never re-reads the shared ring.
static void loom_submit_one(struct Loom *l, const struct loom_sqe *sqe) {
    u64 ud = sqe->user_data;

    // Reserved SQE flags (LINK / DRAIN / CQE_SKIP / MULTISHOT) land at Loom-5;
    // reject any set flag now so a future flag is never silently ignored.
    if (sqe->flags != 0) { loom_complete_inline(l, ud, -(s32)T_E_INVAL); return; }

    switch (sqe->opcode) {
    case LOOM_OP_NOP:
        // The io_uring smoke op: completes immediately, no engine, no handle.
        loom_complete_inline(l, ud, 0);
        return;

    case LOOM_OP_FSYNC: {
        if (sqe->handle_idx >= LOOM_MAX_REG_HANDLES) {
            loom_complete_inline(l, ud, -(s32)T_E_INVAL);
            return;
        }
        // Resolve + PIN the registered handle under l->lock so a concurrent
        // LOOM_REGISTER_HANDLES re-install cannot free the Spoor between the read
        // and the ref. The independent spoor_ref is the I-30 pin (held for the
        // op's lifetime); `rights` is the snapshot the submit-time gate reads.
        struct Spoor *sp; rights_t rt;
        spin_lock(&l->lock);
        sp = l->reg[sqe->handle_idx].spoor;
        rt = l->reg[sqe->handle_idx].rights;
        if (sp) spoor_ref(sp);
        spin_unlock(&l->lock);

        if (!sp) { loom_complete_inline(l, ud, -(s32)T_E_BADF); return; }   // empty slot
        // Submit-time rights gate (I-2 / I-6; the spec's reg in AllowedObjs).
        // Fsync is a durability barrier on written data, so it requires the
        // handle to have been opened for write (RIGHT_WRITE, omode-derived at
        // open -- A-3 F1). A read-only registered handle is denied here, never
        // re-checked at completion (I-30).
        if (!(rt & RIGHT_WRITE)) {
            spoor_clunk(sp);
            loom_complete_inline(l, ud, -(s32)T_E_ACCES);
            return;
        }
        struct p9_client *cl; u32 fid;
        if (dev9p_client_fid(sp, &cl, &fid) != 0) {
            // Not a dev9p-backed Spoor (devsrv conn / devramfs / ...): no 9P
            // engine to dispatch against.
            spoor_clunk(sp);
            loom_complete_inline(l, ud, -(s32)T_E_INVAL);
            return;
        }
        struct loom_async_op *op = kmalloc(sizeof(*op), KP_ZERO);
        if (!op) { spoor_clunk(sp); loom_complete_inline(l, ud, -(s32)T_E_NOMEM); return; }
        op->loom        = l;
        op->client      = cl;
        op->pinned      = sp;               // adopt the pin ref
        op->user_data   = ud;
        op->op_fid      = fid;
        op->op_arg      = (sqe->len != 0) ? 1u : 0u;   // FSYNC: len != 0 -> datasync
        op->opcode      = LOOM_OP_FSYNC;
        op->terminal    = false;
        op->rpc.on_complete = loom_async_complete;
        // Link + count before submitting: a synchronous submit failure fires
        // loom_async_complete (which decrements the count + marks terminal), so
        // the op must already be counted + linked for that to be consistent. The
        // reap sweep then reclaims it.
        spin_lock(&l->lock);
        op->next = l->inflight_ops;
        l->inflight_ops = op;
        l->async_inflight++;
        spin_unlock(&l->lock);
        // Hands ownership of &op->rpc to the engine: exactly one on_complete will
        // fire (now, on failure, or later at demux). No further touch of `op`
        // here -- a fast failure may already have marked it terminal.
        (void)p9_client_submit_async(cl, &op->rpc, loom_build_fsync, op);
        return;
    }

    default:
        // In-range payload opcodes (READ / WRITE / GETATTR / WALK / ...) need a
        // registered-buffer destination/source -- that surface is Loom-6
        // (LOOM.md 10), so they post -ENOSYS until then. WIRE_PASSTHROUGH stays
        // reserved (LOOM.md 7). Out-of-range opcodes are -EINVAL.
        if (sqe->opcode < LOOM_OP_COUNT)
            loom_complete_inline(l, ud, -(s32)T_E_NOSYS);
        else
            loom_complete_inline(l, ud, -(s32)T_E_INVAL);
        return;
    }
}

// Count of posted-unreaped CQEs (kernel-private cq_tail minus the user-owned
// cq_head). The kernel-private cq_tail is authoritative (the SA-1 discipline);
// a hostile cq_head can only make this READ smaller or larger, never drive an
// OOB -- it gates the SYS_LOOM_ENTER wait only, so a corrupt value just makes
// the caller wait wrong (it hurts only itself). Clamped to cq_entries.
static u32 loom_cq_ready(struct Loom *l) {
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    u32 tail = l->cq_tail;                                       // kernel-private
    u32 head = __atomic_load_n(&h->cq_head, __ATOMIC_ACQUIRE);   // user-owned
    u32 d = tail - head;
    return (d > l->cq_entries) ? l->cq_entries : d;
}

// The client of the first still-in-flight (non-terminal) async op, or NULL when
// none remain (no further completion can arrive). The returned client is valid
// to pump because the op holds a pin on a dev9p Spoor whose client this is, and
// the op is not reclaimed until loom_reap_terminal (same SYS_LOOM_ENTER caller,
// after the wait). Under l->lock.
static struct p9_client *loom_first_inflight_client(struct Loom *l) {
    struct p9_client *cl = NULL;
    spin_lock(&l->lock);
    for (struct loom_async_op *op = l->inflight_ops; op; op = op->next) {
        if (!op->terminal) { cl = op->client; break; }
    }
    spin_unlock(&l->lock);
    return cl;
}

// Reclaim terminal async ops: unlink each under l->lock, then release the pin +
// free the container OUTSIDE the lock (spoor_clunk may sleep). Idempotent; safe
// to call when nothing is terminal.
static void loom_reap_terminal(struct Loom *l) {
    struct loom_async_op *done = NULL;
    spin_lock(&l->lock);
    struct loom_async_op **pp = &l->inflight_ops;
    while (*pp) {
        struct loom_async_op *op = *pp;
        if (op->terminal) {
            *pp = op->next;          // unlink
            op->next = done;
            done = op;
        } else {
            pp = &op->next;
        }
    }
    spin_unlock(&l->lock);
    while (done) {
        struct loom_async_op *next = done->next;
        spoor_clunk(done->pinned);   // release the I-30 pin (may sleep)
        kfree(done);
        done = next;
    }
}

// Loom-4 CQ-waiter sleep predicate (specs/loom.tla CqWaitCommitOrSleep). Returns
// 1 once the waiter's wake flag is set -- a CQE was posted (or the session died)
// while this hook was installed on l->cq_waiters. Reads ONLY pw->ready, which
// poll_waiter_list_wake sets under the wait-list lock and follows with
// wakeup(pw->rendez); since this cond runs under that SAME rendez's lock (sleep's
// discipline), the flag write is visible and no wake is lost between the flag set
// and the sleep transition (I-9; poll.c::poll_cond_any_flagged for one waiter).
// Reading the FLAG (not the live cq_tail, which lives behind l->lock) is the
// cross-lock hand-off the spec pins -- and it cannot take l->lock here (the rendez
// lock is BELOW l->lock; doing so would invert the order).
static int loom_cqw_cond(void *arg) {
    const struct poll_waiter *pw = (const struct poll_waiter *)arg;
    return pw->ready ? 1 : 0;
}

// Loom-4 wait/reap phase. Block until min_complete CQEs are available, no async
// op remains in flight, or the caller's Proc is dying. The caller either DRIVES
// the elected reader itself (the Loom-3 behavior) or -- when a sibling thread of
// the SAME Proc already holds the reader role -- sleeps on the ring's CQ wait-list
// until that reader's demux posts a CQE (resolving the Loom-3 concurrent-ENTER
// limitation). register-then-observe (poll.tla lineage) + death-interruptible
// (#811). l->lock NOT held on entry/exit.
//
// Why no waiter strands at v1.0/Loom-4b: a Loom ring is mapped into exactly ONE
// Proc (KObj_Loom is non-transferable, audited), so every concurrent ENTER is a
// sibling THREAD of that one Proc. If the Proc group-terminates, ALL of them are
// death-interrupted together (the sleep returns SLEEP_INTR -> break). If the 9P
// SESSION dies, the active reader's client_mark_dead_locked posts an error CQE for
// every in-flight async op (loom_async_complete -> loom_post_cqe -> wake), so a
// sleeper is woken and observes async_inflight drain to 0. The explicit teardown-
// wakes-waiter the spec models (NoStrandedWaiter) is the SQPOLL-kthread surface
// (Loom-4c): a loom_enter caller holds a loom ref for the whole call, so loom_free
// cannot run while a waiter sleeps here -- NoStrandedWaiter holds vacuously now.
static void loom_wait_for_completions(struct Loom *l, u32 min_complete,
                                      u32 submitted) {
    // Bound the active reader's recv spin so a hostile/buggy server flooding
    // ownerless frames cannot burn a CPU unbounded inside one syscall (Loom-3
    // audit F4). A trusted v1.0 server (stratumd / dev9p) completes within a
    // frame per in-flight op, so the budget is never the limiter.
    u32 pump_budget = submitted + (u32)P9_SESSION_MAX_OUTSTANDING + 1u;
    u32 pumps = 0;

    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter pw;
    poll_waiter_init(&pw, &r);

    for (;;) {
        // CqWaitCommitOrSleep (the give-up arms): enough completions, or nothing
        // more can ever complete. Sampled under l->lock (cq_tail + async_inflight).
        spin_lock(&l->lock);
        u32 ready    = loom_cq_ready(l);
        u32 inflight = l->async_inflight;
        spin_unlock(&l->lock);
        if (ready >= min_complete) break;
        if (inflight == 0)         break;

        // Try to become the elected reader and drive one frame.
        struct p9_client *cl = loom_first_inflight_client(l);
        if (!cl) continue;                     // raced: the op completed/reaped -> re-check
        int rc = p9_client_reader_pump_once(cl);
        if (rc == 1) {                         // demuxed a frame
            if (++pumps >= pump_budget) {
                // Flood budget hit. Hand the reader baton to any sleeping sibling
                // (so it retries instead of stranding), then return what is posted
                // (io_uring-style degradation under a Byzantine server). Death-
                // interruptible regardless; trusted servers never reach here.
                poll_waiter_list_wake(&l->cq_waiters);
                break;
            }
            continue;
        }
        if (rc < 0) break;                     // session dead / self-dying -> stop

        // rc == 0: a sibling thread holds the reader role. Sleep on the CQ wait-
        // list until it posts a CQE. CqWaitRegister: install the hook AND re-sample
        // under l->lock, so a CQE posted just before the hook went live is caught
        // (register-then-observe -- the live edge before register is the sample,
        // the edge after register is the wake-flag the cond reads).
        pw.ready = false;   // safe: pw is off all lists here (never/last-unregistered)
        bool do_sleep;
        spin_lock(&l->lock);
        poll_waiter_list_register(&l->cq_waiters, &pw);
        do_sleep = (loom_cq_ready(l) < min_complete) && (l->async_inflight > 0);
        spin_unlock(&l->lock);
        if (!do_sleep) { poll_waiter_list_unregister(&pw); continue; }
        int s = sleep(&r, loom_cqw_cond, &pw);
        poll_waiter_list_unregister(&pw);
        if (s == SLEEP_INTR) break;            // #811: Proc group-terminating -> unwind
        // woken by a posted CQE -> loop, re-sample.
    }
    pw.magic = 0;   // defense-in-depth before the stack frame pops (poll.c idiom)
}

int loom_enter(struct Loom *l, u32 to_submit, u32 min_complete, u32 flags) {
    if (!l || l->magic != LOOM_MAGIC) return -1;
    if (flags & ~LOOM_ENTER_VALID)    return -1;

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    u32 *sq_array         = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);

    // --- SUBMIT phase. Consume up to `to_submit` SQEs in SQ-index order. ---
    // sq_tail is user-owned (advisory): bound the work against it but never trust
    // it for indexing. The consume position is the kernel-private sq_head; the
    // SQ-index ring slot it points at is user-written, so it is range-checked
    // before it indexes the SQE array (the SA-1 discipline on the SQ side).
    //
    // F1: each per-slot claim (the sq_head read + advance + the SQE read+copy)
    // runs under l->lock so two concurrent SYS_LOOM_ENTERs on the same ring (a
    // multi-thread Proc) never read the same sq_head -> never double-dispatch a
    // slot or lose an sq_head update; each iteration atomically claims exactly one
    // slot. loom_submit_one runs OUTSIDE the lock (it re-takes l->lock for the
    // FSYNC pin + the inflight link -- a separate, non-nested acquisition).
    u32 budget = (to_submit < l->sq_entries) ? to_submit : l->sq_entries;  // bound a hostile to_submit
    u32 submitted = 0;
    for (u32 i = 0; i < budget; i++) {
        struct loom_sqe ksqe;
        bool have;
        spin_lock(&l->lock);
        u32 sq_tail = __atomic_load_n(&h->sq_tail, __ATOMIC_ACQUIRE);
        if (sq_tail == l->sq_head) { spin_unlock(&l->lock); break; }   // drained
        // F2 / I-29 submit-time CQ admission (io_uring's model): consume an SQE
        // only when the CQ can still hold one more completion beyond every
        // already-posted-unreaped CQE AND every in-flight op's eventual CQE. This
        // makes loom_post_cqe's full-CQ drop unreachable for a single consumer
        // (then `overflow` is the pure diagnostic the comment promises); a
        // non-reaping user back-pressures HERE (the SQE waits for the next enter)
        // instead of losing a completion. (Concurrent enters can over-admit by at
        // most a few -- async_inflight is bumped in loom_submit_one after this
        // lock drops -- and loom_post_cqe's CqNeverOverfull guard is the backstop;
        // exact concurrent admission is the Loom-4 coordination.)
        if (loom_cq_ready(l) + l->async_inflight >= l->cq_entries) {
            spin_unlock(&l->lock);
            break;
        }
        u32 ring_pos = l->sq_head & (l->sq_entries - 1u);   // kernel-private mask
        l->sq_head++;
        __atomic_store_n(&h->sq_head, l->sq_head, __ATOMIC_RELEASE);   // publish (user-readable mirror)
        u32 sqe_idx = __atomic_load_n(&sq_array[ring_pos], __ATOMIC_ACQUIRE);
        have = (sqe_idx < l->sq_entries);
        if (have) ksqe = sqes[sqe_idx];                     // COPY to kernel memory (TOCTOU)
        else __atomic_store_n(&h->dropped, h->dropped + 1u, __ATOMIC_RELEASE);
        spin_unlock(&l->lock);
        if (!have) continue;                                // bad indirection -> dropped
        loom_submit_one(l, &ksqe);
        submitted++;
    }

    // --- WAIT / REAP phase (Loom-4). Block until min_complete CQEs are posted or
    // no completion can arrive. The caller drives the elected reader itself, or --
    // when a sibling thread of the same Proc already holds the reader role --
    // sleeps on the ring's CQ wait-list until that reader's demux posts a CQE
    // (loom_wait_for_completions; the multi-waiter coordination that resolves the
    // Loom-3 "concurrent ENTER returns what's posted" limitation). The wait is
    // death-interruptible (#811) and the reader spin is flood-bounded. ---
    if (min_complete > 0 && !(flags & LOOM_ENTER_NONBLOCK)) {
        loom_wait_for_completions(l, min_complete, (u32)submitted);
    }

    loom_reap_terminal(l);
    return (int)submitted;
}
