// P5-hostowner-b-a — kernel-internal tests for the /cap elevation device.
//
// Coverage of devcap.c:
//
//   devcap.registered          — devcap is in the bestiary (dc='k',
//                                name="cap"); attach yields a QTDIR root.
//   devcap.walk_grant_use      — walk to "grant" + "use" yields QTFILE
//                                leaf Spoors with the correct magic.
//   devcap.walk_unknown        — walk to an unknown name returns NULL.
//   devcap.open_writeonly      — OREAD on a leaf fails; OWRITE succeeds.
//   devcap.grant_gate_no_cap   — register without CAP_GRANT_HOSTOWNER → -1.
//   devcap.grant_gate_bad_args — zero stripes / zero mask / non-grantable
//                                cap_mask all reject; pending count stays 0.
//   devcap.grant_replace       — re-register for same stripes replaces;
//                                pending count stays 1.
//   devcap.grant_table_full    — CAP_GRANT_MAX+1 fresh stripes; last fails.
//   devcap.use_gate_no_console — Proc without console-attached → -1.
//   devcap.use_no_pending      — no grant exists → -1.
//   devcap.use_basic           — grant + use → writer->caps gains
//                                CAP_HOSTOWNER; pending count back to 0.
//   devcap.use_one_shot        — second use after consume → -1.
//   devcap.use_mismatched_cap  — grant for X, use for Y → -1, grant kept.
//   devcap.exit_clears_grant   — cap_proc_exit_notify drops the target's
//                                pending grant.
//
// Each test calls cap_reset_table() first so it starts from an empty
// grant table (the harness runs tests sequentially in one address space).

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/devcap.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../../mm/slub.h"   // kfree (for the Walkqid allocated by devcap.walk)

void test_devcap_registered(void);
void test_devcap_walk_grant_use(void);
void test_devcap_walk_unknown(void);
void test_devcap_open_writeonly(void);
void test_devcap_grant_gate_no_cap(void);
void test_devcap_grant_gate_bad_args(void);
void test_devcap_grant_replace(void);
void test_devcap_grant_table_full(void);
void test_devcap_use_gate_no_console(void);
void test_devcap_use_no_pending(void);
void test_devcap_use_basic(void);
void test_devcap_use_one_shot(void);
void test_devcap_use_mismatched_cap(void);
void test_devcap_exit_clears_grant(void);
// A-4a clearance grant/redeem (the legate-creation half).
void test_devcap_clearance_grant_gate_no_cap(void);
void test_devcap_clearance_grant_bad_args(void);
void test_devcap_clearance_redeem_basic(void);
void test_devcap_clearance_self_restriction(void);
void test_devcap_clearance_redeem_beyond_grant(void);
void test_devcap_clearance_one_shot(void);
void test_devcap_clearance_cross_stripes(void);
void test_devcap_clearance_valid_until(void);
void test_devcap_clearance_kind_isolation(void);

// Test-only Proc construction. proc_alloc() draws a fresh stripes, so
// each test Proc has a unique tag. The console-attached + cap setup is
// the test's responsibility — the harness deliberately tests both
// directions (gate-fail and gate-pass).
static struct Proc *make_test_proc(void) {
    return proc_alloc();
}

static struct Proc *make_test_proc_with_caps(u64 caps) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->caps = caps;
    return p;
}

static struct Proc *make_test_proc_with_caps_console(u64 caps) {
    struct Proc *p = make_test_proc_with_caps(caps);
    if (!p) return NULL;
    proc_mark_console_attached(p);
    return p;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_devcap_registered(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('k'), &devcap,
        "devcap registered under dc='k'");
    TEST_EXPECT_EQ(dev_lookup_by_name("cap"), &devcap,
        "devcap registered under name=\"cap\"");

    struct Spoor *root = devcap.attach(NULL);
    TEST_ASSERT(root != NULL, "devcap.attach yields a Spoor");
    TEST_EXPECT_EQ((int)root->qid.type, (int)QTDIR,
        "/cap root Spoor is a directory (QTDIR)");
    TEST_EXPECT_EQ(root->dc, 'k', "/cap root Spoor carries dc='k'");
    spoor_clunk(root);
}

void test_devcap_walk_grant_use(void) {
    struct Spoor *root = devcap.attach(NULL);
    TEST_ASSERT(root != NULL, "attach");

    // Walk to "grant".
    struct Spoor *gnc = spoor_alloc(&devcap);
    TEST_ASSERT(gnc != NULL, "gnc alloc");
    gnc->qid = root->qid;     // shallow clone of root
    gnc->aux = NULL;
    const char *gname[1] = { "grant" };
    struct Walkqid *gw = devcap.walk(root, gnc, gname, 1);
    TEST_ASSERT(gw != NULL, "walk to grant succeeds");
    TEST_EXPECT_EQ(gw->nqid, 1, "walk grant nqid=1");
    TEST_EXPECT_EQ((int)gnc->qid.type, (int)QTFILE, "grant leaf is QTFILE");
    TEST_ASSERT(gnc->aux != NULL, "grant leaf carries aux");
    spoor_clunk(gnc);          // triggers devcap_close
    kfree(gw);

    // Walk to "use".
    struct Spoor *unc = spoor_alloc(&devcap);
    TEST_ASSERT(unc != NULL, "unc alloc");
    unc->qid = root->qid;
    unc->aux = NULL;
    const char *uname[1] = { "use" };
    struct Walkqid *uw = devcap.walk(root, unc, uname, 1);
    TEST_ASSERT(uw != NULL, "walk to use succeeds");
    TEST_EXPECT_EQ(uw->nqid, 1, "walk use nqid=1");
    TEST_EXPECT_EQ((int)unc->qid.type, (int)QTFILE, "use leaf is QTFILE");
    TEST_ASSERT(unc->aux != NULL, "use leaf carries aux");
    // grant.qid.path != use.qid.path — qid.path == leaf magic.
    spoor_clunk(unc);
    kfree(uw);

    spoor_clunk(root);
}

void test_devcap_walk_unknown(void) {
    struct Spoor *root = devcap.attach(NULL);
    TEST_ASSERT(root != NULL, "attach");

    struct Spoor *nc = spoor_alloc(&devcap);
    TEST_ASSERT(nc != NULL, "nc alloc");
    nc->qid = root->qid;
    nc->aux = NULL;
    const char *bad[1] = { "nonexistent" };
    struct Walkqid *w = devcap.walk(root, nc, bad, 1);
    TEST_EXPECT_EQ(w, (struct Walkqid *)NULL, "walk to unknown returns NULL");
    spoor_clunk(nc);
    spoor_clunk(root);
}

void test_devcap_open_writeonly(void) {
    struct Spoor *root = devcap.attach(NULL);
    TEST_ASSERT(root != NULL, "attach");

    // Walk grant, then attempt to open it OREAD (should fail).
    struct Spoor *nc = spoor_alloc(&devcap);
    TEST_ASSERT(nc != NULL, "alloc");
    nc->qid = root->qid;
    nc->aux = NULL;
    const char *gname[1] = { "grant" };
    struct Walkqid *w = devcap.walk(root, nc, gname, 1);
    TEST_ASSERT(w != NULL, "walk grant");
    kfree(w);

    // OREAD = 0
    struct Spoor *r = devcap.open(nc, 0);
    TEST_EXPECT_EQ(r, (struct Spoor *)NULL, "open OREAD on grant rejected");
    // OWRITE = 1
    struct Spoor *o = devcap.open(nc, 1);
    TEST_EXPECT_EQ(o, nc, "open OWRITE on grant succeeds");

    spoor_clunk(nc);
    spoor_clunk(root);
}

void test_devcap_grant_gate_no_cap(void) {
    cap_reset_table();
    struct Proc *p = make_test_proc();          // caps = 0
    TEST_ASSERT(p != NULL, "alloc proc");

    long rc = cap_register_grant_for_writer(p, CAP_HOSTOWNER, 0xDEADBEEF);
    TEST_EXPECT_EQ(rc, -1, "grant rejected: writer lacks CAP_GRANT_HOSTOWNER");
    TEST_EXPECT_EQ(cap_pending_count(), 0, "pending count stays 0");

    drop_test_proc(p);
}

void test_devcap_grant_gate_bad_args(void) {
    cap_reset_table();
    struct Proc *p = make_test_proc_with_caps(CAP_GRANT_HOSTOWNER);
    TEST_ASSERT(p != NULL, "alloc");

    TEST_EXPECT_EQ(cap_register_grant_for_writer(p, CAP_HOSTOWNER, 0), -1,
        "grant rejected: target_stripes == 0");
    TEST_EXPECT_EQ(cap_register_grant_for_writer(p, 0, 0xABCDEF), -1,
        "grant rejected: cap_mask == 0");
    // CAP_HW_CREATE is not in CAP_GRANTABLE.
    TEST_EXPECT_EQ(cap_register_grant_for_writer(p, CAP_HW_CREATE, 0xABCDEF), -1,
        "grant rejected: cap_mask not subset of CAP_GRANTABLE");
    TEST_EXPECT_EQ(cap_pending_count(), 0, "pending count stays 0");

    drop_test_proc(p);
}

void test_devcap_grant_replace(void) {
    cap_reset_table();
    struct Proc *p = make_test_proc_with_caps(CAP_GRANT_HOSTOWNER);
    TEST_ASSERT(p != NULL, "alloc");

    TEST_EXPECT_EQ(cap_register_grant_for_writer(p, CAP_HOSTOWNER, 0x111),
        (long)CAP_GRANT_WRITE_LEN, "first grant ok");
    TEST_EXPECT_EQ(cap_pending_count(), 1, "1 pending after first");

    // Re-register for same stripes — replace, not add.
    TEST_EXPECT_EQ(cap_register_grant_for_writer(p, CAP_HOSTOWNER, 0x111),
        (long)CAP_GRANT_WRITE_LEN, "re-register same stripes ok");
    TEST_EXPECT_EQ(cap_pending_count(), 1, "still 1 pending after replace");

    drop_test_proc(p);
    cap_reset_table();
}

void test_devcap_grant_table_full(void) {
    cap_reset_table();
    struct Proc *p = make_test_proc_with_caps(CAP_GRANT_HOSTOWNER);
    TEST_ASSERT(p != NULL, "alloc");

    // Fill CAP_GRANT_MAX slots with distinct stripes.
    for (u32 i = 0; i < CAP_GRANT_MAX; i++) {
        long rc = cap_register_grant_for_writer(p, CAP_HOSTOWNER,
                                                0x1000ull + i);
        TEST_EXPECT_EQ(rc, (long)CAP_GRANT_WRITE_LEN, "fill slot ok");
    }
    TEST_EXPECT_EQ((u32)cap_pending_count(), CAP_GRANT_MAX,
        "table full of pending");

    // CAP_GRANT_MAX+1th distinct stripes → fails.
    long rc = cap_register_grant_for_writer(p, CAP_HOSTOWNER,
                                            0x1000ull + CAP_GRANT_MAX);
    TEST_EXPECT_EQ(rc, -1, "registration past CAP_GRANT_MAX rejected");

    drop_test_proc(p);
    cap_reset_table();
}

void test_devcap_use_gate_no_console(void) {
    cap_reset_table();
    // Set up: a grantor with CAP_GRANT_HOSTOWNER, a redeemer with no
    // console-attached.
    struct Proc *grantor = make_test_proc_with_caps(CAP_GRANT_HOSTOWNER);
    TEST_ASSERT(grantor != NULL, "alloc grantor");
    struct Proc *redeemer = make_test_proc();   // caps=0, no console
    TEST_ASSERT(redeemer != NULL, "alloc redeemer");

    u64 target = proc_stripes(redeemer);
    TEST_EXPECT_EQ(cap_register_grant_for_writer(grantor, CAP_HOSTOWNER, target),
        (long)CAP_GRANT_WRITE_LEN, "grant ok");

    long rc = cap_redeem_grant_for_writer(redeemer, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(rc, -1, "use rejected: writer not console-attached");
    TEST_EXPECT_EQ(redeemer->caps & CAP_HOSTOWNER, (u64)0,
        "redeemer did not gain CAP_HOSTOWNER");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_use_no_pending(void) {
    cap_reset_table();
    struct Proc *p = make_test_proc_with_caps_console(0);
    TEST_ASSERT(p != NULL, "alloc");

    long rc = cap_redeem_grant_for_writer(p, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(rc, -1, "use rejected: no pending grant");

    drop_test_proc(p);
}

void test_devcap_use_basic(void) {
    cap_reset_table();
    struct Proc *grantor = make_test_proc_with_caps(CAP_GRANT_HOSTOWNER);
    TEST_ASSERT(grantor != NULL, "alloc grantor");
    struct Proc *redeemer = make_test_proc_with_caps_console(0);
    TEST_ASSERT(redeemer != NULL, "alloc redeemer");

    u64 target = proc_stripes(redeemer);
    TEST_EXPECT_EQ(cap_register_grant_for_writer(grantor, CAP_HOSTOWNER, target),
        (long)CAP_GRANT_WRITE_LEN, "grant ok");
    TEST_EXPECT_EQ(cap_pending_count(), 1, "1 pending");

    long rc = cap_redeem_grant_for_writer(redeemer, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(rc, (long)CAP_USE_WRITE_LEN, "use ok");
    TEST_EXPECT_NE(redeemer->caps & CAP_HOSTOWNER, (u64)0,
        "redeemer gained CAP_HOSTOWNER");
    TEST_EXPECT_EQ(cap_pending_count(), 0, "pending consumed");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_use_one_shot(void) {
    cap_reset_table();
    struct Proc *grantor = make_test_proc_with_caps(CAP_GRANT_HOSTOWNER);
    TEST_ASSERT(grantor != NULL, "alloc grantor");
    struct Proc *redeemer = make_test_proc_with_caps_console(0);
    TEST_ASSERT(redeemer != NULL, "alloc redeemer");

    u64 target = proc_stripes(redeemer);
    cap_register_grant_for_writer(grantor, CAP_HOSTOWNER, target);
    TEST_EXPECT_EQ(cap_redeem_grant_for_writer(redeemer, CAP_HOSTOWNER),
        (long)CAP_USE_WRITE_LEN, "first use ok");

    // Second redeem with no fresh grant → -1.
    long rc = cap_redeem_grant_for_writer(redeemer, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(rc, -1, "second use rejected: grant consumed");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_use_mismatched_cap(void) {
    cap_reset_table();
    struct Proc *grantor = make_test_proc_with_caps(CAP_GRANT_HOSTOWNER);
    TEST_ASSERT(grantor != NULL, "alloc grantor");
    struct Proc *redeemer = make_test_proc_with_caps_console(0);
    TEST_ASSERT(redeemer != NULL, "alloc redeemer");

    u64 target = proc_stripes(redeemer);
    cap_register_grant_for_writer(grantor, CAP_HOSTOWNER, target);

    // Ask for a different (non-grantable) cap_mask → fail.
    long rc = cap_redeem_grant_for_writer(redeemer, CAP_HW_CREATE);
    TEST_EXPECT_EQ(rc, -1, "use mismatched cap rejected at gate");
    // Grant retained (not consumed by the failed redeem).
    TEST_EXPECT_EQ(cap_pending_count(), 1, "grant retained on mismatch");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_exit_clears_grant(void) {
    cap_reset_table();
    struct Proc *grantor = make_test_proc_with_caps(CAP_GRANT_HOSTOWNER);
    TEST_ASSERT(grantor != NULL, "alloc grantor");
    struct Proc *target = make_test_proc();
    TEST_ASSERT(target != NULL, "alloc target");

    u64 target_stripes = proc_stripes(target);
    cap_register_grant_for_writer(grantor, CAP_HOSTOWNER, target_stripes);
    TEST_EXPECT_EQ(cap_pending_count(), 1, "1 pending before exit-notify");

    // Simulate the target's exit hook (the real exits() calls this).
    cap_proc_exit_notify(target);
    TEST_EXPECT_EQ(cap_pending_count(), 0, "pending dropped on target exit");

    drop_test_proc(target);
    drop_test_proc(grantor);
}

// =============================================================================
// A-4a clearance grant/redeem -- the legate-creation half (the teardown half
// is exercised by test_proc.c's legate-teardown-walk tests, since the actual
// death fires at the EL0 die-check which kernel test threads never reach).
// =============================================================================

void test_devcap_clearance_grant_gate_no_cap(void) {
    cap_reset_table();
    struct Proc *p = make_test_proc();   // caps = 0; lacks CAP_GRANT_CLEARANCE
    TEST_ASSERT(p != NULL, "alloc proc");

    long rc = cap_register_clearance_grant_for_writer(
        p, CAP_DAC_OVERRIDE, 0xDEAD, 0, 0x5E55);
    TEST_EXPECT_EQ(rc, -1, "clearance grant rejected: lacks CAP_GRANT_CLEARANCE");
    TEST_EXPECT_EQ(cap_pending_count(), 0, "pending count stays 0");

    drop_test_proc(p);
}

void test_devcap_clearance_grant_bad_args(void) {
    cap_reset_table();
    struct Proc *p = make_test_proc_with_caps(CAP_GRANT_CLEARANCE);
    TEST_ASSERT(p != NULL, "alloc");

    TEST_EXPECT_EQ(cap_register_clearance_grant_for_writer(
        p, CAP_DAC_OVERRIDE, 0, 0, 0x5E55), -1, "reject: target_stripes == 0");
    TEST_EXPECT_EQ(cap_register_clearance_grant_for_writer(
        p, 0, 0xABC, 0, 0x5E55), -1, "reject: cap_mask == 0");
    // CAP_HOSTOWNER is NOT in CAP_GRANTABLE_CLEARANCE (it stays on the
    // console-gated hostowner path).
    TEST_EXPECT_EQ(cap_register_clearance_grant_for_writer(
        p, CAP_HOSTOWNER, 0xABC, 0, 0x5E55), -1,
        "reject: cap_mask not subset of CAP_GRANTABLE_CLEARANCE");
    // A fork-grantable cap is likewise not clearance-grantable.
    TEST_EXPECT_EQ(cap_register_clearance_grant_for_writer(
        p, CAP_HW_CREATE, 0xABC, 0, 0x5E55), -1, "reject: fork-grantable cap");
    TEST_EXPECT_EQ(cap_register_clearance_grant_for_writer(
        p, CAP_DAC_OVERRIDE, 0xABC, 0, 0), -1, "reject: session_id == 0 sentinel");
    TEST_EXPECT_EQ(cap_register_clearance_grant_for_writer(
        p, CAP_DAC_OVERRIDE, 0xABC, 0, 0x100000000ull), -1,
        "reject: session_id exceeds u32");
    TEST_EXPECT_EQ(cap_pending_count(), 0, "pending count stays 0");

    drop_test_proc(p);
    cap_reset_table();
}

void test_devcap_clearance_redeem_basic(void) {
    cap_reset_table();
    struct Proc *grantor  = make_test_proc_with_caps(CAP_GRANT_CLEARANCE);
    TEST_ASSERT(grantor != NULL, "alloc grantor");
    // The redeemer is NOT console-attached -- a clearance redeem must NOT
    // require it (the key difference from the hostowner path).
    struct Proc *redeemer = make_test_proc();
    TEST_ASSERT(redeemer != NULL, "alloc redeemer");

    u64 target = proc_stripes(redeemer);
    TEST_EXPECT_EQ(cap_register_clearance_grant_for_writer(
        grantor, CAP_DAC_OVERRIDE, target, 0, 0x5E55),
        (long)CAP_GRANT_CLEARANCE_WRITE_LEN, "clearance grant ok");
    TEST_EXPECT_EQ(cap_pending_count(), 1, "1 pending");

    long rc = cap_redeem_grant_for_writer(redeemer, CAP_DAC_OVERRIDE);
    TEST_EXPECT_EQ(rc, (long)CAP_USE_WRITE_LEN, "clearance redeem ok (no console)");
    TEST_EXPECT_NE(redeemer->caps & CAP_DAC_OVERRIDE, (u64)0, "gained DAC_OVERRIDE");
    TEST_EXPECT_NE(redeemer->legate_scope_id, (u32)0, "became a legate (scope set)");
    TEST_EXPECT_EQ(redeemer->legate_session_id, (u32)0x5E55, "session recorded");
    TEST_EXPECT_EQ(redeemer->legate_valid_until, (u64)0, "valid_for 0 -> no deadline");
    TEST_EXPECT_NE(redeemer->proc_flags & PROC_FLAG_LEGATE_ROOT, (u32)0,
        "marked LEGATE_ROOT");
    TEST_EXPECT_EQ(cap_pending_count(), 0, "pending consumed");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_clearance_self_restriction(void) {
    cap_reset_table();
    struct Proc *grantor  = make_test_proc_with_caps(CAP_GRANT_CLEARANCE);
    struct Proc *redeemer = make_test_proc();
    TEST_ASSERT(grantor && redeemer, "alloc");

    u64 target = proc_stripes(redeemer);
    // Grant {DAC_OVERRIDE | CHOWN}; the redeemer self-restricts to DAC only.
    cap_register_clearance_grant_for_writer(
        grantor, CAP_DAC_OVERRIDE | CAP_CHOWN, target, 0, 0x5E55);
    long rc = cap_redeem_grant_for_writer(redeemer, CAP_DAC_OVERRIDE);
    TEST_EXPECT_EQ(rc, (long)CAP_USE_WRITE_LEN, "redeem subset ok");
    TEST_EXPECT_NE(redeemer->caps & CAP_DAC_OVERRIDE, (u64)0, "gained DAC_OVERRIDE");
    TEST_EXPECT_EQ(redeemer->caps & CAP_CHOWN, (u64)0,
        "did NOT gain CHOWN (self-restricted below the grant)");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_clearance_redeem_beyond_grant(void) {
    cap_reset_table();
    struct Proc *grantor  = make_test_proc_with_caps(CAP_GRANT_CLEARANCE);
    struct Proc *redeemer = make_test_proc();
    TEST_ASSERT(grantor && redeemer, "alloc");

    u64 target = proc_stripes(redeemer);
    // Grant only DAC_OVERRIDE; redeem asks for DAC_OVERRIDE | CHOWN (beyond).
    cap_register_clearance_grant_for_writer(
        grantor, CAP_DAC_OVERRIDE, target, 0, 0x5E55);
    long rc = cap_redeem_grant_for_writer(redeemer, CAP_DAC_OVERRIDE | CAP_CHOWN);
    TEST_EXPECT_EQ(rc, -1, "redeem beyond grant rejected");
    TEST_EXPECT_EQ(redeemer->caps & (CAP_DAC_OVERRIDE | CAP_CHOWN), (u64)0,
        "no cap gained on rejected redeem");
    TEST_EXPECT_EQ(redeemer->legate_scope_id, (u32)0, "not a legate (redeem failed)");
    TEST_EXPECT_EQ(cap_pending_count(), 1, "grant retained on beyond-grant reject");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_clearance_one_shot(void) {
    cap_reset_table();
    struct Proc *grantor  = make_test_proc_with_caps(CAP_GRANT_CLEARANCE);
    struct Proc *redeemer = make_test_proc();
    TEST_ASSERT(grantor && redeemer, "alloc");

    u64 target = proc_stripes(redeemer);
    cap_register_clearance_grant_for_writer(
        grantor, CAP_DAC_OVERRIDE, target, 0, 0x5E55);
    TEST_EXPECT_EQ(cap_redeem_grant_for_writer(redeemer, CAP_DAC_OVERRIDE),
        (long)CAP_USE_WRITE_LEN, "first redeem ok");
    long rc = cap_redeem_grant_for_writer(redeemer, CAP_DAC_OVERRIDE);
    TEST_EXPECT_EQ(rc, -1, "second redeem rejected: grant consumed");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_clearance_cross_stripes(void) {
    cap_reset_table();
    struct Proc *grantor  = make_test_proc_with_caps(CAP_GRANT_CLEARANCE);
    struct Proc *target   = make_test_proc();    // grant is FOR this one's stripes
    struct Proc *other    = make_test_proc();    // a DIFFERENT Proc tries to redeem
    TEST_ASSERT(grantor && target && other, "alloc");

    cap_register_clearance_grant_for_writer(
        grantor, CAP_DAC_OVERRIDE, proc_stripes(target), 0, 0x5E55);
    long rc = cap_redeem_grant_for_writer(other, CAP_DAC_OVERRIDE);
    TEST_EXPECT_EQ(rc, -1, "cross-stripes redeem rejected");
    TEST_EXPECT_EQ(other->legate_scope_id, (u32)0, "other did not become a legate");
    TEST_EXPECT_EQ(cap_pending_count(), 1, "grant retained (wrong stripes)");

    drop_test_proc(other);
    drop_test_proc(target);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_clearance_valid_until(void) {
    cap_reset_table();
    struct Proc *grantor  = make_test_proc_with_caps(CAP_GRANT_CLEARANCE);
    struct Proc *r1       = make_test_proc();
    struct Proc *r2       = make_test_proc();
    TEST_ASSERT(grantor && r1 && r2, "alloc");

    // valid_for > 0 -> legate_valid_until is a future deadline (> 0).
    cap_register_clearance_grant_for_writer(
        grantor, CAP_DAC_OVERRIDE, proc_stripes(r1), 1000000000000ull, 0x5E55);
    TEST_EXPECT_EQ(cap_redeem_grant_for_writer(r1, CAP_DAC_OVERRIDE),
        (long)CAP_USE_WRITE_LEN, "redeem r1 ok");
    TEST_EXPECT_NE(r1->legate_valid_until, (u64)0,
        "valid_for > 0 -> a deadline was computed");

    // valid_for == 0 -> no deadline (scope ends only on root exit).
    cap_register_clearance_grant_for_writer(
        grantor, CAP_DAC_OVERRIDE, proc_stripes(r2), 0, 0x5E56);
    TEST_EXPECT_EQ(cap_redeem_grant_for_writer(r2, CAP_DAC_OVERRIDE),
        (long)CAP_USE_WRITE_LEN, "redeem r2 ok");
    TEST_EXPECT_EQ(r2->legate_valid_until, (u64)0, "valid_for 0 -> no deadline");

    drop_test_proc(r2);
    drop_test_proc(r1);
    drop_test_proc(grantor);
    cap_reset_table();
}

void test_devcap_clearance_kind_isolation(void) {
    // A clearance grant then a hostowner re-register for the SAME stripes must
    // fully reset the entry to the hostowner kind -- a stale clearance flag
    // must NOT let a no-console writer redeem. Proves cap_set_entry_locked
    // resets every discriminator field.
    cap_reset_table();
    struct Proc *grantor  = make_test_proc_with_caps(
        CAP_GRANT_CLEARANCE | CAP_GRANT_HOSTOWNER);
    struct Proc *redeemer = make_test_proc();   // NOT console-attached
    TEST_ASSERT(grantor && redeemer, "alloc");

    u64 target = proc_stripes(redeemer);
    // First a clearance grant (no-console redeemable), then re-register a
    // hostowner grant over the SAME stripes.
    cap_register_clearance_grant_for_writer(
        grantor, CAP_DAC_OVERRIDE, target, 0, 0x5E55);
    TEST_EXPECT_EQ(cap_register_grant_for_writer(grantor, CAP_HOSTOWNER, target),
        (long)CAP_GRANT_WRITE_LEN, "re-register hostowner over same stripes");
    TEST_EXPECT_EQ(cap_pending_count(), 1, "still 1 pending (replaced in place)");

    // The entry is now a HOSTOWNER grant -> redeem requires console. A
    // no-console redeem must fail (if the stale clearance kind had survived,
    // this would wrongly succeed without console).
    long rc = cap_redeem_grant_for_writer(redeemer, CAP_HOSTOWNER);
    TEST_EXPECT_EQ(rc, -1, "no-console hostowner redeem rejected (kind reset)");
    TEST_EXPECT_EQ(redeemer->caps & CAP_HOSTOWNER, (u64)0, "no CAP_HOSTOWNER gained");
    TEST_EXPECT_EQ(redeemer->legate_scope_id, (u32)0, "did NOT become a legate");
    TEST_EXPECT_EQ(cap_pending_count(), 1, "grant retained (console gate)");

    drop_test_proc(redeemer);
    drop_test_proc(grantor);
    cap_reset_table();
}
