// P5-spawn-caps — kernel-internal tests for SYS_SPAWN_WITH_CAPS.
//
// The cap arithmetic itself (parent_caps & cap_mask) is already covered
// by the existing rfork_with_caps tests (caps.rfork_with_caps_grants_subset,
// caps.rfork_with_caps_clamps_to_parent). These tests verify the syscall
// wrapper: handler validation + child Proc receives the expected caps
// via the underlying primitive.
//
// Tests inspect the spawned child's caps via proc_find_by_pid BEFORE
// reaping it via wait_pid. caps are monotonic-on-rfork and stable for
// the Proc's lifetime, so this read-then-reap pattern is race-free.
//
// Coverage:
//
//   sys_spawn_with_caps.happy_path_zero_mask
//     cap_mask=0 → child gets no caps + exits cleanly.
//
//   sys_spawn_with_caps.happy_path_subset_of_parent
//     cap_mask=CAP_LOCK_PAGES → child gets exactly CAP_LOCK_PAGES
//     (since kproc has CAP_ALL ⊇ CAP_LOCK_PAGES).
//
//   sys_spawn_with_caps.clamps_to_parent
//     cap_mask=CAP_ALL | unused-bit → child gets parent's actual caps,
//     not anything beyond.
//
//   sys_spawn_with_caps.rejects_missing_binary
//
//   sys_spawn_with_caps.rejects_oversize_name

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern int sys_spawn_with_caps_for_proc(struct Proc *p, const char *name,
                                         size_t name_len, caps_t cap_mask);

void test_sys_spawn_with_caps_happy_path_zero_mask(void);
void test_sys_spawn_with_caps_happy_path_subset_of_parent(void);
void test_sys_spawn_with_caps_clamps_to_parent(void);
void test_sys_spawn_with_caps_rejects_missing_binary(void);
void test_sys_spawn_with_caps_rejects_oversize_name(void);

static void drain_zombies(void) {
    int status = 0;
    while (wait_pid(&status) > 0) { /* drain */ }
}

void test_sys_spawn_with_caps_happy_path_zero_mask(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int pid = sys_spawn_with_caps_for_proc(t->proc, "hello", 5, CAP_NONE);
    TEST_ASSERT(pid > 0, "spawn with cap_mask=CAP_NONE returns pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for spawned child");
    TEST_EXPECT_EQ((u64)child->caps, (u64)CAP_NONE,
        "cap_mask=CAP_NONE → child has zero caps");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "/hello exits status=0 even with zero caps");
}

void test_sys_spawn_with_caps_happy_path_subset_of_parent(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // kproc carries CAP_ALL = CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ.
    // Granting CAP_LOCK_PAGES alone should produce a child with exactly
    // CAP_LOCK_PAGES set.
    int pid = sys_spawn_with_caps_for_proc(t->proc, "hello", 5, CAP_LOCK_PAGES);
    TEST_ASSERT(pid > 0, "spawn with cap_mask=CAP_LOCK_PAGES returns pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for spawned child");
    TEST_EXPECT_EQ((u64)child->caps, (u64)CAP_LOCK_PAGES,
        "child has exactly CAP_LOCK_PAGES");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "/hello exits status=0");
}

void test_sys_spawn_with_caps_clamps_to_parent(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Pass a mask with bits beyond CAP_ALL. The AND with parent->caps
    // (= CAP_ALL for kproc) clamps to exactly CAP_ALL. The extra bits
    // are silently dropped — this is the I-2 / I-6 monotonic-reduction
    // invariant enforced structurally.
    caps_t mask_with_garbage = (caps_t)0xFFFFFFFFFFFFFFFFull;
    int pid = sys_spawn_with_caps_for_proc(t->proc, "hello", 5, mask_with_garbage);
    TEST_ASSERT(pid > 0, "spawn with cap_mask=0xFFF...FF returns pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for spawned child");
    TEST_EXPECT_EQ((u64)child->caps, (u64)CAP_ALL,
        "child clamped to parent's CAP_ALL; no spurious bits");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "/hello exits status=0");
}

void test_sys_spawn_with_caps_rejects_missing_binary(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int rc = sys_spawn_with_caps_for_proc(t->proc, "nonexistent", 11, CAP_NONE);
    TEST_EXPECT_EQ(rc, -1, "missing binary → -1");
}

void test_sys_spawn_with_caps_rejects_oversize_name(void) {
    drain_zombies();
    char name[SYS_SPAWN_NAME_MAX + 2];
    for (size_t i = 0; i < sizeof(name); i++) name[i] = 'a';
    name[sizeof(name) - 1] = '\0';

    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    int rc = sys_spawn_with_caps_for_proc(t->proc, name,
                                          SYS_SPAWN_NAME_MAX + 1u, CAP_NONE);
    TEST_EXPECT_EQ(rc, -1, "name_len > SYS_SPAWN_NAME_MAX → -1");
}
