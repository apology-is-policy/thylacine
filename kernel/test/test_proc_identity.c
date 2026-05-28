// A-1a: identity-model tests (docs/IDENTITY-DESIGN.md §3.3 + §9.1; ARCH §28 I-22).
//
// The durable identity (principal_id + primary_gid + supp_gids) is INHERITED
// across rfork/spawn — distinct from caps (subset-reduced) and stripes (fresh
// per Proc). kproc is the SYSTEM identity; the spawn path optionally overrides
// the child's identity, gated FAIL-CLOSED on CAP_SET_IDENTITY. These tests run
// in kproc context (the harness thread is a kproc thread), so the "parent"
// identity is SYSTEM and the parent holds CAP_ALL (incl. CAP_SET_IDENTITY).

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// Defined in kernel/syscall.c; identity passed as scalars so this test needs
// no kernel-internal struct spawn_identity.
extern int sys_spawn_full_argv_identity_for_proc(
        struct Proc *p, const char *name, size_t name_len,
        const char *argv_data, u32 argv_data_len, u32 argc,
        const u32 *fds, u32 fd_count,
        caps_t cap_mask, u32 perm_flags,
        bool set_identity, u32 principal_id, u32 primary_gid,
        const u32 *supp_gids, u32 supp_gid_count);

void test_proc_identity_kproc_is_system(void);
void test_proc_identity_rfork_inherits(void);
void test_proc_identity_apply_sets_fields(void);
void test_proc_identity_spawn_set_rejected_without_cap(void);
void test_proc_identity_spawn_set_accepted_with_cap(void);
void test_proc_identity_set_rejects_reserved(void);
void test_proc_identity_set_rejects_system_supp_gid(void);
void test_proc_identity_peer_snapshot_by_stripes(void);

static void drain_zombies(void) {
    int status = 0;
    while (wait_pid(&status) > 0) { /* drain */ }
}

// kproc is the boot/kernel-proc SYSTEM identity (holds caps via the boot
// chain, NOT via identity — I-22). supp_gid_count stays 0 (KP_ZERO).
void test_proc_identity_kproc_is_system(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp != NULL, "kproc() is NULL");
    TEST_EXPECT_EQ(kp->principal_id, (u32)PRINCIPAL_SYSTEM,
                   "kproc principal_id != PRINCIPAL_SYSTEM");
    TEST_EXPECT_EQ(kp->primary_gid, (u32)GID_SYSTEM,
                   "kproc primary_gid != GID_SYSTEM");
    TEST_EXPECT_EQ((u32)kp->supp_gid_count, 0u,
                   "kproc supp_gid_count != 0");
}

// rfork inherits identity (parent -> child) while caps reduce to NONE and
// stripes stay fresh. The single test demonstrates I-22: the child carries
// the SYSTEM *identity* yet holds NO caps — identity conferred no authority.
static u32 inh_principal;
static u32 inh_gid;
static u32 inh_caps;
static u64 inh_stripes;
static void inherit_child_thunk(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    if (!t || !t->proc) extinction("inherit_child: no proc");
    struct Proc *p = t->proc;
    inh_principal = p->principal_id;
    inh_gid       = p->primary_gid;
    inh_caps      = (u32)p->caps;
    inh_stripes   = p->stripes;
    exits("ok");
}
void test_proc_identity_rfork_inherits(void) {
    drain_zombies();
    inh_principal = 0; inh_gid = 0; inh_caps = 0xDEADu; inh_stripes = 0;
    struct Proc *kp = kproc();
    int pid = rfork(RFPROC, inherit_child_thunk, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid");
    TEST_EXPECT_EQ(status, 0, "child exit status");
    // Identity inherited from the SYSTEM parent.
    TEST_EXPECT_EQ(inh_principal, (u32)PRINCIPAL_SYSTEM,
                   "child did not inherit parent principal_id");
    TEST_EXPECT_EQ(inh_gid, (u32)GID_SYSTEM,
                   "child did not inherit parent primary_gid");
    // I-22: SYSTEM identity, yet caps reduced to NONE (no authority leak).
    TEST_EXPECT_EQ(inh_caps, (u32)CAP_NONE,
                   "rfork child holds caps despite inheriting SYSTEM identity");
    // stripes are fresh per Proc — the child's tag DIFFERS from the parent's.
    TEST_EXPECT_NE(inh_stripes, kp->stripes,
                   "child stripes equal parent (must be fresh)");
}

// proc_apply_identity is the single audited mutation site. Run it in a child
// on the child's own Proc, then read back: fields set, supp tail zeroed.
static u32 ap_principal;
static u32 ap_gid;
static u32 ap_count;
static u32 ap_g0, ap_g1, ap_g2, ap_tail;
static void apply_child_thunk(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    if (!t || !t->proc) extinction("apply_child: no proc");
    struct Proc *p = t->proc;
    u32 gids[3] = { 3000u, 3001u, 3002u };
    proc_apply_identity(p, 1000u, 2000u, gids, 3);
    ap_principal = p->principal_id;
    ap_gid       = p->primary_gid;
    ap_count     = (u32)p->supp_gid_count;
    ap_g0        = p->supp_gids[0];
    ap_g1        = p->supp_gids[1];
    ap_g2        = p->supp_gids[2];
    ap_tail      = p->supp_gids[3];   // beyond count -> must be zeroed
    exits("ok");
}
void test_proc_identity_apply_sets_fields(void) {
    drain_zombies();
    ap_principal = ap_gid = ap_count = ap_g0 = ap_g1 = ap_g2 = ap_tail = 0xFFu;
    int pid = rfork(RFPROC, apply_child_thunk, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -42;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid pid");
    TEST_EXPECT_EQ(status, 0, "child exit status");
    TEST_EXPECT_EQ(ap_principal, 1000u, "apply principal_id");
    TEST_EXPECT_EQ(ap_gid, 2000u, "apply primary_gid");
    TEST_EXPECT_EQ(ap_count, 3u, "apply supp_gid_count");
    TEST_EXPECT_EQ(ap_g0, 3000u, "apply supp_gids[0]");
    TEST_EXPECT_EQ(ap_g1, 3001u, "apply supp_gids[1]");
    TEST_EXPECT_EQ(ap_g2, 3002u, "apply supp_gids[2]");
    TEST_EXPECT_EQ(ap_tail, 0u, "apply supp tail not zeroed");
}

// FAIL-CLOSED gate: a SPAWN_IDENTITY_SET request from a Proc lacking
// CAP_SET_IDENTITY returns -1 (rejected; no child spawned). The child here
// holds CAP_NONE (rfork from kproc strips), so the set is refused.
static int reject_rc;
static void reject_child_thunk(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    if (!t || !t->proc) extinction("reject_child: no proc");
    struct Proc *p = t->proc;
    // p has CAP_NONE; request a set identity -> must be -1.
    reject_rc = sys_spawn_full_argv_identity_for_proc(
        p, "hello", 5, NULL, 0u, 0u, NULL, 0u,
        CAP_NONE, 0u, /*set=*/true, 1000u, 1000u, NULL, 0u);
    exits("ok");
}
void test_proc_identity_spawn_set_rejected_without_cap(void) {
    drain_zombies();
    reject_rc = 0;
    int pid = rfork(RFPROC, reject_child_thunk, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -42;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid pid");
    TEST_EXPECT_EQ(status, 0, "child exit status");
    TEST_EXPECT_EQ(reject_rc, -1,
                   "uncapped SET request was not rejected with -1");
}

// Capped accept: kproc context holds CAP_SET_IDENTITY (via CAP_ALL); a SET
// spawn of /hello with a real identity succeeds (the thunk applies it before
// exec; /hello exits clean). proc_apply_identity correctness is pinned by the
// apply_sets_fields unit test; this proves the gate accepts + the full
// spawn-with-identity path completes.
void test_proc_identity_spawn_set_accepted_with_cap(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    TEST_ASSERT((t->proc->caps & CAP_SET_IDENTITY) != 0,
                "kproc context lacks CAP_SET_IDENTITY");
    int pid = sys_spawn_full_argv_identity_for_proc(
        t->proc, "hello", 5, NULL, 0u, 0u, NULL, 0u,
        CAP_NONE, 0u, /*set=*/true, 1000u, 1000u, NULL, 0u);
    TEST_ASSERT(pid > 0, "capped SET spawn did not return positive pid");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid reaps the child");
    TEST_EXPECT_EQ(status, 0, "/hello exits clean under SET identity");
}

// Reserved-value reject: even a capped caller cannot SET INVALID(0) or the
// SYSTEM sentinel (you cannot stamp the never-valid 0 nor forge SYSTEM).
void test_proc_identity_set_rejects_reserved(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    struct Proc *p = t->proc;
    int rc_invalid = sys_spawn_full_argv_identity_for_proc(
        p, "hello", 5, NULL, 0u, 0u, NULL, 0u,
        CAP_NONE, 0u, /*set=*/true, PRINCIPAL_INVALID, 1000u, NULL, 0u);
    TEST_EXPECT_EQ(rc_invalid, -1, "SET principal_id=INVALID not rejected");
    int rc_system = sys_spawn_full_argv_identity_for_proc(
        p, "hello", 5, NULL, 0u, 0u, NULL, 0u,
        CAP_NONE, 0u, /*set=*/true, PRINCIPAL_SYSTEM, 1000u, NULL, 0u);
    TEST_EXPECT_EQ(rc_system, -1, "SET principal_id=SYSTEM not rejected");
    int rc_gid = sys_spawn_full_argv_identity_for_proc(
        p, "hello", 5, NULL, 0u, 0u, NULL, 0u,
        CAP_NONE, 0u, /*set=*/true, 1000u, GID_SYSTEM, NULL, 0u);
    TEST_EXPECT_EQ(rc_gid, -1, "SET primary_gid=SYSTEM not rejected");
    // No grandchildren were spawned (all rejected); nothing to reap.
}

// A-1a R1 F1: a supplementary gid of GID_SYSTEM (or GID_INVALID) is rejected
// by the SET gate, uniformly with the primary id/gid (no smuggling the system
// group into a user's supplementary set). Capped caller; the request is refused
// before any spawn, so there is no grandchild to reap.
void test_proc_identity_set_rejects_system_supp_gid(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    struct Proc *p = t->proc;
    u32 sys_supp[1] = { GID_SYSTEM };
    int rc_sys = sys_spawn_full_argv_identity_for_proc(
        p, "hello", 5, NULL, 0u, 0u, NULL, 0u,
        CAP_NONE, 0u, /*set=*/true, 1000u, 1000u, sys_supp, 1u);
    TEST_EXPECT_EQ(rc_sys, -1, "SET supp_gid=SYSTEM not rejected");
    u32 inv_supp[1] = { GID_INVALID };
    int rc_inv = sys_spawn_full_argv_identity_for_proc(
        p, "hello", 5, NULL, 0u, 0u, NULL, 0u,
        CAP_NONE, 0u, /*set=*/true, 1000u, 1000u, inv_supp, 1u);
    TEST_EXPECT_EQ(rc_inv, -1, "SET supp_gid=INVALID not rejected");
}

// The peer-snapshot data path that feeds srv_peer_info: looking up kproc by
// its stripes yields its identity; the 0 sentinel + an unassigned tag fail.
void test_proc_identity_peer_snapshot_by_stripes(void) {
    struct Proc *kp = kproc();
    caps_t caps = 0; u32 pid_out = 0xABCDu; u32 gid_out = 0xABCDu;
    bool found = proc_peer_snapshot_by_stripes(kp->stripes, &caps,
                                               &pid_out, &gid_out);
    TEST_ASSERT(found, "kproc not found by its own stripes");
    TEST_EXPECT_EQ(pid_out, (u32)PRINCIPAL_SYSTEM, "snapshot principal_id");
    TEST_EXPECT_EQ(gid_out, (u32)GID_SYSTEM, "snapshot primary_gid");
    // 0 sentinel fail-closes; out-params untouched.
    u32 pid2 = 0x1234u;
    bool found0 = proc_peer_snapshot_by_stripes(0, NULL, &pid2, NULL);
    TEST_ASSERT(!found0, "0-sentinel stripes matched a Proc");
    TEST_EXPECT_EQ(pid2, 0x1234u, "0-sentinel touched out-param");
    // An unassigned (far-future) tag matches nothing.
    bool foundx = proc_peer_snapshot_by_stripes(0xFFFFFFFFFFFFFFFEull,
                                                NULL, NULL, NULL);
    TEST_ASSERT(!foundx, "unassigned stripes matched a Proc");
}
