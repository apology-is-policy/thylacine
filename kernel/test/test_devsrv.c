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
//   devsrv.service_keys_distinct
//     Each post's node has a qid.path of its own, never the root's: a file
//     MREPLed over /srv/a shows at a alone, and a union at the registry root
//     leaves /srv/b its service node.
//
// Each test calls srv_registry_reset() first so it starts from an empty
// registry (the harness runs tests sequentially in one address space).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/errno.h>
#include <thylacine/srvconn.h>
#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/poll.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/spoor.h>
#include <thylacine/stalk.h>
#include <thylacine/territory.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

// Test-support registry wipe (non-static; defined in kernel/devsrv.c;
// deliberately not in devsrv.h — no production caller).
extern void srv_registry_reset(void);
extern bool srv_test_accept_pin(struct SrvService *svc, int mode);
extern int sys_srv_accept_for_proc(struct Proc *p, hidx_t service_h);
extern int sys_mount_for_proc(struct Proc *p, hidx_t source_fd,
                              struct Spoor *mountpoint, u32 flags);
extern int sys_unmount_for_proc(struct Proc *p, struct Spoor *mountpoint);

void test_devsrv_registered(void);
void test_devsrv_open_root_dir(void);
void test_devsrv_stat_native_root(void);
void test_devsrv_post_gate(void);
void test_devsrv_post_basic(void);
void test_devsrv_tombstone(void);
void test_devsrv_registry_full(void);
void test_devsrv_post_rollback(void);
void test_devsrv_registry_lifecycle(void);
void test_devsrv_svc_ref_holds_registry(void);
void test_devsrv_post_listener(void);
void test_devsrv_service_keys_distinct(void);

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
    int h = devsrv_post_listener(p, root, name, name_len, SRV_MODE_9P, false, false, false);
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

// #957: crossing into /srv (stalk OR single-hop SYS_WALK_OPEN domount) lands on
// the registry root; cd / ls / fs::is_dir need it to OPEN as a plain directory
// (in place), NOT to connect. Pre-fix devsrv.open returned NULL for the root
// (connect-only), so File::open("/srv") failed once crossing reached the root.
void test_devsrv_open_root_dir(void) {
    struct Spoor *root = devsrv.attach(NULL);
    TEST_ASSERT(root != NULL, "devsrv.attach yields a root Spoor");
    struct Spoor *opened = devsrv.open(root, 0 /* OREAD */);
    TEST_EXPECT_EQ(opened, root,
        "devsrv.open(root) opens the registry root in place (dir open, not connect)");
    TEST_ASSERT((root->flag & COPEN) != 0, "the dir open sets COPEN on the root");
    spoor_clunk(root);
}

// #957: the registry root reports as a directory via stat_native so SYS_FSTAT /
// fs::is_dir / cd see a directory. devsrv is system-owned + world-r/x (0555);
// the per-territory /srv visibility (the mount table), not rwx, is the I-1
// boundary. Pre-fix devsrv had no stat_native slot, so the fstat failed.
void test_devsrv_stat_native_root(void) {
    struct Spoor *root = devsrv.attach(NULL);
    TEST_ASSERT(root != NULL, "devsrv.attach yields a root Spoor");
    TEST_ASSERT(devsrv.stat_native != NULL, "devsrv has a stat_native slot");
    struct t_stat st;
    TEST_EXPECT_EQ(devsrv.stat_native(root, &st), 0, "stat_native(root) succeeds");
    TEST_EXPECT_EQ((int)(st.mode & T_S_IFDIR), (int)T_S_IFDIR,
        "registry root mode carries S_IFDIR (a directory)");
    TEST_EXPECT_EQ((int)(st.mode & 0777u), 0555, "registry root mode is 0555");
    TEST_EXPECT_EQ((int)st.uid, (int)PRINCIPAL_SYSTEM, "registry root owned by SYSTEM");
    TEST_EXPECT_EQ((int)(st.qid_type & QTDIR), (int)QTDIR, "registry root qid_type is QTDIR");
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
    struct Handle sh;
    TEST_ASSERT(handle_get(p, h, &sh) == 0, "service handle is installed");
    TEST_EXPECT_EQ((int)sh.kind, (int)KOBJ_SRV,
        "service handle is KObj_Srv");
    handle_put(&sh);
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
    TEST_ASSERT(svc != NULL, "srv_lookup_in finds the entry");
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

// fill_name — "srvfNN" (two decimal digits), so the fill loop scales with
// SRV_MAX_SERVICES instead of a hardcoded name table (the pre-#30 table
// was pinned to 8).
static void fill_name(char out[8], u32 i) {
    out[0] = 's'; out[1] = 'r'; out[2] = 'v'; out[3] = 'f';
    out[4] = (char)('0' + (i / 10u) % 10u);
    out[5] = (char)('0' + i % 10u);
    out[6] = '\0';
}

void test_devsrv_registry_full(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");

    char name[8];
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        fill_name(name, i);
        TEST_ASSERT(post_svc_9p(p, name, 6) >= 0,
            "post fills the registry up to SRV_MAX_SERVICES");
    }
    TEST_EXPECT_EQ((u32)srv_registry_count(), SRV_MAX_SERVICES,
        "registry is full");

    // One more — past the cap — fails fast.
    fill_name(name, SRV_MAX_SERVICES);
    TEST_EXPECT_EQ(post_svc_9p(p, name, 6), -1,
        "post past SRV_MAX_SERVICES → -1");

    drop_test_proc(p);
    // Leave the registry clean for the rest of boot.
    srv_registry_reset();
}

// #30: the at-capacity asymmetry that stranded cora's home. Tombstones
// never free, so at a FULL registry a FRESH name has no slot to claim
// (cora's /srv/home-cora -> -1 -> the pouch bind's EACCES) while a
// TOMBSTONED name still REBINDS in place (michael's /srv/home-michael,
// minted by the boot login-E2E, rebinds forever). Both behaviors are
// deliberate (the tombstone pins its name + slot against stale service
// handles); the capacity must therefore budget one slot per DISTINCT
// per-boot service name -- the SRV_MAX_SERVICES comment carries the
// ledger.
//
// TEST ROLE (G-5 F4): this pins the at-capacity ASYMMETRY SEMANTICS,
// which are capacity-INDEPENDENT -- the loop scales with
// SRV_MAX_SERVICES, so it passes identically at 8 and 16 and is NOT a
// regression guard for the #30 capacity raise. The capacity guard is
// the boot login-E2E cora leg (usr/joey/joey.c::do_login_e2e): a fresh
// /srv/home-<user> posted past the boot's accumulated posts +
// tombstones fails the BOOT if the registry is full.
void test_devsrv_registry_full_tombstone_rebinds(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");

    char name[8];
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        fill_name(name, i);
        TEST_ASSERT(post_svc_9p(p, name, 6) >= 0,
            "post fills the registry up to SRV_MAX_SERVICES");
    }

    // The poster dies: every entry tombstones; the registry stays FULL
    // (tombstones pin their slots).
    srv_proc_exit_notify(p);
    TEST_EXPECT_EQ((u32)srv_registry_count(), SRV_MAX_SERVICES,
        "poster exit tombstoned in place — the registry is still full");

    struct Proc *p2 = make_marked_test_proc();
    TEST_ASSERT(p2 != NULL, "second marked proc");

    // The cora shape: a FRESH name at a full registry fails fast.
    TEST_EXPECT_EQ(post_svc_9p(p2, "srvnew", 6), -1,
        "fresh name at a full registry → -1 (no free slot)");

    // The michael shape: a TOMBSTONED name rebinds without a free slot.
    fill_name(name, 0);
    TEST_ASSERT(post_svc_9p(p2, name, 6) >= 0,
        "tombstone rebind at a full registry → handle (slot reused)");
    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), name, 6);
    TEST_ASSERT(svc != NULL, "rebound entry present");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_LIVE,
        "rebind brought the tombstoned name back LIVE at full capacity");

    drop_test_proc(p2);
    drop_test_proc(p);
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

    TEST_EXPECT_EQ(post_svc_9p(p, "corvus", 6), -T_E_MFILE,
        "post fails when the handle table is full");
    TEST_EXPECT_EQ(srv_registry_count(), 0,
        "failed post left no stale registry entry (srv_abort rolled back)");

    // The same failure on a mortal registry must undo the listener's
    // provisional retain as well as its reserved name.
    u64 destroyed0 = srv_registry_total_destroyed();
    struct SrvRegistry *reg = srv_registry_create();
    TEST_ASSERT(reg != NULL, "rollback mortal registry");
    struct Spoor *root = devsrv_attach_registry(reg);
    TEST_ASSERT(root != NULL, "rollback registry root");
    TEST_EXPECT_EQ(devsrv_post_listener(p, root, "rollback", 8,
                                       SRV_MODE_BYTE, false, false, false), -T_E_MFILE,
        "mortal post fails at handle allocation");
    TEST_EXPECT_EQ(srv_lookup_in(reg, "rollback", 8), NULL,
        "failed post returns mortal slot to FREE");
    spoor_clunk(root);
    srv_registry_unref(reg);
    TEST_EXPECT_EQ(srv_registry_total_destroyed() - destroyed0, (u64)1,
        "failed listener allocation leaks no registry ref");
    drop_test_proc(p);
}

// devsrv.registry_lifecycle — the stalk-3a registry-ref crux. A heap
// registry's ref counts the devsrv Spoor INSTANCES that carry aux=reg (the
// attached root + each clone-walk-zero of it), each dropped at devsrv_close
// (the Spoor's last clunk). The last registry ref drains + frees. Proves no
// phantom unref (the normalize-aux discipline in devsrv_walk) and no leak.
static void test_session_registry(void);

void test_devsrv_registry_lifecycle(void) {
    test_session_registry();
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

    // A listener pins its containing registry independently of the namespace.
    // Then model poll's precise retain/register/close/sweep/put schedule: a
    // peer closes the table slot while its waiter is still registered.
    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "mortal listener poster");
    reg = srv_registry_create();
    TEST_ASSERT(reg != NULL, "listener registry");
    root = devsrv_attach_registry(reg);
    TEST_ASSERT(root != NULL, "listener registry root");
    int h = devsrv_post_listener(p, root, "pinned", 6,
                                SRV_MODE_BYTE, false, false, false);
    TEST_ASSERT(h >= 0, "post listener in mortal registry");
    spoor_clunk(root);
    srv_registry_unref(reg);
    TEST_ASSERT(srv_registry_total_destroyed() - destroyed0 == 1,
        "listener survives removal of every namespace/creator ref");

    struct Handle held;
    TEST_ASSERT(handle_get(p, h, &held) == 0, "retain listener snapshot");
    struct Rendez rendez = {0};
    struct poll_waiter waiter;
    poll_waiter_init(&waiter, &rendez);
    TEST_EXPECT_EQ(srv_handle_poll(held.obj, POLLIN, &waiter), 0,
        "empty listener registers without readiness");
    TEST_ASSERT(waiter.list != NULL, "listener waiter registered");
    TEST_EXPECT_EQ(handle_close(p, h), 0, "peer closes table slot");
    TEST_ASSERT(srv_registry_total_destroyed() - destroyed0 == 1,
        "retained poll snapshot keeps registry and waiter list alive");
    TEST_EXPECT_EQ(srv_handle_poll(held.obj, POLLIN, NULL), 0,
        "close does not unpost or tombstone the service");
    poll_waiter_list_unregister(&waiter);
    TEST_EXPECT_EQ(waiter.list, NULL, "sweep removes the embedded-list hook");
    srv_proc_exit_notify(p); // membership drops, poll snapshot still pins reg
    TEST_EXPECT_EQ(srv_registry_total_destroyed() - destroyed0, (u64)1,
        "death cannot free a retained poll snapshot");
    handle_put(&held);
    TEST_EXPECT_EQ(srv_registry_total_destroyed() - destroyed0, (u64)2,
        "last snapshot frees mortal registry after poll sweep");
    drop_test_proc(p);
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
    int h = devsrv_post_listener(p, root, "corvus", 6, SRV_MODE_9P, false, false, false);
    TEST_ASSERT(h >= 0, "devsrv_post_listener(9P) -> handle");
    struct Handle sh;
    TEST_ASSERT(handle_get(p, h, &sh) == 0, "listener handle installed");
    TEST_EXPECT_EQ((int)sh.kind, (int)KOBJ_SRV, "listener is KObj_Srv");
    handle_put(&sh);
    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), "corvus", 6);
    TEST_ASSERT(svc != NULL, "service registered in the boot registry");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_LIVE, "service LIVE");
    TEST_EXPECT_EQ((int)svc->mode, (int)SRV_MODE_9P, "service is 9P-mode");
    TEST_EXPECT_EQ(svc->poster_stripes, proc_stripes(p),
        "entry stamped with the poster's stripes");

    // byte-mode post (the DMSRVBYTE arm -> SRV_MODE_BYTE).
    int hb = devsrv_post_listener(p, root, "byter", 5, SRV_MODE_BYTE, false, false, false);
    TEST_ASSERT(hb >= 0, "devsrv_post_listener(BYTE) -> handle");
    struct SrvService *svb = srv_lookup_in(srv_boot_registry(), "byter", 5);
    TEST_ASSERT(svb != NULL, "byte service registered");
    TEST_EXPECT_EQ((int)svb->mode, (int)SRV_MODE_BYTE, "service is byte-mode");

    // The MAY_POST_SERVICE gate holds on the create path: an unmarked Proc
    // cannot post.
    struct Proc *u = make_test_proc();
    TEST_ASSERT(u != NULL, "unmarked proc");
    TEST_EXPECT_EQ(devsrv_post_listener(u, root, "nope", 4, SRV_MODE_9P, false, false, false), -1,
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
    TEST_EXPECT_EQ(devsrv_post_listener(p, svc_ref, "x", 1, SRV_MODE_9P, false, false, false), -1,
        "create=post on a non-root (service-ref) Spoor -> -1");
    spoor_clunk(svc_ref);

    spoor_clunk(root);
    drop_test_proc(u);
    drop_test_proc(p);
    srv_registry_reset();
}


void test_devsrv_cap_post_bounds(void);
void test_devsrv_cap_post_bounds(void) {
    srv_registry_reset();
    struct Proc *p = make_test_proc(), *q = make_test_proc();
    struct Proc *r = make_test_proc(), *s = make_test_proc();
    struct Proc *tcb = make_marked_test_proc();
    TEST_ASSERT(p && q && r && s && tcb, "allocate posters");
    p->caps |= CAP_POST_SERVICE; q->caps |= CAP_POST_SERVICE;
    r->caps |= CAP_POST_SERVICE; s->caps |= CAP_POST_SERVICE;
    // Two children of the SAME scope cannot multiply its budget by forking.
    p->legate_scope_id = q->legate_scope_id = 0x1234;
    r->legate_scope_id = 0x5678;
    s->legate_scope_id = 0x9abc;
    TEST_EXPECT_EQ(proc_may_post_service(p), false, "cap does not confer TCB role");
    TEST_ASSERT(post_svc_9p(p, "cap-a", 5) >= 0, "cap permits post");
    TEST_ASSERT(post_svc_9p(q, "cap-b", 5) >= 0, "second post in scope");
    TEST_EXPECT_EQ(post_svc_9p(q, "cap-c", 5), -1, "scope budget spans posters");
    TEST_ASSERT(post_svc_9p(r, "cap-c", 5) >= 0, "another scope post");
    TEST_ASSERT(post_svc_9p(r, "cap-d", 5) >= 0, "fourth global cap slot");
    TEST_EXPECT_EQ(post_svc_9p(s, "cap-e", 5), -1, "global cap slots bounded");
    TEST_ASSERT(post_svc_9p(tcb, "trusted", 7) >= 0, "TCB can post at cap limit");
    srv_proc_exit_notify(tcb);
    TEST_EXPECT_EQ(post_svc_9p(s, "trusted", 7), -1, "cannot capture TCB tombstone");
    struct SrvService *old = srv_lookup_in(srv_boot_registry(), "cap-a", 5);
    TEST_ASSERT(old != NULL, "find original slot");
    u64 generation = old->generation;
    // Model the exit-notify window with an accepter still holding its pin.
    (void)srv_test_accept_pin(old, 1);
    srv_proc_exit_notify(p);
    int busy_recycle = post_svc_9p(s, "cap-e", 5);
    int busy_rebind = post_svc_9p(s, "cap-a", 5);
    (void)srv_test_accept_pin(old, 0);
    TEST_EXPECT_EQ(busy_recycle, -1, "busy tombstone cannot recycle");
    TEST_EXPECT_EQ(busy_rebind, -1, "busy tombstone cannot rebind");
    TEST_ASSERT(post_svc_9p(s, "cap-e", 5) >= 0, "new name recycles dead cap slot");
    TEST_EXPECT_EQ(old->generation, generation + 1, "recycle changes generation");
    TEST_ASSERT(srv_lookup_in(srv_boot_registry(), "cap-a", 5) == NULL,
                "old name cannot reach recycled service");
    TEST_EXPECT_EQ(srv_registry_count(), 5, "recycling adds no permanent slot");
    srv_proc_exit_notify(q); srv_proc_exit_notify(r); srv_proc_exit_notify(s);
    p->legate_scope_id = q->legate_scope_id = r->legate_scope_id = s->legate_scope_id = 0;
    drop_test_proc(p); drop_test_proc(q); drop_test_proc(r); drop_test_proc(s); drop_test_proc(tcb);
    srv_registry_reset();
}


static struct SrvService *g_accept_service;
static u64 g_accept_owner;
static struct SrvConn *g_accept_result;
static volatile bool g_accept_exited;

static bool test_accept_pinned(struct SrvService *svc) {
    return srv_test_accept_pin(svc, -1);
}

static bool test_accept_sleeping(struct SrvService *svc) {
    irq_state_t irq = spin_lock_irqsave(&svc->accept_rendez.lock);
    bool sleeping = svc->accept_rendez.waiter != NULL;
    spin_unlock_irqrestore(&svc->accept_rendez.lock, irq);
    return sleeping;
}

static void test_accept_worker(void) {
    g_accept_result = srv_accept_blocking(g_accept_service, g_accept_owner);
    test_kthread_park_terminal(&g_accept_exited);
}

void test_devsrv_accept_lifetime(void);
void test_devsrv_accept_lifetime(void) {
    srv_registry_reset();
    struct Proc *p = make_marked_test_proc(), *q = make_marked_test_proc();
    TEST_ASSERT(p && q, "allocate accept owners");
    int listener = post_svc_9p(p, "accept", 6);
    TEST_ASSERT(listener >= 0, "post accept service");
    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), "accept", 6);
    TEST_ASSERT(svc != NULL, "find accept service");
    TEST_ASSERT(srv_accept_blocking(svc, proc_stripes(q)) == NULL,
                "foreign owner cannot accept");
    g_accept_service = svc;
    g_accept_owner = proc_stripes(p);
    g_accept_result = NULL;
    g_accept_exited = false;
    struct Thread *worker = thread_create(kproc(), test_accept_worker);
    TEST_ASSERT(worker != NULL, "allocate accept worker");
    ready(worker);
    TEST_YIELD_UNTIL_SOFT(test_accept_sleeping(svc));
    bool pinned = test_accept_sleeping(svc) && test_accept_pinned(svc);
    bool refused = pinned && srv_accept_blocking(svc, proc_stripes(p)) == NULL;
    // Cleanup before assertions: the actual blocked accepter must observe EOF
    // and release its pin even when an observation above fails.
    srv_proc_exit_notify(p);
    test_kthread_join_free(worker, &g_accept_exited);
    bool released = !test_accept_pinned(svc);
    bool ended = g_accept_result == NULL;
    int rebound = post_svc_9p(q, "accept", 6);
    bool stale_refused = rebound >= 0 && sys_srv_accept_for_proc(p, (hidx_t)listener) < 0;
    srv_proc_exit_notify(q);
    drop_test_proc(p); drop_test_proc(q);
    srv_registry_reset();
    TEST_ASSERT(pinned && refused, "second concurrent accept fails without a second Rendez waiter");
    TEST_ASSERT(released && ended, "tombstone wakes and releases the old accepter");
    TEST_ASSERT(rebound >= 0 && stale_refused, "reuse succeeds only for the new owner");
}

// A devnone Spoor of the given type in `p`'s handle table (RIGHT_READ; the
// table owns the ref).
static hidx_t install_source(struct Proc *p, u8 type, u64 qid_path) {
    struct Spoor *s = spoor_alloc(&devnone);
    if (!s) return -1;
    s->qid.type = type;
    s->qid.path = qid_path;
    return handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, s);
}

// devsrv.service_keys_distinct -- the mount key is (dc, devno, qid.path), and
// a service node shares the registry root's dc and devno. When every node's
// path was the root's 0, a file MREPLed over /srv/a was keyed at the root and
// at every other service too: /srv crossed into the file and /srv/b answered
// ENOTDIR, and a union at the root made every service node a directory.
void test_devsrv_service_keys_distinct(void) {
    srv_registry_reset();

    struct Proc *p = make_marked_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc + mark");
    p->territory = territory_alloc();
    TEST_ASSERT(p->territory != NULL, "territory_alloc");
    TEST_ASSERT(post_svc_9p(p, "a", 1) >= 0, "post a");
    TEST_ASSERT(post_svc_9p(p, "b", 1) >= 0, "post b");

    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    TEST_ASSERT(root != NULL, "attach the boot registry");
    struct Spoor *a  = stalk(p, root, "a", 1, STALK_MOUNT, 0);
    struct Spoor *a2 = stalk(p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *b  = stalk(p, root, "b", 1, STALK_WALK, 0);
    TEST_ASSERT(a != NULL && a2 != NULL && b != NULL, "walk a twice and b");
    TEST_EXPECT_NE((u64)a->qid.path, (u64)root->qid.path,
        "a service node's path is not the registry root's");
    TEST_EXPECT_NE((u64)a->qid.path, (u64)b->qid.path,
        "two posts' nodes have different paths");
    TEST_EXPECT_EQ((u64)a2->qid.path, (u64)a->qid.path,
        "one post keeps its path across walks");
    spoor_clunk(a2);

    // The identities above are the fix; the mount TABLE is what a shared key
    // would break. mount_is_point_id keys on (dc, devno, qid.path) -- exactly
    // what stalk crossing consults -- so it discriminates without crossing (a
    // devnone mount source cannot be crossed; a raw registry root as base
    // resolves member 0 only). Pre-fix a, b and the root share the key 0.
    hidx_t ffd = install_source(p, QTFILE, 0x5au);
    TEST_ASSERT(ffd >= 0, "install a file source");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd, a, MREPL), 0,
        "MREPL a file over /srv/a");
    TEST_ASSERT(mount_is_point_id(p->territory, a->dc, a->devno, a->qid.path),
        "the entry is keyed at /srv/a");
    TEST_ASSERT(!mount_is_point_id(p->territory, b->dc, b->devno, b->qid.path),
        "no entry is keyed at /srv/b (pre-fix b shared a's key)");
    TEST_ASSERT(!mount_is_point_id(p->territory, root->dc, root->devno,
                                   root->qid.path),
        "no entry is keyed at the registry root (pre-fix a was keyed there)");
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, a), 0, "unmount /srv/a");
    spoor_clunk(a);

    // A directory MBEFORE at the registry root starts a union at the ROOT key.
    hidx_t dfd = install_source(p, QTDIR, 0x5bu);
    TEST_ASSERT(dfd >= 0, "install a directory source");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, dfd, root, MBEFORE), 0,
        "MBEFORE a directory at the registry root");
    TEST_ASSERT(mount_is_point_id(p->territory, root->dc, root->devno,
                                  root->qid.path),
        "the union is keyed at the registry root");
    TEST_ASSERT(!mount_is_point_id(p->territory, b->dc, b->devno, b->qid.path),
        "the union is not keyed at /srv/b (pre-fix it was)");
    TEST_ASSERT(mount_member_at(p->territory, b, 0, NULL) == NULL,
        "/srv/b hosts no mount member");
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, root), 0, "unmount the root's union");
    spoor_clunk(b);

    spoor_clunk(root);
    drop_test_proc(p);
    srv_registry_reset();
}

extern s64 sys_srv_registry_new_for_proc(struct Proc *, u64, const struct srv_route *, u64, u64);
extern int spawn_perm_grant_check(struct Proc *, u32);

static struct Spoor *session_leaf(struct Spoor *root, const char *name) {
    struct Spoor *c = spoor_clone(root);
    if (!c) return NULL;
    struct Walkqid *w = devsrv.walk(root, c, &name, 1);
    if (!w) { spoor_clunk(c); return NULL; }
    walkqid_free(w);
    return c;
}

static void test_session_registry(void) {
    srv_registry_reset();
    struct Proc *p = make_marked_test_proc();
    struct Proc *q = make_marked_test_proc();
    TEST_ASSERT(p && q, "session factory procs");
    struct Spoor *boot = devsrv_attach_registry(srv_boot_registry());
    TEST_ASSERT(boot, "boot source");
    boot->flag |= CWALKONLY;
    int fd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, boot);
    TEST_ASSERT(fd >= 0, "source fd");
    struct srv_route r = {.name_len = 8, .name = "resident"};
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, &r, 1, 0), -T_E_ACCES,
        "posting authority alone cannot mint quota domains");
    TEST_EXPECT_EQ(spawn_perm_grant_check(p, SPAWN_PERM_SESSION_REGISTRY), -1,
        "poster cannot delegate factory");
    proc_mark_session_registry(p);
    TEST_EXPECT_EQ(spawn_perm_grant_check(p, SPAWN_PERM_SESSION_REGISTRY), 0,
        "factory holder can explicitly delegate");
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, ~0ULL, &r, 1, 0), -T_E_BADF, "bad fd");
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, &r, 1, 1), -T_E_INVAL, "reserved flags");
    r.reserved = 1;
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, &r, 1, 0), -T_E_INVAL, "reserved route bytes");
    r.reserved = 0;
    struct srv_route bad = r;
    bad.name[31] = 'x';
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, &bad, 1, 0), -T_E_INVAL, "nonzero unused bytes");
    bad = (struct srv_route){.name_len = 1, .name = "."};
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, &bad, 1, 0), -T_E_INVAL, "dot route refused");
    bad = (struct srv_route){.name_len = 2, .name = ".."};
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, &bad, 1, 0), -T_E_INVAL, "parent route refused");
    bad = (struct srv_route){.name_len = 3, .name = "a/b"};
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, &bad, 1, 0), -T_E_INVAL, "separator route refused");
    struct srv_route duplicate[2] = {r, r};
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, duplicate, 2, 0), -T_E_INVAL, "duplicate routes");
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, NULL, 17, 0), -T_E_INVAL, "manifest bounded before dereference");
    // Sixteen simultaneously retained roots, then repeated construction well
    // beyond that bound. Retired domains must actually return their tickets.
    int roots[SRV_MAX_DOMAINS];
    for (u32 i = 0; i < SRV_MAX_DOMAINS; i++) {
        roots[i] = sys_srv_registry_new_for_proc(p, fd, NULL, 0, 0);
        TEST_ASSERT(roots[i] >= 0, "empty manifest root");
    }
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, fd, NULL, 0, 0), -T_E_NOSPC, "retained root bound");
    for (u32 i = 0; i < SRV_MAX_DOMAINS; i++) handle_close(p, roots[i]);
    for (u32 i = 0; i < 40; i++) {
        int fresh = sys_srv_registry_new_for_proc(p, fd, &r, 1, 0);
        TEST_ASSERT(fresh >= 0, "retired domains do not accumulate");
        handle_close(p, fresh);
    }
    int a = sys_srv_registry_new_for_proc(p, fd, &r, 1, 0);
    int b = sys_srv_registry_new_for_proc(p, fd, &r, 1, 0);
    TEST_ASSERT(a >= 0 && b >= 0, "two private views");
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(p, a, &r, 1, 0), -T_E_ACCES,
        "private root cannot create nested quota domains");
    struct Handle ah, bh;
    TEST_ASSERT(handle_get(p, a, &ah) == 0 && handle_get(p, b, &bh) == 0, "hold roots");
    struct Spoor *ar = ah.obj, *br = bh.obj;
    TEST_ASSERT(ar->devno != br->devno, "mount identities independent");
    TEST_ASSERT(p->territory == NULL, "fixture begins without territory");
    p->territory = territory_alloc();
    TEST_ASSERT(p->territory, "session mount territory");
    int point_fd = install_source(p, QTDIR, 0x7171);
    struct Handle point_h;
    TEST_ASSERT(point_fd >= 0 && handle_get(p, point_fd, &point_h) == 0, "mountpoint fixture");
    struct Spoor *point = point_h.obj;
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd, point, MREPL), 0, "initial boot /srv mount");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, a, point, MREPL), 0, "session replaces boot group");
    struct Spoor *member = mount_member_at(p->territory, point, 0, NULL);
    TEST_ASSERT(member && member->devno == ar->devno, "only private registry visible");
    spoor_clunk(member);
    TEST_EXPECT_EQ(mount_member_at(p->territory, point, 1, NULL), NULL, "no boot union tail");
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, point), 0, "remove private registry");
    TEST_EXPECT_EQ(mount_member_at(p->territory, point, 0, NULL), NULL, "unmount cannot reveal boot alias");
    handle_put(&point_h);

    TEST_ASSERT(devsrv_post_listener(p, ar, "local", 5, SRV_MODE_BYTE, false, false, false) >= 0,
        "post first session");
    TEST_ASSERT(devsrv_post_listener(p, br, "local", 5, SRV_MODE_BYTE, false, false, false) >= 0,
        "same name second session does not collide");
    TEST_EXPECT_EQ(devsrv_post_listener(p, ar, "resident", 8, SRV_MODE_BYTE, false, false, false), -1,
        "offline route cannot be shadowed by trusted poster");
    struct Spoor *leaf = session_leaf(ar, "resident");
    TEST_ASSERT(leaf, "offline route reserves stable leaf");
    TEST_EXPECT_EQ(devsrv_open_connect(p, leaf, 0), NULL, "offline route fails connect");
    int listener = devsrv_post_listener(q, boot, "resident", 8, SRV_MODE_BYTE, false, false, false);
    TEST_ASSERT(listener >= 0, "resident starts after view creation");
    TEST_EXPECT_EQ(devsrv_open_connect(p, leaf, 0), NULL, "route does not bypass D8");
    p->caps |= CAP_TCB_DIAL;
    struct Spoor *conn = devsrv_open_connect(p, leaf, 0);
    TEST_ASSERT(conn, "trusted dial through route");
    struct SrvConn *cn = devsrv_conn_of(conn);
    TEST_ASSERT(cn && cn->domain, "routed connection charged to requesting session");
    u32 local, total, all, domains;
    srv_domain_counts(cn->domain, &local, &total, &all, &domains);
    TEST_EXPECT_EQ(local, 1u, "one routed ticket");
    srv_proc_exit_notify(q);
    TEST_EXPECT_EQ(devsrv_open_connect(p, leaf, 0), NULL, "route goes offline on provider death");
    TEST_EXPECT_EQ(devsrv_post_listener(q, boot, "late", 4, SRV_MODE_BYTE, false, false, false), -T_E_ACCES,
        "closed poster cannot republish after death notification");
    struct Proc *restart = make_marked_test_proc();
    TEST_ASSERT(restart, "restart provider");
    TEST_ASSERT(devsrv_post_listener(restart, boot, "resident", 8, SRV_MODE_BYTE, false, false, false) >= 0,
        "trusted provider restarts");
    struct Spoor *again = devsrv_open_connect(p, leaf, 0);
    TEST_ASSERT(again, "old route sees trusted restart");
    spoor_clunk(again);
    spoor_clunk(leaf);
    handle_put(&ah); handle_put(&bh);
    drop_test_proc(restart); drop_test_proc(q); drop_test_proc(p);
    srv_domain_counts(cn->domain, &local, &total, &all, &domains);
    TEST_EXPECT_EQ(domains, 1u, "retained connection keeps domain but not registry");
    spoor_clunk(conn);
    srv_domain_counts(NULL, NULL, NULL, NULL, &domains);
    TEST_EXPECT_EQ(domains, 0u, "last retained endpoint returns domain slot");
    struct Proc *full = make_marked_test_proc();
    TEST_ASSERT(full, "full fd fixture");
    proc_mark_session_registry(full);
    struct Spoor *source = devsrv_attach_registry(srv_boot_registry());
    TEST_ASSERT(source, "full fd source");
    source->flag |= CWALKONLY;
    int source_fd = handle_alloc(full, KOBJ_SPOOR, RIGHT_READ, source);
    TEST_ASSERT(source_fd >= 0, "full fd source installed");
    int wrong = handle_alloc(full, KOBJ_PROCESS, RIGHT_READ, NULL);
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(full, wrong, NULL, 0, 0), -T_E_INVAL, "wrong handle kind");
    while (handle_alloc(full, KOBJ_PROCESS, RIGHT_READ, NULL) >= 0) {}
    TEST_EXPECT_EQ(sys_srv_registry_new_for_proc(full, source_fd, &r, 1, 0), -T_E_MFILE, "root install rollback");
    srv_domain_counts(NULL, NULL, NULL, NULL, &domains);
    TEST_EXPECT_EQ(domains, 0u, "failed fd install returns domain ticket");
    drop_test_proc(full);
    // A route is policy for a resident, not an alias for any boot post.
    struct Proc *factory = make_marked_test_proc();
    struct Proc *cap_only = make_test_proc();
    TEST_ASSERT(factory && cap_only, "cap-posted route fixtures");
    proc_mark_session_registry(factory);
    cap_only->caps |= CAP_POST_SERVICE;
    source = devsrv_attach_registry(srv_boot_registry());
    TEST_ASSERT(source, "cap route boot source");
    source->flag |= CWALKONLY;
    source_fd = handle_alloc(factory, KOBJ_SPOOR, RIGHT_READ, source);
    struct srv_route cap_route = {.name_len = 8, .name = "userboot"};
    int view_fd = sys_srv_registry_new_for_proc(factory, source_fd, &cap_route, 1, 0);
    TEST_ASSERT(view_fd >= 0, "view with capability-posted source name");
    struct Handle view_h;
    TEST_ASSERT(handle_get(factory, view_fd, &view_h) == 0, "retain route view");
    leaf = session_leaf(view_h.obj, "userboot");
    TEST_ASSERT(leaf, "reserved capability route leaf");
    TEST_ASSERT(devsrv_post_listener(cap_only, source, "userboot", 8,
        SRV_MODE_BYTE, false, false, false) >= 0, "capability can post separate boot fixture");
    factory->caps |= CAP_TCB_DIAL;
    TEST_EXPECT_EQ(devsrv_open_connect(factory, leaf, 0), NULL,
        "resident route refuses cap-posted source even for trusted dialer");
    spoor_clunk(leaf); handle_put(&view_h);
    drop_test_proc(cap_only); drop_test_proc(factory);
    srv_domain_counts(NULL, NULL, NULL, NULL, &domains);
    TEST_EXPECT_EQ(domains, 0u, "refused route retains no domain charge");
    srv_registry_reset();
}
