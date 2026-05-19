// P5-corvus-srv-impl-a2 — kernel-internal tests for the /srv service
// registry, the devsrv Dev, and SYS_POST_SERVICE.
//
// Coverage:
//
//   devsrv.registered
//     devsrv is in the bestiary (dc='s', name="srv"); attach yields a
//     QTDIR /srv root Spoor.
//
//   devsrv.post_gate
//     SYS_POST_SERVICE is refused for a Proc without PROC_FLAG_MAY_POST_
//     SERVICE and for a malformed name; accepted once the Proc is marked.
//
//   devsrv.post_basic
//     A post produces a LIVE registry entry stamped with the poster's
//     stripes/pid; a name with a live server cannot be re-posted (same
//     or different Proc); a distinct name posts independently.
//
//   devsrv.tombstone
//     srv_proc_exit_notify tombstones the poster's LIVE service; a
//     tombstoned name is re-postable only by a marked Proc, and rebinding
//     re-stamps the poster identity.
//
//   devsrv.registry_full
//     The registry caps at SRV_MAX_SERVICES; a post past the cap fails.
//
//   devsrv.post_rollback
//     A handle_alloc failure after srv_reserve rolls the reservation
//     back — the registry is left with no stale entry.
//
// Each test calls srv_registry_reset() first so it starts from an empty
// registry (the harness runs tests sequentially in one address space).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

// Inner SVC handler (non-static; defined in kernel/syscall.c).
extern int sys_post_service_for_proc(struct Proc *p, const char *name,
                                     size_t name_len);

// Test-support registry wipe (non-static; defined in kernel/devsrv.c;
// deliberately not in devsrv.h — no production caller).
extern void srv_registry_reset(void);

void test_devsrv_registered(void);
void test_devsrv_post_gate(void);
void test_devsrv_post_basic(void);
void test_devsrv_tombstone(void);
void test_devsrv_registry_full(void);
void test_devsrv_post_rollback(void);

static struct Proc *make_test_proc(void) {
    return proc_alloc();
}

static struct Proc *make_marked_test_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    proc_mark_may_post_service(p);
    return p;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_devsrv_registered(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('s'), &devsrv,
        "devsrv registered under dc='s'");
    TEST_EXPECT_EQ(dev_lookup_by_name("srv"), &devsrv,
        "devsrv registered under name=\"srv\"");

    struct Spoor *root = devsrv.attach(NULL);
    TEST_ASSERT(root != NULL, "devsrv.attach yields a Spoor");
    TEST_EXPECT_EQ((int)root->qid.type, (int)QTDIR,
        "/srv root Spoor is a directory (QTDIR)");
    TEST_EXPECT_EQ(root->dc, 's', "/srv root Spoor carries dc='s'");
    spoor_clunk(root);
}

void test_devsrv_post_gate(void) {
    srv_registry_reset();

    // Unmarked Proc — refused; nothing registered.
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_post_service_for_proc(p, "corvus", 6), -1,
        "post by an unmarked Proc → -1");
    TEST_EXPECT_EQ(srv_registry_count(), 0,
        "refused post created no registry entry");

    // Marked — but a malformed name is still rejected.
    proc_mark_may_post_service(p);
    TEST_EXPECT_EQ(sys_post_service_for_proc(p, "bad/name", 8), -1,
        "post of a name containing '/' → -1");
    TEST_EXPECT_EQ(sys_post_service_for_proc(p, "", 0), -1,
        "post of an empty name → -1");
    TEST_EXPECT_EQ(srv_registry_count(), 0,
        "rejected malformed-name posts created no entry");

    // Marked + well-formed name — accepted.
    int h = sys_post_service_for_proc(p, "corvus", 6);
    TEST_ASSERT(h >= 0, "post by a marked Proc → handle");
    struct Handle *sh = handle_get(p, h);
    TEST_ASSERT(sh != NULL, "service handle is installed");
    TEST_EXPECT_EQ((int)sh->kind, (int)KOBJ_SRV,
        "service handle is KObj_Srv");
    TEST_EXPECT_EQ(srv_registry_count(), 1, "one service registered");

    drop_test_proc(p);
}

void test_devsrv_post_basic(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");

    int h = sys_post_service_for_proc(p, "corvus", 6);
    TEST_ASSERT(h >= 0, "post \"corvus\" → handle");

    struct SrvService *svc = srv_lookup("corvus", 6);
    TEST_ASSERT(svc != NULL, "srv_lookup finds the entry");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_LIVE, "entry is LIVE");
    TEST_EXPECT_EQ(svc->poster_stripes, proc_stripes(p),
        "entry stamped with the poster's stripes");
    TEST_EXPECT_EQ(svc->poster_pid, p->pid,
        "entry stamped with the poster's pid");

    // A name with a live server cannot be re-posted — by the same Proc...
    TEST_EXPECT_EQ(sys_post_service_for_proc(p, "corvus", 6), -1,
        "re-post of a LIVE name by the poster → -1");
    // ...nor by a different marked Proc.
    struct Proc *q = make_marked_test_proc();
    TEST_ASSERT(q != NULL, "second proc");
    TEST_EXPECT_EQ(sys_post_service_for_proc(q, "corvus", 6), -1,
        "re-post of a LIVE name by another Proc → -1");
    TEST_EXPECT_EQ(srv_registry_count(), 1, "still one service");

    // A distinct name posts independently.
    TEST_ASSERT(sys_post_service_for_proc(p, "janus", 5) >= 0,
        "post of a distinct name → handle");
    TEST_EXPECT_EQ(srv_registry_count(), 2, "two services registered");

    drop_test_proc(q);
    drop_test_proc(p);
}

void test_devsrv_tombstone(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");
    TEST_ASSERT(sys_post_service_for_proc(p, "corvus", 6) >= 0,
        "post \"corvus\"");

    // The poster exits — the kernel tombstones the name.
    srv_proc_exit_notify(p);
    struct SrvService *svc = srv_lookup("corvus", 6);
    TEST_ASSERT(svc != NULL, "tombstoned entry is still present");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "poster exit tombstoned the entry");
    TEST_EXPECT_EQ(svc->poster_stripes, (u64)0,
        "tombstone carries no live poster identity");

    // A tombstoned name is NOT re-postable by an unmarked Proc — the
    // marker is the rebind authority (CORVUS-DESIGN.md §6.1).
    struct Proc *u = make_test_proc();
    TEST_ASSERT(u != NULL, "unmarked proc");
    TEST_EXPECT_EQ(sys_post_service_for_proc(u, "corvus", 6), -1,
        "rebind of a tombstone by an unmarked Proc → -1");
    svc = srv_lookup("corvus", 6);
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "refused rebind left the tombstone intact");

    // A marked Proc may rebind — TOMBSTONED → LIVE, re-stamped.
    struct Proc *p2 = make_marked_test_proc();
    TEST_ASSERT(p2 != NULL, "second marked proc");
    TEST_ASSERT(sys_post_service_for_proc(p2, "corvus", 6) >= 0,
        "rebind of a tombstone by a marked Proc → handle");
    svc = srv_lookup("corvus", 6);
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_LIVE,
        "rebind brought the name back LIVE");
    TEST_EXPECT_EQ(svc->poster_stripes, proc_stripes(p2),
        "rebind re-stamped the new poster's stripes");
    TEST_EXPECT_EQ(srv_registry_count(), 1,
        "rebind reused the slot — still one entry");

    drop_test_proc(p2);
    drop_test_proc(u);
    drop_test_proc(p);
}

void test_devsrv_registry_full(void) {
    srv_registry_reset();

    static const char *const full_names[SRV_MAX_SERVICES] = {
        "srvfull0", "srvfull1", "srvfull2", "srvfull3",
        "srvfull4", "srvfull5", "srvfull6", "srvfull7",
    };

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");

    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        TEST_ASSERT(sys_post_service_for_proc(p, full_names[i], 8) >= 0,
            "post fills the registry up to SRV_MAX_SERVICES");
    }
    TEST_EXPECT_EQ((u32)srv_registry_count(), SRV_MAX_SERVICES,
        "registry is full");

    // One more — past the cap — fails fast.
    TEST_EXPECT_EQ(sys_post_service_for_proc(p, "srvfull8", 8), -1,
        "post past SRV_MAX_SERVICES → -1");

    drop_test_proc(p);
    // Leave the registry clean for the rest of boot.
    srv_registry_reset();
}

void test_devsrv_post_rollback(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");

    // Fill p's handle table so the post's KObj_Srv handle_alloc fails
    // AFTER srv_reserve succeeds — exercising the srv_abort rollback.
    int filled = 0;
    while (handle_alloc(p, KOBJ_PROCESS, RIGHT_READ, NULL) >= 0) filled++;
    TEST_ASSERT(filled > 0, "handle table filled");

    TEST_EXPECT_EQ(sys_post_service_for_proc(p, "corvus", 6), -1,
        "post fails when the handle table is full");
    TEST_EXPECT_EQ(srv_registry_count(), 0,
        "failed post left no stale registry entry (srv_abort rolled back)");

    drop_test_proc(p);
}
