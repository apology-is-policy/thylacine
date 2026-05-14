// SYS_PIPE integration tests (P5-fd-pipe).
//
// Exercises the syscall handler's integration of pipe_create +
// handle_alloc + KOBJ_SPOOR release-path discipline. Calls
// `sys_pipe_for_proc(p, ...)` (the non-static inner of the SVC
// handler) with a test Proc; verifies the resulting handles are
// well-formed KOBJ_SPOOR slots; verifies proc_free's
// handle_table_free walks the table and spoor_clunks each entry
// end-to-end.
//
// Coverage:
//
//   sys_pipe.allocates_two_distinct_spoor_handles
//     Returns 0; two distinct fds; both KOBJ_SPOOR; both have
//     RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER. spoor_total_allocated
//     incremented by 2.
//
//   sys_pipe.proc_free_releases_handles
//     pipe_create's ring + Spoors are released when the Proc is
//     dropped — verifies pipe_total_freed increments by 1 AND
//     spoor_total_freed increments by 2.
//
//   sys_pipe.handle_close_releases_one_end
//     Explicit handle_close on one fd releases that Spoor (drops
//     the ring's ref from 2 to 1) but the other end stays alive.
//     Closing the second fd frees the ring.

#include "test.h"

#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

// Inner SVC handler (extern declaration; defined in kernel/syscall.c).
extern int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr);

void test_sys_pipe_allocates_two_distinct_spoor_handles(void);
void test_sys_pipe_proc_free_releases_handles(void);
void test_sys_pipe_handle_close_releases_one_end(void);

// Local copy of the proc-test helpers used by test_handle.c. Kept
// independent so the two test files can be reordered without import
// coupling.
static struct Proc *make_test_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    return p;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_sys_pipe_allocates_two_distinct_spoor_handles(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 spoor_allocated_before = spoor_total_allocated();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0,
        "sys_pipe_for_proc returns 0");
    TEST_ASSERT(fd_rd >= 0 && fd_wr >= 0,
        "both fds allocated");
    TEST_ASSERT(fd_rd != fd_wr, "distinct fds");
    TEST_EXPECT_EQ(spoor_total_allocated() - spoor_allocated_before, 2ull,
        "two Spoors allocated");

    struct Handle *h_rd = handle_get(p, fd_rd);
    struct Handle *h_wr = handle_get(p, fd_wr);
    TEST_ASSERT(h_rd != NULL && h_wr != NULL,
        "handles installed");
    TEST_EXPECT_EQ((int)h_rd->kind, (int)KOBJ_SPOOR, "rd is KOBJ_SPOOR");
    TEST_EXPECT_EQ((int)h_wr->kind, (int)KOBJ_SPOOR, "wr is KOBJ_SPOOR");
    rights_t expected_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER;
    TEST_EXPECT_EQ(h_rd->rights, expected_rights, "rd rights");
    TEST_EXPECT_EQ(h_wr->rights, expected_rights, "wr rights");
    TEST_ASSERT(h_rd->obj != h_wr->obj,
        "rd and wr point at distinct Spoors");

    drop_test_proc(p);
}

void test_sys_pipe_proc_free_releases_handles(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 pipe_freed_before  = pipe_total_freed();
    u64 spoor_freed_before = spoor_total_freed();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0,
        "sys_pipe");

    // Drop the Proc. handle_table_free walks both slots; per-kind
    // release calls spoor_clunk for each KOBJ_SPOOR. spoor_clunk
    // routes through devpipe_close which drops the ring's per-end
    // ref. When both refs hit 0, the ring is freed.
    drop_test_proc(p);

    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 1ull,
        "proc_free released the pipe ring");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 2ull,
        "proc_free released both Spoors");
}

void test_sys_pipe_handle_close_releases_one_end(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 pipe_freed_before  = pipe_total_freed();
    u64 spoor_freed_before = spoor_total_freed();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    // Close the read end. devpipe_close sets write_eof (no — read_eof;
    // closing the read end means read_eof = true) + wakes write_rendez
    // (no waiter; no-op) + drops the ring's per-endpoint ref (2 → 1).
    // Ring is NOT freed yet.
    TEST_EXPECT_EQ(handle_close(p, fd_rd), 0, "close rd");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 0ull,
        "ring NOT freed after one end close");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 1ull,
        "one Spoor freed (the rd-end)");

    // Close the write end. Drops the ring's last per-endpoint ref;
    // ring is freed.
    TEST_EXPECT_EQ(handle_close(p, fd_wr), 0, "close wr");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 1ull,
        "ring freed after second end close");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 2ull,
        "both Spoors freed");

    drop_test_proc(p);
}
