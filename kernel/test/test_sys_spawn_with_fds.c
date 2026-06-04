// P5-stratumd-stub-bringup-b — kernel-internal rejection-path tests
// for SYS_SPAWN_WITH_FDS.
//
// The happy path is exercised end-to-end at EL0 by
// `userspace.stub_driver_round_trip` (kernel/test/test_stub_driver.c)
// which runs the full production-shape orchestration. These tests
// cover the rejection branches that the happy path doesn't surface.
//
// Coverage:
//
//   sys_spawn_with_fds.rejects_oversize_fd_count
//     fd_count > SYS_SPAWN_MAX_FDS → -1.
//
//   sys_spawn_with_fds.rejects_bad_fd
//     fd_list contains a non-existent fd → -1.
//
//   sys_spawn_with_fds.rejects_non_spoor_fd
//     fd_list contains a non-KOBJ_SPOOR handle → -1.
//     (Burrow handle in this test; ARCH I-5 already prohibits MMIO /
//     IRQ / DMA from cross-Proc transfer at the syscall layer.)
//
//   sys_spawn_with_fds.rejects_missing_binary
//     name = "nonexistent" → -1.
//
//   sys_spawn_with_fds.zero_count_succeeds
//     fd_count = 0 is the same as SYS_SPAWN (no inheritance).
//     Happy path with "hello".

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern int sys_spawn_with_fds_for_proc(struct Proc *p, const char *name,
                                        size_t name_len,
                                        const u32 *fds, u32 fd_count);

void test_sys_spawn_with_fds_rejects_oversize_fd_count(void);
void test_sys_spawn_with_fds_rejects_bad_fd(void);
void test_sys_spawn_with_fds_rejects_non_spoor_fd(void);
void test_sys_spawn_with_fds_rejects_missing_binary(void);
void test_sys_spawn_with_fds_zero_count_succeeds(void);
void test_sys_spawn_with_fds_child_rights_subset_of_parent(void);

static void drain_zombies(void) {
    int status = 0;
    while (wait_pid(&status) > 0) { /* drain */ }
}

void test_sys_spawn_with_fds_rejects_oversize_fd_count(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    u32 fds[SYS_SPAWN_MAX_FDS + 1] = { 0 };
    int rc = sys_spawn_with_fds_for_proc(t->proc, "hello", 5,
                                         fds, SYS_SPAWN_MAX_FDS + 1u);
    TEST_EXPECT_EQ(rc, -1, "fd_count > SYS_SPAWN_MAX_FDS → -1");
}

void test_sys_spawn_with_fds_rejects_bad_fd(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // PROC_HANDLE_MAX is the upper bound; an fd of UINT32_MAX is
    // clearly out of range. handle_get returns NULL → handler returns -1.
    u32 fds[1] = { 0xFFFFFFFFu };
    int rc = sys_spawn_with_fds_for_proc(t->proc, "hello", 5, fds, 1);
    TEST_EXPECT_EQ(rc, -1, "fd outside handle-table range → -1");
}

void test_sys_spawn_with_fds_rejects_non_spoor_fd(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Allocate an anonymous Burrow handle in kproc; verify spawn
    // rejects it because kind != KOBJ_SPOOR. Use a small page-aligned
    // size so the Burrow allocation is cheap.
    struct Burrow *b = burrow_create_anon(4096);
    TEST_ASSERT(b != NULL, "burrow_create_anon");

    hidx_t bh = handle_alloc(t->proc, KOBJ_BURROW,
                             RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, b);
    TEST_ASSERT(bh >= 0, "handle_alloc for burrow");

    u32 fds[1] = { (u32)bh };
    int rc = sys_spawn_with_fds_for_proc(t->proc, "hello", 5, fds, 1);
    TEST_EXPECT_EQ(rc, -1, "non-KOBJ_SPOOR fd → -1");

    // Clean up the burrow handle; the test Proc is kproc and will
    // accumulate state otherwise.
    handle_close(t->proc, bh);
}

void test_sys_spawn_with_fds_rejects_missing_binary(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int rc = sys_spawn_with_fds_for_proc(t->proc, "nonexistent", 11,
                                         NULL, 0);
    TEST_EXPECT_EQ(rc, -1, "missing binary → -1");
}

void test_sys_spawn_with_fds_zero_count_succeeds(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // fd_count=0 should behave like SYS_SPAWN (no inheritance).
    int pid = sys_spawn_with_fds_for_proc(t->proc, "hello", 5, NULL, 0);
    TEST_ASSERT(pid > 0, "fd_count=0 + valid binary → positive pid");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "/hello exits status=0 even without inherited fds");
}

// R15 F231 close: regression for the rights-elevation bug.
// Pre-fix, the child's handle_alloc hardcoded RIGHT_READ|WRITE|TRANSFER
// regardless of the parent's slot rights. This test creates a Spoor
// with RIGHT_READ only in the parent, spawns /hello with that fd in
// the inheritance list, and inspects the child's slot rights via
// proc_find_by_pid + polling for the thunk's handle_alloc to complete.
//
// The poll-loop is bounded; if the child finishes its handle_alloc on
// another CPU before we observe it, we still see the installed slot
// (handle table isn't freed until wait_pid). The test inspects the
// child BEFORE wait_pid so the handle table is live.
void test_sys_spawn_with_fds_child_rights_subset_of_parent(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Create a pipe pair; install the read end in kproc's handle
    // table with RIGHT_READ only (the explicit narrowing).
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    const rights_t parent_rights = RIGHT_READ;
    hidx_t parent_fd = handle_alloc(t->proc, KOBJ_SPOOR, parent_rights, rd);
    TEST_ASSERT(parent_fd >= 0, "handle_alloc with RIGHT_READ only");

    u32 fds[1] = { (u32)parent_fd };
    int pid = sys_spawn_with_fds_for_proc(t->proc, "hello", 5, fds, 1);
    TEST_ASSERT(pid > 0, "spawn with 1 inherited fd returns pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for spawned child");

    // Poll for the child's slot to appear (thunk's handle_alloc runs
    // async on child's CPU). Bounded to 1024 sched yields; in practice
    // the child reaches its handle_alloc within a handful of yields.
    struct Handle child_slot;
    bool got_child = false;
    for (int i = 0; i < 1024; i++) {
        if (handle_get(child, 0, &child_slot) == 0) { got_child = true; break; }
        sched();
    }
    TEST_ASSERT(got_child,
        "child has handle at fd 0 after bounded poll");
    TEST_EXPECT_EQ((u64)child_slot.kind, (u64)KOBJ_SPOOR,
        "child fd 0 is KOBJ_SPOOR");

    // The critical invariant: child's rights must equal parent's rights
    // (or be a strict subset). Pre-fix, this would be
    // RIGHT_READ|WRITE|TRANSFER (hardcoded), which is a SUPERSET of the
    // parent's RIGHT_READ — an I-6 violation.
    TEST_EXPECT_EQ((u64)child_slot.rights, (u64)parent_rights,
        "child slot rights == parent slot rights (R15 F231)");
    handle_put(&child_slot);

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "/hello exits status=0");

    // Clean up parent-side handle + boot's pointer on wr.
    handle_close(t->proc, parent_fd);
    spoor_clunk(wr);
}
