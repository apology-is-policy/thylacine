// Loom-2a tests -- the KObj_Loom ring substrate.
//
// Covers loom_create geometry + the kobj refcount lifecycle + the
// registered-handle table at the function level, and SYS_LOOM_SETUP /
// SYS_LOOM_REGISTER through the testable `_for_proc` inners on a fresh
// proc_alloc'd Proc (the sys_burrow_attach_for_proc test pattern). The full
// SYS_LOOM_SETUP user copy-in/out is trivial handler glue (E2E coverage lands
// with the native libthyla-rs API at Loom-6); the SQE dispatch + the CQE post
// arrive at Loom-3.
//
// Maps to specs/loom.tla: the ring geometry + the registered-object table (the
// spec's `reg`) + teardown (the spec's Teardown action) substrate.

#include "test.h"

#include <thylacine/loom.h>
#include <thylacine/burrow.h>
#include <thylacine/errno.h>
#include <thylacine/exec.h>
#include <thylacine/handle.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>   // sched() -- cooperative yield to the SQPOLL kthread
#include <thylacine/thread.h>  // THREAD_SLEEPING -- the F2 park observation
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_loom_create_geometry(void);
void test_loom_create_rejects_bad_args(void);
void test_loom_refcount_lifecycle(void);
void test_loom_setup_via_proc(void);
void test_loom_setup_rejects(void);
void test_loom_register_handles(void);
void test_loom_register_rejects(void);
void test_loom_register_replaces(void);
void test_loom_post_cqe_back_pressure(void);
void test_loom_post_cqe_ignores_hostile_header(void);
void test_loom_dup_rejected(void);
void test_loom_enter_nop(void);
void test_loom_enter_submit_rejects(void);
void test_loom_enter_flags_and_bad_index(void);
void test_loom_enter_cq_admission_backpressure(void);
void test_loom_cq_waiter_wake(void);
void test_loom_cq_waiter_no_spurious_wake_on_full(void);
void test_loom_enter_inline_min_complete(void);
void test_loom_enter_min_complete_no_inflight(void);
void test_loom_sqpoll_setup_and_teardown(void);
void test_loom_sqpoll_drains_sq(void);
void test_loom_sqpoll_parks_on_cq_full(void);

void test_loom_register_buffers(void);
void test_loom_register_buffers_rejects(void);
void test_loom_register_buffers_replace(void);
void test_loom_ring_charge_balance(void);
void test_loom_regbuf_charge_balance(void);
void test_loom_regbuf_foreign_charge_not_refunded(void);

// The testable syscall inners live in kernel/syscall.c (declared in loom.h);
// sys_pipe_for_proc is the KOBJ_SPOOR-pair maker (declared nowhere public --
// forward-declare it like test_sys_burrow.c does for its inner).
extern int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr);
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);

static struct Proc *test_proc_make(void) {
    return proc_alloc();
}

static void test_proc_drop(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// Geometry: the ring layout is consistent, page-rounded, and the header is
// stamped with the immutable masks/counts (visible via the kernel direct map).
// ---------------------------------------------------------------------------
void test_loom_create_geometry(void) {
    u64 created0 = loom_total_created();

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16) returned NULL");
    TEST_EXPECT_EQ(loom_total_created() - created0, (u64)1, "created counter +1");

    TEST_EXPECT_EQ(l->sq_entries, 8u, "sq_entries");
    TEST_EXPECT_EQ(l->cq_entries, 16u, "cq_entries");
    TEST_EXPECT_EQ(l->hdr_off, 0u, "hdr at offset 0");
    TEST_EXPECT_EQ(l->sq_array_size, 8u * 4u, "sq_array_size = entries*4");
    TEST_EXPECT_EQ(l->sqe_size, 8u * 64u, "sqe_size = entries*sizeof(sqe)");
    TEST_EXPECT_EQ(l->cqe_size, 16u * 16u, "cqe_size = cq_entries*sizeof(cqe)");

    // Regions 64-aligned + non-overlapping + within the page-rounded ring.
    TEST_ASSERT((l->sq_array_off & 63u) == 0, "sq_array_off 64-aligned");
    TEST_ASSERT((l->sqe_off & 63u) == 0, "sqe_off 64-aligned");
    TEST_ASSERT((l->cqe_off & 63u) == 0, "cqe_off 64-aligned");
    TEST_ASSERT(l->sq_array_off >= l->hdr_off + 64u, "sq_array after hdr");
    TEST_ASSERT(l->sqe_off >= l->sq_array_off + l->sq_array_size, "sqe after sq_array");
    TEST_ASSERT(l->cqe_off >= l->sqe_off + l->sqe_size, "cqe after sqe");
    TEST_ASSERT(l->ring_size >= l->cqe_off + l->cqe_size, "ring holds all regions");
    TEST_ASSERT((l->ring_size & (PAGE_SIZE - 1u)) == 0, "ring_size page-aligned");
    TEST_ASSERT(l->ring_kva != NULL, "ring_kva populated");

    // The header is stamped (read via the kernel direct map); heads/tails/flags
    // start zero (KP_ZERO).
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    TEST_EXPECT_EQ(h->sq_mask, 7u, "sq_mask = entries-1");
    TEST_EXPECT_EQ(h->sq_entries, 8u, "hdr sq_entries");
    TEST_EXPECT_EQ(h->cq_mask, 15u, "cq_mask = cq_entries-1");
    TEST_EXPECT_EQ(h->cq_entries, 16u, "hdr cq_entries");
    TEST_EXPECT_EQ(h->sq_head, 0u, "sq_head 0");
    TEST_EXPECT_EQ(h->sq_tail, 0u, "sq_tail 0");
    TEST_EXPECT_EQ(h->cq_head, 0u, "cq_head 0");
    TEST_EXPECT_EQ(h->cq_tail, 0u, "cq_tail 0");
    TEST_EXPECT_EQ(h->flags, 0u, "flags 0");
    TEST_EXPECT_EQ(h->dropped, 0u, "dropped 0");
    TEST_EXPECT_EQ(h->overflow, 0u, "overflow 0");

    loom_unref(l);
}

void test_loom_create_rejects_bad_args(void) {
    TEST_ASSERT(loom_create(0, 0) == NULL, "sq_entries 0 rejected");
    TEST_ASSERT(loom_create(3, 6) == NULL, "non-power-of-2 sq rejected");
    TEST_ASSERT(loom_create(LOOM_MAX_ENTRIES * 2u, LOOM_MAX_ENTRIES * 4u) == NULL,
                "sq over max rejected");
    TEST_ASSERT(loom_create(8, 4) == NULL, "cq < sq rejected");
    TEST_ASSERT(loom_create(8, 6) == NULL, "non-power-of-2 cq rejected");
}

void test_loom_refcount_lifecycle(void) {
    u64 destroyed0 = loom_total_destroyed();

    struct Loom *l = loom_create(4, 8);
    TEST_ASSERT(l != NULL, "loom_create(4,8) returned NULL");

    loom_ref(l);   // refcount 1 -> 2
    loom_unref(l); // 2 -> 1, NOT freed
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)0,
                   "not freed while a ref remains");

    loom_unref(l); // 1 -> 0, freed
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1,
                   "freed when last ref drops");
}

// ---------------------------------------------------------------------------
// SYS_LOOM_SETUP via the testable inner: allocate + map the ring + install the
// handle on a fresh Proc.
// ---------------------------------------------------------------------------
void test_loom_setup_via_proc(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    u64 destroyed0 = loom_total_destroyed();

    struct loom_params kp;
    hidx_t fd = -1;
    int rc = sys_loom_setup_for_proc(p, 8, 0, &kp, &fd);
    TEST_EXPECT_EQ(rc, 0, "sys_loom_setup_for_proc succeeds");
    TEST_ASSERT(fd >= 0, "setup returned a valid fd");
    TEST_EXPECT_EQ(kp.sq_entries, 8u, "params sq_entries");
    TEST_EXPECT_EQ(kp.cq_entries, 16u, "params cq_entries (2x default)");
    TEST_ASSERT(kp.ring_va >= EXEC_USER_BURROW_BASE, "ring mapped in burrow window");
    TEST_ASSERT(kp.ring_va < EXEC_USER_BURROW_TOP, "ring below window top");
    TEST_ASSERT((kp.ring_size & (PAGE_SIZE - 1u)) == 0, "ring_size page-aligned");

    // The ring VMA is installed at ring_va.
    struct Vma *vma = vma_lookup(p, kp.ring_va);
    TEST_ASSERT(vma != NULL, "ring VMA installed at ring_va");

    // The handle is a KObj_Loom; its ring header is kernel-readable + stamped.
    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "handle_get(loom fd) succeeds");
    TEST_EXPECT_EQ((int)h.kind, (int)KOBJ_LOOM, "handle is KOBJ_LOOM");
    struct Loom *l = (struct Loom *)h.obj;
    struct loom_ring_hdr *hdr = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    TEST_EXPECT_EQ(hdr->sq_entries, 8u, "ring header readable via direct map");
    handle_put(&h);

    // proc_free closes the handle (-> loom_unref frees the ring) + tears down
    // the VMA. The ring is destroyed exactly once.
    test_proc_drop(p);
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1,
                   "ring freed on proc teardown");
}

void test_loom_setup_rejects(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 0, 0, &kp, &fd), -1, "entries 0 rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 3, 0, &kp, &fd), -1, "non-pow2 rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, LOOM_MAX_ENTRIES * 2u, 0, &kp, &fd), -1,
                   "over-max rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_CQSIZE, &kp, &fd), -1,
                   "still-reserved LOOM_SETUP_CQSIZE flag rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, 0x80000000u, &kp, &fd), -1,
                   "unknown high setup-flag bit rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(NULL, 8, 0, &kp, &fd), -1, "NULL proc rejected");

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// SYS_LOOM_REGISTER: install pipe Spoors into the fixed-handle table; the ring
// holds its own ref (the I-30 pin substrate); the rights snapshot is captured.
// ---------------------------------------------------------------------------
void test_loom_register_handles(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe pair");

    hidx_t fds[2] = { rd, wr };
    int rc = sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, fds, 2);
    TEST_EXPECT_EQ(rc, 0, "register 2 handles succeeds");

    // The ring holds the two Spoors with rights snapshots; slot 2 is empty.
    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_ASSERT(l->reg[0].spoor != NULL, "reg[0] populated");
    TEST_ASSERT(l->reg[1].spoor != NULL, "reg[1] populated");
    TEST_ASSERT(l->reg[2].spoor == NULL, "reg[2] empty");
    TEST_ASSERT(l->reg[0].rights != 0, "reg[0] rights snapshot non-empty");
    handle_put(&h);

    test_proc_drop(p);
}

void test_loom_register_rejects(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe pair");

    hidx_t one[1] = { rd };
    // Unsupported op (BUFFERS reserved for Loom-6).
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_BUFFERS, one, 1),
                   -1, "LOOM_REGISTER_BUFFERS rejected");
    // nargs over the table cap.
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, one,
                                              LOOM_MAX_REG_HANDLES + 1u),
                   -1, "nargs over cap rejected");
    // A non-KOBJ_SPOOR fd (the loom fd itself) is rejected.
    hidx_t bad[1] = { loom_fd };
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, bad, 1),
                   -1, "non-Spoor fd rejected");
    // A bogus loom_fd is rejected.
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, (hidx_t)999, LOOM_REGISTER_HANDLES, one, 1),
                   -1, "bad loom_fd rejected");

    test_proc_drop(p);
}

void test_loom_register_replaces(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe pair");

    hidx_t two[2] = { rd, wr };
    TEST_ASSERT(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, two, 2) == 0,
                "register 2");

    // Re-register a single handle -- the whole table is replaced
    // (IORING_REGISTER_FILES semantics); slot 1 is cleared (its old Spoor
    // clunked).
    hidx_t one[1] = { rd };
    TEST_ASSERT(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, one, 1) == 0,
                "re-register 1");

    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_ASSERT(l->reg[0].spoor != NULL, "reg[0] still populated");
    TEST_ASSERT(l->reg[1].spoor == NULL, "reg[1] cleared after replace");
    handle_put(&h);

    // Clearing the table (n = 0) releases all.
    TEST_ASSERT(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, NULL, 0) == 0,
                "clear table");
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom) again");
    l = (struct Loom *)h.obj;
    TEST_ASSERT(l->reg[0].spoor == NULL, "reg[0] cleared after empty register");
    handle_put(&h);

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// SYS_LOOM_REGISTER(LOOM_REGISTER_BUFFERS) (Loom-6): a VA range resolves to its
// backing anon Burrow, which the table pins (an independent burrow_ref -- the
// I-30 buffer-pin substrate), exposing its contiguous direct-map slice for
// zero-copy payload. Mirrors specs/loom.tla's `reg` slot + ObjPinnedToSnapshot.
// ---------------------------------------------------------------------------
void test_loom_register_buffers(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    s64 bva = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva > 0, "burrow-attach a buffer page");
    struct Vma *bvma = vma_lookup(p, (u64)bva);
    TEST_ASSERT(bvma != NULL && bvma->burrow != NULL, "buffer VMA + Burrow");
    int hc0 = burrow_handle_count(bvma->burrow);

    struct loom_buf_reg br = { .va = (u64)bva, .len = PAGE_SIZE };
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &br, 1), 0,
                   "register one buffer");

    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_EXPECT_EQ((u64)l->n_reg_buf, (u64)1, "n_reg_buf == 1");
    TEST_ASSERT(l->reg_buf[0].burrow == bvma->burrow, "slot 0 -> the buffer Burrow");
    TEST_EXPECT_EQ((u64)l->reg_buf[0].len, (u64)PAGE_SIZE, "slot 0 len");
    TEST_ASSERT(l->reg_buf[0].kva != NULL, "slot 0 kva set");
    TEST_ASSERT(l->reg_buf[1].burrow == NULL, "slot 1 empty");
    handle_put(&h);

    // The table holds its OWN handle_count ref (the pin) on top of the mapping.
    TEST_EXPECT_EQ(burrow_handle_count(bvma->burrow), hc0 + 1, "table holds its own ref");

    test_proc_drop(p);   // clears the table -> drops the pin; frees everything
}

void test_loom_register_buffers_rejects(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    s64 bva = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva > 0, "buffer page");
    int hc0 = burrow_handle_count(vma_lookup(p, (u64)bva)->burrow);   // baseline (just the mapping)

    struct loom_buf_reg one  = { .va = (u64)bva, .len = PAGE_SIZE };
    struct loom_buf_reg zlen = { .va = (u64)bva, .len = 0 };
    struct loom_buf_reg oob  = { .va = (u64)bva, .len = 2u * PAGE_SIZE };  // region is 1 page
    struct loom_buf_reg um   = { .va = 0x1000,   .len = PAGE_SIZE };       // unmapped low VA

    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &one,
                   LOOM_MAX_REG_BUFFERS + 1u), -1, "n over cap rejected");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &zlen, 1), -1, "len 0 rejected");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &oob, 1), -1, "OOB len rejected");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &um, 1), -1, "unmapped VA rejected");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, (hidx_t)999, &one, 1), -1, "bad loom_fd rejected");

    // Every reject is atomic: the table stays empty + no ref leaked.
    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_EXPECT_EQ((u64)l->n_reg_buf, (u64)0, "table untouched by rejects");
    TEST_ASSERT(l->reg_buf[0].burrow == NULL, "slot 0 still empty");
    handle_put(&h);
    TEST_EXPECT_EQ(burrow_handle_count(vma_lookup(p, (u64)bva)->burrow), hc0,
                   "no ref leaked into the buffer Burrow by the rejected attempts");

    test_proc_drop(p);
}

void test_loom_register_buffers_replace(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    s64 bva = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva > 0, "buffer page");
    struct Burrow *b = vma_lookup(p, (u64)bva)->burrow;
    int hc0 = burrow_handle_count(b);

    struct loom_buf_reg br = { .va = (u64)bva, .len = PAGE_SIZE };
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, &br, 1) == 0, "register");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0 + 1, "one pin after register");

    // Re-register the SAME region: the prior pin is released, a fresh one taken
    // (net unchanged -- IORING_REGISTER_BUFFERS replace semantics).
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, &br, 1) == 0, "re-register");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0 + 1, "still one pin (replace freed the old)");

    // Clear (n == 0) releases the pin.
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, NULL, 0) == 0, "clear");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "pin released on clear");

    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_EXPECT_EQ((u64)l->n_reg_buf, (u64)0, "n_reg_buf 0 after clear");
    TEST_ASSERT(l->reg_buf[0].burrow == NULL, "slot 0 cleared");
    handle_put(&h);

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// loom_post_cqe back-pressure (Loom-2b): the POST_CQE front-end's CQ writer
// never overwrites an unreaped completion (CqNeverOverfull, I-29). A full CQ
// refuses the post (-1) + bumps the overflow counter; a user-side reap frees
// slots so later posts succeed (wrapping). Maps to specs/loom.tla PostCqe's
// `Cardinality(cq) < CQ_CAP` guard + CqNeverOverfull.
// ---------------------------------------------------------------------------
void test_loom_post_cqe_back_pressure(void) {
    struct Loom *l = loom_create(4, 4);   // cq_entries = 4 (smallest exercising wrap)
    TEST_ASSERT(l != NULL, "loom_create(4,4)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // Fill the CQ to capacity: every post within capacity succeeds.
    for (u32 i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(loom_post_cqe(l, (u64)(0x1000u + i), (s32)i, 0), 0,
                       "post within capacity succeeds");
    }
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)4, "cq_tail = 4 (full)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no overflow while posts fit");

    // Full CQ: the post is REFUSED, overflow bumped, and -- critically -- no
    // staged CQE is overwritten (CqNeverOverfull).
    TEST_EXPECT_EQ(loom_post_cqe(l, 0x9999u, 42, 0), -1, "post into full CQ refused");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)4, "cq_tail unchanged (no overwrite)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)1, "overflow counter bumped");
    for (u32 i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(cqes[i].user_data, (u64)(0x1000u + i), "staged CQE intact");
        TEST_EXPECT_EQ((u64)(u32)cqes[i].result, (u64)i, "staged CQE result intact");
    }

    // Userspace reaps 2 (advances cq_head) -> 2 slots free -> 2 more posts land
    // in the wrapped slots; the 3rd is refused again.
    __atomic_store_n(&h->cq_head, 2u, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(loom_post_cqe(l, 0x2000u, 7, 0), 0, "post after reap succeeds");
    TEST_EXPECT_EQ(loom_post_cqe(l, 0x2001u, 8, 0), 0, "second post after reap succeeds");
    TEST_EXPECT_EQ(loom_post_cqe(l, 0x2002u, 9, 0), -1, "CQ full again");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)6, "two more posted (tail 4 -> 6)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)2, "overflow bumped once more");
    // tail 4 -> slot (4 & 3) = 0; tail 5 -> slot (5 & 3) = 1.
    TEST_EXPECT_EQ(cqes[4u & h->cq_mask].user_data, (u64)0x2000u, "wrapped slot holds new CQE");
    TEST_EXPECT_EQ(cqes[5u & h->cq_mask].user_data, (u64)0x2001u, "wrapped slot holds new CQE");

    loom_unref(l);
}

// SA-1 regression: loom_post_cqe must compute its write index from KERNEL-PRIVATE
// geometry, never from the shared (userspace-writable) ring header. A hostile
// cq_mask + cq_tail would otherwise drive an out-of-bounds kernel write (a
// Loom-3 trap, since the ring is userspace-controlled there). Here we corrupt
// the shared header and assert the CQE still lands at the kernel-private index.
void test_loom_post_cqe_ignores_hostile_header(void) {
    struct Loom *l = loom_create(4, 4);
    TEST_ASSERT(l != NULL, "loom_create(4,4)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // Simulate a hostile / corrupt userspace mutation of the shared control words.
    __atomic_store_n(&h->cq_mask, 0xFFFFFFFFu, __ATOMIC_RELEASE);
    __atomic_store_n(&h->cq_tail, 0x10000u, __ATOMIC_RELEASE);   // would be idx 0x10000 if trusted

    int rc = loom_post_cqe(l, 0xABCDu, 7, 0);
    TEST_EXPECT_EQ(rc, 0, "post succeeds on kernel-private geometry");
    // The CQE landed at the kernel-private index (priv cq_tail 0 & (cq_entries-1) = 0),
    // NOT at the hostile (0x10000 & 0xFFFFFFFF) which would be far out of bounds.
    TEST_EXPECT_EQ(cqes[0].user_data, (u64)0xABCDu, "CQE at kernel-private idx 0 (not the hostile idx)");
    // The kernel mirrored its own private tail (1), overwriting the hostile value.
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "kernel mirrored its private tail (1), clobbering the hostile cq_tail");

    loom_unref(l);
}

// ---------------------------------------------------------------------------
// Loom-3 SYS_LOOM_ENTER: the SQ consume + SQE dispatch + the inline-completion
// paths (no 9P engine needed). The engine-driven FSYNC dispatch + the
// submit-time rights pin + the #898 quiesce live in test_9p_client.c (they need
// the loopback client harness). Maps to specs/loom.tla Consume + Dispatch +
// PostCqe (the inline branch) + the SQ-index ring TOCTOU bounding.
// ---------------------------------------------------------------------------

// Stage one SQE into the ring at `slot` (identity SQ-index indirection: the SQ
// ring slot `slot` points at SQE `slot`). Writes via the kernel direct map.
static void loom_stage_sqe(struct Loom *l, u32 slot, u8 opcode, u8 flags,
                           u32 handle_idx, u32 len, u64 user_data) {
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *s = &sqes[slot];
    for (u32 i = 0; i < sizeof(*s); i++) ((u8 *)s)[i] = 0;
    s->opcode     = opcode;
    s->flags      = flags;
    s->handle_idx = handle_idx;
    s->len        = len;
    s->user_data  = user_data;
    sqa[slot] = slot;
}

// NOP: the io_uring smoke op completes inline with result 0 (no engine, no
// handle). Also pins the SQ consume + the kernel-private sq_head mirror.
void test_loom_enter_nop(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    loom_stage_sqe(l, 0, LOOM_OP_NOP, 0, 0, 0, 0x4E4F50ULL /* "NOP" */);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "NOP completes inline -> 1 CQE");
    TEST_EXPECT_EQ(cqes[0].user_data, (u64)0x4E4F50ULL, "CQE user_data echoed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)0, "NOP result 0");
    TEST_EXPECT_EQ((u64)h->sq_head, (u64)1, "sq_head advanced + mirrored to the shared header");
    TEST_EXPECT_EQ((u64)l->sq_head, (u64)1, "kernel-private sq_head advanced");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "NOP never goes in flight");

    loom_unref(l);
}

// The submit-time reject paths each post an error CQE (the SQE is still consumed
// -- io_uring semantics), never a dropped/lost SQE: a bad handle_idx (-EINVAL),
// an empty registered slot (-EBADF), a still-unimplemented-but-in-range opcode
// (-ENOSYS -- the direct-descriptor seam #916), and an out-of-range opcode (-EINVAL).
void test_loom_enter_submit_rejects(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    loom_stage_sqe(l, 0, LOOM_OP_FSYNC,   0, 99u, 0, 0xAAu);  // handle_idx out of range
    loom_stage_sqe(l, 1, LOOM_OP_FSYNC,   0, 0u,  0, 0xBBu);  // reg[0] empty (no register call)
    loom_stage_sqe(l, 2, LOOM_OP_WALK,    0, 0u,  0, 0xCCu);  // in-range, unimplemented (direct-descriptor #916)
    loom_stage_sqe(l, 3, LOOM_OP_COUNT,   0, 0u,  0, 0xDDu);  // out-of-range opcode
    __atomic_store_n(&h->sq_tail, 4u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 4, "four SQEs consumed (none dropped)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)4, "four error CQEs posted");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "no op dispatched (all rejected at submit)");
    // CQEs land in submit order (cq slot i = SQE i for the first pass).
    TEST_EXPECT_EQ(cqes[0].user_data, (u64)0xAAu, "cqe0 echoed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_INVAL), "bad handle_idx -> -EINVAL");
    TEST_EXPECT_EQ((u64)(s64)cqes[1].result, (u64)(s64)(-(s32)T_E_BADF),  "empty reg slot -> -EBADF");
    TEST_EXPECT_EQ((u64)(s64)cqes[2].result, (u64)(s64)(-(s32)T_E_NOSYS), "unimplemented opcode -> -ENOSYS");
    TEST_EXPECT_EQ((u64)(s64)cqes[3].result, (u64)(s64)(-(s32)T_E_INVAL), "out-of-range opcode -> -EINVAL");

    loom_unref(l);
}

// A still-reserved SQE flag is rejected (-EINVAL) so a future flag is never
// silently ignored; an out-of-range SQ-index indirection is DROPPED (the
// diagnostic counter bumps, the SQE array is never indexed out of bounds -- the
// SA-1 discipline on the SQ side). LINK/DRAIN/MULTISHOT are now IMPLEMENTED
// (Loom-5), so CQE_SKIP is the remaining reserved flag (deferred out of Loom-5b
// -- it suppresses a success CQE, which needs a loom_order.tla carve-out first).
void test_loom_enter_flags_and_bad_index(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);

    loom_stage_sqe(l, 0, LOOM_OP_NOP, LOOM_SQE_CQE_SKIP, 0, 0, 0x11u);  // reserved flag set
    loom_stage_sqe(l, 1, LOOM_OP_NOP, 0, 0, 0, 0x22u);
    sqa[1] = 999u;   // out-of-range SQ-index indirection -> dropped, never indexes sqes[]
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 2, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed (the flag-reject); the bad-index one was dropped");
    TEST_EXPECT_EQ((u64)h->dropped, (u64)1, "bad SQ indirection counted as dropped");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "one CQE (the reserved-flag reject)");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_INVAL), "reserved flag -> -EINVAL");
    TEST_EXPECT_EQ((u64)h->sq_head, (u64)2, "sq_head consumed both ring slots");

    loom_unref(l);
}

// F2 regression: submit-time CQ admission back-pressures (never silently drops a
// completion -- I-29). Fill the CQ via inline NOPs without reaping; a further
// enter with work available must consume 0 (admission blocks) and NOT bump
// overflow; after reaping, a re-enter consumes again. Maps to specs/loom.tla's
// "an admitted op always reaches a CQE" -- the impl back-pressures at submit
// instead of dropping at completion.
void test_loom_enter_cq_admission_backpressure(void) {
    struct Loom *l = loom_create(4, 4);   // sq 4, cq 4 (cq == sq exercises the cap fast)
    TEST_ASSERT(l != NULL, "loom_create(4,4)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    // Stage 4 NOPs (slots 0..3); enter consumes all 4 (cq_ready 0..3 < cq_entries
    // 4 at each admission check) -> CQ now full (cq_ready == 4).
    for (u32 i = 0; i < 4; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x100u + i);
    __atomic_store_n(&h->sq_tail, 4u, __ATOMIC_RELEASE);
    int n1 = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n1, 4, "first enter consumes all 4 NOPs");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)4, "CQ full (4 CQEs)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no overflow");

    // More work available (re-bump sq_tail to 8, reusing the ring slots), but the
    // CQ is full -> admission blocks BEFORE consuming -> 0 submitted, 0 dropped,
    // 0 overflow (the completion is NOT lost -- the SQE waits for the next enter).
    for (u32 i = 0; i < 4; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x200u + i);
    __atomic_store_n(&h->sq_tail, 8u, __ATOMIC_RELEASE);
    int n2 = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n2, 0, "second enter consumes 0 (CQ-full admission back-pressure)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)4, "cq_tail unchanged (no completion posted/dropped)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no overflow -- back-pressured at submit, not dropped");
    TEST_EXPECT_EQ((u64)h->sq_head, (u64)4, "sq_head unchanged (no SQE consumed)");

    // Reap all 4 (advance cq_head) -> CQ drains -> a re-enter consumes the waiting 4.
    __atomic_store_n(&h->cq_head, 4u, __ATOMIC_RELEASE);
    int n3 = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n3, 4, "after reap, the waiting SQEs are consumed");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "still no overflow across the whole sequence");

    loom_unref(l);
}

// SA-2: KOBJ_LOOM is non-transferable, so handle_dup must reject a loom fd (the
// same gate as the hardware / srv kinds). Pins the non-dup-ability explicitly.
void test_loom_dup_rejected(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &fd) == 0, "setup");

    hidx_t dup = handle_dup(p, fd, RIGHT_READ | RIGHT_WRITE);
    TEST_EXPECT_EQ((int)dup, -1, "handle_dup(loom_fd) rejected (KOBJ_LOOM non-transferable)");

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// Loom-4b: the CQ wait-list. A SYS_LOOM_ENTER thread that finds a sibling
// already holding the reader role installs a poll_waiter here and sleeps;
// loom_post_cqe wakes it after publishing a CQE. These tests pin the wake
// MECHANISM + the new wait-phase early-outs deterministically (white-box).
// The full sleep -> woken-by-a-real-sibling-reader interleaving needs a second
// kernel thread + the loopback client and lands with the deferred multi-in-
// flight / cross-Proc-death SMP harness (shared with #841/#907). Maps to
// specs/loom.tla: PostCqe-wake (NoMissedCqWake, I-9) + CqWaitFlagSound +
// CqWaitCommitOrSleep's give-up arms.
// ---------------------------------------------------------------------------

// PostCqe-wake (NoMissedCqWake): a successful CQE post sets the registered
// CQ-waiter's wake flag, so a thread sleeping in loom_wait_for_completions
// re-evaluates. poll_waiter_list_wake writes pw->ready before signalling, so
// the flag is the cross-lock hand-off loom_cqw_cond reads under the rendez lock.
void test_loom_cq_waiter_wake(void) {
    struct Loom *l = loom_create(4, 8);
    TEST_ASSERT(l != NULL, "loom_create(4,8)");

    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter pw;
    poll_waiter_init(&pw, &r);

    // Register-then-observe install on the ring's CQ wait-list.
    poll_waiter_list_register(&l->cq_waiters, &pw);
    TEST_ASSERT(pw.ready == false, "fresh CQ-waiter not ready before any post");

    // A successful post wakes the registered waiter.
    TEST_EXPECT_EQ(loom_post_cqe(l, 0xCAFEu, 0, 0), 0, "CQE post succeeds");
    TEST_ASSERT(pw.ready == true, "post sets the registered CQ-waiter's wake flag");

    poll_waiter_list_unregister(&pw);
    pw.magic = 0;
    loom_unref(l);
}

// CqWaitFlagSound: a REFUSED post (CQ full -> no completion became available)
// must NOT set the wake flag -- a sleeping waiter is not spuriously woken to
// re-sample an unchanged (already-full) CQ. (The wake lives only on the
// success path, after the cq_tail bump; the CQ-full path returns -1 first.)
void test_loom_cq_waiter_no_spurious_wake_on_full(void) {
    struct Loom *l = loom_create(4, 4);   // cq_entries = 4
    TEST_ASSERT(l != NULL, "loom_create(4,4)");

    for (u32 i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(loom_post_cqe(l, 0x10u + i, 0, 0), 0, "fill CQ");
    }

    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter pw;
    poll_waiter_init(&pw, &r);
    poll_waiter_list_register(&l->cq_waiters, &pw);

    TEST_EXPECT_EQ(loom_post_cqe(l, 0x99u, 0, 0), -1, "post refused (CQ full)");
    TEST_ASSERT(pw.ready == false, "a refused post does not wake (no spurious flag)");

    poll_waiter_list_unregister(&pw);
    pw.magic = 0;
    loom_unref(l);
}

// The reworked wait phase must NOT hang on the common no-sibling path. With
// inline NOPs (which complete during submit), a BLOCKING enter with
// min_complete == the NOP count sees the CQ already satisfied at the first
// sample and returns without ever sleeping (async_inflight stays 0 -- there is
// no reader to drive). CqWaitCommitOrSleep: flag/level satisfied -> return.
void test_loom_enter_inline_min_complete(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    for (u32 i = 0; i < 3; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x300u + i);
    __atomic_store_n(&h->sq_tail, 3u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 3, 3, 0);   // min_complete = 3, BLOCKING (no NONBLOCK)
    TEST_EXPECT_EQ(n, 3, "3 NOPs consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)3, "3 inline CQEs; wait returned without sleeping");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "nothing went in flight (no reader to drive)");

    loom_unref(l);
}

// CqWaitCommitOrSleep give-up arm: min_complete > 0 but nothing is (or can be)
// in flight -> the wait returns immediately rather than blocking for CQEs that
// can never arrive. A blocking enter with no SQEs staged and async_inflight == 0
// must NOT hang.
void test_loom_enter_min_complete_no_inflight(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");

    int n = loom_enter(l, 0, 5, 0);   // min_complete = 5, BLOCKING, but no work
    TEST_EXPECT_EQ(n, 0, "no SQEs consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)0, "no CQEs; wait returned without hanging");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "nothing in flight");

    loom_unref(l);
}

// ---------------------------------------------------------------------------
// Loom-4c: the SQPOLL poll-thread. LOOM_SETUP_SQPOLL spawns a per-ring kproc()
// kthread that drains the SQ zero-syscall + drives the elected reader; loom_free
// joins it. These pin the spawn -> park -> stop -> join lifecycle + the zero-
// syscall drain DETERMINISTICALLY (white-box). The full kthread-vs-ENTER-vs-
// Proc-death interleaving over a live dev9p client lands with the deferred SMP
// harness (#907). Maps to LOOM.md 8.6; the kthread reuses the audited
// specs/loom.tla Consume / Dispatch / PostCqe path -- only the park wait/wake is
// the new modeled surface (CqWaitRegister already covered by 4b).
// ---------------------------------------------------------------------------

// Spawn + clean join. Setting up an SQPOLL ring spawns the kthread (l->sqpoll
// set; out.flags echoes the grant); tearing the ring down (proc_free -> the last
// handle close -> loom_unref -> loom_free) stops + JOINS the kthread and frees
// the ring EXACTLY once -- no hang, no leak. With no SQEs the kthread parks
// immediately, so the join exercises the wake-park -> observe-stopping ->
// EXITING-terminal -> thread_free path end to end.
void test_loom_sqpoll_setup_and_teardown(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 destroyed0 = loom_total_destroyed();

    struct loom_params kp;
    hidx_t fd = -1;
    int rc = sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd);
    TEST_EXPECT_EQ(rc, 0, "SQPOLL setup succeeds");
    TEST_ASSERT(fd >= 0, "setup returned a valid fd");
    TEST_EXPECT_EQ(kp.flags, (u32)LOOM_SETUP_SQPOLL, "out.flags echoes the SQPOLL grant");

    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "handle_get(loom fd)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_ASSERT(l->sqpoll != NULL, "SQPOLL kthread spawned");
    handle_put(&h);

    // proc_free closes the handle -> loom_unref -> loom_free joins the kthread +
    // frees the ring exactly once. A hang here would mean the join never
    // converged (the whole point of the test).
    test_proc_drop(p);
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1,
                   "ring freed (kthread joined) exactly once on teardown");
}

// Zero-syscall drain. On an SQPOLL ring the user stages SQEs + bumps sq_tail; an
// ENTER(to_submit=0) WAKES the kthread (it never submits on SQPOLL), which drains
// the SQ + posts the inline NOP CQEs. cq_tail reaching the staged count proves
// the kthread -- not this call -- consumed. The test cooperatively sched()s so
// the kthread runs even at -smp 1 (the test_torpor.c yield idiom); the spin is
// bounded so a real failure terminates rather than hangs.
void test_loom_sqpoll_drains_sq(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd), 0,
                   "SQPOLL setup");
    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "handle_get");
    struct Loom *l = (struct Loom *)h.obj;
    struct loom_ring_hdr *hdr = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    for (u32 i = 0; i < 3; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x700u + i);
    __atomic_store_n(&hdr->sq_tail, 3u, __ATOMIC_RELEASE);

    // ENTER(to_submit=0, min_complete=0) on an SQPOLL ring submits nothing -- it
    // just wakes the (parked) kthread to drain.
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0,
                   "ENTER on SQPOLL submits nothing (kthread owns submission)");

    // Yield to the kthread until it has drained the 3 NOPs. Bounded so a hung
    // kthread fails the test rather than wedging the whole suite.
    bool drained = false;
    for (u32 round = 0; round < 100000u; round++) {
        // Read the shared-header mirror (loom_post_cqe publishes it with a release
        // store), NOT the kernel-private l->cq_tail (a plain store under l->lock) --
        // the mirror read is a clean acquire with no plain-write/atomic-read race (F5).
        if (__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE) >= 3u) { drained = true; break; }
        sched();   // cooperative yield -> the kthread gets the CPU at -smp 1
    }
    TEST_ASSERT(drained, "SQPOLL kthread drained the SQ zero-syscall (3 CQEs posted)");

    handle_put(&h);
    test_proc_drop(p);
}

// F2 regression: on CQ-full backpressure the SQPOLL kthread PARKS (state ==
// THREAD_SLEEPING) -- it does NOT busy-loop. cq_entries == 2*sq_entries (the
// io_uring default), so the CQ fills only after MORE NOPs than the SQ ring holds --
// submit cq_entries NOPs in sq_entries-sized batches (refilling the ring), draining
// each batch zero-syscall, WITHOUT reaping. Then one more NOP, pending, with the CQ
// full: the kthread cannot admit it. Pre-F2 the park cond fired on sq_tail != sq_head
// alone, sleep() returned WITHOUT sched()ing, and the kthread spun at 100% CPU (on
// -smp 1 it would never yield -> this test would HANG); post-F2 the cond also gates on
// CQ room, so the kthread parks. Then reap (advance cq_head) + ENTER-wake and the
// extra NOP drains -- proving the park RELEASES on the reap+ENTER path (the
// NEED_WAKEUP contract).
void test_loom_sqpoll_parks_on_cq_full(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 4, LOOM_SETUP_SQPOLL, &kp, &fd), 0, "SQPOLL setup");
    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "handle_get");
    struct Loom *l = (struct Loom *)h.obj;
    struct loom_ring_hdr *hdr = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    u32 cqe = l->cq_entries;   // = 2 * sq_entries (8); the SQ ring holds only sq_entries (4)
    u32 sqe = l->sq_entries;

    // Fill the CQ to cq_entries WITHOUT reaping, in sqe-sized batches (each fully
    // drained before the next, so the kthread parks-on-SQ-drained between them).
    u32 produced = 0;
    while (produced < cqe) {
        u32 batch = (cqe - produced < sqe) ? (cqe - produced) : sqe;
        for (u32 i = 0; i < batch; i++)
            loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0xA00u + produced + i);
        produced += batch;
        __atomic_store_n(&hdr->sq_tail, produced, __ATOMIC_RELEASE);
        TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0, "ENTER (fill batch)");
        bool got = false;
        for (u32 r = 0; r < 100000u && !got; r++) {
            if (__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE) >= produced) got = true; else sched();
        }
        TEST_ASSERT(got, "batch drained into the CQ");
    }
    TEST_EXPECT_EQ(__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE), cqe, "CQ full (cq_entries unreaped)");

    // One more NOP, pending, with the CQ full + NOT reaped -> the kthread cannot
    // admit it and must PARK. Stage into ring slot (produced & (sqe-1)).
    loom_stage_sqe(l, produced & (sqe - 1u), LOOM_OP_NOP, 0, 0, 0, 0xAFFu);
    __atomic_store_n(&hdr->sq_tail, produced + 1u, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0, "ENTER (still CQ-full)");

    // l->sqpoll->state is scheduler-written (a best-effort coarse poll -- no clean
    // atomic accessor exists; the hardware load is atomic and TSan-pedantry aside
    // this is a liveness observation). Pre-F2 the kthread never reaches SLEEPING (the
    // always-true cond early-returns from sleep without sched()).
    bool parked = false;
    for (u32 r = 0; r < 100000u && !parked; r++) {
        if (__atomic_load_n(&l->sqpoll->state, __ATOMIC_ACQUIRE) == THREAD_SLEEPING) parked = true;
        else sched();
    }
    TEST_ASSERT(parked, "kthread parks on CQ-full backpressure (no busy-loop)");
    TEST_EXPECT_EQ(__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE), cqe,
                   "extra NOP NOT admitted while CQ full");

    // Reap everything + ENTER-wake: the park releases (cond now sees CQ room) -> the
    // extra NOP drains.
    __atomic_store_n(&hdr->cq_head, cqe, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0, "ENTER after reap");
    bool resumed = false;
    for (u32 r = 0; r < 100000u && !resumed; r++) {
        if (__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE) >= cqe + 1u) resumed = true; else sched();
    }
    TEST_ASSERT(resumed, "kthread resumes + drains the extra NOP after reap+ENTER");

    handle_put(&h);
    test_proc_drop(p);
}

// Fid-lift audit F1: an SQPOLL kthread charges the CREATOR's PROC_THREAD_MAX
// budget (I-32). The kthread runs under kproc (exempt), so pre-F1 an
// unprivileged Proc could mint one 32-KiB-kstack kthread per ring, bounded only
// by the handle table the fid lift just quadrupled. The budget is pre-loaded to
// the boundary (the test_resource.c idiom) so only two real kthreads spawn:
// refusal at the cap, no over-settle by the refused ring's free, plain rings
// uncharged, and the close -> uncharge -> fits-again round trip.
void test_loom_sqpoll_charges_thread_budget(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->principal_id = 1000u;                  // a real user -- NOT I-32 exempt
    u64 destroyed0 = loom_total_destroyed();

    p->thread_count = PROC_THREAD_MAX - 1;    // boundary without 255 real threads

    // At cap-1: one SQPOLL setup fits and takes the charge.
    struct loom_params kp;
    hidx_t fd1 = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd1), 0,
                   "SQPOLL setup at cap-1 succeeds");
    TEST_EXPECT_EQ((u64)p->loom_sqpoll_count, (u64)1, "kthread charged to the creator");

    // At the cap: the next SQPOLL setup is REFUSED (pre-F1 it succeeded and the
    // kthread escaped the budget entirely).
    hidx_t fd2 = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd2) < 0,
                "SQPOLL setup at the cap refused");
    // The refused ring never took a charge, and its free must not settle one
    // (sqpoll_charged gates the uncharge): still exactly 1.
    TEST_EXPECT_EQ((u64)p->loom_sqpoll_count, (u64)1, "refusal did not over-settle");
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1,
                   "the refused ring itself was reclaimed");

    // A plain (non-SQPOLL) ring is not thread-charged -- the budget bounds
    // kthreads, not rings.
    hidx_t fd3 = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, 0, &kp, &fd3), 0,
                   "a plain ring still fits at the thread cap");
    TEST_EXPECT_EQ((u64)p->loom_sqpoll_count, (u64)1, "plain ring took no charge");

    // Close the charged ring: loom_free joins the kthread and settles the
    // charge exactly once; the freed budget admits a new SQPOLL setup.
    TEST_EXPECT_EQ(handle_close(p, fd1), 0, "close the SQPOLL ring");
    TEST_EXPECT_EQ((u64)p->loom_sqpoll_count, (u64)0, "charge settled on close");
    hidx_t fd4 = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd4), 0,
                   "SQPOLL setup fits again after the close (uncharge proof)");

    p->thread_count = 0;                      // reset for proc_free's gate
    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// #130: the I-32 charge balances across the WHOLE Loom lifetime, in both
// directions.
//
// The ring is the one eager charger whose Burrow keeps a second owner past the
// VMA: loom_create holds a handle_count ref for the Loom's whole life so the
// ring pages cannot vanish under the kernel's ring_kva. That makes "the mapping
// went away" and "the pages went away" two DIFFERENT events, and #106-F1 got
// the accounting wrong on each of them in turn:
//
//   - refunding on the VMA's TYPE let EL0 detach the ring, take the refund,
//     keep the pages on the Loom's ref, and re-attach a full PROC_PAGE_MAX --
//     an unprivileged breach of the per-Proc floor;
//   - refunding on a handle_count SAMPLED BEFORE the drop then swung it the
//     other way: on the ordinary teardown order (detach, then close -- what
//     libthyla_rs' Ring::drop does) the sample reads 1, the refund is skipped,
//     and loom_free freed the pages with nothing uncharging -- a PERMANENT
//     over-charge.
//
// This test pins both halves, so it fails on EITHER of those states as well as
// on any future one that gets the pairing wrong: the detach must refund
// NOTHING, and the close must return the counter exactly to baseline.
// ---------------------------------------------------------------------------
void test_loom_ring_charge_balance(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u32 base = __atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE);

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    u32 ring_pages = (u32)burrow_backing_pages((size_t)kp.ring_size);
    TEST_ASSERT(ring_pages > 0, "the ring occupies at least one page");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + ring_pages), "setup charged the ring's occupancy");

    // Detach the ring VA while the Loom still holds its handle_count ref. The
    // pages do NOT free here, so nothing may be refunded -- this is the leg
    // that, refunded, hands EL0 free budget.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, kp.ring_va, kp.ring_size), 0L,
                   "detach the ring VA");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + ring_pages),
                   "a detach that frees nothing refunds nothing");

    // Close the Loom: loom_free drops the last ref, the pages free, and the
    // charge is settled -- exactly once, and only now.
    TEST_EXPECT_EQ(handle_close(p, loom_fd), 0, "close the loom fd");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE), (u64)base,
                   "the loom close settled the ring charge back to baseline");

    test_proc_drop(p);
}

// #130, the registered-buffer twin. A registered buffer is the user's OWN
// attach region -- so it can be the whole page budget, which is what makes the
// over-charge direction matter here more than on the (small) ring. Same shape:
// the table's pin outlives the VMA, so the detach must not refund and the pin's
// release must. Both release sites are covered: the re-register displacement
// (loom_register_buffers) and the teardown (loom_free).
void test_loom_regbuf_charge_balance(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    // Baseline AFTER setup, so the ring's own charge is not in the arithmetic.
    u32 base = __atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE);

    // --- leg 1: the pin is displaced by a re-register ---
    s64 bva = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva > 0, "attach a buffer page");
    u32 bpages = (u32)burrow_backing_pages(PAGE_SIZE);
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + bpages), "the attach charged the buffer");

    struct loom_buf_reg br = { .va = (u64)bva, .len = PAGE_SIZE };
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, &br, 1) == 0, "register");

    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, (u64)bva, PAGE_SIZE), 0L, "detach the buffer VA");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + bpages),
                   "detach with the table still pinning refunds nothing");

    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, NULL, 0) == 0, "clear the table");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE), (u64)base,
                   "the displaced pin's release settled the buffer charge");

    // --- leg 2: the pin is released by the teardown ---
    s64 bva2 = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva2 > 0, "attach a second buffer page");
    struct loom_buf_reg br2 = { .va = (u64)bva2, .len = PAGE_SIZE };
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, &br2, 1) == 0, "register #2");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, (u64)bva2, PAGE_SIZE), 0L, "detach #2's VA");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + bpages), "still pinned, still charged");

    // Detach the ring too, so the close faces the FULL production teardown
    // order: every VA released first (what Ring::drop does), then the fd. Both
    // objects are then charged-but-unmapped, and the close must settle both.
    u32 ring_pages = (u32)burrow_backing_pages((size_t)kp.ring_size);
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, kp.ring_va, kp.ring_size), 0L,
                   "detach the ring VA too");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + bpages), "the ring detach refunds nothing either");

    TEST_EXPECT_EQ(handle_close(p, loom_fd), 0, "close the loom fd");
    // base was sampled AFTER setup, so it still contains the ring's charge;
    // settling both lands one ring below it.
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base - ring_pages),
                   "the teardown settled both the buffer pin and the ring");

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// loom.regbuf_foreign_charge_not_refunded (#132)
//
// A registered buffer is resolved out of ANY writable ANON VMA of the Loom's
// owner -- and a Weft ring that a SHARER allocated and burrow_share_into'd
// satisfies every loom_resolve_buf gate (ANON, writable, one VMA). Weft-6c-1's
// shipped path registers exactly that: WeftFlow::open maps the flow's ring and
// registers the WHOLE of it.
//
// So the owner can hold a Loom pin on a region it did not pay for, and when
// that pin is the drop that frees the pages, refunding the owner pays back
// somebody else's charge. That is an UNDER-count -- it inflates the owner's
// effective budget, the direction that breaks I-32 -- and it is what #130's
// "`p` is the charged Proc by construction" comment let through: that argument
// proves p owns the LOOM, not that p paid for the BUFFER.
//
// The discriminating assertion is that the user's page_count never dips below
// its own baseline. The baseline is sampled AFTER the loom setup so it carries
// the ring's charge, which is what makes a spurious one-page refund visible
// rather than clamped away at zero.
// ---------------------------------------------------------------------------
void test_loom_regbuf_foreign_charge_not_refunded(void) {
    struct Proc *payer = test_proc_make();   // allocates + pays for the region
    struct Proc *user  = test_proc_make();   // maps it and pins it in its Loom
    TEST_ASSERT(payer != NULL && user != NULL, "proc_alloc x2");

    // DISTINCT pids, or this test cannot discriminate. proc_alloc stamps pid 0
    // (rfork_internal assigns the real one), so two bare test Procs are both 0
    // and the charge record's owner check would match the wrong Proc trivially
    // -- the test would pass without proving attribution. Production Procs get
    // distinct monotonic pids; the fixture has to reproduce that, not assume it.
    payer->pid = 90001;
    user->pid  = 90002;

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(user, 8, 0, &kp, &loom_fd) == 0, "setup");
    u32 ubase = __atomic_load_n(&user->as->page_count, __ATOMIC_ACQUIRE);
    TEST_ASSERT(ubase > 0, "the baseline carries the ring charge, so a refund is visible");

    u32 pbase = __atomic_load_n(&payer->as->page_count, __ATOMIC_ACQUIRE);
    s64 va = sys_burrow_attach_for_proc(payer, PAGE_SIZE);
    TEST_ASSERT(va > 0, "the payer attaches the region");
    u32 pages = (u32)burrow_backing_pages(PAGE_SIZE);
    TEST_EXPECT_EQ((u64)__atomic_load_n(&payer->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(pbase + pages), "the attach charged the PAYER");

    spin_lock(&payer->as->lock);
    struct Vma *pv = vma_lookup(payer, (u64)va);
    struct Burrow *v = pv ? pv->burrow : NULL;
    spin_unlock(&payer->as->lock);
    TEST_ASSERT(v != NULL, "the region's Burrow");

    // Share it into the user (the SYS_WEFT_MAP half) and register the whole of
    // it as a Loom buffer (the WeftFlow::open half).
    // Inside [EXEC_USER_BURROW_BASE, TOP): SYS_BURROW_DETACH bounds-checks the
    // VA against that window before it even looks up the VMA, so a share placed
    // below 4 GiB is unreachable to the detach half of this test.
    const u64 uva = 0x140000000ull;
    spin_lock(&user->as->lock);
    TEST_EXPECT_EQ(burrow_share_into(user, v, uva, VMA_PROT_RW), 0, "share into the user");
    spin_unlock(&user->as->lock);
    TEST_EXPECT_EQ((u64)__atomic_load_n(&user->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)ubase, "a share does not charge the user's page_count");

    struct loom_buf_reg br = { .va = uva, .len = PAGE_SIZE };
    TEST_ASSERT(sys_loom_register_buffers_for_proc(user, loom_fd, &br, 1) == 0,
                "the user registers the shared region as a Loom buffer");

    // The payer walks away (#131): its charge settles here, while the region
    // lives on under the user's mapping + the Loom pin.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(payer, (u64)va, PAGE_SIZE), 0L, "payer detaches");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&payer->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)pbase, "the payer's charge settled at its own detach");

    // The user drops its mapping. SHARED_IN, so no page_count movement -- and
    // the Loom pin is now the ONLY thing holding the region.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(user, uva, PAGE_SIZE), 0L, "user detaches");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&user->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)ubase, "detaching a SHARED_IN mapping refunds nothing");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 0, "no mappings left -- only the Loom pin");

    // THE FIX: clearing the table drops that pin and FREES the pages. Pre-#132
    // this refunded the user for the payer's region.
    u64 destroyed_before = burrow_total_destroyed();
    TEST_ASSERT(sys_loom_register_buffers_for_proc(user, loom_fd, NULL, 0) == 0, "clear the table");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
                   "the displaced pin was the drop that freed the pages");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&user->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)ubase,
                   "freeing a region the user never paid for refunds the user NOTHING");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&payer->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)pbase, "and does not refund the payer twice either");

    TEST_EXPECT_EQ(handle_close(user, loom_fd), 0, "close the loom fd");
    test_proc_drop(user);
    test_proc_drop(payer);
}
