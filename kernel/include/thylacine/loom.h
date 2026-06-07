// Loom -- a shared-memory ring transport for 9P (the io_uring inversion).
//
// docs/LOOM.md (signed off 2026-06-05). Userspace posts 9P-shaped ops into a
// submission ring living in a shared Burrow; the kernel's #841 elected-reader
// 9P client drives them; R-messages return as completion-queue entries. No new
// opcode namespace -- the opcodes ARE the p9_client_* surface, so the async
// batching layer covers files, /net, /proc, /srv, and devices uniformly.
//
// Spec: specs/loom.tla (TLC-green; gates the impl). Reserves ARCH 28 I-29
// (completion integrity: no lost / double / stale) + I-30 (submit-time
// capability pin). Audit-trigger surface per ARCH 25.4.
//
// This header carries BOTH the userspace ABI (loom_sqe / loom_cqe /
// loom_ring_hdr / loom_params + the opcode/flag constants, mirrored by
// libthyla-rs at Loom-6) AND the kernel-internal `struct Loom` (the KObj_Loom
// object) + its lifecycle. The ABI portion is byte-pinned by _Static_assert; a
// layout change is an ABI break.
//
// Sub-chunk status (LOOM.md 10):
//   Loom-2a (this): the ring substrate -- SYS_LOOM_SETUP (alloc + map the ring
//     Burrow + report geometry), SYS_LOOM_REGISTER(LOOM_REGISTER_HANDLES) (the
//     fixed-handle table), KObj_Loom lifecycle. No op flows yet -- the SQE
//     opcodes + the SQE/CQE flags are reserved ABI, dispatched at Loom-3..5.
//   Loom-2b: the pluggable-completion seam in the 9P engine.
//   Loom-3: SYS_LOOM_ENTER + SQE dispatch + the submit-time pin + CQE post.

#ifndef THYLACINE_LOOM_H
#define THYLACINE_LOOM_H

#include <thylacine/handle.h>     // rights_t
#include <thylacine/poll.h>       // struct poll_waiter_list (the Loom-4 CQ wait-list)
#include <thylacine/rendez.h>     // struct Rendez (the Loom-4c SQPOLL park)
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct Burrow;
struct Spoor;
struct Proc;
struct loom_async_op;   // Loom-3: one in-flight async op (defined in kernel/loom.c)
struct loom_chain_op;   // Loom-5b: one held LINK/DRAIN chain entry (defined in kernel/loom.c)

#define LOOM_MAGIC 0x4C4F4F4D52494E47ULL   // "LOOMRING"

// =============================================================================
// Limits.
// =============================================================================

// Max submission-queue entries (must be a power of two). With 4096 entries the
// ring Burrow is ~400 KiB (16 KiB sq-index + 256 KiB sqes + 128 KiB cqes at the
// 2x cq default) -- well within burrow_create_anon's reach.
#define LOOM_MAX_ENTRIES        4096u

// Max registered handles per ring (the "fixed files" analog). Matches
// PROC_HANDLE_MAX -- a ring need name no more handles than its Proc can hold.
#define LOOM_MAX_REG_HANDLES    64u

// =============================================================================
// SQE -- one submission entry (a native 9P op-descriptor). Fixed 64 bytes (one
// cache line; the io_uring SQE size). LOOM.md 7: native descriptor dispatched
// straight to p9_client_<op>. The reserved tail holds opcode-specific fields
// carved in at Loom-3..5 (walk name slices, create mode/gid, ...) without an
// ABI break.
// =============================================================================

struct loom_sqe {
    u8  opcode;          // LOOM_OP_*
    u8  flags;           // LOOM_SQE_* (LINK / DRAIN / CQE_SKIP / MULTISHOT)
    u16 _resv0;
    u32 handle_idx;      // registered-handle index, or LOOM_HANDLE_RAW
    u64 offset;          // op offset (read / write / readdir)
    u32 len;             // op length / count
    u32 buf_idx_or_off;  // registered-buffer index or in-ring offset (Loom-6)
    u64 user_data;       // correlation token (echoed verbatim in the CQE)
    u64 _resv1[4];       // opcode-specific / future
};

_Static_assert(sizeof(struct loom_sqe) == 64, "loom_sqe ABI pinned at 64 bytes");
_Static_assert(__builtin_offsetof(struct loom_sqe, opcode) == 0, "opcode at 0");
_Static_assert(__builtin_offsetof(struct loom_sqe, user_data) == 24, "user_data at 24");

// =============================================================================
// CQE -- one completion entry. 16 bytes. result >= 0 = byte count / packed qid
// / 0; result < 0 = -errno (the Rlerror passthrough, mapped by the client's
// existing errno convention).
// =============================================================================

struct loom_cqe {
    u64 user_data;       // echoed from the submitting SQE
    s32 result;          // >= 0 success payload; < 0 = -errno
    u32 flags;           // LOOM_CQE_* (LOOM_CQE_MORE for multishot)
};

_Static_assert(sizeof(struct loom_cqe) == 16, "loom_cqe ABI pinned at 16 bytes");
_Static_assert(__builtin_offsetof(struct loom_cqe, user_data) == 0, "user_data at 0");
_Static_assert(__builtin_offsetof(struct loom_cqe, result) == 8, "result at 8");

// =============================================================================
// Ring header -- the shared control words, at offset 0 of the ring Burrow. The
// head/tail pairs are the wait/wake state machine specs/loom.tla models (ring
// TOCTOU; CQ back-pressure). 64 bytes (one cache line).
//
// Ownership (single-producer / single-consumer per ring):
//   sq_tail -- USER writes (produces SQEs); kernel reads.
//   sq_head -- KERNEL writes (consumes SQEs); user reads.
//   cq_tail -- KERNEL writes (posts CQEs); user reads.
//   cq_head -- USER writes (reaps CQEs); kernel reads.
// Indices are free-running u32; the live slot is `index & mask`. A side only
// ever advances its own producer/consumer word, so no torn cross-write.
// =============================================================================

struct loom_ring_hdr {
    u32 sq_head;         // kernel-owned: consumed (kernel advances)
    u32 sq_tail;         // user-owned:   produced (user advances)
    u32 sq_mask;         // sq_entries - 1 (immutable; set at setup)
    u32 sq_entries;      // immutable
    u32 cq_head;         // user-owned:   reaped (user advances)
    u32 cq_tail;         // kernel-owned: posted (kernel advances)
    u32 cq_mask;         // cq_entries - 1 (immutable)
    u32 cq_entries;      // immutable
    u32 flags;           // LOOM_RING_* runtime flags (SQ_NEED_WAKEUP -- Loom-4)
    u32 dropped;         // SQEs dropped (invalid index) -- diagnostics (Loom-3)
    u32 overflow;        // CQEs that could not post (CQ full) -- back-pressure
    u32 _pad[5];
};

_Static_assert(sizeof(struct loom_ring_hdr) == 64, "loom_ring_hdr pinned at 64 bytes");
_Static_assert(__builtin_offsetof(struct loom_ring_hdr, sq_head) == 0, "sq_head at 0");

// =============================================================================
// loom_params -- the SYS_LOOM_SETUP in/out struct. `flags` is IN (the
// LOOM_SETUP_* the caller requests). Everything else is OUT (the kernel fills
// the resulting geometry so userspace can map the regions). 88 bytes.
// =============================================================================

struct loom_params {
    u32 flags;           // IN: LOOM_SETUP_*; OUT: echoed
    u32 sq_entries;      // OUT: actual (power-of-two rounded)
    u32 cq_entries;      // OUT: 2 * sq_entries by default
    u32 ring_size;       // OUT: total mapped bytes (page-rounded)
    u64 ring_va;         // OUT: base user-VA of the mapped ring Burrow
    u32 hdr_off;         // OUT: byte offsets within the ring
    u32 sq_array_off;
    u32 sqe_off;
    u32 cqe_off;
    u32 sq_array_size;   // OUT: byte sizes of each region
    u32 sqe_size;
    u32 cqe_size;
    u32 _resv0;
    u64 _resv1[4];
};

_Static_assert(sizeof(struct loom_params) == 88, "loom_params ABI pinned at 88 bytes");
_Static_assert(__builtin_offsetof(struct loom_params, ring_va) == 16, "ring_va at 16");

// =============================================================================
// Opcodes -- the p9_client_* surface (LOOM.md 8.3). Defined now as the fixed
// ABI; dispatched at Loom-3..5. LOOM_OP_WIRE_PASSTHROUGH is the reserved seam
// (designed, NOT implemented at v1.0 -- LOOM.md 7).
// =============================================================================

enum {
    LOOM_OP_NOP        = 0,    // no-op; completes with result 0 (Loom-3 smoke)
    LOOM_OP_WALK       = 1,
    LOOM_OP_LOPEN      = 2,
    LOOM_OP_LCREATE    = 3,
    LOOM_OP_READ       = 4,
    LOOM_OP_WRITE      = 5,
    LOOM_OP_GETATTR    = 6,
    LOOM_OP_SETATTR    = 7,
    LOOM_OP_READDIR    = 8,
    LOOM_OP_FSYNC      = 9,
    LOOM_OP_CLUNK      = 10,
    LOOM_OP_RENAMEAT   = 11,
    LOOM_OP_UNLINKAT   = 12,
    LOOM_OP_MKDIR      = 13,
    LOOM_OP_SYMLINK    = 14,
    LOOM_OP_LINK       = 15,
    LOOM_OP_MKNOD      = 16,
    LOOM_OP_READLINK   = 17,
    LOOM_OP_STATFS     = 18,
    LOOM_OP_WIRE_PASSTHROUGH = 19,  // RESERVED -- not implemented at v1.0
    LOOM_OP_COUNT      = 20,
};

// SQE flags (LOOM.md 8.3). Reserved ABI; LINK/DRAIN/CQE_SKIP at Loom-5,
// MULTISHOT at Loom-5.
#define LOOM_SQE_LINK       (1u << 0)   // the next SQE runs after this one
#define LOOM_SQE_DRAIN      (1u << 1)   // wait for all prior ops before this one
#define LOOM_SQE_CQE_SKIP   (1u << 2)   // suppress this op's CQE on success
#define LOOM_SQE_MULTISHOT  (1u << 3)   // one SQE -> many CQEs (accept loop)

// CQE flags. LOOM_CQE_MORE: more completions for this SQE follow (multishot).
#define LOOM_CQE_MORE       (1u << 0)

// handle_idx sentinel: the op carries a raw (path-resolved at submit) target
// rather than a registered-handle index. Reserved; v1.0 uses registered
// handles only.
#define LOOM_HANDLE_RAW     0xFFFFFFFFu

// Ring runtime flags (loom_ring_hdr.flags). SQ_NEED_WAKEUP: the SQPOLL thread
// idled; userspace must SYS_LOOM_ENTER to wake it (Loom-4).
#define LOOM_RING_SQ_NEED_WAKEUP  (1u << 0)

// =============================================================================
// SYS_LOOM_SETUP flags. Reserved ABI; each rejected until its sub-chunk lands
// (Loom-2a accepts flags == 0 only).
// =============================================================================
#define LOOM_SETUP_SQPOLL   (1u << 0)   // start a cpu_pinned-able poll-thread (Loom-4)
#define LOOM_SETUP_CQSIZE   (1u << 1)   // caller-chosen cq size (later)
#define LOOM_SETUP_VALID    (LOOM_SETUP_SQPOLL | LOOM_SETUP_CQSIZE)

// =============================================================================
// SYS_LOOM_ENTER flags (Loom-3). The wait is driven by min_complete; NONBLOCK
// suppresses it. GETEVENTS is the io_uring-compat "wait" signal, accepted but
// informational at v1.0 (min_complete drives the wait).
// =============================================================================
#define LOOM_ENTER_GETEVENTS  (1u << 0)   // wait for min_complete (informational)
#define LOOM_ENTER_NONBLOCK   (1u << 1)   // never block; reap only what is posted
#define LOOM_ENTER_VALID      (LOOM_ENTER_GETEVENTS | LOOM_ENTER_NONBLOCK)

// SYS_LOOM_REGISTER ops.
enum {
    LOOM_REGISTER_HANDLES = 0,   // install KObj_Spoor handles into the fixed table
    LOOM_REGISTER_BUFFERS = 1,   // pin Burrow regions for zero-copy payload (Loom-6)
};

// =============================================================================
// Kernel-internal: the KObj_Loom object.
// =============================================================================

// One registered-handle table slot (the "fixed file" analog; LOOM.md 8.1).
// The held Spoor ref + the rights snapshot are the I-30 substrate: the
// dispatch (Loom-3) resolves (client, fid) from `spoor` at submit and the held
// ref pins the object for the op's lifetime; `rights` is the snapshot the
// submit-time gate reads. `spoor == NULL` => empty slot.
struct loom_reg_handle {
    struct Spoor *spoor;
    rights_t      rights;
};

struct Loom {
    u64                  magic;        // LOOM_MAGIC; offset 0 (SLUB-free UAF defense)
    int                  refcount;     // atomic ACQ_REL; 1 at create (the handle's ref)
    spin_lock_t          lock;         // protects reg[] + cq_tail + sq_head + inflight_ops + async_inflight + chain/chain_tail/chain_len (geometry is immutable post-setup); cq_waiters carries its OWN lock
    u32                  _pad;
    struct Burrow       *ring;         // the SQ/CQ ring Burrow (handle_count ref held)
    u8                  *ring_kva;     // kernel direct-map base of `ring` (stable for life)
    // Geometry (immutable after loom_create). Mirrors loom_params.
    u32 sq_entries;
    u32 cq_entries;
    u32 hdr_off;
    u32 sq_array_off;
    u32 sqe_off;
    u32 cqe_off;
    u32 sq_array_size;
    u32 sqe_size;
    u32 cqe_size;
    u32 ring_size;
    // Kernel-PRIVATE authoritative completion-queue tail (under `lock`). The
    // shared `loom_ring_hdr.cq_tail` is a userspace-READABLE mirror; loom_post_cqe
    // computes its write index from THIS + the private `cq_entries` mask, NEVER
    // from the shared header (which userspace can corrupt -> an OOB kernel write).
    u32 cq_tail;
    // Loom-3. Kernel-PRIVATE authoritative submission-queue head (under `lock`):
    // the SQ-index ring slot the kernel consumes next. The shared
    // loom_ring_hdr.sq_head is a userspace-READABLE mirror; the consume index is
    // computed from THIS + the private sq_entries mask, NEVER the shared header
    // (the SA-1 discipline generalized to the SQ -- a userspace-corrupted
    // sq_head/sq_mask must not drive the kernel's read index).
    u32 sq_head;
    // In-flight async ops (Loom-3). Singly-linked via loom_async_op.next; mutated
    // under `lock` (submit links, reap/quiesce unlink). async_inflight counts the
    // submitted-not-yet-terminal ops -- when it is 0 a SYS_LOOM_ENTER wait knows
    // no more completions can arrive. The loom owns each op; loom_free quiesces
    // any still in flight (the #898 abandon-before-free).
    struct loom_async_op *inflight_ops;
    u32                   async_inflight;
    // Loom-5 (specs/loom_multishot.tla): count of ops in the MORE-pending re-arm
    // state (op->rearm == true) -- a multishot op that posted a MORE shot and is
    // awaiting re-issue. Mutated under `lock` (++ in loom_async_complete, -- in
    // loom_rearm_pending when an op is claimed for re-arm), but READ LOCK-FREE by
    // the SQPOLL park cond (which cannot take `lock`): a non-zero count with CQ
    // room means the kthread has deferred re-arm work, so it must NOT stay parked
    // on a reap-only wake (the back-pressure-resume liveness). Atomic for the
    // cross-lock read.
    u32                   rearm_pending;
    // Loom-5b (specs/loom_order.tla): the LINK/DRAIN held-submission chain. An
    // ordering-relevant SQE (LINK or DRAIN set, or any SQE consumed while the
    // chain is already non-empty) is enqueued HELD here instead of dispatched
    // immediately; loom_admit_chain walks head->tail and dispatches each entry
    // once its ordering gates open (a linked successor after its predecessor is
    // done ok; a drain barrier after all prior are done), cancelling the rest of
    // a chain when a link member fails (each cancelled op posts exactly one
    // -ECANCELED CQE -- EveryDoneOpPosted). The chain is bounded (chain_len capped
    // at cq_entries) so a drain-blocked burst can't grow it unbounded. All three
    // fields are mutated under `lock`. chain_tail gives O(1) append; chain_len
    // is the back-pressure bound. Invariants LinkOrdered / DrainOrdered /
    // EveryDoneOpPosted / NoOrphanCancel.
    struct loom_chain_op *chain;
    struct loom_chain_op *chain_tail;
    u32                   chain_len;
    // Loom-4 (LOOM.md 8.6): the CQ wait-list. A thread in SYS_LOOM_ENTER with
    // min_complete >= 1 that finds a sibling thread already holding the elected-
    // reader role installs a poll_waiter here and sleeps; loom_post_cqe wakes the
    // list after publishing a CQE (the SQPOLL kthread / a peer ENTER's demux is
    // the producer). The list owns its own lock -- the register-then-observe
    // discipline is poll.tla's (object lock = l->lock for the readiness sample;
    // the list lock serializes the hook walk). Lock order: l->lock -> cq_waiters
    // list -> g_timerwait -> rendez -> cpu_sched (the poll.h global order, with
    // l->lock as the "object"). specs/loom.tla: CqWaitRegister / PostCqe-wake /
    // CqWaitCommitOrSleep; invariants CqFlagTracksCq + NoMissedCqWake (I-9 on the
    // CQ wait-list) + NoStrandedWaiter + CqWaitFlagSound.
    struct poll_waiter_list cq_waiters;
    // Loom-4c (LOOM.md 8.6): the SQPOLL poll-thread. NULL unless the ring was
    // set up with LOOM_SETUP_SQPOLL. A per-ring kproc() kthread (the console_mgr
    // precedent) that drains the SQ + drives the elected reader so steady-state
    // submission is zero-syscall. The Loom OWNS the kthread: loom_free sets
    // `sqpoll_stopping`, wakes `sqpoll_park`, and JOINS (spins on `sqpoll_exited`,
    // then thread_free) BEFORE freeing the ring -- the kthread only ever touches
    // the still-allocated `struct Loom`, guaranteed by the join (the kthread
    // holds NO loom ref; a ref would deadlock loom_free's join). `sqpoll` is set
    // once at setup (before the handle is returned to userspace) and never
    // rewritten while alive. `sqpoll_stopping` / `sqpoll_exited` are the
    // single-writer flags of the join handshake (release/acquire paired). The
    // kthread is gated to a deadline-capable transport (loom_register_handles
    // rejects a NULL-deadline dev9p client into an SQPOLL ring) so its
    // frame-boundary idle-deadline always lets it re-check `sqpoll_stopping` ->
    // the join always terminates.
    struct Thread          *sqpoll;          // the kthread (NULL = no SQPOLL)
    bool                    sqpoll_stopping; // loom_free sets (release); kthread reads (acquire)
    bool                    sqpoll_exited;   // kthread sets at terminal (release); joiner reads (acquire)
    struct Rendez           sqpoll_park;     // kthread parks here when idle; woken by ENTER / stop
    // The registered-handle table.
    struct loom_reg_handle reg[LOOM_MAX_REG_HANDLES];
};

_Static_assert(__builtin_offsetof(struct Loom, magic) == 0,
               "magic at offset 0 -- SLUB freelist write on free clobbers it "
               "(use-after-free defense, mirrors struct Burrow / struct Spoor)");

// Create a Loom ring sized for `sq_entries` (power-of-two, 1..LOOM_MAX_ENTRIES)
// + `cq_entries` (power-of-two, >= sq_entries, <= 2*LOOM_MAX_ENTRIES).
// Allocates the ring Burrow (handle_count = 1, held by the returned Loom),
// computes + stamps the geometry into the ring header, refcount = 1. Does NOT
// map the ring into any address space -- SYS_LOOM_SETUP's handler does that.
// Returns NULL on bad args / OOM.
struct Loom *loom_create(u32 sq_entries, u32 cq_entries);

// Refcount. loom_unref's last drop clunks every registered Spoor and
// burrow_unref's the ring (releasing the kernel's handle_count; the user
// mapping's mapping_count, if any, independently keeps the pages until the VMA
// tears down -- the dual-refcount discipline of #847).
void loom_ref(struct Loom *l);
void loom_unref(struct Loom *l);

// Replace the registered-handle table with the `n` (<= LOOM_MAX_REG_HANDLES)
// Spoors in `spoors` (with their rights snapshots in `rights`). ADOPTS the
// caller's ref on each spoor[i] on SUCCESS (the table releases them at
// loom_unref / the next re-register); on failure (n out of range) the caller
// retains its refs. Any previously-registered Spoors are clunked (outside the
// lock). Returns 0 / -1.
int loom_register_handles(struct Loom *l, struct Spoor **spoors,
                          const rights_t *rights, u32 n);

// Post one completion into the CQ ring (the POST_CQE front-end's terminal
// action; Loom-2b). Writes a loom_cqe {user_data, result, flags} at the kernel
// cq_tail and publishes the bump with a release store (so a user-side
// load-acquire of cq_tail sees the CQE bytes). Under l->lock. Returns 0 on a
// successful post.
//
// CQ back-pressure (I-29 CqNeverOverfull): if the CQ is FULL the kernel does
// NOT overwrite an unreaped CQE -- it increments the shared `overflow` counter
// and returns -1. The no-LOST-completion liveness (hold the completion until a
// slot frees) is realized by Loom-3's submit-time admission (consume an SQE
// only when the CQ can hold its completion = io_uring's model), which makes a
// full CQ at completion unreachable -- at which point `overflow` is a pure
// diagnostic. Safe to call from a completion callback running under the 9P
// client's lock (l->lock is a leaf; no sleep, no nesting with c->lock).
//
// Loom-4: after a successful post the CQ becomes non-empty -- this wakes the
// ring's CQ wait-list (poll_waiter_list_wake) so a SYS_LOOM_ENTER thread sleeping
// for min_complete re-evaluates. The wake runs AFTER l->lock is released (the
// pipe.c producer precedent) and does NOT sleep or re-enter p9_client_*, so it
// composes with the c->lock the async-completion path holds (the seam contract).
int loom_post_cqe(struct Loom *l, u64 user_data, s32 result, u32 flags);

// Diagnostics (tests).
u64 loom_total_created(void);
u64 loom_total_destroyed(void);

// Loom-4c: the SQPOLL poll-thread entry + its lifecycle (LOOM.md 8.6). The
// kthread is a kproc() thread (the console_mgr precedent) that drains the SQ +
// drives the elected reader. loom_start_sqpoll spawns + readies it (sets
// l->sqpoll on success); loom_free joins it. loom_sqpoll_main is the kthread
// body (noreturn -- it exits via the EXITING terminal handshake, never returns
// to its caller). Both touch only the still-allocated `struct Loom`.
void loom_sqpoll_main(void *arg);
int  loom_start_sqpoll(struct Loom *l);

// Testable setup inner (the spoor_stat_native pattern -- fills a KERNEL
// loom_params; the SVC handler does the user copy-in/out). Creates the Loom,
// maps the ring into `p` (RW, in the burrow-attach window), installs a
// KObj_Loom handle in p, fills `*out` + `*out_fd`. `flags` accepts
// LOOM_SETUP_SQPOLL (Loom-4c: spawns the poll-thread before the handle is
// returned); other LOOM_SETUP_* bits reject until their sub-chunk lands.
// Returns 0 on success, -1 on bad args / OOM / handle-table-full / SQPOLL
// spawn failure (rolling back the map + the Loom -- which joins any spawned
// kthread -- on any failure).
int sys_loom_setup_for_proc(struct Proc *p, u32 entries, u32 flags,
                            struct loom_params *out, hidx_t *out_fd);

// Testable register inner. Resolves the `n` fds in `fds[]` (each must be a
// KOBJ_SPOOR handle in p) into the ring at `loom_fd`'s registered-handle table.
// Returns 0 / -1.
int sys_loom_register_for_proc(struct Proc *p, hidx_t loom_fd, u32 op,
                               const hidx_t *fds, u32 n);

// Loom-3 batch-enter core. Consume up to `to_submit` SQEs from the SQ index ring
// (in SQ-index order), copy each to kernel memory (ring TOCTOU), and dispatch:
//   - LOOM_OP_NOP completes inline (CQE result 0);
//   - LOOM_OP_FSYNC resolves handle_idx -> the registered Spoor, snapshots+pins
//     rights (the I-30 submit-time pin), checks RIGHT_WRITE, resolves (client,
//     fid), and submits an async Tfsync to the 9P engine (reply -> CQE later);
//   - every other in-range opcode posts -ENOSYS (the payload opcodes land with
//     Loom-6's registered-buffer surface), out-of-range posts -EINVAL.
// Then, if min_complete > 0 and not LOOM_ENTER_NONBLOCK, wait for completions:
// the caller either DRIVES the elected reader itself (blocking on recv, death-
// interruptible #811) or -- when a sibling thread of the same Proc already holds
// the reader role -- sleeps on the ring's CQ wait-list until that reader posts a
// CQE (Loom-4; resolves the Loom-3 "concurrent ENTER returns what's posted"
// limitation). Waits until at least min_complete CQEs are available OR no async
// op remains in flight. Finally reap completed-op containers. Returns the number
// of SQEs consumed (>= 0), or -1 on bad args
// (NULL/corrupt l, invalid flags). The caller (SYS_LOOM_ENTER handler) holds a
// loom ref across this call, so loom_free cannot run concurrently.
int loom_enter(struct Loom *l, u32 to_submit, u32 min_complete, u32 flags);

// Testable enter inner. Resolves `loom_fd` -> KObj_Loom in `p` (holding a loom
// ref for the call), runs loom_enter, and returns its result. -1 on a bad fd /
// wrong kind. The SVC handler is a thin current_thread() wrapper.
int sys_loom_enter_for_proc(struct Proc *p, hidx_t loom_fd, u32 to_submit,
                            u32 min_complete, u32 flags);

#endif // THYLACINE_LOOM_H
