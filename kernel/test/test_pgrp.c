// PTY-1a: POSIX sessions + process groups (PTY-DESIGN.md section 4).
//
// Four tests:
//
//   pgrp.defaults_and_inherit
//     kproc is its own session/group leader (sid = pgid = 0 = pid); a REAL
//     rfork child JOINS the parent's session + group (the rfork_internal
//     inherit overrides proc_alloc's own-session default -- the child's
//     sid/pgid read 0, not its own pid). Exercises the actual inherit path
//     synthetic fabrication cannot.
//
//   pgrp.setsid_semantics
//     A real child setsids: new sid = pgid = its pid; a second setsid from
//     the now-leader rejects -T_E_ACCES. kproc (a leader by construction)
//     also rejects. getsid/getpgid read the moved values back.
//
//   pgrp.setpgid_rule_matrix
//     The POSIX rule matrix over SYNTHETIC Procs (proc_test_link splices
//     them in as kproc's children; deterministic, no scheduling): mint a
//     new group; join an existing group in-session; reject a nonexistent
//     group, a cross-session target, a session-leader target, a stranger
//     pid, a negative pgid.
//
//   pgrp.zombie_reads_alive_mutations
//     A ZOMBIE target answers getpgid/getsid (waitpid(-pgid) matches
//     zombies by pgid -- PTY-1e depends on this) but rejects the setpgid
//     mutation with -T_E_SRCH.
//
// The setpgid caller in every case is the boot kthread's Proc (kproc):
// synthetic Procs linked via proc_test_link are kproc's direct children, and
// same-session cases fabricate the inherited state (sid = pgid = 0) that
// rfork would have copied -- proc_alloc alone defaults a Proc to its OWN
// session, which is exactly the cross-session reject case.

#include "test.h"

#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_pgrp_defaults_and_inherit(void);
void test_pgrp_setsid_semantics(void);
void test_pgrp_setpgid_rule_matrix(void);
void test_pgrp_zombie_reads_alive_mutations(void);

// Test-harness hooks (the test_proc.c pattern).
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);

static volatile u32 g_child_sid;
static volatile u32 g_child_pgid;
static volatile int g_child_setsid_rv;
static volatile int g_child_setsid_again_rv;
static volatile u32 g_child_sid_after;
static volatile u32 g_child_pgid_after;
static volatile int g_child_getsid_rv;
static volatile int g_child_getpgid_rv;

static void pgrp_child_record(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_child_sid  = p->sid;
    g_child_pgid = p->pgid;
    exits("ok");
}

static void pgrp_child_setsid(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_child_setsid_rv       = proc_setsid(p);
    g_child_sid_after       = p->sid;
    g_child_pgid_after      = p->pgid;
    g_child_setsid_again_rv = proc_setsid(p);   // now a leader -> reject
    g_child_getsid_rv       = proc_getsid(p, 0);
    g_child_getpgid_rv      = proc_getpgid(p, 0);
    exits("ok");
}

void test_pgrp_defaults_and_inherit(void) {
    struct Proc *self = current_thread()->proc;

    // kproc: pid 0, its own session/group leader by the parentless default.
    TEST_ASSERT(self->pid == 0, "test thread not in kproc");
    TEST_ASSERT(self->sid == 0 && self->pgid == 0,
                "kproc sid/pgid not the own-session default");

    g_child_sid = g_child_pgid = 0xffffffffu;
    int pid = rfork(RFPROC, pgrp_child_record, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -1;
    int reaped = wait_pid_for(pid, 0, &status);
    TEST_ASSERT(reaped == pid && status == 0, "child reap failed");

    // The child JOINED kproc's session + group (inherit overrode the
    // proc_alloc own-session default, which would have read pid/pid).
    TEST_ASSERT(g_child_sid == 0 && g_child_pgid == 0,
                "rfork child did not inherit the parent's sid/pgid");
}

void test_pgrp_setsid_semantics(void) {
    struct Proc *self = current_thread()->proc;

    // kproc is a group leader by construction (pgid == pid == 0): reject.
    TEST_ASSERT(proc_setsid(self) == -T_E_ACCES,
                "setsid by a group leader did not reject");

    g_child_setsid_rv = g_child_setsid_again_rv = -999;
    int pid = rfork(RFPROC, pgrp_child_setsid, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -1;
    int reaped = wait_pid_for(pid, 0, &status);
    TEST_ASSERT(reaped == pid && status == 0, "child reap failed");

    TEST_ASSERT(g_child_setsid_rv == pid,
                "setsid did not return the new sid (= the caller's pid)");
    TEST_ASSERT(g_child_sid_after == (u32)pid && g_child_pgid_after == (u32)pid,
                "setsid did not set sid = pgid = pid");
    TEST_ASSERT(g_child_setsid_again_rv == -T_E_ACCES,
                "second setsid from the new leader did not reject");
    TEST_ASSERT(g_child_getsid_rv == pid && g_child_getpgid_rv == pid,
                "getsid/getpgid(0) did not read the moved values back");
}

// Fabricate a linked ALIVE synthetic Proc "inherited into" kproc's session
// (sid = pgid = 0, what rfork_internal would have copied).
static struct Proc *pgrp_make_linked_in_session(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->sid  = 0;
    p->pgid = 0;
    proc_test_link(p);
    return p;
}

static void pgrp_drop_linked(struct Proc *p) {
    if (!p) return;
    proc_test_unlink(p);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_pgrp_setpgid_rule_matrix(void) {
    struct Proc *self = current_thread()->proc;

    struct Proc *a = pgrp_make_linked_in_session();   // in-session children
    struct Proc *b = pgrp_make_linked_in_session();
    struct Proc *alien  = pgrp_make_linked_in_session();
    struct Proc *leader = proc_alloc();               // own-session default:
    TEST_ASSERT(a && b && alien && leader, "proc_alloc failed");
    proc_test_link(leader);                           //  sid = pgid = pid =
                                                      //  a session leader
    // alien: cross-session but NOT a session leader (a fake foreign session
    // id that is not its pid) -- isolates the sid arm of the compound
    // reject. A target that is BOTH (the proc_alloc default) cannot tell
    // the arms apart.
    alien->sid  = 5;
    alien->pgid = 5;

    // Negative pgid: EINVAL before any lookup.
    TEST_ASSERT(proc_setpgid(self, a->pid, -2) == -T_E_INVAL,
                "negative pgid did not EINVAL");

    // Stranger pid (not self, not a direct child): ESRCH. pid -1 never
    // matches a child.
    TEST_ASSERT(proc_setpgid(self, -1, 0) == -T_E_SRCH,
                "stranger pid did not ESRCH");

    // Mint a new group of the target's own pid (pgid 0 shorthand).
    TEST_ASSERT(proc_setpgid(self, a->pid, 0) == 0, "mint-new-group failed");
    TEST_ASSERT(a->pgid == (u32)a->pid, "minted pgid != target pid");
    TEST_ASSERT(proc_getpgid(self, a->pid) == a->pid, "getpgid(a) mismatch");

    // Join an existing group in the caller's session.
    TEST_ASSERT(proc_setpgid(self, b->pid, a->pid) == 0,
                "join-existing-group failed");
    TEST_ASSERT(b->pgid == (u32)a->pid, "joined pgid != a's group");

    // Nonexistent group in-session: EPERM contour (-T_E_ACCES). Group id
    // 0x7ffffff0 names nothing.
    TEST_ASSERT(proc_setpgid(self, b->pid, 0x7ffffff0) == -T_E_ACCES,
                "nonexistent group did not EACCES");

    // Cross-session target (not a leader -- the isolated sid arm).
    TEST_ASSERT(proc_setpgid(self, alien->pid, 0) == -T_E_ACCES,
                "cross-session target did not EACCES");

    // Session-leader rule, isolated via SELF-target: a leader inside the
    // caller's session can only be the caller itself (leader sid == leader
    // pid), so the clean exercise is the leader moving ITSELF -- the sid
    // arm is trivially equal on self, leaving only the leader rule.
    TEST_ASSERT(proc_setpgid(leader, 0, 0) == -T_E_ACCES,
                "session-leader self-move did not EACCES");

    // kproc (pid 0) is likewise its session's leader by construction.
    TEST_ASSERT(proc_setpgid(self, 0, 0) == -T_E_ACCES,
                "kproc session-leader self-move did not EACCES");

    pgrp_drop_linked(a);
    pgrp_drop_linked(b);
    pgrp_drop_linked(alien);
    pgrp_drop_linked(leader);
}

void test_pgrp_zombie_reads_alive_mutations(void) {
    struct Proc *self = current_thread()->proc;

    struct Proc *z = pgrp_make_linked_in_session();
    TEST_ASSERT(z, "proc_alloc failed");
    TEST_ASSERT(proc_setpgid(self, z->pid, 0) == 0, "pre-zombie mint failed");
    int zpid  = z->pid;
    z->state = PROC_STATE_ZOMBIE;

    // Reads answer on a zombie (PTY-1e waitpid(-pgid) matches by these).
    TEST_ASSERT(proc_getpgid(self, zpid) == zpid, "zombie getpgid failed");
    TEST_ASSERT(proc_getsid(self, zpid) == 0,     "zombie getsid failed");

    // Mutation rejects on a corpse.
    TEST_ASSERT(proc_setpgid(self, zpid, 0) == -T_E_SRCH,
                "zombie setpgid did not ESRCH");

    proc_test_unlink(z);
    proc_free(z);
}
