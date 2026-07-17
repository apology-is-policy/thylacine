// Per-Proc environment group + /env device tests (ARCH section 9.7 / G15).
//
// Covers the env.c primitives (set/get, create-idempotent, overwrite+truncate,
// unset + the monotonic-id stale-resolution guarantee, the readdir iterator
// order, the DoS bounds, deep-copy clone independence, NULL-tolerant free) on
// throwaway Procs, plus the devenv Dev structurally (bestiary, the reuse-nc walk
// contract) and an end-to-end walk+read against the CURRENT Proc's env (devenv
// resolves current_thread()->proc->env, cleaned up via env_free).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/env.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_env_set_get(void);
void test_env_create_idempotent(void);
void test_env_overwrite_truncate(void);
void test_env_unset_monotonic(void);
void test_env_iter_order(void);
void test_env_bounds(void);
void test_env_clone_deep_independent(void);
void test_env_free_null_tolerant(void);
void test_devenv_bestiary(void);
void test_devenv_walk_reuse_nc(void);
void test_devenv_walk_read(void);

// proc_alloc gives a fresh Proc with a NULL (empty) env; ZOMBIE + proc_free
// releases it (env_free runs in proc_free). Mirrors test_allowance.c::amk/adrop.
static struct Proc *emk(void) { return proc_alloc(); }
static void edrop(struct Proc *p) { if (!p) return; p->state = PROC_STATE_ZOMBIE; proc_free(p); }

static bool bytes_eq(const char *a, const char *b, long n) {
    for (long i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

void test_env_set_get(void) {
    struct Proc *p = emk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(p->env == NULL, "fresh Proc env is NULL (empty)");
    TEST_EXPECT_EQ(env_lookup(p, "GOROOT", 6), (u64)0, "lookup on empty -> 0");

    u64 id = env_create(p, "GOROOT", 6);
    TEST_ASSERT(id != 0, "create GOROOT");
    TEST_ASSERT(p->env != NULL, "env lazily allocated on create");
    TEST_EXPECT_EQ(env_write(p, id, 0, "/goroot", 7), (long)7, "write 7 bytes");

    char buf[32];
    TEST_EXPECT_EQ(env_read(p, id, 0, buf, sizeof(buf)), (long)7, "read 7 bytes");
    TEST_ASSERT(bytes_eq(buf, "/goroot", 7), "value round-trips");
    TEST_EXPECT_EQ(env_lookup(p, "GOROOT", 6), id, "lookup returns the id");
    TEST_EXPECT_EQ(env_lookup(p, "NOPE", 4), (u64)0, "absent name -> 0");

    // offset-sliced read (os.ReadFile-style advancing offset).
    TEST_EXPECT_EQ(env_read(p, id, 4, buf, sizeof(buf)), (long)3, "read at off 4 -> 3 bytes");
    TEST_ASSERT(bytes_eq(buf, "oot", 3), "tail slice correct");
    TEST_EXPECT_EQ(env_read(p, id, 7, buf, sizeof(buf)), (long)0, "read at EOF -> 0");
    edrop(p);
}

void test_env_create_idempotent(void) {
    struct Proc *p = emk();
    u64 a = env_create(p, "X", 1);
    u64 b = env_create(p, "X", 1);
    TEST_ASSERT(a != 0, "first create");
    TEST_EXPECT_EQ(a, b, "create-or-get: same name -> same id");
    TEST_EXPECT_EQ(p->env->count, 1, "no duplicate slot");
    edrop(p);
}

void test_env_overwrite_truncate(void) {
    struct Proc *p = emk();
    u64 id = env_create(p, "V", 1);
    env_write(p, id, 0, "aaaa", 4);
    char buf[8];
    TEST_EXPECT_EQ(env_read(p, id, 0, buf, sizeof(buf)), (long)4, "len 4");

    // truncate (OTRUNC path) then write a shorter value -- the os.WriteFile flow.
    env_truncate(p, id);
    TEST_EXPECT_EQ(env_read(p, id, 0, buf, sizeof(buf)), (long)0, "truncated -> empty");
    env_write(p, id, 0, "bb", 2);
    TEST_EXPECT_EQ(env_read(p, id, 0, buf, sizeof(buf)), (long)2, "rewritten len 2");
    TEST_ASSERT(bytes_eq(buf, "bb", 2), "new value");
    edrop(p);
}

void test_env_unset_monotonic(void) {
    struct Proc *p = emk();
    u64 id1 = env_create(p, "A", 1);
    env_write(p, id1, 0, "1", 1);
    TEST_ASSERT(env_unset(p, "A", 1), "unset A");
    TEST_EXPECT_EQ(env_lookup(p, "A", 1), (u64)0, "A gone from lookup");
    TEST_EXPECT_EQ(p->env->count, 0, "count back to 0");

    // The net-3d slot-reuse guarantee: a stale id read after unset fails clean.
    char buf[8];
    TEST_EXPECT_EQ(env_read(p, id1, 0, buf, sizeof(buf)), (long)-1, "stale id read -> -1");

    // Re-creating the SAME name mints a NEW id (ids never reused).
    u64 id2 = env_create(p, "A", 1);
    TEST_ASSERT(id2 != 0, "re-create A");
    TEST_ASSERT(id2 != id1, "monotonic: new id != old id");
    // The old id STILL does not resolve (no ABA onto the reused name's slot).
    TEST_EXPECT_EQ(env_read(p, id1, 0, buf, sizeof(buf)), (long)-1, "old id still fails clean");
    edrop(p);
}

void test_env_iter_order(void) {
    struct Proc *p = emk();
    u64 a = env_create(p, "A", 1);
    u64 b = env_create(p, "B", 1);
    u64 c = env_create(p, "C", 1);
    TEST_ASSERT(a < b && b < c, "ids monotonic");

    u64 id = 0; char nm[ENV_NAME_MAX]; u32 nl = 0;
    TEST_ASSERT(env_iter(p, 0, &id, nm, sizeof(nm), &nl), "iter from 0");
    TEST_EXPECT_EQ(id, a, "first entry is the smallest id");
    TEST_ASSERT(nl == 1 && nm[0] == 'A', "name A");
    TEST_ASSERT(env_iter(p, id, &id, nm, sizeof(nm), &nl) && id == b, "next -> B");
    TEST_ASSERT(env_iter(p, id, &id, nm, sizeof(nm), &nl) && id == c, "next -> C");
    TEST_ASSERT(!env_iter(p, id, &id, nm, sizeof(nm), &nl), "no entry past the last (cookie never 0)");

    // Removing the middle entry leaves the iterator monotonic + complete.
    env_unset(p, "B", 1);
    id = 0;
    TEST_ASSERT(env_iter(p, 0, &id, nm, sizeof(nm), &nl) && id == a, "A still first");
    TEST_ASSERT(env_iter(p, id, &id, nm, sizeof(nm), &nl) && id == c, "skips removed B -> C");
    edrop(p);
}

void test_env_bounds(void) {
    struct Proc *p = emk();

    // Name too long (>= ENV_NAME_MAX) -> create refused.
    char longname[ENV_NAME_MAX + 8];
    for (u32 i = 0; i < sizeof(longname); i++) longname[i] = 'x';
    TEST_EXPECT_EQ(env_create(p, longname, ENV_NAME_MAX + 1), (u64)0, "name >= MAX refused");
    TEST_EXPECT_EQ(env_create(p, "", 0), (u64)0, "empty name refused");
    // A name with '/' is refused (no path smuggling).
    TEST_EXPECT_EQ(env_lookup(p, "a/b", 3), (u64)0, "embedded '/' refused");

    // Value over ENV_VALUE_MAX -> write refused (the value stays empty).
    u64 id = env_create(p, "BIG", 3);
    static char huge[ENV_VALUE_MAX + 16];
    for (u32 i = 0; i < sizeof(huge); i++) huge[i] = 'y';
    TEST_EXPECT_EQ(env_write(p, id, 0, huge, ENV_VALUE_MAX + 1), (long)-1, "value > MAX refused");
    char buf[4];
    TEST_EXPECT_EQ(env_read(p, id, 0, buf, sizeof(buf)), (long)0, "rejected write left it empty");
    // A write exactly at the cap succeeds.
    TEST_EXPECT_EQ(env_write(p, id, 0, huge, ENV_VALUE_MAX), (long)ENV_VALUE_MAX, "value == MAX ok");
    // Prove the at-cap value is STORED, not just that the right count was returned
    // (the new_len cast + the copy loops at the exact ENV_VALUE_MAX boundary).
    char tail[2];
    TEST_EXPECT_EQ(env_read(p, id, ENV_VALUE_MAX - 2, tail, 2), (long)2, "at-cap read-back len");
    TEST_ASSERT(tail[0] == 'y' && tail[1] == 'y', "at-cap boundary bytes stored");

    // ENV_MAX_ENTRIES cap: fill, then the next distinct name is refused.
    struct Proc *q = emk();
    char nm[8];
    int made = 0;
    for (int i = 0; i < ENV_MAX_ENTRIES + 4; i++) {
        nm[0] = 'k'; nm[1] = (char)('0' + (i / 100)); nm[2] = (char)('0' + ((i / 10) % 10));
        nm[3] = (char)('0' + (i % 10)); nm[4] = '\0';
        if (env_create(q, nm, 4) != 0) made++;
    }
    TEST_EXPECT_EQ(made, ENV_MAX_ENTRIES, "create caps at ENV_MAX_ENTRIES");
    edrop(q);
    edrop(p);
}

void test_env_clone_deep_independent(void) {
    struct Proc *parent = emk();
    u64 g = env_create(parent, "GOROOT", 6);
    env_write(parent, g, 0, "/goroot", 7);
    u64 h = env_create(parent, "HOME", 4);
    env_write(parent, h, 0, "/root", 5);

    struct Proc *child = emk();
    TEST_EXPECT_EQ(env_clone_into(child, parent), 0, "clone_into ok");
    TEST_ASSERT(child->env != NULL, "child got an env");
    TEST_EXPECT_EQ(child->env->count, 2, "child has both vars");

    // The child resolves the SAME names to (possibly different) ids with the
    // SAME values -- a deep copy.
    u64 cg = env_lookup(child, "GOROOT", 6);
    TEST_ASSERT(cg != 0, "child has GOROOT");
    char buf[16];
    TEST_EXPECT_EQ(env_read(child, cg, 0, buf, sizeof(buf)), (long)7, "child GOROOT len");
    TEST_ASSERT(bytes_eq(buf, "/goroot", 7), "child GOROOT value copied");

    // Independence: mutating the child does not touch the parent (distinct value
    // allocations -- the deep-copy property).
    env_truncate(child, cg);
    env_write(child, cg, 0, "/elsewhere", 10);
    long pr = env_read(parent, g, 0, buf, sizeof(buf));
    TEST_EXPECT_EQ(pr, (long)7, "parent GOROOT unchanged after child mutate");
    TEST_ASSERT(bytes_eq(buf, "/goroot", 7), "parent value intact");

    edrop(child);
    edrop(parent);
}

void test_env_free_null_tolerant(void) {
    struct Proc *p = emk();
    // No env yet: env_free is a no-op (and proc_free's env_free too).
    env_free(p);
    TEST_ASSERT(p->env == NULL, "env_free on NULL env stays NULL");

    // clone from a NULL-env parent leaves the child NULL.
    struct Proc *c = emk();
    TEST_EXPECT_EQ(env_clone_into(c, p), 0, "clone from empty parent ok");
    TEST_ASSERT(c->env == NULL, "child of empty parent is empty");

    // env_free after a set, then a second env_free -> no double-free (p->env nulled).
    env_create(p, "Z", 1);
    TEST_ASSERT(p->env != NULL, "env allocated");
    env_free(p);
    TEST_ASSERT(p->env == NULL, "env_free clears p->env");
    env_free(p);  // must not double-free / extinct
    TEST_ASSERT(p->env == NULL, "second env_free no-ops");
    edrop(c);
    edrop(p);
}

void test_devenv_bestiary(void) {
    TEST_EXPECT_EQ(devenv.dc, 'E', "devenv.dc == 'E'");
    TEST_ASSERT(devenv.perm_enforced == false, "devenv is per-Proc-content, visibility not authority");
    TEST_ASSERT(devenv.attach != NULL && devenv.walk != NULL, "attach/walk present");
    TEST_ASSERT(devenv.read != NULL && devenv.write != NULL, "read/write present");
    TEST_ASSERT(devenv.create != NULL, "create present (SYS_WALK_CREATE)");
    TEST_ASSERT(devenv.readdir != NULL, "readdir present (enumerate)");
    TEST_ASSERT(devenv.unlink != NULL, "unlink present (unset)");
}

void test_devenv_walk_reuse_nc(void) {
    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env root");
    TEST_EXPECT_EQ(root->qid.type, QTDIR, "root is QTDIR");

    // The mount-cross shape: a 0-element walk returns nc unchanged with nqid 0.
    struct Spoor *nc0 = spoor_clone(root);
    TEST_ASSERT(nc0 != NULL, "clone nc (clone_walk_zero analog)");
    struct Walkqid *wq0 = devenv.walk(root, nc0, NULL, 0);
    TEST_ASSERT(wq0 != NULL, "0-element walk allocates");
    TEST_EXPECT_EQ(wq0->spoor, nc0, "reuse-nc: returned spoor IS nc");
    TEST_EXPECT_EQ(wq0->nqid, 0, "0-element walk -> nqid 0");
    TEST_EXPECT_EQ(nc0->qid.path, (u64)0, "nc unchanged at root");
    walkqid_free(wq0);
    spoor_unref(nc0);
    spoor_unref(root);
}

void test_devenv_walk_read(void) {
    // devenv resolves current_thread()->proc->env; populate the current Proc's
    // env, drive the Dev end to end, then clean it back to empty.
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "current proc");
    struct Proc *p = t->proc;
    env_free(p);  // start from a clean env

    u64 id = env_create(p, "TESTVAR", 7);
    TEST_ASSERT(id != 0, "create TESTVAR on the current Proc");
    env_write(p, id, 0, "hi", 2);

    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env root");

    const char *names[1] = { "TESTVAR" };
    struct Walkqid *wq = devenv.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL && wq->nqid == 1, "walk /env/TESTVAR resolves");
    struct Spoor *vf = wq->spoor;
    walkqid_free(wq);
    TEST_EXPECT_EQ(vf->qid.path, id, "value qid.path == the entry id");
    TEST_EXPECT_EQ(vf->qid.type, QTFILE, "value is QTFILE");

    char buf[8];
    TEST_EXPECT_EQ(devenv.read(vf, buf, sizeof(buf), 0), (long)2, "devenv read -> 2 bytes");
    TEST_ASSERT(buf[0] == 'h' && buf[1] == 'i', "value read via the Dev");

    // The directory itself does not byte-read (readdir is its enumeration path).
    TEST_EXPECT_EQ(devenv.read(root, buf, sizeof(buf), 0), (long)-1, "dir read rejected");

    // Create under a value file (a non-root parent) is rejected -- a value file
    // is not a directory, even though env_create resolves names against the env
    // root. Without the guard this would mint a top-level var + return vf.
    TEST_ASSERT(devenv.create(vf, "NOPE", 1, 0644, 0) == NULL, "create under a value file rejected");

    // An absent var misses cleanly.
    const char *gone[1] = { "ABSENT" };
    struct Walkqid *wq2 = devenv.walk(root, NULL, gone, 1);
    TEST_ASSERT(wq2 != NULL && wq2->nqid == 0, "walk absent -> miss (nqid 0)");
    spoor_unref(wq2->spoor);
    walkqid_free(wq2);

    spoor_unref(vf);
    spoor_unref(root);
    env_free(p);  // restore the current Proc to an empty env
}
