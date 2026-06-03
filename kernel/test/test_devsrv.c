// P5-corvus-srv-impl-a2 — kernel-internal tests for the /srv service
// registry, the devsrv Dev, and the create=post path. (stalk-3c retired
// the name-only SYS_POST_SERVICE syscall; posting is now SYS_WALK_CREATE
// on a /srv dir -> devsrv_post_listener, exercised here via post_svc_9p.)
//
// Coverage:
//
//   devsrv.registered
//     devsrv is in the bestiary (dc='s', name="srv"); attach yields a
//     QTDIR /srv root Spoor.
//
//   devsrv.post_gate
//     A create=post is refused for a Proc without PROC_FLAG_MAY_POST_
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
//     A handle_alloc failure after the reserve phase rolls the
//     reservation back — the registry is left with no stale entry.
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

// Test-support registry wipe (non-static; defined in kernel/devsrv.c;
// deliberately not in devsrv.h — no production caller).
extern void srv_registry_reset(void);

void test_devsrv_registered(void);
void test_devsrv_post_gate(void);
void test_devsrv_post_basic(void);
void test_devsrv_tombstone(void);
void test_devsrv_registry_full(void);
void test_devsrv_post_rollback(void);
void test_devsrv_registry_lifecycle(void);
void test_devsrv_svc_ref_holds_registry(void);
void test_devsrv_post_listener(void);

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

// post_svc_9p — post a 9P-mode service into the boot registry via the
// production create=post path (devsrv_post_listener on a transient boot
// /srv root). Replaces the retired SYS_POST_SERVICE name-only entry
// (stalk-3c): the SAME MAY_POST_SERVICE gate + name hygiene + reserve/
// commit two-phase + KObj_Srv listener install. Returns the listener
// handle (>= 0) or -1.
static int post_svc_9p(struct Proc *p, const char *name, size_t name_len) {
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    if (!root) return -1;
    int h = devsrv_post_listener(p, root, name, name_len, SRV_MODE_9P);
    spoor_clunk(root);   // the listener handle's obj is the registry entry,
                         // not the root; the root is transient
    return h;
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
    TEST_EXPECT_EQ(post_svc_9p(p, "corvus", 6), -1,
        "post by an unmarked Proc → -1");
    TEST_EXPECT_EQ(srv_registry_count(), 0,
        "refused post created no registry entry");

    // Marked — but a malformed name is still rejected.
    proc_mark_may_post_service(p);
    TEST_EXPECT_EQ(post_svc_9p(p, "bad/name", 8), -1,
        "post of a name containing '/' → -1");
    TEST_EXPECT_EQ(post_svc_9p(p, "", 0), -1,
        "post of an empty name → -1");
    TEST_EXPECT_EQ(srv_registry_count(), 0,
        "rejected malformed-name posts created no entry");

    // Marked + well-formed name — accepted.
    int h = post_svc_9p(p, "corvus", 6);
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

    int h = post_svc_9p(p, "corvus", 6);
    TEST_ASSERT(h >= 0, "post \"corvus\" → handle");

    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), "corvus", 6);
    TEST_ASSERT(svc != NULL, "srv_lookup finds the entry");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_LIVE, "entry is LIVE");
    TEST_EXPECT_EQ(svc->poster_stripes, proc_stripes(p),
        "entry stamped with the poster's stripes");
    TEST_EXPECT_EQ(svc->poster_pid, p->pid,
        "entry stamped with the poster's pid");

    // A name with a live server cannot be re-posted — by the same Proc...
    TEST_EXPECT_EQ(post_svc_9p(p, "corvus", 6), -1,
        "re-post of a LIVE name by the poster → -1");
    // ...nor by a different marked Proc.
    struct Proc *q = make_marked_test_proc();
    TEST_ASSERT(q != NULL, "second proc");
    TEST_EXPECT_EQ(post_svc_9p(q, "corvus", 6), -1,
        "re-post of a LIVE name by another Proc → -1");
    TEST_EXPECT_EQ(srv_registry_count(), 1, "still one service");

    // A distinct name posts independently.
    TEST_ASSERT(post_svc_9p(p, "janus", 5) >= 0,
        "post of a distinct name → handle");
    TEST_EXPECT_EQ(srv_registry_count(), 2, "two services registered");

    drop_test_proc(q);
    drop_test_proc(p);
}

void test_devsrv_tombstone(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");
    TEST_ASSERT(post_svc_9p(p, "corvus", 6) >= 0,
        "post \"corvus\"");

    // The poster exits — the kernel tombstones the name.
    srv_proc_exit_notify(p);
    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), "corvus", 6);
    TEST_ASSERT(svc != NULL, "tombstoned entry is still present");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "poster exit tombstoned the entry");
    TEST_EXPECT_EQ(svc->poster_stripes, (u64)0,
        "tombstone carries no live poster identity");

    // A tombstoned name is NOT re-postable by an unmarked Proc — the
    // marker is the rebind authority (CORVUS-DESIGN.md §6.1).
    struct Proc *u = make_test_proc();
    TEST_ASSERT(u != NULL, "unmarked proc");
    TEST_EXPECT_EQ(post_svc_9p(u, "corvus", 6), -1,
        "rebind of a tombstone by an unmarked Proc → -1");
    svc = srv_lookup_in(srv_boot_registry(), "corvus", 6);
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "refused rebind left the tombstone intact");

    // A marked Proc may rebind — TOMBSTONED → LIVE, re-stamped.
    struct Proc *p2 = make_marked_test_proc();
    TEST_ASSERT(p2 != NULL, "second marked proc");
    TEST_ASSERT(post_svc_9p(p2, "corvus", 6) >= 0,
        "rebind of a tombstone by a marked Proc → handle");
    svc = srv_lookup_in(srv_boot_registry(), "corvus", 6);
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
        TEST_ASSERT(post_svc_9p(p, full_names[i], 8) >= 0,
            "post fills the registry up to SRV_MAX_SERVICES");
    }
    TEST_EXPECT_EQ((u32)srv_registry_count(), SRV_MAX_SERVICES,
        "registry is full");

    // One more — past the cap — fails fast.
    TEST_EXPECT_EQ(post_svc_9p(p, "srvfull8", 8), -1,
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
    // AFTER the reserve phase succeeds — exercising the srv_abort rollback.
    int filled = 0;
    while (handle_alloc(p, KOBJ_PROCESS, RIGHT_READ, NULL) >= 0) filled++;
    TEST_ASSERT(filled > 0, "handle table filled");

    TEST_EXPECT_EQ(post_svc_9p(p, "corvus", 6), -1,
        "post fails when the handle table is full");
    TEST_EXPECT_EQ(srv_registry_count(), 0,
        "failed post left no stale registry entry (srv_abort rolled back)");

    drop_test_proc(p);
}

// devsrv.registry_lifecycle — the stalk-3a registry-ref crux. A heap
// registry's ref counts the devsrv Spoor INSTANCES that carry aux=reg (the
// attached root + each clone-walk-zero of it), each dropped at devsrv_close
// (the Spoor's last clunk). The last registry ref drains + frees. Proves no
// phantom unref (the normalize-aux discipline in devsrv_walk) and no leak.
void test_devsrv_registry_lifecycle(void) {
    u64 created0   = srv_registry_total_created();
    u64 destroyed0 = srv_registry_total_destroyed();
    u64 sp_alloc0  = spoor_total_allocated();
    u64 sp_freed0  = spoor_total_freed();

    struct SrvRegistry *reg = srv_registry_create();      // create ref = 1
    TEST_ASSERT(reg != NULL, "srv_registry_create");
    TEST_EXPECT_EQ(srv_registry_total_created() - created0, (u64)1,
        "one registry created");

    struct Spoor *root = devsrv_attach_registry(reg);     // reg ref = 2
    TEST_ASSERT(root != NULL, "devsrv_attach_registry");
    TEST_EXPECT_EQ((int)root->qid.type, (int)QTDIR, "attached root is QTDIR");
    TEST_EXPECT_EQ(root->dc, 's', "attached root carries dc='s'");

    // clone_walk_zero (the cross_mounts cross): spoor_clone copies aux=reg
    // (NO reg ref); the 0-element walk takes the clone's OWN reg ref.
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "spoor_clone(root)");
    struct Walkqid *w = devsrv.walk(root, nc, (const char **)0, 0);  // reg ref = 3
    TEST_ASSERT(w != NULL && w->spoor == nc,
        "walk0 returns the clone as a fresh root instance");
    walkqid_free(w);

    // Dropping the clone runs devsrv_close -> srv_registry_unref (3->2);
    // the registry is NOT freed (root + the test's create ref remain).
    spoor_clunk(nc);
    TEST_EXPECT_EQ(srv_registry_total_destroyed() - destroyed0, (u64)0,
        "registry alive after the clone is clunked");

    // Dropping the root (2->1); still alive (the test holds the create ref).
    spoor_clunk(root);
    TEST_EXPECT_EQ(srv_registry_total_destroyed() - destroyed0, (u64)0,
        "registry alive after the root is clunked");

    // The last (create) ref: drain + free. `reg` is INVALID after this.
    srv_registry_unref(reg);
    TEST_EXPECT_EQ(srv_registry_total_destroyed() - destroyed0, (u64)1,
        "registry freed at the last unref");

    // No Spoor leaked: root + nc both allocated + freed (net zero delta).
    TEST_EXPECT_EQ(spoor_total_allocated() - sp_alloc0,
                   spoor_total_freed() - sp_freed0,
        "no Spoor leaked across the registry lifecycle");
}

// devsrv.svc_ref_holds_registry — a /srv/<name> service-ref Spoor + a 2nd
// root over the BOOT registry each take + drop a registry ref via
// devsrv_walk / devsrv_close; the boot registry is NEVER freed by this
// churn (it is immortal — its kproc /srv mount holds a ref forever).
void test_devsrv_svc_ref_holds_registry(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");
    TEST_ASSERT(post_svc_9p(p, "corvus", 6) >= 0, "post corvus");

    u64 destroyed0 = srv_registry_total_destroyed();

    // A 2nd root over the boot registry (boot ref +1).
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    TEST_ASSERT(root != NULL, "attach a 2nd boot root");

    // Walk /corvus on it -> a service-ref Spoor (boot ref +1).
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone root for the service walk");
    const char *names[1] = { "corvus" };
    struct Walkqid *w = devsrv.walk(root, nc, names, 1);
    TEST_ASSERT(w != NULL && w->nqid == 1 && w->spoor == nc,
        "walk /corvus yields a service-ref Spoor");
    walkqid_free(w);

    // Tear down: svc-ref then root. Each devsrv_close drops one boot ref.
    spoor_clunk(nc);
    spoor_clunk(root);

    // The boot registry is unaffected: never freed, still resolvable, still
    // holds the posted service.
    TEST_EXPECT_EQ(srv_registry_total_destroyed() - destroyed0, (u64)0,
        "boot registry not freed by service-ref churn (immortal)");
    TEST_ASSERT(srv_boot_registry() != NULL, "boot registry still present");
    TEST_ASSERT(srv_lookup_in(srv_boot_registry(), "corvus", 6) != NULL,
        "boot registry still holds the posted service");

    drop_test_proc(p);
    srv_registry_reset();
}

// devsrv.post_listener — the stalk-3b create=post MECHANISM. A SYS_WALK_CREATE
// against a /srv directory routes to devsrv_post_listener, which mints a
// KObj_Srv listener in the registry behind that directory Spoor (here the boot
// registry, via a 2nd attached root). Proves: the listener is KObj_Srv + LIVE,
// the mode (9P / byte) is what the caller selected, the MAY_POST_SERVICE gate
// holds on this path too, and the parent must be a registry ROOT (a service-
// ref Spoor is rejected). The handler's perm->mode glue (syscall.c) is thin
// and is exercised end-to-end when a client posts via SYS_WALK_CREATE.
void test_devsrv_post_listener(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");

    // A /srv root over the boot registry -- devsrv_post_listener resolves the
    // registry from this root's aux (the same boot registry srv_lookup_in
    // resolves via srv_boot_registry()).
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    TEST_ASSERT(root != NULL, "attach a boot /srv root");

    // 9P-mode post via the create path.
    int h = devsrv_post_listener(p, root, "corvus", 6, SRV_MODE_9P);
    TEST_ASSERT(h >= 0, "devsrv_post_listener(9P) -> handle");
    struct Handle *sh = handle_get(p, h);
    TEST_ASSERT(sh != NULL, "listener handle installed");
    TEST_EXPECT_EQ((int)sh->kind, (int)KOBJ_SRV, "listener is KObj_Srv");
    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), "corvus", 6);
    TEST_ASSERT(svc != NULL, "service registered in the boot registry");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_LIVE, "service LIVE");
    TEST_EXPECT_EQ((int)svc->mode, (int)SRV_MODE_9P, "service is 9P-mode");
    TEST_EXPECT_EQ(svc->poster_stripes, proc_stripes(p),
        "entry stamped with the poster's stripes");

    // byte-mode post (the DMSRVBYTE arm -> SRV_MODE_BYTE).
    int hb = devsrv_post_listener(p, root, "byter", 5, SRV_MODE_BYTE);
    TEST_ASSERT(hb >= 0, "devsrv_post_listener(BYTE) -> handle");
    struct SrvService *svb = srv_lookup_in(srv_boot_registry(), "byter", 5);
    TEST_ASSERT(svb != NULL, "byte service registered");
    TEST_EXPECT_EQ((int)svb->mode, (int)SRV_MODE_BYTE, "service is byte-mode");

    // The MAY_POST_SERVICE gate holds on the create path: an unmarked Proc
    // cannot post.
    struct Proc *u = make_test_proc();
    TEST_ASSERT(u != NULL, "unmarked proc");
    TEST_EXPECT_EQ(devsrv_post_listener(u, root, "nope", 4, SRV_MODE_9P), -1,
        "create=post by an unmarked Proc -> -1 (MAY_POST_SERVICE gate)");

    // Defense-in-depth: the parent must be a devsrv ROOT (SRV_REGISTRY_MAGIC).
    // A service-ref Spoor (walk /corvus -> DEVSRV_SVC_MAGIC aux) is rejected.
    struct Spoor *svc_ref = spoor_clone(root);
    TEST_ASSERT(svc_ref != NULL, "clone root for the svc-ref walk");
    const char *names[1] = { "corvus" };
    struct Walkqid *w = devsrv.walk(root, svc_ref, names, 1);
    TEST_ASSERT(w != NULL && w->nqid == 1 && w->spoor == svc_ref,
        "walk /corvus -> service-ref Spoor");
    walkqid_free(w);
    TEST_EXPECT_EQ(devsrv_post_listener(p, svc_ref, "x", 1, SRV_MODE_9P), -1,
        "create=post on a non-root (service-ref) Spoor -> -1");
    spoor_clunk(svc_ref);

    spoor_clunk(root);
    drop_test_proc(u);
    drop_test_proc(p);
    srv_registry_reset();
}
