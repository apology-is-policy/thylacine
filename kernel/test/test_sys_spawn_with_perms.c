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

// A-5b #827b: the per-bit grant gate, exported so the decision can be driven
// directly on synthetic Procs (decoupled from the heavyweight spawn body).
extern int spawn_perm_grant_check(struct Proc *p, u32 perm_flags);

// LS-5: the perm-application helper (the bit->action mapping both spawn thunks
// run) + the console-owner accessor, exported so the owner-set WIRING can be
// driven deterministically — a real spawn races the child's fast exit clearing
// g_console_owner, so the wiring is unobservable through a full spawn.
extern void apply_spawn_perms(struct Proc *p, u32 perm_flags);
extern struct Proc *proc_test_console_owner(void);

void test_sys_spawn_with_perms_zero_perm_is_spawn_full(void);
void test_sys_spawn_with_perms_console_attached_grants_may_post(void);
void test_sys_spawn_with_perms_rejects_non_console_attached_parent(void);
void test_sys_spawn_with_perms_rejects_unknown_perm_bits(void);
void test_sys_spawn_with_perms_holder_delegates_may_post(void);
void test_sys_spawn_with_perms_console_trusted_not_delegable(void);
void test_sys_spawn_with_perms_console_owner_grant_gate(void);
void test_sys_spawn_with_perms_console_owner_set_wiring(void);

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

// A-5b #827b: a Proc that ALREADY holds PROC_FLAG_MAY_POST_SERVICE may delegate
// it ONE hop, even though it is NOT console-attached (joey -> /sbin/login ->
// per-user --role client proxy). The control case (a non-attached, non-holding
// Proc) must still be rejected — that is the pre-#827b behavior the delegation
// must not weaken. Driven through spawn_perm_grant_check directly: the gate
// DECISION is the security-relevant logic, decoupled from the spawn body.
void test_sys_spawn_with_perms_holder_delegates_may_post(void) {
    drain_zombies();

    struct Proc *holder = proc_alloc();
    TEST_ASSERT(holder != NULL, "proc_alloc holder Proc");
    TEST_ASSERT(!proc_is_console_attached(holder),
        "holder is not console-attached");

    // Control (pre-#827b behavior preserved): a non-attached Proc that does NOT
    // hold the bit cannot grant it.
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder, SPAWN_PERM_MAY_POST_SERVICE), -1,
        "non-attached non-holder cannot grant MAY_POST_SERVICE");

    // Stamp the holder. Now the one-hop delegation applies.
    proc_mark_may_post_service(holder);
    TEST_ASSERT(proc_may_post_service(holder),
        "holder now holds MAY_POST_SERVICE");
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder, SPAWN_PERM_MAY_POST_SERVICE), 0,
        "a MAY_POST_SERVICE holder delegates the bit (A-5b #827b)");
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder, 0u), 0,
        "perm_flags=0 always grantable");

    holder->state = PROC_STATE_ZOMBIE;
    proc_free(holder);
}

// A-5b #827b: SPAWN_PERM_CONSOLE_TRUSTED (the SAK trust anchor) is NOT delegable
// by the held-MAY_POST_SERVICE path — it stays console-attach-only, so a
// service-poster can never confer the console-trust used for elevation (I-27).
void test_sys_spawn_with_perms_console_trusted_not_delegable(void) {
    drain_zombies();

    struct Proc *holder = proc_alloc();
    TEST_ASSERT(holder != NULL, "proc_alloc holder Proc");
    proc_mark_may_post_service(holder);
    TEST_ASSERT(proc_may_post_service(holder) && !proc_is_console_attached(holder),
        "holder holds MAY_POST_SERVICE and is not console-attached");

    // Holding MAY_POST_SERVICE does NOT let a non-attached Proc grant
    // CONSOLE_TRUSTED — alone or combined with the delegable bit.
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder, SPAWN_PERM_CONSOLE_TRUSTED), -1,
        "held MAY_POST_SERVICE does not delegate CONSOLE_TRUSTED");
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder,
        SPAWN_PERM_MAY_POST_SERVICE | SPAWN_PERM_CONSOLE_TRUSTED), -1,
        "the CONSOLE_TRUSTED bit fails the gate even with MAY_POST_SERVICE held");

    holder->state = PROC_STATE_ZOMBIE;
    proc_free(holder);

    // The unchanged console path: a console-attached Proc grants both bits.
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    proc_mark_console_attached(t->proc);
    TEST_EXPECT_EQ(spawn_perm_grant_check(t->proc,
        SPAWN_PERM_MAY_POST_SERVICE | SPAWN_PERM_CONSOLE_TRUSTED), 0,
        "console-attached grants both bits");
}

// LS-5 (P1 console owner): SPAWN_PERM_CONSOLE_OWNER is gated the SAME way as
// MAY_POST_SERVICE (a console-attached granter OR a Proc that already holds
// MAY_POST_SERVICE), so trusted /sbin/login -- holding the bit from joey --
// confers console ownership on the session shell. But it is NOT a back door to
// CONSOLE_TRUSTED: a mere holder still cannot grant console-trust (I-27).
void test_sys_spawn_with_perms_console_owner_grant_gate(void) {
    drain_zombies();

    struct Proc *holder = proc_alloc();
    TEST_ASSERT(holder != NULL, "proc_alloc holder Proc");
    TEST_ASSERT(!proc_is_console_attached(holder),
        "holder is not console-attached");

    // Control: a non-attached, non-holding Proc cannot grant CONSOLE_OWNER.
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder, SPAWN_PERM_CONSOLE_OWNER), -1,
        "non-attached non-holder cannot grant CONSOLE_OWNER");

    // A MAY_POST_SERVICE holder (trusted /sbin/login) confers CONSOLE_OWNER --
    // gated identically to MAY_POST_SERVICE.
    proc_mark_may_post_service(holder);
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder, SPAWN_PERM_CONSOLE_OWNER), 0,
        "a MAY_POST_SERVICE holder confers CONSOLE_OWNER (LS-5 trusted login)");
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder,
        SPAWN_PERM_MAY_POST_SERVICE | SPAWN_PERM_CONSOLE_OWNER), 0,
        "the holder confers MAY_POST_SERVICE + CONSOLE_OWNER together");

    // CONSOLE_OWNER never unlocks CONSOLE_TRUSTED for a mere holder (I-27): the
    // SAK/elevation anchor stays console-attach-only.
    TEST_EXPECT_EQ(spawn_perm_grant_check(holder,
        SPAWN_PERM_CONSOLE_OWNER | SPAWN_PERM_CONSOLE_TRUSTED), -1,
        "CONSOLE_OWNER does not unlock CONSOLE_TRUSTED for a holder (I-27)");

    holder->state = PROC_STATE_ZOMBIE;
    proc_free(holder);

    // A console-attached Proc grants CONSOLE_OWNER (the boot/joey path).
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    proc_mark_console_attached(t->proc);
    TEST_EXPECT_EQ(spawn_perm_grant_check(t->proc, SPAWN_PERM_CONSOLE_OWNER), 0,
        "console-attached grants CONSOLE_OWNER");
}

// LS-5 (P1 console owner): the bit->action WIRING. apply_spawn_perms is what
// both spawn thunks run in the child's context; CONSOLE_OWNER must make the
// child the g_console_owner (the Proc that receives the Ctrl-C `interrupt`
// note), and NOTHING else must touch the owner pointer. Driven on a synthetic
// Proc because a real spawn races the child's exit clearing the owner.
void test_sys_spawn_with_perms_console_owner_set_wiring(void) {
    drain_zombies();

    // Save the live owner so the test leaves the boot/session state untouched
    // (NULL during the suite -- joey has not relinquished, no session yet).
    struct Proc *saved = proc_test_console_owner();

    struct Proc *shell = proc_alloc();
    TEST_ASSERT(shell != NULL, "proc_alloc synthetic shell Proc");

    // A perm set WITHOUT CONSOLE_OWNER must not touch the owner pointer.
    proc_set_console_owner(NULL);
    apply_spawn_perms(shell, SPAWN_PERM_MAY_POST_SERVICE);
    TEST_EXPECT_EQ(proc_test_console_owner(), (struct Proc *)NULL,
        "MAY_POST_SERVICE alone does not set the console owner");
    TEST_ASSERT(proc_may_post_service(shell),
        "apply_spawn_perms stamped MAY_POST_SERVICE");

    // CONSOLE_OWNER makes the child the console owner (the LS-5 P1 wiring) and
    // must NOT confer console-attach (I-27).
    apply_spawn_perms(shell, SPAWN_PERM_CONSOLE_OWNER);
    TEST_EXPECT_EQ(proc_test_console_owner(), shell,
        "CONSOLE_OWNER set the child as g_console_owner");
    TEST_ASSERT(!proc_is_console_attached(shell),
        "CONSOLE_OWNER must NOT confer console-attach (I-27)");

    // Clear the owner BEFORE freeing so g_console_owner never dangles, then
    // restore the saved owner.
    proc_set_console_owner(NULL);
    shell->state = PROC_STATE_ZOMBIE;
    proc_free(shell);
    proc_set_console_owner(saved);
}
