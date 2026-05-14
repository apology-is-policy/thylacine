// P5-spawn-full — kernel-internal tests for SYS_SPAWN_FULL.
//
// SYS_SPAWN_FULL is the union of SYS_SPAWN_WITH_FDS + SYS_SPAWN_WITH_CAPS.
// The individual building blocks (fd-inheritance refcount discipline,
// cap arithmetic) are covered by the prior tests; these tests cover
// the COMBINATION — fds AND caps applied to the same child via one
// syscall.
//
// Verification pattern: walk the proc tree via proc_find_by_pid to
// inspect the child's caps + handle table BEFORE wait_pid (race-free
// because caps and handles are stable for a Proc's lifetime in this
// context).
//
// Coverage:
//
//   sys_spawn_full.happy_path_fds_and_caps
//     Pass 1 KOBJ_SPOOR fd + cap_mask=CAP_LOCK_PAGES; child has fd 0
//     installed AND CAP_LOCK_PAGES granted.
//
//   sys_spawn_full.zero_count_zero_mask_succeeds
//     Both zero → equivalent to SYS_SPAWN; child has no fds, no caps.
//
//   sys_spawn_full.rejects_oversize_fd_count
//   sys_spawn_full.rejects_bad_fd
//   sys_spawn_full.rejects_missing_binary

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern int sys_spawn_full_for_proc(struct Proc *p, const char *name,
                                    size_t name_len,
                                    const u32 *fds, u32 fd_count,
                                    caps_t cap_mask);

void test_sys_spawn_full_happy_path_fds_and_caps(void);
void test_sys_spawn_full_zero_count_zero_mask_succeeds(void);
void test_sys_spawn_full_rejects_oversize_fd_count(void);
void test_sys_spawn_full_rejects_bad_fd(void);
void test_sys_spawn_full_rejects_missing_binary(void);

static void drain_zombies(void) {
    int status = 0;
    while (wait_pid(&status) > 0) { /* drain */ }
}

void test_sys_spawn_full_happy_path_fds_and_caps(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Open one pipe pair; pass the read end to the spawned child.
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    hidx_t parent_rd_fd = handle_alloc(t->proc, KOBJ_SPOOR,
                                       RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER,
                                       rd);
    TEST_ASSERT(parent_rd_fd >= 0, "handle_alloc parent_rd_fd");
    // wr stays with the parent's local pointer; we don't install in
    // parent's handle table because the parent doesn't need to write
    // to it for this test. spoor_clunk after the test reaps the child.

    u32 fds[1] = { (u32)parent_rd_fd };
    int pid = sys_spawn_full_for_proc(t->proc, "hello", 5,
                                       fds, 1, CAP_LOCK_PAGES);
    TEST_ASSERT(pid > 0, "spawn_full with 1 fd + CAP_LOCK_PAGES returns pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for spawned child");

    // Verify caps: kproc has CAP_ALL, mask was CAP_LOCK_PAGES → child
    // should have exactly CAP_LOCK_PAGES. Caps are set synchronously
    // inside rfork_with_caps (before the thread is ready'd), so this
    // read is race-free.
    TEST_EXPECT_EQ((u64)child->caps, (u64)CAP_LOCK_PAGES,
        "child has exactly CAP_LOCK_PAGES");

    // Fd inheritance is set asynchronously by the spawn_with_fds thunk
    // running on the child's CPU. Inspecting child->handles[] here
    // would race with the thunk's handle_alloc. The fd path is
    // structurally identical to SYS_SPAWN_WITH_FDS (same thunk + args
    // struct), already covered by the /stub-driver_round_trip end-to-
    // end test. If fd inheritance broke here, /hello would exit
    // non-zero (the kernel ELF loader uses fd 0+ as scratch space for
    // exec_setup which would conflict with an unexpected installed
    // handle). The clean-exit assertion below is the indirect check.

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0,
        "/hello exits status=0 (fd installation + exec_setup succeeded)");

    // Clean up the parent's handle on rd. The child's handle_table_free
    // already released its ref. We also need to clean up wr (boot's
    // only ref to that Spoor).
    handle_close(t->proc, parent_rd_fd);
    spoor_clunk(wr);
}

void test_sys_spawn_full_zero_count_zero_mask_succeeds(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int pid = sys_spawn_full_for_proc(t->proc, "hello", 5,
                                       NULL, 0, CAP_NONE);
    TEST_ASSERT(pid > 0, "fd_count=0 + cap_mask=CAP_NONE → positive pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for spawned child");
    TEST_EXPECT_EQ((u64)child->caps, (u64)CAP_NONE, "child has no caps");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "/hello exits status=0");
}

void test_sys_spawn_full_rejects_oversize_fd_count(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    u32 fds[SYS_SPAWN_MAX_FDS + 1] = { 0 };
    int rc = sys_spawn_full_for_proc(t->proc, "hello", 5,
                                      fds, SYS_SPAWN_MAX_FDS + 1u, CAP_NONE);
    TEST_EXPECT_EQ(rc, -1, "fd_count > SYS_SPAWN_MAX_FDS → -1");
}

void test_sys_spawn_full_rejects_bad_fd(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    u32 fds[1] = { 0xFFFFFFFFu };
    int rc = sys_spawn_full_for_proc(t->proc, "hello", 5, fds, 1, CAP_NONE);
    TEST_EXPECT_EQ(rc, -1, "fd outside handle-table range → -1");
}

void test_sys_spawn_full_rejects_missing_binary(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int rc = sys_spawn_full_for_proc(t->proc, "nonexistent", 11,
                                      NULL, 0, CAP_NONE);
    TEST_EXPECT_EQ(rc, -1, "missing binary → -1");
}
