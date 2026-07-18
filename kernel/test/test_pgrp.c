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
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_pgrp_defaults_and_inherit(void);
void test_pgrp_setsid_semantics(void);
void test_pgrp_setpgid_rule_matrix(void);
void test_pgrp_zombie_reads_alive_mutations(void);
void test_tty_note_post_gate(void);
void test_tty_pgrp_fanout_exactly_once(void);
void test_tty_terminate_class_gate(void);

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

// -- PTY-1b: the tty:* note class + notes_post_pgrp -------------------------

static int tty_streq(const char *a, const char *b) {
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 0;
}

// Count queued notes named `name` in p's queue (under q->lock).
static u32 tty_count_named(struct Proc *p, const char *name) {
    struct NoteQueue *q = p->notes;
    u32 found = 0;
    spin_lock(&q->lock);
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        if (tty_streq(q->ring[idx].name, name)) found++;
        idx = (idx + 1) % NOTE_QUEUE_DEPTH;
    }
    spin_unlock(&q->lock);
    return found;
}

void test_tty_note_post_gate(void) {
    struct Proc *p = pgrp_make_linked_in_session();
    TEST_ASSERT(p, "proc_alloc failed");

    // Userspace (synthetic=false) post of ANY tty:* name: rejected -- the
    // load-bearing F4 gate (the names ARE in the supported set, so this
    // prefix gate is the only barrier; without it a parent could post
    // tty:cont to a debug-stopped child, the I-39 leak).
    TEST_ASSERT(notes_post(p, NOTE_NAME_TTY_CONT, 0u, NULL, false) == -1,
                "userspace tty:cont post not rejected");
    TEST_ASSERT(notes_post(p, NOTE_NAME_TTY_HUP, 0u, NULL, false) == -1,
                "userspace tty:hup post not rejected");
    TEST_ASSERT(tty_count_named(p, NOTE_NAME_TTY_CONT) == 0,
                "rejected post left a queue entry");

    // Kernel-synthetic post: accepted + queued (catchable-informational).
    TEST_ASSERT(notes_post(p, NOTE_NAME_TTY_WINCH, 7u, NULL, true) == 0,
                "synthetic tty:winch post failed");
    TEST_ASSERT(tty_count_named(p, NOTE_NAME_TTY_WINCH) == 1,
                "synthetic tty:winch not queued");

    // A tty:-prefixed name OUTSIDE the supported set rejects even synthetic
    // (the supported-set validation, unchanged).
    TEST_ASSERT(notes_post(p, "tty:bogus", 0u, NULL, true) == -1,
                "unknown tty name accepted");

    // The snare gate is unregressed.
    TEST_ASSERT(notes_post(p, "snare:segv", 0u, NULL, false) == -1,
                "userspace snare post not rejected");

    pgrp_drop_linked(p);
}

void test_tty_pgrp_fanout_exactly_once(void) {
    struct Proc *self = current_thread()->proc;

    struct Proc *m1 = pgrp_make_linked_in_session();
    struct Proc *m2 = pgrp_make_linked_in_session();
    struct Proc *outsider = pgrp_make_linked_in_session();
    TEST_ASSERT(m1 && m2 && outsider, "proc_alloc failed");

    // Build a real group: m1 mints it, m2 joins; outsider stays in pgid 0.
    TEST_ASSERT(proc_setpgid(self, m1->pid, 0) == 0, "mint failed");
    TEST_ASSERT(proc_setpgid(self, m2->pid, m1->pid) == 0, "join failed");

    // Fan out: exactly the two members, exactly once each.
    int posted = notes_post_pgrp((u32)m1->pid, NOTE_NAME_TTY_WINCH, 3u);
    TEST_ASSERT(posted == 2, "fan-out did not post to exactly 2 members");
    TEST_ASSERT(tty_count_named(m1, NOTE_NAME_TTY_WINCH) == 1,
                "m1 did not get exactly one note");
    TEST_ASSERT(tty_count_named(m2, NOTE_NAME_TTY_WINCH) == 1,
                "m2 did not get exactly one note");
    TEST_ASSERT(tty_count_named(outsider, NOTE_NAME_TTY_WINCH) == 0,
                "outsider got a note");

    // pgid 0 (the boot group) is refused -- nothing posted anywhere.
    TEST_ASSERT(notes_post_pgrp(0u, NOTE_NAME_TTY_HUP, 0u) == 0,
                "pgid-0 fan-out not refused");
    TEST_ASSERT(tty_count_named(outsider, NOTE_NAME_TTY_HUP) == 0,
                "pgid-0 refusal leaked a post");

    pgrp_drop_linked(m1);
    pgrp_drop_linked(m2);
    pgrp_drop_linked(outsider);
}

void test_tty_terminate_class_gate(void) {
    struct Proc *p = pgrp_make_linked_in_session();
    TEST_ASSERT(p, "proc_alloc failed");
    struct Thread *t = current_thread();
    u32 saved_mask = t->note_mask;

    // tty:quit on a handler-less, non-self-managing Proc: terminate-class.
    TEST_ASSERT(notes_post(p, NOTE_NAME_TTY_QUIT, 0u, NULL, true) == 0,
                "synthetic tty:quit post failed");
    spin_lock(&p->notes->lock);
    const char *tname = notes_terminate_note_name_locked(p, NULL);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(tname && tty_streq(tname, NOTE_NAME_TTY_QUIT),
                "tty:quit did not report terminate-class");

    // The TTY latch armed; the INTERRUPT latch untouched (per-class).
    u32 flags = __atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE);
    TEST_ASSERT((flags & PROC_FLAG_TTY_TERMINATE_PENDING) != 0,
                "tty terminate latch not armed");
    TEST_ASSERT((flags & PROC_FLAG_INTR_TERMINATE_PENDING) == 0,
                "interrupt latch spuriously armed");

    // A thread that masked the tty FAMILY defers it (per-family mask).
    t->note_mask = (1u << NOTE_BIT_TTY);
    spin_lock(&p->notes->lock);
    tname = notes_terminate_note_name_locked(p, t);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(tname == NULL, "masked tty family still terminate-pending");
    t->note_mask = saved_mask;

    // Registering a handler flips the disposition to CATCH: no terminate,
    // both latches cleared. Unregistering does NOT re-arm (LS-5 contract).
    notes_set_handler(p, 0x100000u);
    spin_lock(&p->notes->lock);
    tname = notes_terminate_note_name_locked(p, NULL);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(tname == NULL, "handler-bearing Proc still terminate-pending");
    flags = __atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE);
    TEST_ASSERT((flags & (PROC_FLAG_TTY_TERMINATE_PENDING |
                          PROC_FLAG_INTR_TERMINATE_PENDING)) == 0,
                "handler registration did not clear the latches");
    // Unregistering splits the two surfaces (the LS-5 contract): the LATCH
    // (the lock-free wake hint) is NOT re-armed -- but the TRUTH function
    // reports the still-queued note again, because the unregistering thread
    // reaches its own EL0-return tail right after the syscall, where the
    // re-evaluation legitimately terminates it (nothing drained the note).
    notes_set_handler(p, 0u);
    flags = __atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE);
    TEST_ASSERT((flags & (PROC_FLAG_TTY_TERMINATE_PENDING |
                          PROC_FLAG_INTR_TERMINATE_PENDING)) == 0,
                "unregister re-armed the latch");
    spin_lock(&p->notes->lock);
    tname = notes_terminate_note_name_locked(p, NULL);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(tname && tty_streq(tname, NOTE_NAME_TTY_QUIT),
                "truth function lost the still-queued note after unregister");

    pgrp_drop_linked(p);

    // tty:winch is informational: queued, never terminate-class.
    struct Proc *w = pgrp_make_linked_in_session();
    TEST_ASSERT(w, "proc_alloc failed");
    TEST_ASSERT(notes_post(w, NOTE_NAME_TTY_WINCH, 0u, NULL, true) == 0,
                "synthetic winch post failed");
    spin_lock(&w->notes->lock);
    tname = notes_terminate_note_name_locked(w, NULL);
    spin_unlock(&w->notes->lock);
    TEST_ASSERT(tname == NULL, "tty:winch wrongly terminate-class");
    TEST_ASSERT((__atomic_load_n(&w->proc_flags, __ATOMIC_ACQUIRE) &
                 PROC_FLAG_TTY_TERMINATE_PENDING) == 0,
                "winch armed the terminate latch");

    // `interrupt` still terminates + arms ITS latch only (regression +
    // per-class independence).
    TEST_ASSERT(notes_post(w, "interrupt", 0u, NULL, true) == 0,
                "interrupt post failed");
    spin_lock(&w->notes->lock);
    tname = notes_terminate_note_name_locked(w, NULL);
    spin_unlock(&w->notes->lock);
    TEST_ASSERT(tname && tty_streq(tname, "interrupt"),
                "interrupt terminate regressed");
    flags = __atomic_load_n(&w->proc_flags, __ATOMIC_ACQUIRE);
    TEST_ASSERT((flags & PROC_FLAG_INTR_TERMINATE_PENDING) != 0,
                "interrupt latch not armed");
    TEST_ASSERT((flags & PROC_FLAG_TTY_TERMINATE_PENDING) == 0,
                "interrupt armed the tty latch");

    pgrp_drop_linked(w);
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
