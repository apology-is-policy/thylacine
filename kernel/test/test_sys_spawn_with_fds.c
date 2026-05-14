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
