// #58 / REVENANT R-4 exec-from-namespace -- kernel-internal tests for
// exec_resolve_from_namespace.
//
// The userspace happy path is the live boot: joey spawns /hello, /bin/corvus,
// /bin/login, etc. through the SYS_SPAWN_* family, which routes every binary
// lookup through exec_resolve_from_namespace -> stalk instead of the flat
// boot-cpio devramfs_lookup. Since REVENANT R-4 the function RESOLVES + PINS the
// executable Spoor (the bytes are read later -- the header in the child, the
// text demand-paged) rather than slurping the whole ELF. These tests cover the
// resolution mechanism + the two security gates directly:
//
//   exec_ns.resolve_absolute_ok    "/hello" -> a non-NULL pinned Spoor + size>0.
//   exec_ns.resolve_relative_ok    "hello" (cwd-joined to "/hello") -> non-NULL.
//   exec_ns.miss_returns_null      a name the namespace cannot reach -> NULL.
//                                  This is the reverse-leak closure: spawn
//                                  resolves ONLY through the caller's namespace;
//                                  there is no devramfs_lookup fallback, so a
//                                  name a confined Proc cannot stalk cannot be
//                                  spawned (I-1 / I-28 for the exec path).
//   exec_ns.non_executable_denied  "/version" (a 0644 data file) -> NULL. The
//                                  OEXEC X-search gate (perm_want_for_omode =
//                                  PERM_R|PERM_X) denies a file without the
//                                  execute bit, even for the SYSTEM owner.
//
// The test Proc is kproc (PRINCIPAL_SYSTEM, rooted at the devramfs root by the
// harness's joey_root_kproc_at_devramfs() call before the suite). A confined-
// territory containment test (a Proc rooted at a subdir cannot name a sibling)
// is covered by the login session E2E (a CAP_SET_IDENTITY user shell cannot exec
// outside its namespace); a deterministic kernel-side version is an owed test.

#include "test.h"

#include <thylacine/dev.h>         // #217: devnone (the impersonating mount source)
#include <thylacine/env.h>         // #217 F1: env_create/env_write/env_free
#include <thylacine/errno.h>       // #217: T_E_PERM
#include <thylacine/exec.h>        // #217: EXEC_USER_BURROW_BASE
#include <thylacine/handle.h>      // #217: handle_alloc / KOBJ_SPOOR / RIGHT_READ
#include <thylacine/page.h>        // #217: PAGE_SIZE
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/stalk.h>       // section 13: stalk_cross_mounts crossed_pheno
#include <thylacine/syscall.h>
#include <thylacine/territory.h>   // #217: mount / unmount / MNOEXEC
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vivarium.h>    // #217: VIV_PROT_* (the MAP_FIXED prot word)
#include <thylacine/vma.h>         // #217: vma_drain

extern struct Spoor *exec_resolve_from_namespace(struct Proc *p, const char *name,
                                                 size_t name_len, size_t *size_out);
// Non-static in syscall.c but header-less, like the resolver above.
extern s64 sys_mmap_file_for_proc(struct Proc *p, u64 fd_raw, u64 length_raw,
                                  bool exec, u64 offset);
extern s64 sys_mmap_fixed_file_for_proc(struct Proc *p, u64 addr, u64 fd_raw,
                                        u64 length_raw, u32 pr, u64 offset);

void test_exec_ns_resolve_absolute_ok(void);
void test_exec_ns_resolve_relative_ok(void);
void test_exec_ns_miss_returns_null(void);
void test_exec_ns_non_executable_denied(void);
void test_exec_ns_noexec_mount_denied(void);      // #217
void test_mmap_file_noexec_mount_denied(void);    // #217
void test_mmap_file_devenv_never_exec_backs(void); // #217 F1
void test_exec_ns_pheno_mount_crossing(void);      // VIVARIUM section 13

void test_exec_ns_resolve_absolute_ok(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    size_t size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(t->proc, "/hello", 6, &size);
    TEST_ASSERT(exe != NULL, "exec_resolve_from_namespace(\"/hello\") resolves");
    TEST_ASSERT(size > 0, "stat'd executable size is nonzero");
    if (exe) spoor_clunk(exe);     // contract transfers the ref to the caller
}

void test_exec_ns_resolve_relative_ok(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    // Bare "hello" cwd-joins to "/hello" (kproc dot_path == "/") -- the same
    // resolution SYS_SPAWN's bare-name callers get.
    size_t size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(t->proc, "hello", 5, &size);
    TEST_ASSERT(exe != NULL, "exec_resolve_from_namespace(\"hello\") cwd-resolves");
    TEST_ASSERT(size > 0, "stat'd executable size is nonzero");
    if (exe) spoor_clunk(exe);
}

void test_exec_ns_miss_returns_null(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    // A name the namespace cannot reach -> NULL (no flat-table fallback).
    size_t size = 7;
    struct Spoor *exe = exec_resolve_from_namespace(t->proc, "/no-such-binary-xyz", 19, &size);
    TEST_ASSERT(exe == NULL, "a namespace miss returns NULL (no fallback)");
    TEST_ASSERT(size == 0, "size_out is 0 on a miss");
}

void test_exec_ns_non_executable_denied(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    // /version is a 0644 data file (no execute bit). The OEXEC X-search gate
    // denies it even for the SYSTEM owner (owner bits 0o6 = rw-, no x).
    size_t size = 9;
    struct Spoor *exe = exec_resolve_from_namespace(t->proc, "/version", 8, &size);
    TEST_ASSERT(exe == NULL, "a 0644 non-executable file is X-denied (NULL)");
    TEST_ASSERT(size == 0, "size_out is 0 on an X-deny");
}

// -----------------------------------------------------------------------------
// #217: MNOEXEC at the CALL SITES.
//
// test_territory_mount.noexec_covers proves the PREDICATE. These two prove the
// predicate is actually CONSULTED -- a gate wired to nothing passes a
// predicate test identically, which is the failure mode these exist to
// exclude. Each pairs its deny against a control taken through the SAME code
// path, so "the function refuses everything" cannot masquerade as enforcement.
// -----------------------------------------------------------------------------

// Mint a mount source that impersonates `victim`'s DEVICE INSTANCE. The
// predicate keys on (dc, devno), so this is what puts a real, already-resolved
// file under an MNOEXEC verdict without needing a second real filesystem.
static struct Spoor *noexec_source_for(struct Spoor *victim) {
    struct Spoor *s = spoor_alloc(&devnone);
    if (!s) return NULL;
    s->dc    = victim->dc;
    s->devno = victim->devno;
    return s;
}

void test_exec_ns_noexec_mount_denied(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc && t->proc->territory, "current thread has a Territory");

    size_t size = 0;
    struct Spoor *before = exec_resolve_from_namespace(t->proc, "/hello", 6, &size);
    TEST_ASSERT(before != NULL, "CONTROL: /hello resolves before the mount");
    if (!before) return;

    struct Spoor *src = noexec_source_for(before);
    struct Spoor *mp  = spoor_alloc(&devnone);
    TEST_ASSERT(src && mp, "spoor_alloc for the noexec mount");
    if (!src || !mp) { spoor_clunk(before); return; }
    mp->qid.path = 0xB10CC0DE217ull;   // an identity no real walk produces

    int mrc = mount(t->proc->territory, src, mp, MNOEXEC);

    // Resolve UNDER the mount, then take the namespace back to its prior shape
    // BEFORE asserting. This test mutates kproc's live Territory -- the one the
    // rest of the boot execs through -- so a failed assertion must not be able
    // to leave the mount installed and turn one red test into a dead boot.
    size_t denied_size = 7;
    struct Spoor *during = (mrc == 0)
        ? exec_resolve_from_namespace(t->proc, "/hello", 6, &denied_size)
        : NULL;
    if (mrc == 0) (void)unmount(t->proc->territory, mp);
    size_t after_size = 0;
    struct Spoor *after = exec_resolve_from_namespace(t->proc, "/hello", 6, &after_size);

    TEST_EXPECT_EQ(mrc, 0, "mounting the MNOEXEC source succeeded");
    TEST_ASSERT(during == NULL,
        "DENY: exec resolution refuses a binary on an MNOEXEC device instance "
        "-- a noexec mount that still permits exec is not noexec");
    TEST_ASSERT(denied_size == 0, "size_out stays 0 on the noexec deny");
    TEST_ASSERT(after != NULL,
        "CONTROL: the SAME resolve succeeds again once the mount is gone "
        "(so the deny came from MNOEXEC, not from a broken /hello)");

    if (during) spoor_clunk(during);
    if (after)  spoor_clunk(after);
    spoor_clunk(before);
    spoor_unref(src);
    spoor_unref(mp);
}

// VIVARIUM section 13: the phenotype-declaration mount channel. Unlike MNOEXEC
// (a (dc,devno) predicate that a phantom mount can trip without ever being
// crossed), MPHENO_LINUX is detected by an ACTUAL crossing during resolution --
// so this drives stalk_cross_mounts through a real cross of a pheno-mounted
// source and asserts the crossed_pheno report. Paired with a plain-mount control
// through the SAME code path, so "the resolver reports pheno for everything"
// cannot masquerade as detection. Mutates kproc's live Territory, so every mount
// is torn down before the asserts (the noexec test's discipline).
void test_exec_ns_pheno_mount_crossing(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc && t->proc->territory, "current thread has a Territory");
    if (!t || !t->proc || !t->proc->territory) return;

    struct Spoor *src   = spoor_alloc(&devnone);   // the mounted source (crossed)
    struct Spoor *mp    = spoor_alloc(&devnone);   // a synthetic mount point
    struct Spoor *probe = spoor_alloc(&devnone);   // an identity matching mp
    TEST_ASSERT(src && mp && probe, "spoor_alloc for the pheno crossing");
    if (!src || !mp || !probe) {
        if (probe) spoor_unref(probe);
        if (mp)    spoor_unref(mp);
        if (src)   spoor_unref(src);
        return;
    }
    mp->qid.path    = 0xF0F0DEC1A5Eull;   // an identity no real walk produces
    probe->qid.path = mp->qid.path;        // same (dc, devno, qid) -> matches the mount

    // Leg 1: cross an MPHENO_LINUX mount -> crossed_pheno set.
    int mrc1 = mount(t->proc->territory, src, mp, MPHENO_LINUX);
    struct Spoor *out1 = NULL;
    bool pheno_on = false;
    if (mrc1 == 0) (void)stalk_cross_mounts(t->proc, probe, &out1, &pheno_on);
    if (out1) spoor_clunk(out1);
    if (mrc1 == 0) (void)unmount(t->proc->territory, mp);

    // Leg 2 (CONTROL): cross a PLAIN mount over the same point -> stays false.
    int mrc2 = mount(t->proc->territory, src, mp, 0);
    struct Spoor *out2 = NULL;
    bool pheno_off = false;
    if (mrc2 == 0) (void)stalk_cross_mounts(t->proc, probe, &out2, &pheno_off);
    if (out2) spoor_clunk(out2);
    if (mrc2 == 0) (void)unmount(t->proc->territory, mp);

    // The primitive the detection reads: mount_lookup's flag report.
    u32 mflags_pheno = 0xFFFFFFFF, mflags_plain = 0xFFFFFFFF;
    int mrc3 = mount(t->proc->territory, src, mp, MPHENO_LINUX);
    struct Spoor *ls1 = (mrc3 == 0)
        ? mount_lookup(t->proc->territory, probe, &mflags_pheno) : NULL;
    if (ls1) spoor_clunk(ls1);
    if (mrc3 == 0) (void)unmount(t->proc->territory, mp);
    int mrc4 = mount(t->proc->territory, src, mp, 0);
    struct Spoor *ls2 = (mrc4 == 0)
        ? mount_lookup(t->proc->territory, probe, &mflags_plain) : NULL;
    if (ls2) spoor_clunk(ls2);
    if (mrc4 == 0) (void)unmount(t->proc->territory, mp);

    spoor_unref(probe);
    spoor_unref(mp);
    spoor_unref(src);

    TEST_EXPECT_EQ(mrc1, 0, "mount MPHENO_LINUX source");
    TEST_ASSERT(pheno_on == true,
        "DECLARE: crossing an MPHENO_LINUX mount sets crossed_pheno");
    TEST_EXPECT_EQ(mrc2, 0, "mount plain source");
    TEST_ASSERT(pheno_off == false,
        "CONTROL: crossing a PLAIN mount through the SAME path leaves "
        "crossed_pheno false (the detection is not always-on)");
    TEST_ASSERT((mflags_pheno & MPHENO_LINUX) != 0,
        "mount_lookup reports MPHENO_LINUX on the flagged mount");
    TEST_ASSERT((mflags_plain & MPHENO_LINUX) == 0,
        "mount_lookup reports NO MPHENO_LINUX on the plain mount (flag "
        "independence: the report is the entry's, not a constant)");
}

void test_mmap_file_noexec_mount_denied(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Resolve the victim through KPROC's namespace, and map it in a FRESH Proc
    // whose Territory we own outright -- so the MNOEXEC entry never touches the
    // namespace the rest of the boot runs in.
    size_t size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(t->proc, "/hello", 6, &size);
    TEST_ASSERT(exe != NULL && size > 0, "/hello resolves for the map");
    if (!exe) return;

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    if (!p) { spoor_clunk(exe); return; }
    p->territory = territory_alloc();
    TEST_ASSERT(p->territory != NULL, "territory_alloc failed");

    // Three handles: sys_lookup_spoor consumes the ref it hands out, so each
    // call gets its own.
    spoor_ref(exe); hidx_t fd_ctl  = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, exe);
    spoor_ref(exe); hidx_t fd_deny = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, exe);
    spoor_ref(exe); hidx_t fd_read  = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, exe);
    spoor_ref(exe); hidx_t fd_fixed = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, exe);
    TEST_ASSERT(fd_ctl >= 0 && fd_deny >= 0 && fd_read >= 0 && fd_fixed >= 0,
                "handle_alloc");

    struct Spoor *src = noexec_source_for(exe);
    struct Spoor *mp  = spoor_alloc(&devnone);
    TEST_ASSERT(src && mp, "spoor_alloc for the noexec mount");
    if (src && mp) mp->qid.path = 0xB10CC0DE218ull;

    // MEASURE FIRST, ASSERT LAST -- every result is captured, the Proc is torn
    // down, and only then do the assertions run. TEST_ASSERT `return`s on
    // failure, so asserting inline would skip vma_drain/proc_free and strand
    // this Proc's entries in the GLOBAL Image cache; the image.* suite asserts
    // "cache empty at start" and would report six further failures downstream of
    // this one. Measured, not theorised: an earlier draft asserted inline, and
    // sabotaging the gate turned one red test into seven, with the real finding
    // buried in the middle. A test must not make its own failure harder to read.
    s64 ctl = -1, deny = -1, ro = -1, deny_fixed = -1;
    int mrc = -1;
    if (src && mp) {
        ctl  = sys_mmap_file_for_proc(p, (u64)fd_ctl, PAGE_SIZE, true, 0);
        mrc  = mount(p->territory, src, mp, MNOEXEC);
        if (mrc == 0) {
            deny = sys_mmap_file_for_proc(p, (u64)fd_deny, PAGE_SIZE, true, 0);
            ro   = sys_mmap_file_for_proc(p, (u64)fd_read, PAGE_SIZE, false, 0);
            // The MAP_FIXED twin. Its own comment records that no producer on
            // the measured rootfs reaches its demand-paged branch, which is
            // exactly why it needs a test: an untested gate on an unexercised
            // path is indistinguishable from no gate until the day something
            // reaches it. The census that found this arm is worth nothing if
            // the arm it found stays unproven.
            deny_fixed = sys_mmap_fixed_file_for_proc(
                p, EXEC_USER_BURROW_BASE + 0x200000ull, (u64)fd_fixed, PAGE_SIZE,
                (u32)(VIV_PROT_READ | VIV_PROT_EXEC), 0);
        }
    }

    vma_drain(p);
    p->state = 2;                 // PROC_STATE_ZOMBIE
    proc_free(p);
    if (src) spoor_unref(src);
    if (mp)  spoor_unref(mp);
    spoor_clunk(exe);

    // CONTROL: with no MNOEXEC entry the R+X map is admitted. Without it, the
    // deny below would be satisfied by a mapping that never worked at all.
    TEST_ASSERT(ctl > 0, "CONTROL: R+X file map succeeds with no MNOEXEC entry");
    TEST_EXPECT_EQ(mrc, 0, "mounting the MNOEXEC source succeeded");
    TEST_EXPECT_EQ((int)deny, -(int)T_E_PERM,
        "DENY: the R+X file map is refused with T_E_PERM on an MNOEXEC device "
        "instance (the same call that just succeeded)");
    // The third discrimination: MNOEXEC restricts what may become CODE, not what
    // may be READ. A gate that refused both would satisfy the deny above and
    // still be wrong -- it would break every legitimate data mapping.
    TEST_ASSERT(ro > 0,
        "CONTROL: a NON-exec file map off the same MNOEXEC instance is still "
        "admitted (noexec bounds execute, not read)");
    TEST_EXPECT_EQ((int)deny_fixed, -(int)T_E_PERM,
        "DENY: the MAP_FIXED R+X twin is refused too -- the census found this "
        "second exec-mapping site, so it is gated and proven, not assumed");
}

// #217 F1 regression -- the test whose ABSENCE let the finding through.
//
// Every other test here reaches the gate via noexec_source_for(), which FORGES
// the (dc, devno) match by overwriting a devnone Spoor's identity. devenv is
// structurally incapable of producing that match: devenv_walk stamps the
// CALLING Proc's env devno, so a container's /env files never share an identity
// with the /env mount source viv installed, and no MNOEXEC flag can ever cover
// them. So the mount-flag battery stayed green while /env -- the surface the
// scripture NAMES as the reason the mechanism exists -- was wide open.
//
// This one manufactures nothing: a REAL entry, through devenv's own attach and
// walk, into the real syscall body. It reddens on the pre-F1 kernel.
void test_mmap_file_devenv_never_exec_backs(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current proc");
    struct Proc *tp = t->proc;
    env_free(tp);                       // start clean, like test_devenv_walk_read

    u64 id = env_create(tp, "NOEXEC217", 9);
    TEST_ASSERT(id != 0, "create the /env entry");
    char payload[64];
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (char)0x41;
    TEST_EXPECT_EQ((int)env_write(tp, id, 0, payload, sizeof(payload)),
                   (int)sizeof(payload), "write the would-be shellcode");

    struct Spoor *root = devenv.attach("");
    TEST_ASSERT(root != NULL, "attach /env root");
    const char *names[1] = { "NOEXEC217" };
    // Cleanup-then-fail, NOT TEST_ASSERT: the macro expands to `return`, so an
    // assert here would skip every unref below it and leak root + the env entry
    // (and the walked Spoor on the nqid != 1 arm). An earlier draft had the
    // cleanup written AFTER the assert, where it could never run -- dead code
    // that reads like failure-path hygiene. #217 round-2 F3.
    struct Walkqid *wq = devenv.walk(root, NULL, names, 1);
    if (!wq || wq->nqid != 1) {
        if (wq) { spoor_unref(wq->spoor); walkqid_free(wq); }
        spoor_unref(root);
        env_free(tp);
        test_fail("walk /env/NOEXEC217");
        return;
    }
    struct Spoor *vf = wq->spoor;
    walkqid_free(wq);

    struct Proc *p = proc_alloc();
    if (!p) {
        spoor_unref(vf);
        spoor_unref(root);
        env_free(tp);
        test_fail("proc_alloc");
        return;
    }
    p->territory = territory_alloc();

    spoor_ref(vf); hidx_t fd_x = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, vf);
    spoor_ref(vf); hidx_t fd_r = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, vf);

    // Measure first, assert last (the same reason as the sibling above).
    s64 xmap = sys_mmap_file_for_proc(p, (u64)fd_x, PAGE_SIZE, true,  0);
    s64 rmap = sys_mmap_file_for_proc(p, (u64)fd_r, PAGE_SIZE, false, 0);

    vma_drain(p);
    p->state = 2;                       // PROC_STATE_ZOMBIE
    proc_free(p);
    spoor_unref(vf);
    spoor_unref(root);
    env_free(tp);

    TEST_EXPECT_EQ((int)xmap, -(int)T_E_PERM,
        "DENY: a REAL /env entry cannot back an executable mapping. NO mount is "
        "involved -- the Dev allowlist is what closes this, because no mount "
        "flag can reach devenv's per-Proc devno");
    // Deliberately the precise claim, not `rmap > 0`: the floor must gate EXEC
    // and nothing else. A non-exec devenv map may still fail for unrelated
    // reasons; what must never happen is it being refused by the exec floor.
    TEST_ASSERT(rmap != -(s64)T_E_PERM,
        "CONTROL: the same entry mapped NON-exec is not refused by the exec "
        "floor (devenv stays readable)");
}
