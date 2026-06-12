// Path tests (#66 -- the Plan 9 Chan.path namespace-name retention; I-33).
//
// Two parts:
//   A. The path.c string + refcount primitives (make_root / addelem / parent /
//      ref / unref + the alloc/free balance + the overflow/OOM-leaves-NULL
//      property).
//   B. The Spoor bridge helpers (spoor_path_extend / spoor_path_transplant) +
//      spoor_clone SHARE + spoor_free DROP, using a stub Dev so a Spoor can be
//      allocated without a backing namespace. The end-to-end accumulation
//      through the REAL resolver (stalk: append-per-hop + cross-transplant +
//      ..-pop) is exercised in test_stalk.c (which already has the nested
//      fixture + mount setup).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/path.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>   // SYS_OPEN_PATH_MAX
#include <thylacine/types.h>

void test_path_make_root(void);
void test_path_addelem_forms(void);
void test_path_parent_forms(void);
void test_path_addelem_overflow_null(void);
void test_path_ref_balance(void);
void test_path_spoor_clone_shares(void);
void test_path_spoor_extend(void);
void test_path_spoor_transplant(void);

// streq -- a NUL-terminated compare (no libc in the kernel test env).
static bool streq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

// A stub Dev: spoor_alloc only needs dc; spoor_clone copies fields + calls
// spoor_alloc_internal(c->dev); spoor_clunk runs close (left NULL -> no-op). No
// walk/open needed for the helper unit tests.
static struct Dev pathfix_dev = { .dc = 'X', .name = "pathfix" };

// =============================================================================
// Part A -- path.c primitives.
// =============================================================================

void test_path_make_root(void) {
    u64 a0 = path_total_allocated(), f0 = path_total_freed();
    struct Path *r = path_make_root();
    TEST_ASSERT(r != NULL, "path_make_root allocates");
    TEST_ASSERT(streq(r->s, "/"), "root path string is /");
    TEST_EXPECT_EQ(r->len, 1u, "root path len is 1");
    TEST_EXPECT_EQ(path_total_allocated() - a0, 1ull, "one alloc");
    path_unref(r);
    TEST_EXPECT_EQ(path_total_freed() - f0, 1ull, "one free on last unref");
}

void test_path_addelem_forms(void) {
    struct Path *root = path_make_root();
    TEST_ASSERT(root != NULL, "root");

    // root "/" + "bin" -> "/bin" (no doubled separator).
    struct Path *bin = path_addelem(root, "bin", 3);
    TEST_ASSERT(bin != NULL && streq(bin->s, "/bin"), "/ + bin -> /bin");
    TEST_EXPECT_EQ(bin->len, 4u, "/bin len 4");

    // "/bin" + "joey" -> "/bin/joey".
    struct Path *joey = path_addelem(bin, "joey", 4);
    TEST_ASSERT(joey != NULL && streq(joey->s, "/bin/joey"), "/bin + joey -> /bin/joey");

    // "." -> a no-op copy (same string, fresh allocation).
    struct Path *dot = path_addelem(joey, ".", 1);
    TEST_ASSERT(dot != NULL && streq(dot->s, "/bin/joey"), ". keeps the path");
    TEST_ASSERT(dot != joey, ". allocates a fresh Path (copy-on-walk)");

    // ".." -> pop the last element.
    struct Path *up = path_addelem(joey, "..", 2);
    TEST_ASSERT(up != NULL && streq(up->s, "/bin"), ".. pops to /bin");

    // NULL parent -> NULL (unknown stays unknown).
    TEST_ASSERT(path_addelem(NULL, "x", 1) == NULL, "addelem(NULL) -> NULL");

    path_unref(root); path_unref(bin); path_unref(joey); path_unref(dot); path_unref(up);
}

void test_path_parent_forms(void) {
    struct Path *root = path_make_root();
    struct Path *a    = path_addelem(root, "a", 1);          // /a
    struct Path *ab   = path_addelem(a, "b", 1);             // /a/b

    struct Path *p_ab = path_parent(ab);                     // /a/b -> /a
    TEST_ASSERT(p_ab != NULL && streq(p_ab->s, "/a"), "parent(/a/b) = /a");
    struct Path *p_a  = path_parent(a);                      // /a -> /
    TEST_ASSERT(p_a != NULL && streq(p_a->s, "/"), "parent(/a) = /");
    struct Path *p_r  = path_parent(root);                   // / -> /
    TEST_ASSERT(p_r != NULL && streq(p_r->s, "/"), "parent(/) = /");
    TEST_ASSERT(path_parent(NULL) == NULL, "parent(NULL) = NULL");

    path_unref(root); path_unref(a); path_unref(ab);
    path_unref(p_ab); path_unref(p_a); path_unref(p_r);
}

void test_path_addelem_overflow_null(void) {
    struct Path *root = path_make_root();
    // A component longer than the whole-path bound -> NULL (the walk would
    // still succeed; the name becomes "unknown"). namelen > SYS_OPEN_PATH_MAX.
    struct Path *over = path_addelem(root, "x", (u64)SYS_OPEN_PATH_MAX + 1);
    TEST_ASSERT(over == NULL, "over-long component -> NULL");
    // A component that fits the name bound but would push the TOTAL over the
    // limit also -> NULL. Build a near-max path first, then add to it.
    char big[SYS_OPEN_PATH_MAX];
    for (u32 i = 0; i < SYS_OPEN_PATH_MAX - 2; i++) big[i] = 'a';
    struct Path *near = path_addelem(root, big, SYS_OPEN_PATH_MAX - 2);  // ~max
    TEST_ASSERT(near != NULL, "near-max path builds");
    struct Path *tip = path_addelem(near, "morethanfits", 12);
    TEST_ASSERT(tip == NULL, "over-budget total -> NULL");
    path_unref(root); path_unref(near);
}

void test_path_ref_balance(void) {
    u64 a0 = path_total_allocated(), f0 = path_total_freed();
    struct Path *p = path_make_root();
    path_ref(p);   // ref now 2
    path_unref(p); // ref 1; NOT freed
    TEST_EXPECT_EQ(path_total_freed() - f0, 0ull, "shared ref not freed at first drop");
    path_unref(p); // ref 0; freed
    TEST_EXPECT_EQ(path_total_allocated() - a0, 1ull, "one alloc total");
    TEST_EXPECT_EQ(path_total_freed() - f0, 1ull, "freed on last drop");
    // NULL is a safe no-op for both.
    path_ref(NULL); path_unref(NULL);
}

// =============================================================================
// Part B -- the Spoor bridge.
// =============================================================================

void test_path_spoor_clone_shares(void) {
    u64 pa0 = path_total_allocated(), pf0 = path_total_freed();
    struct Spoor *c = spoor_alloc(&pathfix_dev);
    TEST_ASSERT(c != NULL, "spoor_alloc");
    c->path = path_make_root();                 // seed "/" (mimics an attach root)
    TEST_ASSERT(c->path != NULL, "seed root path");

    struct Spoor *nc = spoor_clone(c);          // SHARES c->path (incref)
    TEST_ASSERT(nc != NULL, "clone");
    TEST_ASSERT(nc->path == c->path, "clone shares the SAME Path object");
    TEST_EXPECT_EQ(path_total_allocated() - pa0, 1ull, "clone copies no string (one alloc)");

    spoor_clunk(nc);                            // drops one ref on the shared Path
    TEST_EXPECT_EQ(path_total_freed() - pf0, 0ull, "shared Path survives the clone's free");
    TEST_ASSERT(c->path != NULL, "parent still holds the Path");
    spoor_clunk(c);                             // last holder -> Path freed
    TEST_EXPECT_EQ(path_total_freed() - pf0, 1ull, "Path freed with its last Spoor");
}

void test_path_spoor_extend(void) {
    // Simulate a walk: root -> /a -> /a/b, then ..-pop back to /a.
    struct Spoor *root = spoor_alloc(&pathfix_dev);
    root->path = path_make_root();

    struct Spoor *a = spoor_clone(root);        // shares "/"
    spoor_path_extend(a, "a", 1);               // -> "/a"
    TEST_ASSERT(a->path && streq(a->path->s, "/a"), "extend root by a -> /a");

    struct Spoor *b = spoor_clone(a);           // shares "/a"
    spoor_path_extend(b, "b", 1);               // -> "/a/b"
    TEST_ASSERT(b->path && streq(b->path->s, "/a/b"), "extend /a by b -> /a/b");

    // ".." on a clone of b pops to "/a".
    struct Spoor *up = spoor_clone(b);
    spoor_path_extend(up, "..", 2);
    TEST_ASSERT(up->path && streq(up->path->s, "/a"), ".. pops /a/b -> /a");

    // "." keeps the shared parent path unchanged.
    struct Spoor *dot = spoor_clone(b);
    spoor_path_extend(dot, ".", 1);
    TEST_ASSERT(dot->path && streq(dot->path->s, "/a/b"), ". keeps /a/b");

    spoor_clunk(root); spoor_clunk(a); spoor_clunk(b); spoor_clunk(up); spoor_clunk(dot);
}

void test_path_spoor_transplant(void) {
    // A cross: the crossed clone takes the MOUNT-POINT's name, not the source's.
    struct Spoor *mountpoint = spoor_alloc(&pathfix_dev);
    mountpoint->path = path_make_root();
    spoor_path_extend(mountpoint, "mnt", 3);    // mountpoint name = "/mnt"
    TEST_ASSERT(streq(mountpoint->path->s, "/mnt"), "mount point is /mnt");

    struct Spoor *src = spoor_alloc(&pathfix_dev);
    src->path = path_make_root();               // the mount SOURCE's own name = "/"

    struct Spoor *crossed = spoor_clone(src);   // shares the source's "/"
    TEST_ASSERT(streq(crossed->path->s, "/"), "crossed clone starts at the source name");
    spoor_path_transplant(crossed, mountpoint); // stamp the mount-point's "/mnt"
    TEST_ASSERT(streq(crossed->path->s, "/mnt"), "transplant stamps the mount-point name");

    spoor_clunk(mountpoint); spoor_clunk(src); spoor_clunk(crossed);
}
