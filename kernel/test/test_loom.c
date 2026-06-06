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
#include <thylacine/exec.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
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

// The testable syscall inners live in kernel/syscall.c (declared in loom.h);
// sys_pipe_for_proc is the KOBJ_SPOOR-pair maker (declared nowhere public --
// forward-declare it like test_sys_burrow.c does for its inner).
extern int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr);

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
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd), -1,
                   "unsupported flag rejected at Loom-2a");
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
