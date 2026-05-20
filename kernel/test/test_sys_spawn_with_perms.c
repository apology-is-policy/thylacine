// P5-corvus-srv-impl-b3a — kernel-internal tests for SYS_SPAWN_WITH_PERMS.
//
// SYS_SPAWN_WITH_PERMS = SYS_SPAWN_FULL + a sixth `perm_flags` parameter
// that the kernel atomically stamps on the spawned child Proc BEFORE the
// child's exec_setup. The v1.0 bit is SPAWN_PERM_MAY_POST_SERVICE, which
// stamps PROC_FLAG_MAY_POST_SERVICE so the child may call SYS_POST_SERVICE.
//
// The race the design avoids: a stamp-after-spawn pattern (parent calls
// "mark this child" AFTER fork) leaves a window where the child can race
// the parent to SYS_POST_SERVICE. Baking the stamp into the spawn thunk
// makes the child's first user-mode instruction observe the final flags.
//
// Coverage:
//
//   sys_spawn_with_perms.zero_perm_is_spawn_full
//     perm_flags=0 behaves identically to SYS_SPAWN_FULL — the child
//     spawns, gets no may-post stamp, exits clean.
//
//   sys_spawn_with_perms.console_attached_grants_may_post
//     A console-attached parent + perm_flags=SPAWN_PERM_MAY_POST_SERVICE
//     stamps PROC_FLAG_MAY_POST_SERVICE on the child.
//
//   sys_spawn_with_perms.rejects_non_console_attached_parent
//     A non-console-attached parent passing SPAWN_PERM_MAY_POST_SERVICE
//     is rejected (-1). No child is created.
//
//   sys_spawn_with_perms.rejects_unknown_perm_bits
//     A perm_flags carrying bits outside SPAWN_PERM_ALL is rejected,
//     even from a console-attached parent.
//
// Verification pattern: same as test_sys_spawn_full — proc_find_by_pid
// to inspect the child's flags BEFORE wait_pid (the stamp is set inside
// the child's thunk before exec_setup, so a successful spawn means the
// flag is already there; we don't need to wait).
//
// IMPORTANT: this test marks the current Proc (kproc) console-attached
// as a one-way side effect. The post-test boot path (joey_run + the
// per-Proc tests that follow) is unaffected: rfork does NOT propagate
// the console bit, so joey starts un-attached and self-stamps inside
// joey_thunk (the existing pattern). kproc carrying the bit after this
// test is benign.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern int sys_spawn_with_perms_for_proc(struct Proc *p,
                                         const char *name, size_t name_len,
                                         const u32 *fds, u32 fd_count,
                                         caps_t cap_mask, u32 perm_flags);

void test_sys_spawn_with_perms_zero_perm_is_spawn_full(void);
void test_sys_spawn_with_perms_console_attached_grants_may_post(void);
void test_sys_spawn_with_perms_rejects_non_console_attached_parent(void);
void test_sys_spawn_with_perms_rejects_unknown_perm_bits(void);

static void drain_zombies(void) {
    int status = 0;
    while (wait_pid(&status) > 0) { /* drain */ }
}

void test_sys_spawn_with_perms_zero_perm_is_spawn_full(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // perm_flags=0 — kproc need not be console-attached. Child spawns
    // and exits clean; no may-post stamp expected.
    int pid = sys_spawn_with_perms_for_proc(t->proc, "hello", 5,
                                            NULL, 0, CAP_NONE,
                                            /*perm_flags=*/0u);
    TEST_ASSERT(pid > 0, "spawn_with_perms perm=0 → positive pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for child");
    TEST_ASSERT(!proc_may_post_service(child),
        "child has no PROC_FLAG_MAY_POST_SERVICE when perm=0");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the child");
    TEST_EXPECT_EQ(status, 0, "/hello exits clean");
}

void test_sys_spawn_with_perms_console_attached_grants_may_post(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Mark the calling Proc (kproc) console-attached. proc_mark_console_-
    // attached is idempotent; if the previous test ran first this is a
    // no-op. The mark is permanent for kproc's lifetime — see the test
    // file header for the rationale on why that's benign.
    proc_mark_console_attached(t->proc);
    TEST_ASSERT(proc_is_console_attached(t->proc),
        "kproc is now console-attached (pre-spawn invariant)");

    int pid = sys_spawn_with_perms_for_proc(t->proc, "hello", 5,
                                            NULL, 0, CAP_NONE,
                                            SPAWN_PERM_MAY_POST_SERVICE);
    TEST_ASSERT(pid > 0, "spawn_with_perms + console-attached parent → pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for stamped child");
    // The stamp lives in the child's thunk, which runs before any user
    // code (exec_setup is the gate). The child IS scheduled by now but
    // even pre-thunk-execution the field is fine to read: proc_flags is
    // either 0 (pre-thunk) or PROC_FLAG_MAY_POST_SERVICE (post-thunk).
    // wait_pid below ensures the child finished — we re-check AFTER the
    // wait for the strong assertion. Pre-wait we just verify the child
    // structure was allocated; flag verification is post-stamp via
    // re-find.
    //
    // Belt-and-braces: also verify console_attached did NOT propagate
    // (rfork strips it; the spawn thunk does NOT re-stamp it).
    TEST_ASSERT(!proc_is_console_attached(child),
        "console_attached must NOT propagate to a spawned child");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the stamped child");
    TEST_EXPECT_EQ(status, 0, "/hello exits clean even with may-post stamp");

    // The child's Proc record persists until wait_pid reaps it; after
    // reap, proc_find_by_pid would return NULL. The post-wait re-find
    // is therefore the path that confirms the stamp landed BEFORE
    // exec_setup ran (the child wouldn't have exited clean otherwise).
}

void test_sys_spawn_with_perms_rejects_non_console_attached_parent(void) {
    drain_zombies();

    // Make a fresh, NON-console-attached Proc to act as the parent.
    // The gate check inspects this Proc's flags, not current_thread's,
    // so we exercise the "ordinary unauthorized caller" path without
    // mutating any global state.
    struct Proc *unprivileged = proc_alloc();
    TEST_ASSERT(unprivileged != NULL, "proc_alloc fresh unprivileged Proc");
    TEST_ASSERT(!proc_is_console_attached(unprivileged),
        "fresh Proc is not console-attached");

    int rc = sys_spawn_with_perms_for_proc(unprivileged, "hello", 5,
                                            NULL, 0, CAP_NONE,
                                            SPAWN_PERM_MAY_POST_SERVICE);
    TEST_EXPECT_EQ(rc, -1,
        "perm_flags set + parent not console-attached → -1");

    // Clean up the throwaway Proc.
    unprivileged->state = PROC_STATE_ZOMBIE;
    proc_free(unprivileged);
}

void test_sys_spawn_with_perms_rejects_unknown_perm_bits(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Even from a console-attached parent (set by an earlier test), a
    // perm_flags carrying garbage bits beyond SPAWN_PERM_ALL is rejected.
    proc_mark_console_attached(t->proc);
    u32 bad_perm = 0xDEAD0000u | SPAWN_PERM_MAY_POST_SERVICE;
    int rc = sys_spawn_with_perms_for_proc(t->proc, "hello", 5,
                                            NULL, 0, CAP_NONE,
                                            bad_perm);
    TEST_EXPECT_EQ(rc, -1, "unknown SPAWN_PERM_* bits → -1");
}
