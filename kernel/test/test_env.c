// Per-Proc environment group + /env device tests (ARCH section 9.7 / G15).
//
// Covers the env.c primitives (set/get, create-idempotent, overwrite+truncate,
// unset + the monotonic-id stale-resolution guarantee, the readdir iterator
// order, the DoS bounds, deep-copy clone independence, NULL-tolerant free) on
// throwaway Procs, plus the devenv Dev structurally (bestiary, the reuse-nc walk
// contract) and an end-to-end walk+read against the CURRENT Proc's env (the
// directory resolves current_thread()->proc->env, cleaned up via env_free), and
// the open-handle binding: a handle acts on the Env it was opened in.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/env.h>
#include <thylacine/errno.h>
#include <thylacine/exec.h>      // EXEC_USER_BURROW_BASE
#include <thylacine/handle.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>   // V-4b-5: struct t_stat + T_S_IF*
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vivarium.h>  // VIV_PROT_* (the MAP_FIXED prot word)
#include <thylacine/vma.h>

void test_env_set_get(void);
void test_env_create_idempotent(void);
void test_env_overwrite_truncate(void);
void test_env_unset_monotonic(void);
void test_env_iter_order(void);
void test_env_bounds(void);
void test_env_render_environ(void);              // VIVARIUM V-4b-6
void test_env_render_environ_eq_in_name(void);   // VIVARIUM V-4b-6
void test_env_clone_deep_independent(void);
void test_env_free_null_tolerant(void);
void test_devenv_bestiary(void);
void test_devenv_walk_reuse_nc(void);
void test_devenv_walk_read(void);
void test_devenv_readdir_type_byte(void);
void test_devenv_stat_native_shapes(void);
void test_devenv_open_binds_opener(void);
void test_devenv_create_binds_creator(void);
void test_devenv_unopened_id_needs_its_env(void);
void test_devenv_unlink_answers_its_errno(void);
void test_devenv_entry_backs_no_mapping(void);

// Non-static in syscall.c but header-less.
extern s64 sys_mmap_file_for_proc(struct Proc *p, u64 fd_raw, u64 length_raw,
                                  bool exec, u64 offset);
extern s64 sys_mmap_fixed_file_for_proc(struct Proc *p, u64 addr, u64 fd_raw,
                                        u64 length_raw, u32 pr, u64 offset);

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

// VIVARIUM V-4b-6: the flat "NAME=VALUE\0" block behind /proc/<pid>/environ.
// Pins the encoding, the id ordering, the offset windowing (which is the whole
// reason this render is not a format-and-slice like its /proc siblings), and the
// skip rule for entries the Linux shape cannot carry.
void test_env_render_environ(void) {
    struct Proc *p = emk();
    char buf[256];

    // No Env at all -> an empty file, not an error. (A Proc that never set a
    // variable carries env == NULL; -1 here would make every such /proc/<pid>/
    // environ read fail instead of reading empty, which is what Linux does for a
    // kernel thread.)
    TEST_EXPECT_EQ(env_render_environ(p, 0, buf, (long)sizeof(buf)), 0L,
                   "V-4b-6: a Proc with no env renders an empty block");

    u64 a = env_create(p, "A", 1);
    u64 b = env_create(p, "BB", 2);
    TEST_ASSERT(a != 0 && b != 0 && a < b, "two vars, ids monotonic");
    TEST_EXPECT_EQ(env_write(p, a, 0, "1", 1), 1L, "A=1");
    TEST_EXPECT_EQ(env_write(p, b, 0, "22", 2), 2L, "BB=22");

    // Records are NUL-TERMINATED (not NUL-separated): the block ends with one,
    // exactly like Linux's, so a consumer that reads to EOF and splits on NUL
    // gets two records rather than two-and-an-empty-tail.
    const char want[] = "A=1\0BB=22\0";
    const long wlen = (long)sizeof(want) - 1;      // 10: drop the literal's own NUL
    long n = env_render_environ(p, 0, buf, (long)sizeof(buf));
    TEST_EXPECT_EQ(n, wlen, "V-4b-6: block is NAME=VALUE NUL per var, id order");
    TEST_ASSERT(bytes_eq(buf, want, wlen), "V-4b-6: block bytes exact");

    // Past EOF reads 0 (the read loop's terminator), never -1.
    TEST_EXPECT_EQ(env_render_environ(p, wlen, buf, (long)sizeof(buf)), 0L,
                   "V-4b-6: off at EOF -> 0");
    TEST_EXPECT_EQ(env_render_environ(p, wlen + 99, buf, (long)sizeof(buf)), 0L,
                   "V-4b-6: off past EOF -> 0");

    // THE OFFSET WINDOW. Read the block back 3 bytes at a time and reassemble:
    // this is what a real consumer does, and it is the property a format-and-
    // slice render could not provide for an environment larger than the devproc
    // buffer. A window that starts or ends mid-record must still be exact.
    char asm_[64];
    long got = 0;
    for (long off = 0; off < wlen; off += 3) {
        long k = env_render_environ(p, off, asm_ + got, 3);
        TEST_ASSERT(k > 0, "V-4b-6: a windowed read inside the block makes progress");
        got += k;
    }
    TEST_EXPECT_EQ(got, wlen, "V-4b-6: windowed reads reassemble the whole block");
    TEST_ASSERT(bytes_eq(asm_, want, wlen), "V-4b-6: reassembled bytes match");

    // An empty value still renders its record ("NAME=\0") -- set-but-empty and
    // unset are different states, and getenv distinguishes them.
    u64 c = env_create(p, "C", 1);
    TEST_ASSERT(c != 0, "create C with no value");
    n = env_render_environ(p, 0, buf, (long)sizeof(buf));
    const char want2[] = "A=1\0BB=22\0C=\0";
    TEST_EXPECT_EQ(n, (long)sizeof(want2) - 1, "V-4b-6: empty value renders NAME=");
    TEST_ASSERT(bytes_eq(buf, want2, (long)sizeof(want2) - 1),
                "V-4b-6: empty-value record bytes exact");
    TEST_ASSERT(env_unset(p, "C", 1), "drop C");

    // THE SKIP RULE. A value containing a NUL cannot be encoded (the NUL IS the
    // record terminator), so the entry is omitted WHOLE rather than emitted and
    // silently split -- a split would make its tail parse as a NAME=VALUE that
    // was never set. The rest of the block is unaffected.
    u64 d = env_create(p, "D", 1);
    TEST_ASSERT(d != 0, "create D");
    TEST_EXPECT_EQ(env_write(p, d, 0, "x\0y", 3), 3L, "D's value carries a NUL");
    n = env_render_environ(p, 0, buf, (long)sizeof(buf));
    TEST_EXPECT_EQ(n, wlen, "V-4b-6: a NUL-bearing value is skipped, not split");
    TEST_ASSERT(bytes_eq(buf, want, wlen), "V-4b-6: the other records survive it");

    edrop(p);
}

// The other half of the skip rule: a NAME containing '=' cannot be encoded
// either (the first '=' IS the separator, so "X=Y" with value "1" would read
// back as name "X" value "Y=1" -- one variable masquerading as another). Its own
// test because reaching it needs env_create's back door: devenv's walk passes a
// single path component, which cannot contain '/' but CAN contain '='.
void test_env_render_environ_eq_in_name(void) {
    struct Proc *p = emk();
    u64 ok  = env_create(p, "K", 1);
    u64 bad = env_create(p, "X=Y", 3);
    TEST_ASSERT(ok != 0, "create K");
    TEST_ASSERT(bad != 0, "'=' in a name is legal in /env (only NUL and / are not)");
    TEST_EXPECT_EQ(env_write(p, ok,  0, "v", 1), 1L, "K=v");
    TEST_EXPECT_EQ(env_write(p, bad, 0, "1", 1), 1L, "X=Y gets a value too");

    char buf[64];
    long n = env_render_environ(p, 0, buf, (long)sizeof(buf));
    const char want[] = "K=v\0";
    TEST_EXPECT_EQ(n, (long)sizeof(want) - 1,
                   "V-4b-6: a '=' in a name is skipped, not mis-attributed");
    TEST_ASSERT(bytes_eq(buf, want, n), "V-4b-6: only the encodable record renders");

    // And /env still serves it natively -- the skip is the LINUX render declining
    // to speak, not the variable disappearing. This is what keeps the reformatter
    // honest (VIVARIUM section 6.2: the native surface stays the truth).
    TEST_EXPECT_EQ(env_lookup(p, "X=Y", 3), bad, "/env still resolves the name");
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

// stat_native: /env is a directory, an entry is a regular file with a REAL
// size, and both are owned by the calling Proc (VIVARIUM V-4b-5). /env had no
// stat_native at all, so stat("/env"), fstat on an /env fd, and lseek(SEEK_END)
// all failed -- and realpath() of anything under it with them.
void test_devenv_stat_native_shapes(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "current proc");
    struct Proc *p = t->proc;
    env_free(p);                              // start from a clean env

    u64 id = env_create(p, "STATVAR", 7);
    TEST_ASSERT(id != 0, "create STATVAR");
    TEST_EXPECT_EQ(env_write(p, id, 0, "abcde", 5), (long)5, "write 5 bytes");

    struct t_stat st;
    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env");
    TEST_ASSERT(devenv.stat_native != NULL, "/env has a stat_native slot");
    TEST_EXPECT_EQ(devenv.stat_native(root, &st), 0, "stat_native(/env) ok");
    TEST_EXPECT_EQ(st.mode & (u32)T_S_IFMT, (u32)T_S_IFDIR,
                   "/env S_IFMT = S_IFDIR (S_ISDIR is true)");
    TEST_EXPECT_EQ(st.mode & ~(u32)T_S_IFMT, (u32)0755u, "/env perms = 0755");
    TEST_EXPECT_EQ(st.qid_type, QTDIR,          "/env is QTDIR");
    // Your environment is yours: every devenv op resolves the CALLING Proc's
    // env, so the owner it reports is the caller, not a system principal.
    TEST_EXPECT_EQ(st.uid, p->principal_id,     "/env uid = the calling Proc");
    TEST_EXPECT_EQ(st.gid, p->primary_gid,      "/env gid = the calling Proc");

    const char *names[1] = { "STATVAR" };
    struct Walkqid *wq = devenv.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL && wq->nqid == 1, "walk /env/STATVAR");
    struct Spoor *vf = wq->spoor;
    walkqid_free(wq);

    TEST_EXPECT_EQ(devenv.stat_native(vf, &st), 0, "stat_native(STATVAR) ok");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0644u), "entry = S_IFREG|0644");
    TEST_EXPECT_EQ(st.qid_type, QTFILE,               "entry is QTFILE");
    // UNLIKE a /ctl report, a stored value has a definite length that does not
    // move between the stat and the read -- so the size is real, and
    // lseek(SEEK_END) (which is this same call) lands on it.
    TEST_EXPECT_EQ(st.size, (u64)5, "entry size = the value length");
    TEST_EXPECT_EQ(env_write(p, id, 5, "fgh", 3), (long)3, "extend to 8 bytes");
    TEST_EXPECT_EQ(devenv.stat_native(vf, &st), 0, "re-stat ok");
    TEST_EXPECT_EQ(st.size, (u64)8, "size tracks the value");

    // The FILE IDENTITY must be unique tree-wide. Every other Dev's qid namespace
    // is global, so its Spoors carry the static devno 0 and (devno, qid.path)
    // still names one file; devenv's ids are PER-PROC and restart at 1, so
    // without a per-Env devno two Procs' unrelated variables both report (0, 1)
    // and claim to be the same file -- a wrong fstat, and (because the REVENANT
    // Image cache is keyed on that pair) an exec of an /env path serving one Proc
    // another's bytes. spoor_stat_native reports the SPOOR's devno (#100), so the
    // walk is what must stamp it.
    TEST_ASSERT(env_devno(p) != 0, "a live Env has a device number");
    TEST_EXPECT_EQ(vf->devno, env_devno(p), "the walked Spoor carries it");
    struct Proc *q = emk();
    TEST_ASSERT(q != NULL, "second Proc");
    TEST_ASSERT(env_create(q, "STATVAR", 7) != 0, "same name in a second env");
    TEST_ASSERT(env_devno(q) != env_devno(p),
                "two Procs' envs are different devices (no identity collision)");
    edrop(q);

    // A variable removed between the walk and the stat reports GONE rather than
    // another variable's length -- the monotonic-id guarantee, which every
    // other devenv op already honours.
    TEST_ASSERT(env_unset(p, "STATVAR", 7), "unset STATVAR");
    TEST_EXPECT_EQ(devenv.stat_native(vf, &st), -1, "a stale entry qid -> -1");

    spoor_unref(vf);
    spoor_unref(root);
    env_free(p);                              // restore an empty env
}

// An /env entry's readdir record carries d_type DT_REG (8) in its type byte
// (offset 21), beside QTFILE in its qid (byte 0).
void test_devenv_readdir_type_byte(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "current proc");
    struct Proc *p = t->proc;
    env_free(p);
    TEST_ASSERT(env_create(p, "TESTVAR", 7) != 0, "create TESTVAR on the current Proc");
    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env root");
    static u8 buf[256];
    long got = devenv.readdir(root, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ(got, (long)(24 + 7), "one record, for TESTVAR");
    TEST_EXPECT_EQ(buf[0], (u8)QTFILE, "its qid type is QTFILE");
    TEST_EXPECT_EQ(buf[21], (u8)8, "its type byte is DT_REG (8)");
    spoor_unref(root);
    env_free(p);
}

// The current Proc stands in for both ends of a passed handle: install `e` as
// its Env to act as the Proc holding it. Returns the Env it replaced.
static struct Env *env_become(struct Proc *p, struct Env *e) {
    struct Env *old = p->env;
    __atomic_store_n(&p->env, e, __ATOMIC_RELEASE);
    return old;
}

static int env_refs(struct Env *e) { return __atomic_load_n(&e->ref, __ATOMIC_ACQUIRE); }

// ut's `echo -n rich > /env/BEACON`: ut opens /env/BEACON (O_TRUNC) in its own
// Proc and hands the descriptor to echo, a spawned child holding a COPY of the
// environment. The write has to land in ut's Env: per-caller resolution put it
// in echo's copy, which died with echo, and left ut's variable truncated.
void test_devenv_open_binds_opener(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "current proc");
    struct Proc *p = t->proc;
    env_free(p);
    u64 id = env_create(p, "BEACON", 6);
    TEST_ASSERT(id != 0, "create BEACON");
    TEST_EXPECT_EQ(env_write(p, id, 0, "none", 4), (long)4, "BEACON=none");
    struct Env *shell = p->env;

    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env");
    const char *names[1] = { "BEACON" };
    struct Walkqid *wq = devenv.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL && wq->nqid == 1, "walk /env/BEACON");
    struct Spoor *h = wq->spoor;
    walkqid_free(wq);
    TEST_ASSERT(devenv.open(h, 0x01 | 0x10) == h, "open OWRITE|OTRUNC");
    TEST_EXPECT_EQ(env_refs(shell), 2, "the open handle holds a ref on the opener's Env");
    TEST_ASSERT(h->aux == shell, "and names it");

    struct Proc *echo = emk();
    TEST_ASSERT(echo != NULL, "the child");
    TEST_EXPECT_EQ(env_clone_into(echo, p), 0, "the child's copy of the environment");
    struct Env *copy = echo->env;

    // A spoor_clone of the open handle shallow-copies aux but not COPEN, so it
    // owns nothing: its last clunk must not drop the handle's ref.
    struct Spoor *cl = spoor_clone(h);
    TEST_ASSERT(cl != NULL && (cl->flag & COPEN) == 0, "a clone is not open");
    spoor_clunk(cl);
    TEST_EXPECT_EQ(env_refs(shell), 2, "a clone's clunk leaves the handle's ref");

    env_become(p, copy);                         // now act as echo, holding h
    TEST_EXPECT_EQ(devenv.write(h, "rich", 4, 0), (long)4, "echo writes through the handle");
    char buf[8];
    TEST_EXPECT_EQ(devenv.read(h, buf, sizeof(buf), 0), (long)4, "and reads back through it");
    TEST_ASSERT(bytes_eq(buf, "rich", 4), "the value it wrote");
    struct t_stat st;
    TEST_EXPECT_EQ(devenv.stat_native(h, &st), 0, "fstat through the handle");
    TEST_EXPECT_EQ(st.size, (u64)4, "sizes the opener's variable");
    TEST_EXPECT_EQ(env_read_in(copy, id, 0, buf, sizeof(buf)), (long)0,
                   "echo's own copy is untouched (truncated before the spawn)");
    env_become(p, shell);                        // back to the shell

    TEST_EXPECT_EQ(env_read(p, id, 0, buf, sizeof(buf)), (long)4, "the shell's BEACON");
    TEST_ASSERT(bytes_eq(buf, "rich", 4), "is rich, as the next child will inherit");
    edrop(echo);

    // The handle keeps the Env alive past its Proc's drop, and its close frees it.
    env_free(p);
    TEST_EXPECT_EQ(env_refs(shell), 1, "the handle's ref is the last");
    TEST_EXPECT_EQ(devenv.read(h, buf, sizeof(buf), 0), (long)4, "still readable");
    spoor_clunk(h);                              // the last ref: env_destroy
    spoor_unref(root);
}

// A create through /env opens the new variable bound to the creator's Env, and
// stamps that Env's devno: the parent directory carried 0 while the Proc had
// no Env.
void test_devenv_create_binds_creator(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "current proc");
    struct Proc *p = t->proc;
    env_free(p);

    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env");
    struct Walkqid *wq = devenv.walk(root, NULL, NULL, 0);
    TEST_ASSERT(wq != NULL && wq->nqid == 0, "0-element walk to the directory");
    struct Spoor *dir = wq->spoor;
    walkqid_free(wq);
    TEST_EXPECT_EQ(dir->devno, (u64)0, "no Env yet: the directory carries 0");

    struct Spoor *h = devenv.create(dir, "NEWVAR", 0x01, 0644, 0);
    TEST_ASSERT(h == dir, "create transitions the parent into the file");
    struct Env *mine = p->env;
    TEST_ASSERT(mine != NULL, "the create allocated the Env");
    TEST_ASSERT(h->aux == mine, "bound to the creator's Env");
    TEST_EXPECT_EQ(h->devno, mine->devno, "and stamped with its devno");
    TEST_EXPECT_EQ(env_refs(mine), 2, "Proc + handle");

    struct Proc *child = emk();
    TEST_ASSERT(child != NULL, "the child");
    TEST_EXPECT_EQ(env_clone_into(child, p), 0, "its copy");
    env_become(p, child->env);
    TEST_EXPECT_EQ(devenv.write(h, "v1", 2, 0), (long)2, "write through the created handle");
    env_become(p, mine);
    char buf[4];
    TEST_EXPECT_EQ(env_read(p, h->qid.path, 0, buf, sizeof(buf)), (long)2, "lands in the creator's Env");
    TEST_ASSERT(bytes_eq(buf, "v1", 2), "v1");
    edrop(child);

    spoor_clunk(h);
    TEST_EXPECT_EQ(env_refs(mine), 1, "the close dropped the handle's ref");
    spoor_unref(root);
    env_free(p);
}

// Ids restart in every Env and a spawned copy keeps them, so an id resolved in
// one Env names a different variable in another. A walked, unopened entry (an
// O_PATH handle passed on) carries its Env's devno, and an op from a Proc with
// a different Env fails rather than act on the other variable.
void test_devenv_unopened_id_needs_its_env(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "current proc");
    struct Proc *p = t->proc;
    env_free(p);
    u64 id = env_create(p, "PATHVAR", 7);
    TEST_ASSERT(id != 0, "create PATHVAR");
    TEST_EXPECT_EQ(env_write(p, id, 0, "parent", 6), (long)6, "PATHVAR=parent");
    struct Env *mine = p->env;

    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env");
    const char *names[1] = { "PATHVAR" };
    struct Walkqid *wq = devenv.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL && wq->nqid == 1, "walk /env/PATHVAR");
    struct Spoor *vf = wq->spoor;
    walkqid_free(wq);
    TEST_EXPECT_EQ(vf->devno, mine->devno, "the walk stamps the resolving Env");

    struct Proc *child = emk();
    TEST_ASSERT(child != NULL, "the child");
    TEST_EXPECT_EQ(env_clone_into(child, p), 0, "its copy keeps the ids");
    struct Env *copy = child->env;
    TEST_EXPECT_EQ(env_write_in(copy, id, 0, "child!", 6), (long)6,
                   "the copy's PATHVAR diverges under the same id");

    env_become(p, copy);                         // act as the child holding vf
    char buf[8];
    TEST_EXPECT_EQ(devenv.read(vf, buf, sizeof(buf), 0), (long)-1,
                   "a read of the parent's id in the child's Env fails");
    struct t_stat st;
    TEST_EXPECT_EQ(devenv.stat_native(vf, &st), -1, "so does a stat");
    // A 0-element walk from the entry (an O_PATH re-open) resolves nothing, so
    // it keeps the id's Env, and opening it here fails too.
    struct Walkqid *w0 = devenv.walk(vf, NULL, NULL, 0);
    TEST_ASSERT(w0 != NULL && w0->nqid == 0, "0-element walk from the entry");
    struct Spoor *re = w0->spoor;
    walkqid_free(w0);
    TEST_EXPECT_EQ(re->devno, mine->devno, "keeps the id's Env");
    TEST_ASSERT(devenv.open(re, 0) == NULL, "an open in another Env is refused");
    spoor_clunk(re);
    // A walk BY NAME is the caller's: the child reaches its own PATHVAR.
    struct Walkqid *w1 = devenv.walk(root, NULL, names, 1);
    TEST_ASSERT(w1 != NULL && w1->nqid == 1, "the child walks /env/PATHVAR");
    struct Spoor *own = w1->spoor;
    walkqid_free(w1);
    TEST_EXPECT_EQ(own->devno, copy->devno, "in its own Env");
    TEST_EXPECT_EQ(devenv.read(own, buf, sizeof(buf), 0), (long)6, "reads its own value");
    TEST_ASSERT(bytes_eq(buf, "child!", 6), "child!");
    spoor_unref(own);
    env_become(p, mine);

    TEST_EXPECT_EQ(devenv.read(vf, buf, sizeof(buf), 0), (long)6, "the parent reads its id");
    TEST_ASSERT(bytes_eq(buf, "parent", 6), "its own value");
    edrop(child);
    spoor_unref(vf);
    spoor_unref(root);
    env_free(p);
}

// The unlink contract: a specific errno per refusal. A name the caller's Env does
// not hold -- absent, or one no Env could hold -- is ENOENT, so a remover (ut's
// unset) can tell "nothing there" from a failure; /env holds no directories.
void test_devenv_unlink_answers_its_errno(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "current proc");
    struct Proc *p = t->proc;
    env_free(p);
    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env");
    TEST_EXPECT_EQ((s64)devenv.unlink(root, "ABSENT", 0), -(s64)T_E_NOENT,
                   "no Env yet: ENOENT");
    u64 id = env_create(p, "GONE", 4);
    TEST_ASSERT(id != 0, "create GONE");
    TEST_EXPECT_EQ((s64)devenv.unlink(root, "ABSENT", 0), -(s64)T_E_NOENT,
                   "an absent name: ENOENT");
    TEST_EXPECT_EQ((s64)devenv.unlink(root, "a/b", 0), -(s64)T_E_NOENT,
                   "a name no Env holds: ENOENT");
    TEST_EXPECT_EQ((s64)devenv.unlink(root, "GONE", SYS_UNLINK_REMOVEDIR),
                   -(s64)T_E_NOTDIR, "a directory removal of a variable: ENOTDIR");
    TEST_EXPECT_EQ((s64)devenv.unlink(root, "ABSENT", SYS_UNLINK_REMOVEDIR),
                   -(s64)T_E_NOENT, "a directory removal of nothing: ENOENT");
    TEST_EXPECT_EQ(env_lookup(p, "GONE", 4), id, "which left GONE in place");
    TEST_EXPECT_EQ((s64)devenv.unlink(root, "GONE", 0), (s64)0, "a held name goes");
    TEST_EXPECT_EQ(env_lookup(p, "GONE", 4), (u64)0, "gone");
    TEST_EXPECT_EQ((s64)devenv.unlink(root, "GONE", 0), -(s64)T_E_NOENT,
                   "a second removal: ENOENT");
    spoor_unref(root);
    env_free(p);
}

// A variable backs no file mapping (Dev.no_file_mapping). The open entry passes
// every earlier gate of both mapping cores -- it is readable and its size is
// known -- so ENODEV is that gate's answer and no other's. Measure first, assert
// last: a broken gate would leave a mapping to drain before any assert returns.
void test_devenv_entry_backs_no_mapping(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "current proc");
    struct Proc *me = t->proc;
    env_free(me);
    u64 id = env_create(me, "MAPVAR", 6);
    TEST_ASSERT(id != 0, "create MAPVAR");
    TEST_EXPECT_EQ(env_write(me, id, 0, "abc", 3), (long)3, "MAPVAR=abc");
    struct Env *mine = me->env;
    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env");
    const char *names[1] = { "MAPVAR" };
    struct Walkqid *wq = devenv.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL && wq->nqid == 1, "walk /env/MAPVAR");
    struct Spoor *vf = wq->spoor;
    walkqid_free(wq);
    TEST_ASSERT(devenv.open(vf, 0) == vf, "open binds MAPVAR");
    char buf[4];
    TEST_EXPECT_EQ(devenv.read(vf, buf, sizeof(buf), 0), (long)3, "premise: readable");
    struct t_stat st;
    TEST_EXPECT_EQ(devenv.stat_native(vf, &st), 0, "premise: its size is known");

    struct Proc *p = emk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->territory = territory_alloc();
    spoor_ref(vf); hidx_t fd  = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, vf);
    spoor_ref(vf); hidx_t fdx = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, vf);
    s64 m = 0, mf = 0, mx = 0, mfx = 0;
    if (p->territory && fd >= 0 && fdx >= 0) {
        m  = sys_mmap_file_for_proc(p, (u64)fd, PAGE_SIZE, false, 0);
        mf = sys_mmap_fixed_file_for_proc(p, EXEC_USER_BURROW_BASE + 0x200000ull,
                                          (u64)fdx, PAGE_SIZE, (u32)VIV_PROT_READ, 0);
        mx  = sys_mmap_file_for_proc(p, (u64)fd, PAGE_SIZE, true, 0);
        mfx = sys_mmap_fixed_file_for_proc(p, EXEC_USER_BURROW_BASE + 0x200000ull,
                                           (u64)fdx, PAGE_SIZE,
                                           (u32)(VIV_PROT_READ | VIV_PROT_EXEC), 0);
    }
    vma_drain(p);
    edrop(p);

    TEST_ASSERT(fd >= 0 && fdx >= 0, "handle_alloc");
    TEST_EXPECT_EQ(m, -(s64)T_E_NODEV, "a map of a variable: ENODEV");
    TEST_EXPECT_EQ(mf, -(s64)T_E_NODEV, "its MAP_FIXED twin: ENODEV");
    // Both cores ask the exec floor first, as Linux asks noexec before mmap.
    TEST_EXPECT_EQ(mx, -(s64)T_E_PERM, "an executable map of a variable: EPERM");
    TEST_EXPECT_EQ(mfx, -(s64)T_E_PERM, "its MAP_FIXED twin: EPERM");
    spoor_clunk(vf);
    // The gate's purpose, not only its errno: nothing (no cached Image entry)
    // kept the open entry, so its close dropped the Env reference it held.
    TEST_EXPECT_EQ(env_refs(mine), 1, "no mapping kept the entry open");
    spoor_unref(root);
    env_free(me);
}
