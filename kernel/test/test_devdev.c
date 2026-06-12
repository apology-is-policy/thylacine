// /dev Dev tests (#57b).
//
// Covers registration, walks, the trivial leaves (null/zero/full/random/
// urandom), and -- the load-bearing one -- the I-27 gate-at-namespace-open:
// devdev.open enforces proc_is_console_attached for cons/consctl, so binding
// /dev/cons as a walkable path adds NO ungated console front door.
//
// The mount + cross (reuse-nc through clone_walk_zero) is covered by
// test_namespace_layout (the /dev step).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_devdev_bestiary_smoke(void);
void test_devdev_attach_returns_dir(void);
void test_devdev_walk_to_each_leaf(void);
void test_devdev_walk_unknown_misses(void);
void test_devdev_trivial_leaves(void);
void test_devdev_cons_gate(void);

// =============================================================================
// Helpers.
// =============================================================================

// Walk /dev/<name> (NULL nc = the legacy direct-call shape). Caller spoor_unref's
// the result. Does NOT open -- the gate test drives open() separately.
static struct Spoor *walk_to(const char *name) {
    struct Spoor *root = devdev.attach("");
    if (!root) return NULL;
    const char *names[1] = { name };
    struct Walkqid *wq = devdev.walk(root, NULL, names, 1);
    spoor_unref(root);
    if (!wq) return NULL;
    if (wq->nqid != 1) {
        spoor_unref(wq->spoor);
        walkqid_free(wq);
        return NULL;
    }
    struct Spoor *leaf = wq->spoor;
    walkqid_free(wq);
    return leaf;
}

// =============================================================================
// Tests.
// =============================================================================

void test_devdev_bestiary_smoke(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('d'),     &devdev, "lookup 'd' = devdev");
    TEST_EXPECT_EQ(dev_lookup_by_name("dev"), &devdev, "lookup 'dev' = devdev");
    TEST_EXPECT_EQ(devdev.dc, 'd',                     "devdev.dc = 'd'");
}

void test_devdev_attach_returns_dir(void) {
    struct Spoor *c = devdev.attach("");
    TEST_ASSERT(c != NULL, "attach OK");
    TEST_EXPECT_EQ(c->qid.path, (u64)0, "root qid.path = 0");
    TEST_EXPECT_EQ(c->qid.type, QTDIR, "root QTDIR");
    spoor_unref(c);
}

void test_devdev_walk_to_each_leaf(void) {
    static const char *leaf_names[] = {
        "null", "zero", "full", "random", "urandom", "cons", "consctl",
    };
    for (size_t i = 0; i < sizeof(leaf_names) / sizeof(leaf_names[0]); i++) {
        struct Spoor *leaf = walk_to(leaf_names[i]);
        TEST_ASSERT(leaf != NULL, "walk to leaf succeeds");
        TEST_EXPECT_EQ(leaf->qid.type, QTFILE, "leaf is QTFILE");
        TEST_ASSERT(leaf->qid.path != 0, "leaf path != root");
        spoor_unref(leaf);
    }
}

void test_devdev_walk_unknown_misses(void) {
    struct Spoor *root = devdev.attach("");
    const char *names[1] = { "does-not-exist" };
    struct Walkqid *wq = devdev.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL, "walk allocates");
    TEST_EXPECT_EQ(wq->nqid, 0, "walk to unknown leaf misses");
    spoor_unref(wq->spoor);
    walkqid_free(wq);
    spoor_unref(root);
}

// The trivial leaves are world-rw + UNGATED: these opens succeed regardless of
// console-attach state (the test thread is not console-attached). That implicitly
// proves the gate is leaf-specific (cons/consctl only).
void test_devdev_trivial_leaves(void) {
    char buf[16];

    // /dev/null: read EOF; write consumed.
    struct Spoor *nul = walk_to("null");
    TEST_ASSERT(nul != NULL && devdev.open(nul, 0) != NULL, "open /dev/null (ungated)");
    TEST_EXPECT_EQ(devdev.read(nul, buf, 16, 0), (long)0, "/dev/null read EOF");
    TEST_EXPECT_EQ(devdev.write(nul, "abc", 3, 0), (long)3, "/dev/null consumes write");
    devdev.close(nul); spoor_unref(nul);

    // /dev/zero: read zero-fills; write consumed.
    struct Spoor *zer = walk_to("zero");
    TEST_ASSERT(zer != NULL && devdev.open(zer, 0) != NULL, "open /dev/zero");
    for (int i = 0; i < 16; i++) buf[i] = 0x5a;
    TEST_EXPECT_EQ(devdev.read(zer, buf, 16, 0), (long)16, "/dev/zero reads 16");
    {
        bool all_zero = true;
        for (int i = 0; i < 16; i++) if (buf[i] != 0) all_zero = false;
        TEST_ASSERT(all_zero, "/dev/zero zero-fills");
    }
    TEST_EXPECT_EQ(devdev.write(zer, "x", 1, 0), (long)1, "/dev/zero consumes write");
    devdev.close(zer); spoor_unref(zer);

    // /dev/full: read zero-fills; write FAILS (full disk).
    struct Spoor *ful = walk_to("full");
    TEST_ASSERT(ful != NULL && devdev.open(ful, 0) != NULL, "open /dev/full");
    TEST_EXPECT_EQ(devdev.read(ful, buf, 8, 0), (long)8, "/dev/full reads 8");
    TEST_EXPECT_EQ(devdev.write(ful, "x", 1, 0), (long)-1, "/dev/full write fails");
    devdev.close(ful); spoor_unref(ful);

    // /dev/random: CSPRNG (two reads differ); write consumed.
    struct Spoor *rnd = walk_to("random");
    TEST_ASSERT(rnd != NULL && devdev.open(rnd, 0) != NULL, "open /dev/random");
    char a[16], b[16];
    long ra = devdev.read(rnd, a, 16, 0);
    long rb = devdev.read(rnd, b, 16, 0);
    TEST_ASSERT(ra == 16 && rb == 16, "/dev/random reads 16");
    {
        bool differ = false;
        for (int i = 0; i < 16; i++) if (a[i] != b[i]) differ = true;
        TEST_ASSERT(differ, "/dev/random two reads differ (CSPRNG)");
    }
    TEST_EXPECT_EQ(devdev.write(rnd, "x", 1, 0), (long)1, "/dev/random consumes write");
    devdev.close(rnd); spoor_unref(rnd);

    // /dev/urandom: alias of random.
    struct Spoor *urn = walk_to("urandom");
    TEST_ASSERT(urn != NULL && devdev.open(urn, 0) != NULL, "open /dev/urandom");
    TEST_EXPECT_EQ(devdev.read(urn, buf, 8, 0), (long)8, "/dev/urandom reads 8");
    devdev.close(urn); spoor_unref(urn);
}

// The I-27 gate-at-namespace-open: devdev.open of cons/consctl requires
// proc_is_console_attached -- identical to SYS_CONSOLE_OPEN. Without attach, the
// name resolves (walk) but open FAILS, so binding /dev/cons adds no ungated
// console front door. Results are gathered under controlled attach state, then
// the state is RESTORED before the asserts (a failing assert must never leave the
// test thread wrongly console-attached) -- the devctl kernel-base temp-elevate
// pattern.
void test_devdev_cons_gate(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "test thread has a proc");
    struct Proc *p = t->proc;
    bool saved = proc_is_console_attached(p);

    // DENY: not console-attached -> open of cons/consctl fails.
    proc_revoke_console_attached(p);
    struct Spoor *cons_d = walk_to("cons");
    bool cons_deny = (cons_d != NULL) && (devdev.open(cons_d, 0) == NULL);
    struct Spoor *cc_d = walk_to("consctl");
    bool cc_deny = (cc_d != NULL) && (devdev.open(cc_d, 0) == NULL);

    // ALLOW: console-attached -> open succeeds.
    proc_mark_console_attached(p);
    struct Spoor *cons_a = walk_to("cons");
    struct Spoor *cons_a_open = cons_a ? devdev.open(cons_a, 0) : NULL;
    bool cons_allow = (cons_a_open != NULL);

    // Restore BEFORE asserting (so a failing assert cannot strand the attach bit).
    if (cons_a_open) devdev.close(cons_a);
    if (saved) proc_mark_console_attached(p);
    else       proc_revoke_console_attached(p);

    TEST_ASSERT(cons_d != NULL, "walk /dev/cons resolves the name");
    TEST_ASSERT(cons_deny, "I-27: non-attached open of /dev/cons DENIED");
    TEST_ASSERT(cc_d != NULL, "walk /dev/consctl resolves the name");
    TEST_ASSERT(cc_deny, "I-27: non-attached open of /dev/consctl DENIED");
    TEST_ASSERT(cons_a != NULL, "walk /dev/cons (attached)");
    TEST_ASSERT(cons_allow, "console-attached open of /dev/cons ALLOWED");

    if (cons_d) spoor_unref(cons_d);
    if (cc_d) spoor_unref(cc_d);
    if (cons_a) spoor_unref(cons_a);
}
