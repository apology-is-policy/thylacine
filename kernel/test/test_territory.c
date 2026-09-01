// Territory primitives tests (P2-Eb).
//
// Three tests:
//
//   territory.bind_smoke
//     Allocate a fresh Territory; bind a few non-cyclic edges; verify
//     territory_nbinds tracks the count and unbind removes correctly.
//
//   territory.cycle_rejected
//     Bind two non-cyclic edges; attempt a third bind that would close
//     a cycle; verify the third bind returns -1 (cycle rejected).
//     Models the spec's BuggyBind being statically prevented at the
//     impl level (bind() always calls would_create_cycle before insert).
//
//   territory.fork_isolated
//     Allocate parent Territory; bind some edges. territory_clone child. Bind
//     more on parent. Verify child's bindings reflect only the
//     pre-clone state (independent function values per the spec's
//     ForkClone action).
//
// Maps to specs/territory.tla state invariant `NoCycle` (cycle_rejected)
// and the structural Isolation property (fork_isolated).

#include "test.h"

#include <thylacine/territory.h>
#include <thylacine/types.h>

void test_namespace_bind_smoke(void);
void test_namespace_cycle_rejected(void);
void test_namespace_fork_isolated(void);

void test_namespace_bind_smoke(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    TEST_EXPECT_EQ(territory_nbinds(p), 0,
        "fresh Territory must have 0 bindings");

    // bind 1: a -> b (walking b yields a). Acyclic.
    TEST_EXPECT_EQ(bind(p, 1u, 2u), 0,
        "first bind should succeed");
    TEST_EXPECT_EQ(territory_nbinds(p), 1,
        "nbinds should be 1 after one bind");

    // bind 2: b -> c (walking c yields b). Still acyclic — c can reach
    // b can reach a, but none can reach themselves.
    TEST_EXPECT_EQ(bind(p, 2u, 3u), 0,
        "second bind should succeed");
    TEST_EXPECT_EQ(territory_nbinds(p), 2,
        "nbinds should be 2 after two binds");

    // Idempotent re-bind returns -2.
    TEST_EXPECT_EQ(bind(p, 1u, 2u), -2,
        "duplicate bind must return -2 (already bound)");

    // unbind: remove the second edge.
    TEST_EXPECT_EQ(unbind(p, 2u, 3u), 0,
        "unbind of existing edge should succeed");
    TEST_EXPECT_EQ(territory_nbinds(p), 1,
        "nbinds back to 1 after unbind");

    // unbind of non-existent edge returns -1.
    TEST_EXPECT_EQ(unbind(p, 99u, 100u), -1,
        "unbind of non-existent edge must return -1");

    territory_unref(p);
}

void test_namespace_cycle_rejected(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    // Build chain: walking c yields b; walking b yields a. So from c
    // we can reach a (c -> b -> a in walk direction).
    //
    // bind args (src, dst): walking dst yields src.
    //   bind(b, a) -> walking a yields b. Edge a -> b.
    //   bind(c, b) -> walking b yields c. Edge b -> c.
    // After both: starting at a, walking yields b, then walking b
    // yields c. So a reaches {b, c}.
    TEST_EXPECT_EQ(bind(p, 2u, 1u), 0, "bind 2->1 should succeed");
    TEST_EXPECT_EQ(bind(p, 3u, 2u), 0, "bind 3->2 should succeed");

    // Now attempt bind(1, 3): walking 3 would yield 1. Edge 3 -> 1.
    // From 3 we already reach 1 (chain 3 -> 2 -> 1 yields ... wait,
    // let me re-check the direction).
    //
    // Actually: edge a -> b in our model means walking a yields b
    // (since bind(b, a) means a is the mount point that yields b).
    // Wait that's getting confusing. Let me re-derive.
    //
    // bind(src=2, dst=1): adds the binding "1 has 2 bound at it." So
    // walking 1 yields 2. Edge 1 -> 2 (walking 1 -> reaches 2).
    // bind(src=3, dst=2): walking 2 yields 3. Edge 2 -> 3.
    //
    // From 1, walking yields 2, then walking 2 yields 3. So Reachable(1)
    // = {1, 2, 3}.
    //
    // Now bind(src=1, dst=3): would add "walking 3 yields 1." Edge 3 -> 1.
    // Cycle check: would adding edge 3 -> 1 create a cycle? Cycle iff
    // 3 is reachable from 1 (then 1 -> ... -> 3 -> (new) -> 1 closes).
    // Yes: Reachable(1) = {1, 2, 3} contains 3. So bind must reject.
    TEST_EXPECT_EQ(bind(p, 1u, 3u), -1,
        "bind that would close a cycle must return -1");
    TEST_EXPECT_EQ(territory_nbinds(p), 2,
        "rejected bind must NOT modify the bind table");

    // Self-bind is also rejected as a degenerate cycle (length-1 cycle).
    TEST_EXPECT_EQ(bind(p, 5u, 5u), -4,
        "self-bind (src == dst) must return -4");
    TEST_EXPECT_EQ(territory_nbinds(p), 2,
        "rejected self-bind must NOT modify the bind table");

    territory_unref(p);
}

void test_namespace_fork_isolated(void) {
    struct Territory *parent = territory_alloc();
    TEST_ASSERT(parent != NULL, "parent territory_alloc returned NULL");

    TEST_EXPECT_EQ(bind(parent, 1u, 2u), 0,
        "parent bind 1->2 should succeed");
    TEST_EXPECT_EQ(bind(parent, 3u, 4u), 0,
        "parent bind 3->4 should succeed");
    TEST_EXPECT_EQ(territory_nbinds(parent), 2,
        "parent has 2 bindings before clone");

    struct Territory *child = territory_clone(parent);
    TEST_ASSERT(child != NULL, "territory_clone returned NULL");
    TEST_EXPECT_EQ(territory_nbinds(child), 2,
        "child should have 2 bindings (cloned from parent)");

    // Add a binding to parent only. Child should be unaffected.
    TEST_EXPECT_EQ(bind(parent, 5u, 6u), 0,
        "parent bind 5->6 (post-clone) should succeed");
    TEST_EXPECT_EQ(territory_nbinds(parent), 3,
        "parent has 3 bindings");
    TEST_EXPECT_EQ(territory_nbinds(child), 2,
        "child must remain at 2 bindings after parent's post-clone bind");

    // Add a binding to child only. Parent should be unaffected.
    TEST_EXPECT_EQ(bind(child, 7u, 8u), 0,
        "child bind 7->8 should succeed");
    TEST_EXPECT_EQ(territory_nbinds(child), 3,
        "child has 3 bindings");
    TEST_EXPECT_EQ(territory_nbinds(parent), 3,
        "parent must remain at 3 bindings after child's bind");

    territory_unref(parent);
    territory_unref(child);
}

// Design D (VIVARIUM 13.10.3; review F2 = self-audit SA-1): the namespace-level
// phenotype declaration is copied by territory_clone. territory_clone copies its
// fields one by one, so an omitted copy would let a container child LOOK Linux
// (the rfork phenotype inherit) and revert to native on its first execve.
// Paired with the undeclared control (a clone of nothing declares nothing) and
// the post-clone independence check (a later parent declaration stays the
// parent's), so "the clone always reports true" cannot pass.
void test_territory_clone_copies_root_pheno(void);
void test_territory_clone_copies_root_pheno(void) {
    struct Territory *undecl = territory_alloc();
    struct Territory *decl   = territory_alloc();
    TEST_ASSERT(undecl != NULL && decl != NULL, "territory_alloc");
    if (!undecl || !decl) {
        if (undecl) territory_unref(undecl);
        if (decl)   territory_unref(decl);
        return;
    }
    bool fresh_undecl = territory_root_pheno(undecl);
    bool null_undecl  = territory_root_pheno(NULL);
    territory_declare_linux(decl);
    bool decl_set = territory_root_pheno(decl);
    territory_declare_linux(decl);               // idempotent
    bool decl_still = territory_root_pheno(decl);

    struct Territory *cu = territory_clone(undecl);
    struct Territory *cd = territory_clone(decl);
    bool cu_pheno = cu ? territory_root_pheno(cu) : true;    // fail toward the wrong answer
    bool cd_pheno = cd ? territory_root_pheno(cd) : false;
    territory_declare_linux(undecl);            // AFTER the clone
    bool cu_after = cu ? territory_root_pheno(cu) : true;

    if (cu) territory_unref(cu);
    if (cd) territory_unref(cd);
    territory_unref(undecl);
    territory_unref(decl);

    TEST_ASSERT(cu != NULL && cd != NULL, "territory_clone");
    TEST_ASSERT(fresh_undecl == false, "a fresh Territory declares nothing (KP_ZERO, fail-safe)");
    TEST_ASSERT(null_undecl == false,  "NULL declares nothing");
    TEST_ASSERT(decl_set && decl_still, "territory_declare_linux sets it, idempotently");
    TEST_ASSERT(cu_pheno == false, "CONTROL: a clone of an undeclared Territory declares nothing");
    TEST_ASSERT(cd_pheno == true,  "F2/SA-1: territory_clone copies the declaration");
    TEST_ASSERT(cu_after == false, "a declaration made AFTER the clone stays the parent's");
}

// --- LS-4: per-Proc cwd ("dot") --------------------------------------------

static int cwd_streq(const char *a, const char *b) {
    u64 i = 0;
    while (a[i] != '\0' && b[i] != '\0') { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

void test_territory_cwd_lexical(void);
void test_territory_cwd_dot(void);
void test_territory_cwd_join(void);

// territory.cwd_join -- the RESOLUTION join (#83). The property under test is
// what it does NOT do: it must not resolve "." / ".." and must not drop a
// trailing separator, so that a cwd-relative path reaches stalk's #79/#81/#82
// gates carrying the same tokens the absolute spelling would.
void test_territory_cwd_join(void) {
    char out[256];
    int r;

    r = cwd_join("/home/michael", "foo.txt", 7, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/home/michael/foo.txt"),
        "relative join against cwd");

    r = cwd_join("/home/michael", "/etc/passwd", 11, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/etc/passwd"),
        "absolute path ignores cwd");

    // The #83 core: dots survive the join. Pre-fix these collapsed here and
    // stalk never saw them, so `f/..` on a FILE -- and worse, `nope/..` on a
    // path that does not exist -- resolved successfully.
    r = cwd_join("/a/b/c", "../x", 4, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/a/b/c/../x"),
        "'..' is PRESERVED for stalk (not collapsed to /a/b/x)");

    r = cwd_join("/a", "f/.", 3, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/a/f/."),
        "'.' is PRESERVED for stalk");

    r = cwd_join("/a", "f/", 2, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/a/f/"),
        "a trailing separator is PRESERVED for stalk");

    r = cwd_join("/a", "nope/..", 7, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/a/nope/.."),
        "a '..' through a missing component is PRESERVED (stalk must walk it)");

    // Root is stored as "/", so the join must not emit "//".
    r = cwd_join("/", "foo", 3, out, sizeof(out));
    TEST_ASSERT(r == 4 && cwd_streq(out, "/foo"),
        "root cwd does not double the separator");

    r = cwd_join((const char *)0, "foo", 3, out, sizeof(out));
    TEST_ASSERT(r == 4 && cwd_streq(out, "/foo"),
        "NULL cwd treated as root");

    r = cwd_join("/home", "/", 1, out, sizeof(out));
    TEST_ASSERT(r == 1 && cwd_streq(out, "/"),
        "absolute / yields /");

    // No production caller passes an empty input (every syscall rejects
    // path_len == 0 first), but the result must not gain a trailing separator
    // that would newly assert a directory.
    r = cwd_join("/a/b", "", 0, out, sizeof(out));
    TEST_ASSERT(r == 4 && cwd_streq(out, "/a/b"),
        "empty input names the cwd, without gaining a trailing slash");

    // Overflow is rejected, never truncated. The join is longer than the old
    // lexical resolve for '..'-bearing paths, so this bound matters more now.
    char tiny[4];
    r = cwd_join("/aaaa", "bbbb", 4, tiny, sizeof(tiny));
    TEST_ASSERT(r == -1, "over-capacity result rejected");

    // The SYS_CHDIR step-3 contract: canonicalization runs on the ALREADY
    // absolute join with dot == NULL, so dot_path is not re-read and cannot
    // race a peer thread's chdir.
    r = cwd_lexical_resolve((const char *)0, "/a/b/..", 7, out, sizeof(out));
    TEST_ASSERT(r == 2 && cwd_streq(out, "/a"),
        "canonicalizing an absolute join collapses '..'");
    r = cwd_lexical_resolve((const char *)0, "/etc/", 5, out, sizeof(out));
    TEST_ASSERT(r == 4 && cwd_streq(out, "/etc"),
        "canonicalizing drops a trailing separator (dot_path stays canonical)");
    r = cwd_lexical_resolve((const char *)0, "/..", 3, out, sizeof(out));
    TEST_ASSERT(r == 1 && cwd_streq(out, "/"),
        "canonicalizing clamps '..' at root (cd .. from / stores /)");
}

// territory.cwd_lexical -- the pure canonicalizer (cwd_lexical_resolve). Since
// #83 its ONLY production role is computing the string SYS_CHDIR stores in
// dot_path; it is NOT the resolution path (see test_territory_cwd_join).
void test_territory_cwd_lexical(void) {
    char out[256];
    int r;

    r = cwd_lexical_resolve("/home/michael", "foo.txt", 7, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/home/michael/foo.txt"),
        "relative join against cwd");

    r = cwd_lexical_resolve("/home/michael", "/etc/passwd", 11, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/etc/passwd"),
        "absolute path ignores cwd");

    r = cwd_lexical_resolve("/a", "./b//c", 6, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/a/b/c"),
        "dot skipped + double-slash collapsed");

    r = cwd_lexical_resolve("/a/b/c", "../x", 4, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/a/b/x"),
        "dotdot pops one component");

    // ".." can never escape above "/" (the I-28-adjacent lexical clamp; stalk
    // ALSO re-clamps at root_spoor, so this is belt-and-suspenders).
    r = cwd_lexical_resolve("/a", "../../../etc", 12, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/etc"),
        "dotdot clamped at root");

    r = cwd_lexical_resolve((const char *)0, "foo", 3, out, sizeof(out));
    TEST_ASSERT(r > 0 && cwd_streq(out, "/foo"),
        "NULL cwd treated as root");

    r = cwd_lexical_resolve("/a", "..", 2, out, sizeof(out));
    TEST_ASSERT(r == 1 && cwd_streq(out, "/"),
        "dotdot from /a nets back to /");

    r = cwd_lexical_resolve("/home", "/", 1, out, sizeof(out));
    TEST_ASSERT(r == 1 && cwd_streq(out, "/"),
        "absolute / yields /");

    // A result that does not fit the output buffer is rejected (no overflow).
    char tiny[4];
    r = cwd_lexical_resolve("/aaaa", "bbbb", 4, tiny, sizeof(tiny));
    TEST_ASSERT(r == -1, "over-capacity result rejected");
}

// territory.cwd_dot -- getdot / setdot / resolve_cwd + clone snapshot isolation.
void test_territory_cwd_dot(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    char buf[256];
    int len = territory_getdot(p, buf, sizeof(buf));
    TEST_ASSERT(len == 1 && cwd_streq(buf, "/"), "fresh cwd is /");

    TEST_EXPECT_EQ(territory_setdot(p, "/home/michael"), 0, "setdot ok");
    len = territory_getdot(p, buf, sizeof(buf));
    TEST_ASSERT(len == 13 && cwd_streq(buf, "/home/michael"), "cwd round-trips");

    int r = territory_join_cwd(p, "x", 1, buf, sizeof(buf));
    TEST_ASSERT(r > 0 && cwd_streq(buf, "/home/michael/x"),
        "join_cwd joins relative against the stored dot");

    // The "/" sentinel resets dot_path to NULL; getdot reads "/" again.
    TEST_EXPECT_EQ(territory_setdot(p, "/"), 0, "setdot / ok");
    len = territory_getdot(p, buf, sizeof(buf));
    TEST_ASSERT(len == 1 && cwd_streq(buf, "/"), "cwd back to / after setdot(/)");

    TEST_EXPECT_EQ(territory_setdot(p, "/abcdef"), 0, "setdot /abcdef ok");
    char small[4];
    TEST_EXPECT_EQ(territory_getdot(p, small, sizeof(small)), -1,
        "getdot rejects a too-small buffer");

    // A clone inherits the cwd snapshot; the parent's later setdot does NOT
    // affect the child (independent kmalloc'd copy -- POSIX fork semantics).
    struct Territory *child = territory_clone(p);
    TEST_ASSERT(child != NULL, "territory_clone returned NULL");
    len = territory_getdot(child, buf, sizeof(buf));
    TEST_ASSERT(len == 7 && cwd_streq(buf, "/abcdef"), "child inherits cwd");
    TEST_EXPECT_EQ(territory_setdot(p, "/changed"), 0, "parent setdot ok");
    len = territory_getdot(child, buf, sizeof(buf));
    TEST_ASSERT(len == 7 && cwd_streq(buf, "/abcdef"),
        "child cwd unaffected by parent setdot (snapshot isolation)");

    territory_unref(child);
    territory_unref(p);
}
