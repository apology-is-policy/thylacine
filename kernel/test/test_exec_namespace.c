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

#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern struct Spoor *exec_resolve_from_namespace(struct Proc *p, const char *name,
                                                 size_t name_len, size_t *size_out);

void test_exec_ns_resolve_absolute_ok(void);
void test_exec_ns_resolve_relative_ok(void);
void test_exec_ns_miss_returns_null(void);
void test_exec_ns_non_executable_denied(void);

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
