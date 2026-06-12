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
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/page.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>       // kproc (the SQPOLL kthread parent)
#include <thylacine/rendez.h>
#include <thylacine/sched.h>      // ready / sched (the SQPOLL kthread lifecycle)
#include <thylacine/spoor.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>     // thread_create_with_arg / thread_free / THREAD_EXITING
#include <thylacine/types.h>
#include <thylacine/vma.h>        // vma_lookup / VMA_PROT_* (Loom-6 registered buffers)

#include "../mm/slub.h"
#include "../arch/arm64/timer.h"  // timer_now_ns (the SQPOLL frame-boundary idle deadline)

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
    p9_session_build_fn    build;       // the Tmsg builder -- re-run verbatim on re-arm (Loom-5)
    u64                    user_data;  // echoed verbatim into the CQE
    u32                    op_fid;      // resolved 9P fid (from `pinned`, at submit)
    u32                    op_arg;      // opcode-specific scalar (FSYNC: datasync 0/1)
    // Loom-6 payload ops (READ / WRITE). op_offset is the FILE offset, op_count
    // the byte count. pinned_buf is an INDEPENDENT burrow_ref on the registered
    // buffer's Burrow -- the I-30 buffer pin, symmetric with `pinned` for the fid:
    // held for the op's whole lifetime so a concurrent LOOM_REGISTER_BUFFERS
    // re-install / munmap cannot free the pages under an in-flight copy. Dropped
    // (burrow_unref) at reap / abandon, never re-resolved. buf_kva points at the
    // pinned slice in the Burrow's stable, contiguous direct map; the build thunk
    // (WRITE) reads it, loom_async_complete (READ) writes it -- both under a lock,
    // both pinned kernel memory (no fault, no sleep). NULL for scalar ops.
    u64                    op_offset;   // READ/WRITE: file offset
    u32                    op_count;    // READ/WRITE: requested byte count
    struct Burrow         *pinned_buf;  // READ/WRITE: the I-30 buffer pin (burrow_ref)
    u8                    *buf_kva;     // READ/WRITE: the pinned buffer slice base
    // Loom-6b-2 metadata-MUTATION ops. The pinned buffer region (buf_kva,
    // op_count bytes) holds the name(s) the build thunk reads (the WRITE-payload
    // discipline) or the input struct p9_setattr. `sqe` is the TOCTOU-safe SQE
    // snapshot the mutation build thunks decode their op-specific scalars + name
    // sub-lengths from (they carry more fields than op_offset/op_count hold). The
    // two-fid ops (RENAMEAT / LINK) additionally pin a SECOND registered fid:
    // `pinned2` is its independent I-30 spoor_ref (NULL for single-fid ops),
    // `op_fid2` its resolved 9P fid. Both fids are in the SAME p9_client (checked
    // at submit -- a 9P renameat/link is within one session). `pinned2` is
    // released symmetric with `pinned` at reap / abandon / loom_free.
    struct Spoor          *pinned2;     // RENAMEAT/LINK: second I-30 pin (NULL otherwise)
    u32                    op_fid2;      // RENAMEAT/LINK: resolved second fid
    struct loom_sqe        sqe;          // TOCTOU snapshot (mutation build-thunk decode)
    // Multishot (Loom-5; specs/loom_multishot.tla). A multishot op re-arms after
    // each NON-terminal reply: loom_async_complete posts a LOOM_CQE_MORE CQE +
    // sets `rearm`; the drive loop (loom_rearm_pending, OUTSIDE c->lock) re-issues
    // `build` on the SAME pinned (client, fid) -- the pin is reused, NEVER
    // re-resolved (ObjPinnedAcrossShots). `shot_limit` is the SYNTHETIC terminal
    // bound for the FSYNC vehicle (total CQEs in the stream; Loom-6's real
    // event-fd READ terminates on the source's EOF/error instead). `shots` counts
    // the MORE CQEs posted so far (< shot_limit). `multishot == false` is the
    // Loom-3 single-shot op (its lone CQE is its terminal).
    u32                    shot_limit;  // multishot: total CQEs before the terminal (>= 1)
    u32                    shots;       // multishot: MORE CQEs posted so far
    bool                   multishot;   // re-arms after each non-terminal reply
    bool                   rearm;       // a MORE shot posted; the drive loop must re-issue
    u8                     opcode;      // LOOM_OP_*
    bool                   terminal;    // CQE posted (set under l->lock by completion)
    // Loom-5b: when this async op backs a LINK/DRAIN chain entry, `chain` points
    // at it (NULL for a fast-path op). On the op's TERMINAL, loom_async_complete
    // records the result into chain->state (DONE_OK / DONE_FAIL) so the admission
    // pass can admit / cancel the chain successors. Set at dispatch, BEFORE
    // p9_client_submit_async, so a synchronous failure's completion sees it. A
    // chain op is never multishot (the combo is rejected), so its terminal is its
    // lone completion.
    struct loom_chain_op  *chain;
    struct loom_async_op  *next;        // l->inflight_ops chain (under l->lock)
};

_Static_assert(__builtin_offsetof(struct loom_async_op, rpc) == 0,
               "p9_rpc must be at offset 0 -- loom_async_complete recovers the "
               "container by casting the struct p9_rpc * the engine hands it");

// =============================================================================
// One LINK/DRAIN chain entry (Loom-5b; specs/loom_order.tla). Heap-allocated per
// ordering-relevant SQE; owned by the Loom (linked into l->chain). Layered ON TOP
// of the audited loom_async_op lifecycle -- NOT unified with it: an ordering-
// relevant op is HELD here until its admission gates open, THEN dispatched via
// loom_submit_one (which, for an async op, creates a loom_async_op carrying a
// back-pointer to this entry). The chain encodes the per-op `link`/`drain` flags
// + the submission-order successor; loom_admit_chain walks it head->tail.
//
// State machine (loom_order.tla phase/result): HELD (submitted, gates not yet
// open) -> INFLIGHT (dispatched; an async reply or an inline completion pending)
// -> DONE_OK / DONE_FAIL (completed; its own CQE posted) | CANCELLED (a linked
// predecessor failed; one -ECANCELED CQE posted, never ran). INFLIGHT is also the
// transient claim a dispatch sets under l->lock so a concurrent admit never
// double-dispatches; loom_submit_one immediately overwrites it with the real
// terminal (inline) or leaves it (async, until the reply).
// =============================================================================
enum loom_chain_state {
    LOOM_CHAIN_HELD = 0,    // submitted; ordering gates not yet open
    LOOM_CHAIN_INFLIGHT,    // dispatched (async reply pending) / the dispatch claim
    LOOM_CHAIN_DONE_OK,     // completed ok; its CQE posted
    LOOM_CHAIN_DONE_FAIL,   // completed with -errno; its CQE posted
    LOOM_CHAIN_CANCELLED,   // a linked predecessor failed; one -ECANCELED CQE posted
};

struct loom_chain_op {
    struct loom_sqe        sqe;         // the kernel SQE copy (TOCTOU-safe; dispatched at admit)
    bool                   link;        // LOOM_SQE_LINK: this op links to its successor
    bool                   drain;       // LOOM_SQE_DRAIN: this op is a barrier
    enum loom_chain_state  state;       // under l->lock
    struct loom_chain_op  *chain_next;  // l->chain successor (submission order; under l->lock)
};

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
    rendez_init(&l->sqpoll_park);            // Loom-4c SQPOLL park (no kthread until loom_start_sqpoll)
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
    // Loom-4c: JOIN the SQPOLL kthread FIRST -- before the #898 quiesce. The
    // kthread is the only other mutator of inflight_ops (it submits + reaps in
    // its loop), so it must be fully stopped before this function touches the
    // list. The join: set sqpoll_stopping (release) so the kthread's next loop
    // top / park-cond observes it, wake the park Rendez (an idle kthread is
    // sleeping there; a kthread mid-recv self-returns at its frame-boundary
    // idle-deadline and re-checks stopping -- the gate guarantees a deadline-
    // capable transport so this terminates), then spin until the kthread signals
    // sqpoll_exited (release; pairs with this acquire -> the kthread's
    // state=EXITING write is visible) and thread_free it (which internally spins
    // on on_cpu, the wait_pid reap discipline). refcount is 0 so no ENTER runs;
    // the kthread holds no loom ref, so this join is the sole lifetime authority.
    if (l->sqpoll) {
        __atomic_store_n(&l->sqpoll_stopping, true, __ATOMIC_RELEASE);
        wakeup(&l->sqpoll_park);
        while (!__atomic_load_n(&l->sqpoll_exited, __ATOMIC_ACQUIRE)) {
            __asm__ __volatile__("yield" ::: "memory");
        }
        thread_free(l->sqpoll);
        l->sqpoll = NULL;
    }

    // specs/loom.tla Teardown-wakes-waiters (NoStrandedWaiter). VACUOUS in the
    // impl: a SYS_LOOM_ENTER caller sleeping on cq_waiters holds a loom ref for
    // its whole call (handle_get), so refcount cannot reach 0 -- this function
    // cannot run -- while any cq_waiter sleeps; the list is empty here. The wake
    // is the spec's teardown action made literal (a no-op walk of an empty list),
    // kept as defense if that ref invariant ever weakens.
    poll_waiter_list_wake(&l->cq_waiters);

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
        if (op->pinned_buf) burrow_unref(op->pinned_buf);   // Loom-6: release the buffer pin
        if (op->pinned2)    spoor_clunk(op->pinned2);        // Loom-6b-2: second fid pin
        spoor_clunk(op->pinned);
        kfree(op);
        op = next;
    }

    // Loom-5b: free the LINK/DRAIN chain. refcount is 0 (the SQPOLL kthread is
    // joined; no ENTER runs), so nothing mutates l->chain here. A HELD entry never
    // started -- it holds no pin, no tag, no async op -- so it is abandoned
    // silently (the ring is dying; userspace will not read its CQE). An INFLIGHT
    // entry's backing loom_async_op was already abandoned + freed above; the chain
    // entry never references it (only op->chain points the other way), so freeing
    // the entry now is safe.
    struct loom_chain_op *ce = l->chain;
    l->chain = NULL;
    l->chain_tail = NULL;
    l->chain_len = 0;
    while (ce) {
        struct loom_chain_op *cnext = ce->chain_next;
        kfree(ce);
        ce = cnext;
    }

    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++) {
        if (l->reg[i].spoor) {
            spoor_clunk(l->reg[i].spoor);
            l->reg[i].spoor = NULL;
        }
    }
    // Loom-6: release every registered-buffer pin (the table's burrow_refs).
    for (u32 i = 0; i < LOOM_MAX_REG_BUFFERS; i++) {
        if (l->reg_buf[i].burrow) {
            burrow_unref(l->reg_buf[i].burrow);
            l->reg_buf[i].burrow = NULL;
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

    // Loom-4c SQPOLL deadline-capable gate. On an SQPOLL ring the poll-thread
    // block-recvs on the dev9p client of any in-flight async op, in process
    // context with no death-interrupt (kproc never group-terminates) -- so a
    // NULL-deadline transport would block it un-interruptibly and HANG teardown's
    // join. Reject registering such a handle. A non-dev9p Spoor (dev9p_client_fid
    // fails) is allowed: it can never go async (loom_submit_one -EINVALs it
    // inline), so the kthread never recvs on it. Checked BEFORE any ref is
    // adopted, so the caller's rollback (it retains its refs on failure) holds.
    if (l->sqpoll) {
        for (u32 i = 0; i < n; i++) {
            struct p9_client *cl; u32 fid;
            if (dev9p_client_fid(spoors[i], &cl, &fid) == 0 &&
                !p9_client_recv_is_deadline_capable(cl)) {
                return -1;
            }
        }
    }

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

// Loom-6: resolve + PIN one registered-buffer VA range into *out. On success
// fills *out (with a burrow_ref HELD on out->burrow) and returns 0; on any
// validation failure returns -1 having taken NO ref and left *out untouched.
// Caller holds p->vma_lock (vma_lookup needs it; lock order vma_lock ->
// burrow->lock matches burrow_map). The range must lie within ONE anonymous
// (CPU-accessible, contiguous), writable VMA -- the same posture io_uring's
// IORING_REGISTER_BUFFERS pins. The kva is computed from the Burrow's stable
// direct-map base, NOT the VA, so the pin keeps it valid past a later munmap
// (the #847 dual-refcount).
static int loom_resolve_buf(struct Proc *p, const struct loom_buf_reg *b,
                            struct loom_reg_buffer *out) {
    u64 va = b->va, len = b->len;
    if (len == 0 || len > 0xFFFFFFFFull)              return -1;  // u32 slot; empty useless
    if (va + len < va)                               return -1;  // VA-range wrap
    struct Vma *vma = vma_lookup(p, va);
    if (!vma || !vma->burrow)                        return -1;  // unmapped / guard VMA
    // BURROW_TYPE_ANON is LOAD-BEARING beyond the device-mem reject: it is the sole
    // guarantee that the Burrow is one physically-contiguous alloc_pages chunk, which
    // is what makes the single-base `pa_to_kva(page_to_pa(pages)) + boff` kva valid
    // across the whole [va, va+len) span below. 6c audit F3: do NOT widen this type
    // gate to admit a CPU-accessible-but-scatter-gather Burrow type (a future
    // file-backed / sg-list successor) without first making the kva computation
    // chunk-walking -- else boff past the first physical chunk yields a wrong/OOB
    // kernel address with no tripwire.
    if (vma->burrow->type != BURROW_TYPE_ANON)       return -1;  // device mem (MMIO/DMA) / non-contiguous: no
    if ((vma->prot & VMA_PROT_WRITE) == 0)           return -1;  // need RW (READ writes into it)
    if (va < vma->vaddr_start || va + len > vma->vaddr_end) return -1;  // within ONE VMA
    // Byte offset of the slice into the backing Burrow + its contiguous direct-map
    // base (valid because of the BURROW_TYPE_ANON contiguity guarantee above).
    u64 boff = vma->burrow_offset + (va - vma->vaddr_start);
    if (boff + len > vma->burrow->size)              return -1;  // defensive (VMA invariant implies)
    u8 *base = (u8 *)pa_to_kva(page_to_pa(vma->burrow->pages));
    out->burrow = vma->burrow;
    out->kva    = base + boff;
    out->len    = (u32)len;
    out->_pad   = 0;
    burrow_ref(vma->burrow);     // the table's pin (released at re-register / loom_unref)
    return 0;
}

int loom_register_buffers(struct Loom *l, struct Proc *p,
                          const struct loom_buf_reg *bufs, u32 n) {
    if (!l || l->magic != LOOM_MAGIC || !p)  return -1;
    if (n > LOOM_MAX_REG_BUFFERS)            return -1;
    if (n > 0 && !bufs)                      return -1;

    // Resolve + pin the WHOLE new set first (all-or-nothing, like
    // loom_register_handles): under p->vma_lock so vma_lookup is stable. On any
    // failure roll back the refs taken so far and leave the live table untouched.
    struct loom_reg_buffer fresh[LOOM_MAX_REG_BUFFERS];
    for (u32 i = 0; i < LOOM_MAX_REG_BUFFERS; i++) {
        fresh[i].burrow = NULL; fresh[i].kva = NULL; fresh[i].len = 0; fresh[i]._pad = 0;
    }
    spin_lock(&p->vma_lock);
    u32 done = 0;
    int rc = 0;
    for (; done < n; done++) {
        if (loom_resolve_buf(p, &bufs[done], &fresh[done]) != 0) { rc = -1; break; }
    }
    spin_unlock(&p->vma_lock);
    if (rc != 0) {
        for (u32 i = 0; i < done; i++) burrow_unref(fresh[i].burrow);   // roll back
        return -1;
    }

    // Install: swap the table under l->lock, then unref the displaced pins OUTSIDE
    // the lock (burrow_unref may free the Burrow -> never under a spin_lock).
    struct Burrow *old[LOOM_MAX_REG_BUFFERS];
    spin_lock(&l->lock);
    for (u32 i = 0; i < LOOM_MAX_REG_BUFFERS; i++) {
        old[i]        = l->reg_buf[i].burrow;
        l->reg_buf[i] = fresh[i];
    }
    l->n_reg_buf = n;
    spin_unlock(&l->lock);

    for (u32 i = 0; i < LOOM_MAX_REG_BUFFERS; i++) {
        if (old[i]) burrow_unref(old[i]);
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

// Byte copy (the 9p_client.c client_copy precedent; no freestanding memcpy
// symbol). Both ends are pinned kernel memory in the only caller.
static void loom_bufcopy(u8 *dst, const u8 *src, u32 n) {
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
}

// Loom-6: the CQE result for a completed op + (for READ) the wire->buffer copy.
// Runs UNDER c->lock from loom_async_complete; `dr` aliases the transport recv
// buffer and is valid ONLY for that callback, so the READ copy MUST happen here.
// The destination is the op's submit-time-pinned, contiguous direct-map slice
// (no fault, no sleep), so the copy composes with the seam contract. An error
// completion (status < 0) copies nothing and passes the -errno through.
//   READ  -> copy min(reply count, the op's slice) into the buffer; result is
//            the byte count actually read.
//   WRITE -> result is the server-accepted byte count; the data already went out
//            in the build thunk.
//   else  -> the scalar mapping (NOP / FSYNC).
// The Loom GETATTR / STATFS output ABI: the kernel copies the parsed record
// verbatim into the registered buffer, so struct p9_attr / struct p9_statfs ARE
// the userspace-visible output layout (the native libthyla_rs::loom side mirrors
// them at Loom-6d). The field set mirrors 9P2000.L Rgetattr / Rstatfs (itself a
// stable wire ABI); these asserts pin the size so a future kernel field add
// cannot silently shift the Loom output ABI -- it trips the build instead.
_Static_assert(sizeof(struct p9_attr) == 160, "Loom GETATTR output ABI pinned at 160 bytes");
_Static_assert(sizeof(struct p9_statfs) == 64, "Loom STATFS output ABI pinned at 64 bytes");
// Loom-6b-2 SETATTR INPUT ABI: userspace writes a struct p9_setattr into the
// registered buffer; the build thunk reads it (align-safe copy). Pinned so a
// future kernel field add cannot silently shift the input layout.
_Static_assert(sizeof(struct p9_setattr) == 56, "Loom SETATTR input ABI pinned at 56 bytes");
// 6c audit F4: pin the load-bearing field OFFSETS too, not just the size -- a
// same-size field reorder (e.g. swapping two u32s) leaves sizeof unchanged but
// silently shifts the byte-pinned Loom ABI the native libthyla_rs::loom mirror
// reads/writes. Catch that drift at build time (CLAUDE.md compile-time invariants).
_Static_assert(__builtin_offsetof(struct p9_attr, valid) == 0,  "Loom p9_attr.valid ABI offset");
_Static_assert(__builtin_offsetof(struct p9_attr, mode)  == 24, "Loom p9_attr.mode ABI offset");
_Static_assert(__builtin_offsetof(struct p9_attr, uid)   == 28, "Loom p9_attr.uid ABI offset");
_Static_assert(__builtin_offsetof(struct p9_attr, gid)   == 32, "Loom p9_attr.gid ABI offset");
_Static_assert(__builtin_offsetof(struct p9_attr, size)  == 56, "Loom p9_attr.size ABI offset");
_Static_assert(__builtin_offsetof(struct p9_setattr, valid) == 0,  "Loom p9_setattr.valid ABI offset");
_Static_assert(__builtin_offsetof(struct p9_setattr, mode)  == 4,  "Loom p9_setattr.mode ABI offset");
_Static_assert(__builtin_offsetof(struct p9_setattr, uid)   == 8,  "Loom p9_setattr.uid ABI offset");
_Static_assert(__builtin_offsetof(struct p9_setattr, gid)   == 12, "Loom p9_setattr.gid ABI offset");
_Static_assert(__builtin_offsetof(struct p9_setattr, size)  == 16, "Loom p9_setattr.size ABI offset");
_Static_assert(__builtin_offsetof(struct p9_statfs, type)    == 0,  "Loom p9_statfs.type ABI offset");
_Static_assert(__builtin_offsetof(struct p9_statfs, bsize)   == 4,  "Loom p9_statfs.bsize ABI offset");
_Static_assert(__builtin_offsetof(struct p9_statfs, blocks)  == 8,  "Loom p9_statfs.blocks ABI offset");
_Static_assert(__builtin_offsetof(struct p9_statfs, namelen) == 56, "Loom p9_statfs.namelen ABI offset");

// 6c audit F1: map a byte COUNT to a CQE result without aliasing the error
// region. The CQE ABI is result >= 0 = byte count, result < 0 = -errno; a count
// that would set the sign bit (> INT32_MAX -- a multi-GiB registered buffer, or
// an over-large server-reported write/read count) must NOT wrap to a spurious
// -errno. The data was already placed (READ) / accepted (WRITE); only the
// reported magnitude caps. Clamp so the s32 cast stays non-negative.
static s32 loom_count_result(u32 got) {
    if (got > 0x7FFFFFFFu) got = 0x7FFFFFFFu;
    return (s32)got;
}

static s32 loom_payload_result(struct loom_async_op *op, int status,
                               struct p9_dispatch_result *dr) {
    if (status < 0) return (s32)status;        // error reply: -errno, no payload
    switch (op->opcode) {
    case LOOM_OP_READ: {
        if (!dr) return 0;
        u32 got = dr->read_count;
        if (got > op->op_count) got = op->op_count;   // never overrun the pinned slice
        if (got != 0 && dr->read_data && op->buf_kva)
            loom_bufcopy(op->buf_kva, dr->read_data, got);
        return loom_count_result(got);
    }
    case LOOM_OP_WRITE:
        return dr ? loom_count_result(dr->write_count) : 0;
    // Loom-6b read-shaped output: copy the reply payload INTO the pinned dest
    // slice, bounded by op_count (the dest capacity) so a short buffer can never
    // overrun. READDIR / READLINK stream bytes; GETATTR / STATFS copy the parsed
    // fixed record. The CQE result is the byte count placed in the buffer.
    case LOOM_OP_READDIR: {
        if (!dr) return 0;
        u32 got = dr->readdir_count;
        if (got > op->op_count) got = op->op_count;
        if (got != 0 && dr->readdir_data && op->buf_kva)
            loom_bufcopy(op->buf_kva, dr->readdir_data, got);
        return loom_count_result(got);
    }
    case LOOM_OP_READLINK: {
        if (!dr) return 0;
        u32 got = dr->readlink_target_len;
        if (got > op->op_count) got = op->op_count;
        if (got != 0 && dr->readlink_target && op->buf_kva)
            loom_bufcopy(op->buf_kva, dr->readlink_target, got);
        return loom_count_result(got);
    }
    case LOOM_OP_GETATTR: {
        if (!dr) return 0;
        u32 got = (u32)sizeof(struct p9_attr);
        if (got > op->op_count) got = op->op_count;   // honor a short dest (no overrun)
        if (got != 0 && op->buf_kva)
            loom_bufcopy(op->buf_kva, (const u8 *)&dr->attr, got);
        return loom_count_result(got);
    }
    case LOOM_OP_STATFS: {
        if (!dr) return 0;
        u32 got = (u32)sizeof(struct p9_statfs);
        if (got > op->op_count) got = op->op_count;
        if (got != 0 && op->buf_kva)
            loom_bufcopy(op->buf_kva, (const u8 *)&dr->statfs, got);
        return loom_count_result(got);
    }
    default:
        return loom_scalar_result(status);            // NOP / FSYNC
    }
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
    struct loom_async_op *op = (struct loom_async_op *)rpc;   // rpc at offset 0
    struct Loom *l = op->loom;

    // Compute the CQE result + (READ) copy the reply payload into the pinned
    // buffer NOW, while `dr` is valid (it aliases the recv buffer for this call
    // only). For a multishot stream this runs once per shot. Scalar ops (NOP /
    // FSYNC) fall through to the scalar mapping.
    s32 result = loom_payload_result(op, status, dr);

    // MORE vs terminal (specs/loom_multishot.tla ReplyArrives). A single-shot op
    // (Loom-3) always terminates. A multishot op posts a LOOM_CQE_MORE shot and
    // RE-ARMS on a successful non-terminal reply, and TERMINATES on an error reply
    // (the stream ends on error), on reaching its shot bound, or -- defensively --
    // if the shot CQE cannot post (submit-time admission makes that unreachable;
    // fail-safe to terminal rather than re-arm into a lost shot). The terminal CQE
    // clears LOOM_CQE_MORE (TerminalEndsStream -- the Tapestry recycle gate).
    bool term;
    if (!op->multishot)                        term = true;
    else if (status < 0)                       term = true;   // error ends the stream
    else if (op->shots + 1u >= op->shot_limit) term = true;   // synthetic FSYNC bound
    else                                       term = false;  // a MORE shot -> re-arm

    u32 cqe_flags = term ? 0u : LOOM_CQE_MORE;
    int posted = loom_post_cqe(l, op->user_data, result, cqe_flags);

    // Update op state under l->lock so the reap / re-arm sweeps observe a
    // consistent (terminal | rearm, async_inflight) pair. The CQE was already
    // posted above. This op's reservation is resolved EITHER WAY: a MORE shot
    // re-acquires its next slot in loom_rearm_pending; a terminal needs none.
    spin_lock(&l->lock);
    if (l->async_inflight > 0) l->async_inflight--;
    if (!term && posted == 0) {
        op->shots++;
        op->rearm = true;          // the drive loop re-issues `build` (outside c->lock)
        __atomic_fetch_add(&l->rearm_pending, 1u, __ATOMIC_RELEASE);  // SQPOLL park-cond hint
    } else {
        op->terminal = true;       // terminal (or a failed MORE post -> end the stream)
        // Loom-5b: record the chain result so the next admission pass admits the
        // linked/post-drain successors (DONE_OK) or cancels the chain (DONE_FAIL).
        // A chain op is never multishot, so `term` is always true here -- the
        // guard is defensive. The state write is under l->lock, paired with the
        // admit pass's read under l->lock (no torn read; the successor sees the
        // result on the drive loop's next loom_admit_chain).
        if (op->chain) {
            op->chain->state = (status < 0) ? LOOM_CHAIN_DONE_FAIL
                                            : LOOM_CHAIN_DONE_OK;
        }
    }
    spin_unlock(&l->lock);
}

// Tmsg builder thunk for LOOM_OP_FSYNC. Invoked by p9_client_submit_async under
// the client's c->lock; allocates the 9P tag + marks outstanding. `ctx` is the
// loom_async_op (it carries the resolved fid + datasync).
static int loom_build_fsync(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_fsync(s, out, cap, op->op_fid, op->op_arg);
}

// Tmsg builder thunks for the data-path ops (Loom-6). Invoked by
// p9_client_submit_async under c->lock. READ carries no payload (the reply
// brings the data); WRITE's payload is read FROM the pinned registered buffer
// slice (op->buf_kva, pinned kernel memory -- no fault, no sleep). An over-msize
// count makes p9_session_send_* return < 0, which submit_async turns into one
// -P9_E_IO error CQE (the op is single-frame; userspace splits large I/O).
static int loom_build_read(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_read(s, out, cap, op->op_fid, op->op_offset, op->op_count);
}
static int loom_build_write(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_write(s, out, cap, op->op_fid, op->op_offset,
                                 op->op_count, op->buf_kva);
}

// Loom-6b read-shaped builders. READDIR mirrors READ (op_offset = dir offset,
// op_count = byte count -> a dirent stream reply). READLINK / GETATTR / STATFS
// carry no request count (the reply size is server-chosen): the registered
// buffer is purely the completion-time destination, and op_count is the dest
// capacity that bounds the copy. GETATTR's op_offset carries the request_mask.
static int loom_build_readdir(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_readdir(s, out, cap, op->op_fid, op->op_offset, op->op_count);
}
static int loom_build_readlink(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_readlink(s, out, cap, op->op_fid);
}
static int loom_build_getattr(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_getattr(s, out, cap, op->op_fid, op->op_offset);
}
static int loom_build_statfs(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_statfs(s, out, cap, op->op_fid);
}

// Loom-6b-2 metadata-MUTATION builders. Each reads its name(s) FROM the pinned
// buffer region (op->buf_kva, op->op_count bytes -- the WRITE-payload discipline)
// and decodes its op-specific scalars + name sub-lengths from the TOCTOU SQE
// snapshot (op->sqe). The submit gate validated every sub-length <= the pinned
// span and SETATTR's struct span, so these reads never overrun the slice. The
// two-fid builders pass op->op_fid2 (the second resolved fid). The reply is
// scalar (Rsetattr / Rmkdir-with-qid-dropped / Runlinkat / ... -> the default
// 0/-errno mapping in loom_payload_result).
static int loom_build_setattr(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    // Copy the input out of the (possibly unaligned) pinned slice into a local:
    // the wire encoder reads u64 fields, which need natural alignment. The submit
    // gate checked op_count >= sizeof(struct p9_setattr), so the copy is in-bounds.
    struct p9_setattr attr;
    loom_bufcopy((u8 *)&attr, op->buf_kva, (u32)sizeof(attr));
    return p9_session_send_setattr(s, out, cap, op->op_fid, &attr);
}
static int loom_build_unlinkat(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_unlinkat(s, out, cap, op->op_fid,
                                    op->buf_kva, op->op_count, (u32)op->sqe.offset);
}
static int loom_build_mkdir(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_mkdir(s, out, cap, op->op_fid,
                                 op->buf_kva, op->op_count,
                                 (u32)op->sqe._resv1[1], (u32)op->sqe._resv1[2]);
}
static int loom_build_mknod(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    return p9_session_send_mknod(s, out, cap, op->op_fid,
                                 op->buf_kva, op->op_count,
                                 (u32)op->sqe._resv1[1], (u32)op->sqe._resv1[2],
                                 (u32)op->sqe._resv1[3], (u32)op->sqe.offset);
}
static int loom_build_symlink(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    u32 name_len = (u32)op->sqe._resv1[1];          // the split (<= op_count, submit-checked)
    return p9_session_send_symlink(s, out, cap, op->op_fid,
                                   op->buf_kva, name_len,
                                   op->buf_kva + name_len, op->op_count - name_len,
                                   (u32)op->sqe._resv1[2]);
}
static int loom_build_renameat(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    u32 oldname_len = (u32)op->sqe._resv1[1];       // the split (<= op_count, submit-checked)
    return p9_session_send_renameat(s, out, cap,
                                    op->op_fid,  op->buf_kva, oldname_len,
                                    op->op_fid2, op->buf_kva + oldname_len,
                                    op->op_count - oldname_len);
}
static int loom_build_link(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct loom_async_op *op = (struct loom_async_op *)ctx;
    // dfid = op_fid (where the link lands); source inode = op_fid2; name = region.
    return p9_session_send_link(s, out, cap, op->op_fid, op->op_fid2,
                                op->buf_kva, op->op_count);
}

// Post a terminal CQE inline (no engine round-trip): the NOP success (result 0)
// and every submit-time-gate failure (bad opcode / handle / rights / OOM ->
// -errno). The SQE was consumed; userspace learns the result via the CQE, not a
// dropped SQE (io_uring semantics).
static void loom_complete_inline(struct Loom *l, u64 user_data, s32 result) {
    (void)loom_post_cqe(l, user_data, result, 0);
}

// Loom-5b: record a chain entry's INLINE-completion result (a fast-path op passes
// chain == NULL -> no-op). The async path does NOT call this -- it sets the back-
// pointer + leaves the entry INFLIGHT, and loom_async_complete records the result
// on the reply (under l->lock). The write is UNDER l->lock (audit F3) to match the
// locked readers (loom_admit_chain's walk + loom_chain_drain_admits) + the locked
// writer (loom_async_complete's chain->state store) -- a data-race-free single-
// writer per entry (the INFLIGHT claim under l->lock serializes which driver
// dispatches an entry, so only that driver reaches here for it).
static void loom_chain_done(struct Loom *l, struct loom_chain_op *chain, bool ok) {
    if (!chain) return;
    spin_lock(&l->lock);
    chain->state = ok ? LOOM_CHAIN_DONE_OK : LOOM_CHAIN_DONE_FAIL;
    spin_unlock(&l->lock);
}

// Dispatch a buffer-backed payload op (READ / WRITE / READDIR / READLINK /
// GETATTR / STATFS -- Loom-6a + 6b-1; SETATTR / MKDIR / MKNOD / SYMLINK /
// UNLINKAT / RENAMEAT / LINK -- Loom-6b-2). Resolves + PINS the registered file
// handle (spoor_ref) AND the registered buffer (burrow_ref) -- two independent
// I-30 submit-time pins held for the op's whole lifetime so a concurrent
// re-register / clunk / munmap cannot make completion act against a different
// object or freed pages. The two-fid mutation ops (RENAMEAT / LINK) pin a THIRD
// object -- a second registered handle (the same I-30 mechanism applied again),
// required in the SAME 9P session as the first. Bounds-checks the buffer slice +
// (mutation) the name sub-lengths / the SETATTR struct span, rights-gates per
// opcode (mirroring the kernel-syscall handle-rights for the equivalent op), and
// submits the async Tmsg. The builder + the required rights are selected
// per-opcode; the rest of the pin / bounds / submit machinery is shared.
// Single-shot only: a real multishot payload op needs an event-source dev
// (post-Loom graphics), so MULTISHOT here is rejected by the caller. Every
// failure path routes through `fail` -> releases whatever it pinned + posts one
// CQE.
static void loom_submit_payload(struct Loom *l, const struct loom_sqe *sqe,
                                struct loom_chain_op *chain) {
    u64 ud = sqe->user_data;

    // All resolve locals declared before the first `goto fail` (no
    // jump-bypasses-init). `fail` releases whatever is non-NULL, so the
    // before-pin early checks route through it harmlessly (all NULL there).
    // All locals (incl. the sqe-derived ones) declared + initialized BEFORE the
    // first `goto fail`, so no goto bypasses an initializer (-Wjump-misses-init
    // clean). `fail` releases whatever is non-NULL, so a before-pin early check
    // routes through it harmlessly (sp / sp2 / buf all still NULL there).
    p9_session_build_fn build = NULL;
    rights_t            need1 = 0;
    rights_t            need2 = 0;          // 0 => single-fid op
    bool                has_second = false;
    struct Spoor  *sp  = NULL, *sp2 = NULL; rights_t rt = 0, rt2 = 0;
    struct Burrow *buf = NULL; u8 *buf_kva = NULL;
    struct p9_client *cl = NULL; u32 fid = 0, fid2 = 0;
    struct loom_async_op *op = NULL;
    bool buf_bad = false;
    s32  err = 0;
    u32  idx2    = LOOM_SQE_FID2(sqe);
    u64  buf_off = LOOM_SQE_BUF_OFF(sqe);
    u32  count   = sqe->len;                // total pinned-region span
    u32  bidx    = sqe->buf_idx_or_off;

    // Opcode -> (Tmsg builder, primary right, secondary right). need2 != 0 marks
    // a two-fid op (RENAMEAT / LINK) whose SECOND registered-handle index lives in
    // LOOM_SQE_FID2(sqe). The rights mirror the kernel-syscall handle gates:
    // RIGHT_WRITE on a mutated directory / file (create / unlink / rename / wstat),
    // RIGHT_READ on a merely-referenced source (LINK's). loom_submit_one routes
    // exactly the buffer-backed opcodes here, so the default is unreachable.
    switch (sqe->opcode) {
    case LOOM_OP_WRITE:    build = loom_build_write;    need1 = RIGHT_WRITE; break;
    case LOOM_OP_READ:     build = loom_build_read;     need1 = RIGHT_READ;  break;
    case LOOM_OP_READDIR:  build = loom_build_readdir;  need1 = RIGHT_READ;  break;
    case LOOM_OP_READLINK: build = loom_build_readlink; need1 = RIGHT_READ;  break;
    case LOOM_OP_GETATTR:  build = loom_build_getattr;  need1 = RIGHT_READ;  break;
    case LOOM_OP_STATFS:   build = loom_build_statfs;   need1 = RIGHT_READ;  break;
    case LOOM_OP_SETATTR:  build = loom_build_setattr;  need1 = RIGHT_WRITE; break;
    case LOOM_OP_MKDIR:    build = loom_build_mkdir;    need1 = RIGHT_WRITE; break;
    case LOOM_OP_MKNOD:    build = loom_build_mknod;    need1 = RIGHT_WRITE; break;
    case LOOM_OP_SYMLINK:  build = loom_build_symlink;  need1 = RIGHT_WRITE; break;
    case LOOM_OP_UNLINKAT: build = loom_build_unlinkat; need1 = RIGHT_WRITE; break;
    case LOOM_OP_RENAMEAT: build = loom_build_renameat; need1 = RIGHT_WRITE; need2 = RIGHT_WRITE; break;
    case LOOM_OP_LINK:     build = loom_build_link;     need1 = RIGHT_WRITE; need2 = RIGHT_READ;  break;
    default: err = -(s32)T_E_INVAL; goto fail;
    }
    has_second = (need2 != 0);

    if (sqe->handle_idx >= LOOM_MAX_REG_HANDLES) { err = -(s32)T_E_INVAL; goto fail; }
    if (has_second && idx2 >= LOOM_MAX_REG_HANDLES) { err = -(s32)T_E_INVAL; goto fail; }

    // Submit-time descriptor validation on the kernel SNAPSHOT (ring TOCTOU --
    // never re-read the shared ring after this). These are MEMORY-SAFETY gates
    // that bound the build thunk's reads into the pinned region:
    //   - a two-name op (SYMLINK / RENAMEAT) splits the region at _resv1[1]; that
    //     sub-length must lie strictly inside the span -- 0 < split < count -- so
    //     buf_kva+split stays in the slice AND both halves are non-empty (6c audit
    //     F2: the old `> count` gate let split == count through, sending an empty
    //     second name that SYMLINK accepted but RENAMEAT rejected -- inconsistent;
    //     and a valid two-name op has two non-empty names, so reject both degenerate
    //     splits uniformly here rather than diverging per-op deep in the builders);
    //   - SETATTR's region must hold a whole struct p9_setattr.
    // (A merely-over-msize name is caught later by the builder returning < 0 -> one
    // error CQE; THIS check is the memory-safety gate, not the protocol one.)
    if ((sqe->opcode == LOOM_OP_SYMLINK || sqe->opcode == LOOM_OP_RENAMEAT) &&
        ((u32)sqe->_resv1[1] == 0 || (u32)sqe->_resv1[1] >= count)) {
        err = -(s32)T_E_INVAL; goto fail;
    }
    if (sqe->opcode == LOOM_OP_SETATTR &&
        count < (u32)sizeof(struct p9_setattr)) { err = -(s32)T_E_INVAL; goto fail; }

    // Resolve + pin the primary handle, the SECOND handle (two-fid ops), AND the
    // registered buffer TOGETHER under one l->lock, so a concurrent
    // LOOM_REGISTER_HANDLES / LOOM_REGISTER_BUFFERS re-install cannot free any of
    // them between the read and the ref.
    spin_lock(&l->lock);
    sp = l->reg[sqe->handle_idx].spoor;
    rt = l->reg[sqe->handle_idx].rights;
    if (sp) spoor_ref(sp);
    if (has_second) {
        sp2 = l->reg[idx2].spoor;
        rt2 = l->reg[idx2].rights;
        if (sp2) spoor_ref(sp2);
    }
    if (bidx < l->n_reg_buf && l->reg_buf[bidx].burrow) {
        struct loom_reg_buffer *rb = &l->reg_buf[bidx];
        // Slice bounds (no overflow: rb->len is u32, buf_off u64): the op's
        // [buf_off, buf_off+count) must lie within the registered buffer.
        if (buf_off <= (u64)rb->len && (u64)count <= (u64)rb->len - buf_off) {
            buf     = rb->burrow;
            buf_kva = rb->kva + buf_off;
            burrow_ref(buf);                 // the I-30 buffer pin
        } else {
            buf_bad = true;
        }
    } else {
        buf_bad = true;
    }
    spin_unlock(&l->lock);

    // Failure ladder -> the single cleanup epilogue. The success path adopts
    // sp / sp2 / buf into the op and returns BEFORE `fail`.
    if (!sp)                          { err = -(s32)T_E_BADF;  goto fail; }   // empty primary slot
    if (has_second && !sp2)           { err = -(s32)T_E_BADF;  goto fail; }   // empty second slot
    if (buf_bad)                      { err = -(s32)T_E_INVAL; goto fail; }   // bad buf idx / OOB slice
    // Submit-time rights gates (I-2 / I-6; reg in AllowedObjs). Snapshot at
    // submit, NEVER re-checked at completion (I-30). need1/need2 were selected
    // from the opcode above.
    if (!(rt & need1))                { err = -(s32)T_E_ACCES; goto fail; }
    if (has_second && !(rt2 & need2)) { err = -(s32)T_E_ACCES; goto fail; }
    // #81 F2: an O_PATH (CWALKONLY) handle does NO byte/dir-content I/O. Reject the
    // content opcodes (READ/WRITE/READDIR) on a CWALKONLY-pinned Spoor -- mirrors the
    // syscall-path gate. The metadata opcodes (GETATTR/STATFS/READLINK) are the
    // fstat-equivalent class, allowed on O_PATH; the mutation opcodes (MKDIR/SETATTR/
    // ...) are the create-from-O_PATH-base pattern, legitimately allowed like
    // SYS_WALK_CREATE. Defense-in-depth: today an O_PATH dev9p fid is un-Tlopen'd so
    // the server rejects Tread/Treaddir, but this makes the block in-kernel.
    if ((sqe->opcode == LOOM_OP_READ || sqe->opcode == LOOM_OP_WRITE ||
         sqe->opcode == LOOM_OP_READDIR) && (sp->flag & CWALKONLY)) {
        err = -(s32)T_E_INVAL; goto fail;
    }
    if (dev9p_client_fid(sp, &cl, &fid) != 0) { err = -(s32)T_E_INVAL; goto fail; }   // not a dev9p Spoor
    if (has_second) {
        struct p9_client *cl2; u32 f2;
        if (dev9p_client_fid(sp2, &cl2, &f2) != 0) { err = -(s32)T_E_INVAL; goto fail; }
        // A 9P renameat/link names two fids in ONE fid namespace -- reject a
        // cross-session pair (mirrors sys_rename's same-Dev/same-p9_client gate).
        if (cl2 != cl)                             { err = -(s32)T_E_INVAL; goto fail; }
        fid2 = f2;
    }
    op = kmalloc(sizeof(*op), KP_ZERO);
    if (!op) { err = -(s32)T_E_NOMEM; goto fail; }
    op->loom        = l;
    op->client      = cl;
    op->pinned      = sp;                 // adopt the primary pin
    op->pinned2     = sp2;                // adopt the second pin (NULL for single-fid)
    op->build       = build;              // opcode-selected (above)
    op->user_data   = ud;
    op->op_fid      = fid;
    op->op_fid2     = fid2;
    op->op_offset   = sqe->offset;        // file/dir offset / GETATTR request_mask / op scalar
    op->op_count    = count;              // wire count / dest cap / pinned-region span
    op->pinned_buf  = buf;                // adopt the buffer pin
    op->buf_kva     = buf_kva;
    op->sqe         = *sqe;               // TOCTOU snapshot (mutation build-thunk decode)
    op->opcode      = sqe->opcode;
    op->terminal    = false;
    op->multishot   = false;              // single-shot (no event-source dev yet)
    op->shot_limit  = 1u;
    op->chain       = chain;
    op->rpc.on_complete = loom_async_complete;
    // Link + count BEFORE submitting (the FSYNC discipline): a synchronous submit
    // failure fires loom_async_complete, which needs the op counted + linked.
    spin_lock(&l->lock);
    op->next = l->inflight_ops;
    l->inflight_ops = op;
    l->async_inflight++;
    spin_unlock(&l->lock);
    (void)p9_client_submit_async(cl, &op->rpc, op->build, op);
    return;

fail:
    if (buf) burrow_unref(buf);
    if (sp2) spoor_clunk(sp2);
    if (sp)  spoor_clunk(sp);
    loom_chain_done(l, chain, false);
    loom_complete_inline(l, ud, err);
}

// Dispatch ONE already-copied-to-kernel SQE (`sqe` is a kernel-private snapshot,
// NEVER the shared ring slot -- ring TOCTOU, LOOM.md 6.1). Either completes
// inline (NOP / a rejected op posts a CQE) or submits an async 9P op whose reply
// drives loom_async_complete later. Never re-reads the shared ring.
//
// `chain` (Loom-5b) is non-NULL when dispatching a LINK/DRAIN chain entry: the
// entry was claimed INFLIGHT by loom_admit_chain; this sets its real terminal
// state on an inline completion (loom_chain_done) or wires the async back-pointer
// + leaves it INFLIGHT (loom_async_complete records the eventual reply). The fast
// path passes chain == NULL and behaves exactly as the Loom-3 dispatch.
static void loom_submit_one(struct Loom *l, const struct loom_sqe *sqe,
                            struct loom_chain_op *chain) {
    u64 ud = sqe->user_data;

    // Accept the multishot + ordering flags; LINK/DRAIN are consumed by the chain
    // scheduler (loom_drain_sq routes them) and IGNORED here. Reject CQE_SKIP
    // (Loom-5b defers it -- suppressing a success CQE breaks loom_order.tla's
    // EveryDoneOpPosted; it needs a spec carve-out first) + any unknown bit, so a
    // future flag is never silently ignored.
    if (sqe->flags & ~(u32)(LOOM_SQE_MULTISHOT | LOOM_SQE_LINK | LOOM_SQE_DRAIN)) {
        loom_chain_done(l, chain, false);
        loom_complete_inline(l, ud, -(s32)T_E_INVAL);
        return;
    }
    bool multishot = (sqe->flags & LOOM_SQE_MULTISHOT) != 0;
    // MULTISHOT may not combine with chain ordering: a chain member must be
    // single-completion (loom_order.tla models one CQE per op). This rejects both
    // the explicit MULTISHOT+LINK/DRAIN combo AND a bare MULTISHOT op routed into
    // a non-empty chain. Deterministic for one batch; mixing the two across enters
    // is the unspecced usage this declines (LOOM.md 10).
    if (chain && multishot) {
        loom_chain_done(l, chain, false);
        loom_complete_inline(l, ud, -(s32)T_E_INVAL);
        return;
    }

    switch (sqe->opcode) {
    case LOOM_OP_NOP:
        // The io_uring smoke op: completes immediately, no engine, no handle. An
        // inline op cannot re-arm (no async reply), so MULTISHOT degrades to a
        // single terminal CQE here -- the mechanism is async-only (the FSYNC path).
        loom_chain_done(l, chain, true);
        loom_complete_inline(l, ud, 0);
        return;

    case LOOM_OP_FSYNC: {
        if (sqe->handle_idx >= LOOM_MAX_REG_HANDLES) {
            loom_chain_done(l, chain, false);
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

        if (!sp) {                                                          // empty slot
            loom_chain_done(l, chain, false);
            loom_complete_inline(l, ud, -(s32)T_E_BADF);
            return;
        }
        // Submit-time rights gate (I-2 / I-6; the spec's reg in AllowedObjs).
        // Fsync is a durability barrier on written data, so it requires the
        // handle to have been opened for write (RIGHT_WRITE, omode-derived at
        // open -- A-3 F1). A read-only registered handle is denied here, never
        // re-checked at completion (I-30).
        if (!(rt & RIGHT_WRITE)) {
            spoor_clunk(sp);
            loom_chain_done(l, chain, false);
            loom_complete_inline(l, ud, -(s32)T_E_ACCES);
            return;
        }
        struct p9_client *cl; u32 fid;
        if (dev9p_client_fid(sp, &cl, &fid) != 0) {
            // Not a dev9p-backed Spoor (devsrv conn / devramfs / ...): no 9P
            // engine to dispatch against.
            spoor_clunk(sp);
            loom_chain_done(l, chain, false);
            loom_complete_inline(l, ud, -(s32)T_E_INVAL);
            return;
        }
        struct loom_async_op *op = kmalloc(sizeof(*op), KP_ZERO);
        if (!op) {
            spoor_clunk(sp);
            loom_chain_done(l, chain, false);
            loom_complete_inline(l, ud, -(s32)T_E_NOMEM);
            return;
        }
        op->loom        = l;
        op->client      = cl;
        op->pinned      = sp;               // adopt the pin ref
        op->build       = loom_build_fsync; // re-run verbatim on each re-arm (multishot)
        op->user_data   = ud;
        op->op_fid      = fid;
        op->op_arg      = (sqe->len != 0) ? 1u : 0u;   // FSYNC: len != 0 -> datasync
        op->opcode      = LOOM_OP_FSYNC;
        op->terminal    = false;
        op->multishot   = multishot;
        op->chain       = chain;            // Loom-5b back-pointer (NULL fast path)
        // SYNTHETIC terminal bound for the FSYNC multishot vehicle: the total
        // number of CQEs (>= 1) the stream emits before its terminal. Carried in
        // sqe->offset (FSYNC ignores the offset). Loom-6's real event-fd READ
        // terminates on the event source's EOF/error, not a count.
        op->shot_limit  = multishot
            ? (sqe->offset == 0          ? 1u
               : sqe->offset > 0xFFFFFFFFull ? 0xFFFFFFFFu
               : (u32)sqe->offset)
            : 1u;
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

    case LOOM_OP_READ:
    case LOOM_OP_WRITE:
    case LOOM_OP_READDIR:
    case LOOM_OP_READLINK:
    case LOOM_OP_GETATTR:
    case LOOM_OP_STATFS:
    case LOOM_OP_SETATTR:
    case LOOM_OP_MKDIR:
    case LOOM_OP_MKNOD:
    case LOOM_OP_SYMLINK:
    case LOOM_OP_UNLINKAT:
    case LOOM_OP_RENAMEAT:
    case LOOM_OP_LINK:
        // Buffer-backed payload ops (Loom-6a READ/WRITE + Loom-6b-1 read-shaped
        // metadata + Loom-6b-2 metadata mutation). Each names a registered file/
        // dir fid + a registered buffer (the names / the input struct); the
        // two-fid ops name a second registered fid too. loom_submit_payload pins
        // all of them (the I-30 submit-time pins) and bounds-checks every slice +
        // sub-length. MULTISHOT needs a real event-source dev (a file fid replies
        // once, so a multishot file op would spin re-issuing the same Tmsg) --
        // reject the combo until that dev exists; the multishot mechanism stays
        // validated by the synthetic FSYNC vehicle.
        if (multishot) {
            loom_chain_done(l, chain, false);
            loom_complete_inline(l, ud, -(s32)T_E_INVAL);
            return;
        }
        loom_submit_payload(l, sqe, chain);
        return;

    default:
        // The remaining in-range opcodes are the fid-lifecycle / direct-
        // descriptor ops (WALK / LOPEN / LCREATE / CLUNK) -- they mint, open, or
        // release a fid, but our registered handles wrap already-OPEN fids, so
        // these need the registered-slot install/release surface (the io_uring
        // direct-descriptor analog) + its own audit: a documented post-6b seam
        // (LOOM.md, task #916). WIRE_PASSTHROUGH stays reserved (LOOM.md 7). All
        // post -ENOSYS; out-of-range opcodes are -EINVAL.
        loom_chain_done(l, chain, false);
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
// none remain (no further completion can arrive). To keep the borrowed client
// alive across the caller's pump -- which runs AFTER l->lock is dropped -- this
// takes an EXTRA ref on that op's pinned Spoor (the I-30 pin substrate: a live
// dev9p Spoor implies a live client). F1: without it, between this unlock and the
// caller's deref a concurrent reaper (a sibling SYS_LOOM_ENTER, or -- new at
// Loom-4 -- the SQPOLL kthread's own next iteration) that finds the op terminal
// AND a re-register that drops the registered-table ref could free the Spoor ->
// the client -> a UAF in the pump. The single Loom-3 reaper (same caller, after
// its own wait) made this safe before; Loom-4's second concurrent reaper does
// not. The caller MUST spoor_clunk(*pin_out) after the pump (process context, may
// sleep). *pin_out is ALWAYS written (NULL when no in-flight op; the pin + its ref
// are set together when a client is returned), so a caller need not pre-init it.
// Under l->lock.
static struct p9_client *loom_first_inflight_client(struct Loom *l,
                                                    struct Spoor **pin_out) {
    struct p9_client *cl = NULL;
    *pin_out = NULL;   // R2-F1: define the out-param unconditionally (footgun-proof for future callers)
    spin_lock(&l->lock);
    for (struct loom_async_op *op = l->inflight_ops; op; op = op->next) {
        if (!op->terminal) {
            cl = op->client;
            *pin_out = op->pinned;
            spoor_ref(op->pinned);   // borrow-guard: keep the Spoor (=> client) alive past the unlock
            break;
        }
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
        if (done->pinned_buf) burrow_unref(done->pinned_buf);   // Loom-6: buffer pin
        if (done->pinned2)    spoor_clunk(done->pinned2);        // Loom-6b-2: second fid pin
        spoor_clunk(done->pinned);   // release the I-30 pin (may sleep)
        kfree(done);
        done = next;
    }
}

// Re-arm (re-issue) every MORE-pending multishot op the CQ can admit (Loom-5;
// specs/loom_multishot.tla PostShot -> re-arm). Runs in the drive loop OUTSIDE
// c->lock and l->lock: p9_client_submit_async takes c->lock, and the seam
// contract forbids loom_async_complete (which runs UNDER c->lock) from
// re-entering it -- so the re-issue is deferred to here.
//
// Each re-arm reserves one CQ slot (async_inflight++) under l->lock -- the same
// submit-time admission as loom_drain_sq -- so a MORE shot ALWAYS has a slot when
// it completes (CqNeverOverfull; loom_post_cqe's full-CQ drop stays unreachable).
// A rearm op the CQ cannot yet admit stays flagged (back-pressure): the next
// enter / SQPOLL iteration re-arms it once userspace reaps a slot -- the shot is
// HELD, never dropped.
//
// The pin (op->pinned) is held throughout: the op is NON-terminal here (rearm,
// not terminal), so loom_reap_terminal never reclaims it and loom_free cannot run
// (the drive-loop caller holds a loom ref), so the borrowed op->client stays
// valid across the submit -- no F1 borrow-guard ref is needed. The claim (clear
// rearm + bump async_inflight) is atomic under l->lock, so two concurrent drivers
// (a sibling ENTER + the SQPOLL kthread) never double-issue the same op. A
// synchronous submit failure fires loom_async_complete (status < 0 -> terminal),
// which releases the reservation -- consistent.
static void loom_rearm_pending(struct Loom *l) {
    for (;;) {
        struct loom_async_op *op = NULL;
        spin_lock(&l->lock);
        for (struct loom_async_op *o = l->inflight_ops; o; o = o->next) {
            if (!o->rearm) continue;
            if (loom_cq_ready(l) + l->async_inflight >= l->cq_entries) continue; // back-pressure
            o->rearm = false;
            __atomic_fetch_sub(&l->rearm_pending, 1u, __ATOMIC_RELEASE);  // claimed for re-arm
            l->async_inflight++;        // reserve the next shot's CQE slot
            op = o;
            break;
        }
        spin_unlock(&l->lock);
        if (!op) break;                 // nothing admittable -> held (or none pending)
        (void)p9_client_submit_async(op->client, &op->rpc, op->build, op);
    }
}

// =============================================================================
// Loom-5b: the LINK/DRAIN held-submission chain scheduler (specs/loom_order.tla).
// =============================================================================

// True if chain entry `e` is past its DRAIN ordering gates. Called UNDER l->lock
// (reads chain states + async_inflight). Two gates (loom_order.tla DrainAdmits):
//   - after-drain: every EARLIER chain entry that is a drain must be done, so a
//     post-drain op waits for the barrier;
//   - drain-self: if `e` is itself a drain, EVERY earlier chain entry must be done
//     AND async_inflight must be 0. The async_inflight==0 catches the prior FAST
//     async ops (not in the chain) -- once the chain is non-empty all subsequent
//     ops route to it, so async_inflight, when every chain-before-e is done,
//     counts only ops submitted BEFORE the chain started. Together: a drain
//     executes only after ALL prior submitted ops are done (the full barrier).
static bool loom_chain_drain_admits(struct Loom *l, struct loom_chain_op *e) {
    bool e_is_drain = e->drain;
    for (struct loom_chain_op *j = l->chain; j && j != e; j = j->chain_next) {
        bool j_done = (j->state == LOOM_CHAIN_DONE_OK ||
                       j->state == LOOM_CHAIN_DONE_FAIL ||
                       j->state == LOOM_CHAIN_CANCELLED);
        if (j->drain && !j_done)   return false;   // after-drain: wait behind an earlier barrier
        if (e_is_drain && !j_done) return false;   // drain-self: wait for every chain-before op
    }
    // Drain-self: wait for the prior FAST async ops. async_inflight counts ops with
    // an outstanding request; rearm_pending counts a FAST MULTISHOT op that posted a
    // MORE shot and is awaiting re-issue (a multishot op is never in the chain, so a
    // rearm-pending op is always a prior FAST op, submitted before the chain started
    // -- once the chain is non-empty every later op routes to it). A rearm-pending op
    // is logically still in flight (its stream is not done), but it is momentarily
    // NOT in async_inflight (async_inflight-- at the MORE completion, ++ at re-arm;
    // their SUM is the op's reservation, invariant across the transition). Without
    // the rearm_pending term a drain admitted via loom_enter's submit-phase
    // loom_admit_chain (which has no preceding loom_rearm_pending) could jump ahead
    // of a live FAST multishot stream (DrainOrdered violation; audit F1). The park
    // cond reads rearm_pending the same way.
    if (e_is_drain && (l->async_inflight != 0 ||
                       __atomic_load_n(&l->rearm_pending, __ATOMIC_ACQUIRE) != 0u))
        return false;
    return true;
}

// Reclaim terminal chain entries. SAFE only when NO entry is HELD: a HELD entry
// may still read a predecessor's terminal result (the link gate admits on DONE_OK
// / cancels on DONE_FAIL; the drain gate counts prior-done), so reclaiming a
// terminal predecessor under a live HELD successor would lose the dependency.
// When no entry is HELD, every entry is INFLIGHT or terminal; an INFLIGHT entry
// was already admitted (its gates satisfied) and never re-consults predecessors,
// so freeing the terminal entries is safe. (A non-empty chain is the routing
// signal -- so this also lets the ring return to fast-path dispatch once the
// chain fully settles.) Frees the loom_chain_op structs; the backing
// loom_async_op of an INFLIGHT entry is reaped separately by loom_reap_terminal.
static void loom_reclaim_chain(struct Loom *l) {
    struct loom_chain_op *freelist = NULL;
    spin_lock(&l->lock);
    bool any_held = false;
    for (struct loom_chain_op *e = l->chain; e; e = e->chain_next) {
        if (e->state == LOOM_CHAIN_HELD) { any_held = true; break; }
    }
    if (!any_held) {
        struct loom_chain_op **pp = &l->chain;
        l->chain_tail = NULL;
        while (*pp) {
            struct loom_chain_op *e = *pp;
            bool terminal = (e->state == LOOM_CHAIN_DONE_OK ||
                             e->state == LOOM_CHAIN_DONE_FAIL ||
                             e->state == LOOM_CHAIN_CANCELLED);
            if (terminal) {
                *pp = e->chain_next;          // unlink
                if (l->chain_len > 0) l->chain_len--;
                e->chain_next = freelist;
                freelist = e;
            } else {
                l->chain_tail = e;            // last surviving (INFLIGHT) entry
                pp = &e->chain_next;
            }
        }
    }
    spin_unlock(&l->lock);
    while (freelist) {
        struct loom_chain_op *next = freelist->chain_next;
        kfree(freelist);
        freelist = next;
    }
}

// Walk the LINK/DRAIN chain head->tail and take ONE action per pass (dispatch an
// admittable HELD op, or cancel a HELD op whose linked predecessor failed),
// looping until nothing is actionable, then reclaim. Runs in the drive loops
// (loom_enter after submit, loom_wait_for_completions / loom_sqpoll_main loop top)
// OUTSIDE c->lock -- loom_submit_one re-enters the 9P engine (c->lock) for an
// async op, which loom_async_complete (running UNDER c->lock) must NOT do, so the
// dispatch is deferred to here (the seam contract, mirroring loom_rearm_pending).
//
// Each action posts exactly one CQE, so it needs CQ admission room beyond every
// reserved completion (loom_cq_ready + async_inflight < cq_entries). No room ->
// hold the whole chain (back-pressure; re-tried when userspace reaps + re-enters,
// or the SQPOLL kthread wakes). A dispatch claims the entry INFLIGHT under l->lock
// so a concurrent admit cannot double-dispatch it; loom_submit_one then sets the
// real terminal (inline) or leaves INFLIGHT (async). The cancel-cascade is a
// single head->tail walk: a just-cancelled victim becomes the non-ok predecessor
// of the next iteration's walk.
//
// CONCURRENCY CONTRACT (audit F4): the chain scheduler assumes EFFECTIVELY ONE
// admitter per ring -- the SQPOLL kthread is the sole admitter on an SQPOLL ring,
// and a non-SQPOLL ring follows the io_uring single-producer SQ contract (one
// submitting thread). The INFLIGHT claim makes the chain memory-safe even under
// concurrent non-SQPOLL drivers (no UAF / double-dispatch / double-CQE), but two
// concurrent admitters can still over-reserve the CQ (the inherited Loom-3 over-
// admit window: the room check at the top of this loop and the async_inflight bump
// in loom_submit_one are not atomic). The cancel leg is hardened (revert-to-HELD +
// retry below, so a cancel is never lost); the dispatch leg's amplified residual --
// a dropped chain-op TERMINAL CQE under concurrent over-admission (the chain still
// continues; only that op's completion notification is missed) -- is the SAME
// accepted Loom-3 residual, whose exact-concurrent-admission coordination is OWED
// to Loom-6 (with the deterministic two-thread-same-loom_fd SMP harness, carried
// since #841). v1.0 has no userspace Loom consumer, so concurrent chained ENTERs
// cannot fire it. See docs/reference/107-loom.md "Known caveats".
static void loom_admit_chain(struct Loom *l) {
    for (;;) {
        struct loom_chain_op *to_dispatch = NULL;
        struct loom_chain_op *to_cancel = NULL;
        u64 cancel_ud = 0;

        spin_lock(&l->lock);
        if (loom_cq_ready(l) + l->async_inflight >= l->cq_entries) {
            spin_unlock(&l->lock);      // CQ full -> hold (back-pressure)
            break;
        }
        struct loom_chain_op *prev = NULL;
        for (struct loom_chain_op *e = l->chain; e; prev = e, e = e->chain_next) {
            if (e->state != LOOM_CHAIN_HELD) continue;
            // Link cancel-cascade: an immediate LINKED predecessor that finished
            // non-ok (fail / cancelled) cancels this op (loom_order.tla
            // CancelVictim). `prev` is the submission-order predecessor (NULL at
            // the chain head).
            if (prev && prev->link &&
                (prev->state == LOOM_CHAIN_DONE_FAIL ||
                 prev->state == LOOM_CHAIN_CANCELLED)) {
                e->state  = LOOM_CHAIN_CANCELLED;
                to_cancel = e;
                cancel_ud = e->sqe.user_data;
                break;
            }
            // Link gate: a linked successor waits until its predecessor is done ok
            // (loom_order.tla LinkAdmits). A non-linking predecessor (or none)
            // leaves this open.
            if (prev && prev->link && prev->state != LOOM_CHAIN_DONE_OK) continue;
            // Drain gates (loom_order.tla DrainAdmits).
            if (!loom_chain_drain_admits(l, e)) continue;
            // Gates open -> claim INFLIGHT (so a concurrent admit never double-
            // dispatches); loom_submit_one overwrites with the real state.
            e->state    = LOOM_CHAIN_INFLIGHT;
            to_dispatch = e;
            break;
        }
        spin_unlock(&l->lock);

        if (to_cancel) {
            if (loom_post_cqe(l, cancel_ud, -(s32)T_E_CANCELED, 0) != 0) {
                // The CQ filled between the top-of-loop room check and this post
                // (a concurrent driver consumed the reserved slot -- the inherited
                // Loom-3 over-admit window). Do NOT drop the -ECANCELED: revert the
                // victim to HELD so a later admit pass re-cancels + retries the post
                // once userspace reaps a slot (EveryDoneOpPosted -- a cancel is
                // never lost, only deferred). audit F2 (cancel leg).
                spin_lock(&l->lock);
                to_cancel->state = LOOM_CHAIN_HELD;
                spin_unlock(&l->lock);
                break;
            }
            continue;
        }
        if (to_dispatch) {
            loom_submit_one(l, &to_dispatch->sqe, to_dispatch);
            continue;
        }
        break;                          // nothing actionable
    }
    loom_reclaim_chain(l);
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
        // SQPOLL ring: the kthread is the SOLE driver (rearm / admit / pump). A
        // min_complete ENTER reaches this wait even on an SQPOLL ring; it MUST NOT
        // drive loom_rearm_pending / loom_admit_chain / the pump here -- doing so
        // writes l->chain + l->cq_tail (under l->lock) while the parked kthread
        // reads them lock-free in loom_sqpoll_park_cond (a data race, R4-F1), and
        // breaks the Loom-5 F4 single-admitter premise (widening the over-admit
        // residual to single-threaded SQPOLL+chain). Instead just sample + wait
        // for the kthread to post CQEs. "Nothing more can complete" mirrors the
        // park cond EXACTLY: no inflight op AND no held re-arm AND no held chain
        // (a back-pressured kthread-owned stream resumes on the post-reap wake, so
        // the ENTER caller must sleep on it, not give up). Reads under l->lock are
        // race-free vs the kthread's own l->lock-held chain/rearm writes.
        if (l->sqpoll) {
            spin_lock(&l->lock);
            u32 sq_ready    = loom_cq_ready(l);
            bool sq_more    = (l->async_inflight > 0)
                              || __atomic_load_n(&l->rearm_pending, __ATOMIC_ACQUIRE) != 0u
                              || l->chain != NULL;
            spin_unlock(&l->lock);
            if (sq_ready >= min_complete) break;
            if (!sq_more)                 break;

            pw.ready = false;
            bool sq_sleep;
            spin_lock(&l->lock);
            poll_waiter_list_register(&l->cq_waiters, &pw);
            sq_more  = (l->async_inflight > 0)
                       || __atomic_load_n(&l->rearm_pending, __ATOMIC_ACQUIRE) != 0u
                       || l->chain != NULL;
            sq_sleep = (loom_cq_ready(l) < min_complete) && sq_more;
            spin_unlock(&l->lock);
            if (!sq_sleep) { poll_waiter_list_unregister(&pw); continue; }
            int ss = sleep(&r, loom_cqw_cond, &pw);
            poll_waiter_list_unregister(&pw);
            if (ss == SLEEP_INTR) break;   // #811: ENTER caller's Proc group-terminating
            continue;                       // woken by a posted CQE -> re-sample
        }

        // Re-arm any MORE-pending multishot op (Loom-5) BEFORE the give-up sample:
        // a stream that posted a MORE shot last pump (async_inflight--, rearm set)
        // must re-issue (async_inflight++) here, else the inflight==0 give-up arm
        // would prematurely abandon a live stream. Re-issue is outside l->lock.
        loom_rearm_pending(l);

        // Admit any newly-unblocked LINK/DRAIN chain op (Loom-5b): a completion
        // last pump may have opened a successor's gate (predecessor done ok) or
        // require cancelling the chain (predecessor failed). Like the re-arm, this
        // runs BEFORE the give-up sample so a freshly-admitted op is counted as
        // in-flight + the inflight==0 arm does not abandon a chain mid-dispatch.
        // Dispatch is outside l->lock (the seam contract).
        loom_admit_chain(l);

        // CqWaitCommitOrSleep (the give-up arms): enough completions, or nothing
        // more can ever complete. Sampled under l->lock (cq_tail + async_inflight).
        spin_lock(&l->lock);
        u32 ready    = loom_cq_ready(l);
        u32 inflight = l->async_inflight;
        spin_unlock(&l->lock);
        if (ready >= min_complete) break;
        if (inflight == 0)         break;

        // Try to become the elected reader and drive one frame. The borrow-guard
        // ref (F1) keeps the client alive across the pump even if a concurrent
        // reaper + a re-register drop the op's own pin + the registered-table ref.
        struct Spoor *cl_pin = NULL;
        struct p9_client *cl = loom_first_inflight_client(l, &cl_pin);
        if (!cl) continue;                     // raced: the op completed/reaped -> re-check
        int rc = p9_client_reader_pump_once(cl);
        spoor_clunk(cl_pin);                   // cl not derefed below -> release the guard now
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

// Consume up to `budget` SQEs from the SQ index ring in SQ-index order, copying
// each to kernel memory (ring TOCTOU) and either dispatching it (fast path,
// loom_submit_one) or enqueueing it HELD in the LINK/DRAIN chain (Loom-5b).
// Returns the number consumed. Shared by SYS_LOOM_ENTER (the non-SQPOLL submit
// phase) and the SQPOLL kthread (zero-syscall submit). l->lock NOT held on entry.
//
// sq_tail is user-owned (advisory): bound the work against it but never trust it
// for indexing. The consume position is the kernel-private sq_head; the SQ-index
// ring slot it points at is user-written, so it is range-checked before it
// indexes the SQE array (the SA-1 discipline on the SQ side).
//
// F1: each per-slot claim (the sq_head read + advance + the SQE read+copy) runs
// under l->lock so two concurrent consumers on the same ring (two ENTERs of a
// multi-thread Proc, or an ENTER racing the SQPOLL kthread) never read the same
// sq_head -> never double-dispatch a slot or lose an sq_head update; each
// iteration atomically claims exactly one slot. loom_submit_one / the chain
// enqueue run OUTSIDE the lock (a separate, non-nested acquisition).
//
// Loom-5b chain routing: an SQE is ORDERING-RELEVANT -- it goes HELD in l->chain
// rather than dispatching immediately -- iff it sets LINK/DRAIN, OR the chain is
// already non-empty (a pending drain/link forces every subsequent op to order
// after it; an independent op so routed just admits immediately). The chain only
// GROWS during a drain call (loom_admit_chain/reclaim run between calls, not
// inside), so the routing is monotone within a batch: once non-empty, every
// later SQE in the batch joins it -- which keeps a LINK group together even when
// split across concurrent drainers (each op's own link flag + chain order encode
// the dependency). Independent fast-path ops (chain empty, no flags) dispatch
// exactly as Loom-3. A drain-blocked chain is bounded at cq_entries entries so a
// flood cannot grow it without bound (held ops don't count in async_inflight, so
// the CQ admission alone would not bound them).
static u32 loom_drain_sq(struct Loom *l, u32 budget) {
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    u32 *sq_array         = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);

    if (budget > l->sq_entries) budget = l->sq_entries;   // bound a hostile to_submit
    u32 submitted = 0;
    for (u32 i = 0; i < budget; i++) {
        struct loom_sqe ksqe;
        bool have;
        bool to_chain;
        spin_lock(&l->lock);
        // Loom-5b chain back-pressure: a non-empty chain at the length cap holds
        // further consumes until it drains (admit + reclaim free entries between
        // drain calls). The head op is always eventually admittable, so the chain
        // shrinks -> no deadlock. Checked before claiming a slot so sq_head is not
        // advanced past an SQE we decline to consume.
        if (l->chain != NULL && l->chain_len >= l->cq_entries) {
            spin_unlock(&l->lock);
            break;
        }
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
        // exact concurrent admission is the Loom-4 coordination.) A chain op held
        // here will need its CQ slot only when loom_admit_chain dispatches it (the
        // admit-time gate is the load-bearing one for held ops); the consume-time
        // gate is a conservative shared back-pressure for both paths.
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
        // Route under the SAME lock so the chain-non-empty test is consistent with
        // the snapshot we just took (the chain only grows during this call).
        to_chain = have && ((ksqe.flags & (LOOM_SQE_LINK | LOOM_SQE_DRAIN)) != 0 ||
                            l->chain != NULL);
        spin_unlock(&l->lock);
        if (!have) continue;                                // bad indirection -> dropped

        if (to_chain) {
            // Ordering-relevant: enqueue HELD; loom_admit_chain dispatches it once
            // its gates open. The allocation is OUTSIDE l->lock (kmalloc may
            // sleep); the append re-takes the lock. An OOM here posts -ENOMEM for
            // this op (it was consumed); a LINK predecessor that fails to enqueue
            // leaves its successor to route as the new chain head (best-effort
            // under memory pressure).
            struct loom_chain_op *ce = kmalloc(sizeof(*ce), KP_ZERO);
            if (!ce) {
                loom_complete_inline(l, ksqe.user_data, -(s32)T_E_NOMEM);
                submitted++;
                continue;
            }
            ce->sqe   = ksqe;
            ce->link  = (ksqe.flags & LOOM_SQE_LINK)  != 0;
            ce->drain = (ksqe.flags & LOOM_SQE_DRAIN) != 0;
            ce->state = LOOM_CHAIN_HELD;
            spin_lock(&l->lock);
            ce->chain_next = NULL;
            if (l->chain_tail) l->chain_tail->chain_next = ce;
            else               l->chain = ce;
            l->chain_tail = ce;
            l->chain_len++;
            spin_unlock(&l->lock);
            submitted++;
        } else {
            loom_submit_one(l, &ksqe, NULL);                // fast path (Loom-3)
            submitted++;
        }
    }
    return submitted;
}

int loom_enter(struct Loom *l, u32 to_submit, u32 min_complete, u32 flags) {
    if (!l || l->magic != LOOM_MAGIC) return -1;
    if (flags & ~LOOM_ENTER_VALID)    return -1;

    // --- SUBMIT phase. ---
    // On an SQPOLL ring the poll-thread OWNS submission (LOOM.md 8.6): ENTER does
    // NOT consume SQEs -- it WAKES the (possibly idled) kthread so it drains the
    // SQ the caller just produced into, clearing NEED_WAKEUP. `submitted` is
    // reported 0 (the kthread, not this call, consumes). On a non-SQPOLL ring,
    // ENTER consumes inline.
    u32 submitted = 0;
    if (l->sqpoll) {
        struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
        __atomic_fetch_and(&h->flags, ~(u32)LOOM_RING_SQ_NEED_WAKEUP, __ATOMIC_RELEASE);
        wakeup(&l->sqpoll_park);
    } else {
        submitted = loom_drain_sq(l, to_submit);
        // Loom-5b: dispatch the chain head(s) the submit just enqueued (+ any
        // inline LINK cancel-cascade). Done here, not only in the wait loop, so a
        // submit-only enter (min_complete == 0 / NONBLOCK) still starts the chain
        // -- the wait loop below runs only when blocking.
        loom_admit_chain(l);
    }

    // --- WAIT / REAP phase (Loom-4). Block until min_complete CQEs are posted or
    // no completion can arrive. On a non-SQPOLL ring the caller drives the elected
    // reader itself, or -- when a sibling thread of the same Proc already holds the
    // reader role -- sleeps on the ring's CQ wait-list until that reader posts a
    // CQE. On an SQPOLL ring the kthread is USUALLY the reader, so a min_complete
    // wait usually takes the sleep arm (loom_wait_for_completions tries the pump,
    // gets P9_PUMP_BUSY=0 from the kthread's reader_active, and sleeps on
    // cq_waiters); in the narrow window where the kthread is between frames an ENTER
    // caller may briefly win the reader role + drive a frame itself (the kthread
    // then yields on BUSY, SA-2). Either way a posted CQE wakes the sleeper. The
    // wait is death-interruptible (#811) and flood-bounded. ---
    if (min_complete > 0 && !(flags & LOOM_ENTER_NONBLOCK)) {
        loom_wait_for_completions(l, min_complete, submitted);
    }

    loom_reap_terminal(l);
    return (int)submitted;
}

// =============================================================================
// Loom-4c: the SQPOLL poll-thread (LOOM.md 8.6). A per-ring kproc() kthread that
// drains the SQ + drives the elected reader so steady-state submission is
// zero-syscall. Spawned by loom_start_sqpoll at SYS_LOOM_SETUP; joined by
// loom_free. Touches only the still-allocated `struct Loom` (the join is the
// lifetime guarantee; it holds no loom ref).
// =============================================================================

// The frame-boundary idle deadline the kthread arms while draining replies: it
// bounds how long a between-frames recv blocks before the kthread re-checks
// sqpoll_stopping + the SQ. Short enough that teardown is prompt; long enough
// that a busy ring isn't waking spuriously. (A mid-frame recv is NOT deadline-
// bounded -- the body must complete to keep the stream synced, #841 -- so a
// Byzantine server mid-frame can delay a stop until the frame finishes / EOFs;
// the trusted v1.0 servers always complete promptly.)
#define LOOM_SQPOLL_IDLE_NS  (10ull * 1000ull * 1000ull)   // 10 ms

// Park predicate (runs under the park Rendez lock during sleep -- MUST NOT take
// l->lock, which is above the rendez in the global order). Wake when stopping is
// set OR there is ADMITTABLE work AND CQ room for its completion. "Work" is a
// pending SQE (a new submission) OR a deferred re-arm (a back-pressured multishot
// MORE shot, Loom-5). The inflight count is deliberately NOT consulted: the
// kthread parks ONLY when async_inflight == 0 (it drained everything), and nothing
// adds an inflight op while it sleeps (only the kthread's own loom_drain_sq /
// loom_rearm_pending do) -- so at park time there is no in-flight request, hence no
// concurrent loom_async_complete writing l->cq_tail; the kthread is the SOLE
// cq_tail writer here (a back-pressured rearm op is QUIESCENT -- its reply was
// already demuxed, it holds no tag). There is nothing to wake FOR but new SQEs, a
// CQ slot freeing for a held re-arm, or stop.
//
// F2: gating on "SQ non-empty" ALONE busy-loops when the CQ is full and sq_tail is
// pinned ahead (a non-reaping / bursting user) -- loom_drain_sq breaks on the
// admission check, submits 0, the cond fires again, sleep returns WITHOUT blocking
// (it never sched()s), repeat: a tight 100%-CPU spin. A full CQ frees only when the
// user reaps (advances cq_head) and ENTER-wakes us (the NEED_WAKEUP contract), so
// park until an admittable slot exists. Loom-5: the same admission gate covers a
// held re-arm -- a back-pressured multishot op (rearm_pending > 0, async_inflight
// 0) would otherwise strand, because the user reaps without a new SQE so the
// SQE-only cond stays 0 forever; gating ALSO on rearm_pending lets the post-reap
// ENTER-wake resume the stream. Both reads are race-free HERE: in the park branch
// async_inflight == 0, so the kthread is the SOLE writer of l->cq_tail -- no
// concurrent loom_post_cqe -- and sq_head is the kthread's own single-writer field.
// cq_head is user-owned (advisory); loom_cq_ready clamps it, so a hostile value
// only makes the kthread fill-to-cq_entries then park, never an OOB or unbounded
// spin. Register-then-observe: an ENTER that bumped sq_tail / a completion that set
// rearm_pending before this sample is caught here; one after is caught by the
// ENTER's wakeup of the park (and a wake while still CQ-full re-evaluates to 0 ->
// re-sleep, NOT a spin).
static int loom_sqpoll_park_cond(void *arg) {
    struct Loom *l = (struct Loom *)arg;
    if (__atomic_load_n(&l->sqpoll_stopping, __ATOMIC_ACQUIRE)) return 1;
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    bool has_sqe   = __atomic_load_n(&h->sq_tail, __ATOMIC_ACQUIRE) != l->sq_head;
    bool has_rearm = __atomic_load_n(&l->rearm_pending, __ATOMIC_ACQUIRE) != 0u;
    // Loom-5b: a HELD chain op that could not admit for lack of CQ room (a drain
    // or link successor back-pressured) keeps l->chain non-empty -- it resumes
    // only when userspace reaps a slot + ENTER-wakes us. l->chain is the kthread's
    // OWN single-writer field on an SQPOLL ring (it is the sole drainer / admitter
    // / reclaimer; loom_async_complete touches chain->state, never the l->chain
    // list head), so this read is same-thread + race-free. At park async_inflight
    // is 0, so a non-empty chain here is either CQ-back-pressured-held or
    // not-yet-reclaimed-terminal; both want a CQ-room wake (a spurious wake just
    // re-reclaims). Without this a back-pressured chain would strand on the SQE-
    // only park cond (the user reaps without a new SQE).
    bool has_chain = (l->chain != NULL);
    if (!has_sqe && !has_rearm && !has_chain) return 0;  // nothing to do -> stay parked
    return (loom_cq_ready(l) < l->cq_entries) ? 1 : 0;   // work pending -> wake iff the CQ can admit
}

void loom_sqpoll_main(void *arg) {
    struct Loom *l = (struct Loom *)arg;
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    for (;;) {
        if (__atomic_load_n(&l->sqpoll_stopping, __ATOMIC_ACQUIRE)) break;

        // Zero-syscall submit: drain whatever SQEs userspace produced. NOPs
        // complete inline; FSYNCs go async (added to inflight_ops).
        (void)loom_drain_sq(l, l->sq_entries);

        // Re-arm any MORE-pending multishot op (Loom-5): a stream that posted a
        // MORE shot last iteration re-issues here (the kthread is the driver). Like
        // the loom_wait_for_completions loop top, before the inflight sample so a
        // re-armed op is seen as inflight + pumped this iteration.
        loom_rearm_pending(l);

        // Admit any newly-unblocked LINK/DRAIN chain op (Loom-5b): the kthread is
        // the chain's driver on an SQPOLL ring -- a completion last iteration may
        // have opened a successor's gate or require the cancel-cascade. Before the
        // inflight sample so a freshly-dispatched chain op is pumped this iteration
        // and the kthread does not park while the chain still has admittable work.
        loom_admit_chain(l);

        bool inflight;
        spin_lock(&l->lock);
        inflight = (l->async_inflight > 0);
        spin_unlock(&l->lock);

        if (inflight) {
            // Drive the elected reader so async replies become CQEs (each post
            // wakes cq_waiters -- a min_complete ENTER caller). The reader is
            // gated to a deadline-capable transport (register gate), so a
            // between-frames recv returns IDLE at the boundary within
            // LOOM_SQPOLL_IDLE_NS, letting this loop re-check sqpoll_stopping.
            struct Spoor *cl_pin = NULL;
            struct p9_client *cl = loom_first_inflight_client(l, &cl_pin);
            if (cl) {
                u64 deadline = timer_now_ns() + LOOM_SQPOLL_IDLE_NS;
                int rc = p9_client_reader_pump_once_deadline(cl, deadline);
                spoor_clunk(cl_pin);   // release the F1 borrow-guard (cl not derefed below)
                // PROGRESS: demuxed a frame (a CQE may have posted). IDLE: the
                // boundary deadline lapsed, stream still synced. BUSY: a peer ENTER
                // momentarily holds the reader -- yield (SA-2) instead of tight-
                // looping until it releases the role / the frame arrives. DEAD: the
                // session died -- client_mark_dead_locked already error-completed
                // every inflight op (CQEs posted + cq_waiters woken), so the loop
                // drains async_inflight to 0 next iteration and parks. All cases: loop.
                if (rc == P9_PUMP_BUSY) sched();
            }
            // cl == NULL: raced (the op was reaped) -> loop, re-sample.
        } else {
            // No work: announce NEED_WAKEUP so userspace knows to ENTER-wake us,
            // then park. The cond re-samples the SQ under the park Rendez lock
            // (register-then-observe) so an SQE produced + ENTER-woken in the
            // announce..sleep window is not missed. kproc never group-terminates,
            // so this never returns SLEEP_INTR -- only stopping / new SQEs wake it.
            __atomic_fetch_or(&h->flags, LOOM_RING_SQ_NEED_WAKEUP, __ATOMIC_RELEASE);
            (void)sleep(&l->sqpoll_park, loom_sqpoll_park_cond, l);
            __atomic_fetch_and(&h->flags, ~(u32)LOOM_RING_SQ_NEED_WAKEUP, __ATOMIC_RELEASE);
        }

        // Reclaim terminal ops so async_inflight + the container list stay bounded.
        loom_reap_terminal(l);
    }

    // Terminal: the ring is being torn down (loom_free set sqpoll_stopping).
    // Reap any stragglers (loom_free's #898 quiesce abandons whatever remains),
    // then hand off to the joiner via the EXITING handshake. IRQs are masked
    // (the idle-loop idiom, sched.c bootcpu_idle_main) across the state=EXITING
    // write + the sqpoll_exited release so no preempt can fire between them: the
    // joiner, on observing sqpoll_exited (acquire, pairing this release), is
    // guaranteed to see state==EXITING and so thread_free's not-RUNNING gate
    // holds. sched() then switches away permanently (EXITING is never
    // re-enqueued); the joiner's thread_free spins on on_cpu (cleared by the
    // next thread's finish-task-switch) before reclaiming. This is the wait_pid
    // reap terminal minus the Proc-zombie bookkeeping a kproc thread cannot run
    // (thread_exit_self extincts from kproc).
    loom_reap_terminal(l);

    (void)spin_lock_irqsave(NULL);           // mask preempt for the terminal window
    current_thread()->state = THREAD_EXITING;
    __atomic_store_n(&l->sqpoll_exited, true, __ATOMIC_RELEASE);
    sched();
    extinction("loom_sqpoll_main: returned from terminal sched");
}

int loom_start_sqpoll(struct Loom *l) {
    if (!l || l->magic != LOOM_MAGIC) return -1;
    // Spawn the kthread under kproc() (PID 0; immortal -- so the thread never
    // group-terminates and its only exit is the stop-flag terminal above). Set
    // l->sqpoll BEFORE ready() so the (not-yet-running) join logic in loom_free
    // would observe it; ready() then makes it runnable.
    struct Thread *kt = thread_create_with_arg(kproc(), loom_sqpoll_main, l);
    if (!kt) return -1;
    l->sqpoll = kt;
    ready(kt);
    return 0;
}
