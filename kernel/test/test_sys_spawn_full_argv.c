// P6-pouch-stratumd-boot 16b-alpha — kernel-internal tests for
// SYS_SPAWN_FULL_ARGV.
//
// SYS_SPAWN_FULL_ARGV is the combined spawn primitive that subsumes the
// SYS_SPAWN_* family and adds argv pass-through. The kernel reads a
// struct sys_spawn_args from user memory; this test exercises the
// internal sys_spawn_full_argv_for_proc body directly (kernel-resident
// args; bypasses the uaccess layer the handler runs).
//
// Coverage:
//
//   sys_spawn_full_argv.no_argv_acts_as_spawn_with_perms
//     argc == 0 + argv_data_len == 0 — Shape A; identical to
//     SYS_SPAWN_WITH_PERMS for the same (name, fds, cap_mask, perm_flags)
//     inputs. The child's initial frame is the legacy 144-byte shape.
//
//   sys_spawn_full_argv.golden_argc4
//     argc == 4 + 4 NUL-terminated strings — Shape B. Child spawns and
//     exits clean. The full argv delivery is exercised by the userspace
//     /pouch-hello-argv probe; this test only proves the spawn path
//     succeeds + the child reaches a clean exit (no panic in
//     exec_build_init_stack under Shape B).
//
//   sys_spawn_full_argv.rejects_argc_over_max
//     argc > SYS_SPAWN_ARGV_MAX → -1. Defense-in-depth at the body;
//     handler-side check is mirrored here.
//
//   sys_spawn_full_argv.rejects_data_len_over_max
//     argv_data_len > SYS_SPAWN_ARGV_DATA_MAX → -1.
//
//   sys_spawn_full_argv.rejects_missing_trailing_nul
//     argv_data without a trailing NUL → -1.
//
//   sys_spawn_full_argv.rejects_nul_count_mismatch
//     argv_data with N NULs but argc != N → -1. Both N > argc (more NULs
//     than expected) and N < argc (fewer NULs than expected) are checked.
//
//   sys_spawn_full_argv.rejects_argc_with_zero_data_len
//     argc > 0 but argv_data_len == 0 → -1.
//
//   sys_spawn_full_argv.rejects_zero_argc_with_nonzero_data
//     argc == 0 but argv_data_len > 0 → -1.
//
// The verification pattern matches the existing test_sys_spawn_with_
// perms suite: spawn → wait_pid → assert status (golden); spawn → assert
// negative return (negative).

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern int sys_spawn_full_argv_for_proc(struct Proc *p,
                                        const char *name, size_t name_len,
                                        const char *argv_data, u32 argv_data_len,
                                        u32 argc,
                                        const u32 *fds, u32 fd_count,
                                        caps_t cap_mask, u32 perm_flags);
// R1 F1 fix: handler-side field-bound validation extracted as an
// internal helper so tests can exercise the handler's distinctive
// checks (_pad_envp != 0, perm_flags & ~ALL, oversize fields) without
// SVC instrumentation or a user-VA fixture.
extern int sys_spawn_full_argv_validate_req(const struct sys_spawn_args *req);

void test_sys_spawn_full_argv_no_argv_acts_as_spawn_with_perms(void);
void test_sys_spawn_full_argv_golden_argc4(void);
void test_sys_spawn_full_argv_rejects_argc_over_max(void);
void test_sys_spawn_full_argv_rejects_data_len_over_max(void);
void test_sys_spawn_full_argv_rejects_missing_trailing_nul(void);
void test_sys_spawn_full_argv_rejects_nul_count_mismatch(void);
void test_sys_spawn_full_argv_rejects_argc_with_zero_data_len(void);
void test_sys_spawn_full_argv_rejects_zero_argc_with_nonzero_data(void);
// R1 F1 + F11 fixes:
void test_sys_spawn_full_argv_validate_req_golden(void);
void test_sys_spawn_full_argv_validate_req_rejects_pad_envp(void);
void test_sys_spawn_full_argv_validate_req_rejects_unknown_perm_bits(void);
void test_sys_spawn_full_argv_validate_req_rejects_oversize_fields(void);
void test_sys_spawn_full_argv_rejects_non_console_attached_perm_flags(void);

static void drain_zombies(void) {
    int status = 0;
    while (wait_pid(&status) > 0) { /* drain */ }
}

void test_sys_spawn_full_argv_no_argv_acts_as_spawn_with_perms(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // argc=0 + argv_data_len=0 — Shape A; child gets the legacy
    // single-NULL argv[] terminator + envp terminator + auxv. /hello is
    // a libt binary whose _start ignores argc/argv, so the exit-clean
    // path is sufficient evidence the kernel side built the legacy frame
    // correctly through the new entry.
    int pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                           /*argv_data=*/NULL,
                                           /*argv_data_len=*/0u,
                                           /*argc=*/0u,
                                           /*fds=*/NULL, /*fd_count=*/0u,
                                           CAP_NONE, /*perm_flags=*/0u);
    TEST_ASSERT(pid > 0, "spawn_full_argv (argc=0) -> positive pid");
    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the child");
    TEST_EXPECT_EQ(status, 0, "/hello exits clean under Shape A");
}

void test_sys_spawn_full_argv_golden_argc4(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // argc=4 — Shape B. The full argv-echo round-trip is exercised by
    // /pouch-hello-argv in joey; this test only proves the spawn body's
    // argv path is sound under a realistic shape (4 strings, mixed
    // lengths). /hello ignores argv so we just need clean exit; the
    // frame-build correctness for Shape B is the load-bearing claim.
    static const char argv_data[] = "hello\0one\0two\0three";
    // sizeof includes the implicit trailing NUL of the last string =>
    // 6 + 4 + 4 + 6 = 20 bytes, NUL count = 4 = argc.
    int pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                           argv_data, sizeof(argv_data),
                                           /*argc=*/4u,
                                           NULL, 0u, CAP_NONE, 0u);
    TEST_ASSERT(pid > 0, "spawn_full_argv (argc=4) -> positive pid");
    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the child");
    TEST_EXPECT_EQ(status, 0, "/hello exits clean under Shape B");
}

void test_sys_spawn_full_argv_rejects_argc_over_max(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // argc = SYS_SPAWN_ARGV_MAX + 1 — out-of-bound.
    static const char argv_data[] = "x\0";
    int pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                           argv_data, sizeof(argv_data),
                                           SYS_SPAWN_ARGV_MAX + 1u,
                                           NULL, 0u, CAP_NONE, 0u);
    TEST_EXPECT_EQ(pid, -1, "argc > SYS_SPAWN_ARGV_MAX rejected");
}

void test_sys_spawn_full_argv_rejects_data_len_over_max(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // argv_data_len > SYS_SPAWN_ARGV_DATA_MAX. We don't actually
    // populate a buffer that large; the body's first-check rejects
    // before reading argv_data, so a non-NULL but unsized pointer is
    // adequate. The body should NOT read past the validated len.
    static const char argv_data[] = "x\0";
    int pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                           argv_data,
                                           SYS_SPAWN_ARGV_DATA_MAX + 1u,
                                           /*argc=*/1u,
                                           NULL, 0u, CAP_NONE, 0u);
    TEST_EXPECT_EQ(pid, -1, "argv_data_len > SYS_SPAWN_ARGV_DATA_MAX rejected");
}

void test_sys_spawn_full_argv_rejects_missing_trailing_nul(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // "abc" + "def" with no trailing NUL — last byte is 'f', not '\0'.
    // argv_data_len = 7 (3 + 1 + 3); NULs in buffer = 1; final byte not NUL.
    static const char argv_data[] = "abc\0def";  // sizeof = 8 (includes
                                                  // implicit trailing \0)
    // Pass len = 7 (excluding the implicit trailing NUL) so the buffer
    // ends with 'f' from the kernel's view.
    int pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                           argv_data, /*len=*/7u,
                                           /*argc=*/2u,
                                           NULL, 0u, CAP_NONE, 0u);
    TEST_EXPECT_EQ(pid, -1, "argv_data not NUL-terminated rejected");
}

void test_sys_spawn_full_argv_rejects_nul_count_mismatch(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // argv_data = "a\0b\0c\0" — 3 NULs (one per string + the implicit
    // terminator). Caller declares argc=2 (off-by-one).
    static const char argv_data[] = "a\0b\0c";  // sizeof = 6 (3 NULs)
    int pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                           argv_data, sizeof(argv_data),
                                           /*argc=*/2u,
                                           NULL, 0u, CAP_NONE, 0u);
    TEST_EXPECT_EQ(pid, -1, "NUL count > argc rejected");

    // argv_data = "a\0b\0c\0" — 3 NULs. Caller declares argc=4 (too many).
    pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                       argv_data, sizeof(argv_data),
                                       /*argc=*/4u,
                                       NULL, 0u, CAP_NONE, 0u);
    TEST_EXPECT_EQ(pid, -1, "NUL count < argc rejected");
}

void test_sys_spawn_full_argv_rejects_argc_with_zero_data_len(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                           /*argv_data=*/NULL,
                                           /*argv_data_len=*/0u,
                                           /*argc=*/1u,
                                           NULL, 0u, CAP_NONE, 0u);
    TEST_EXPECT_EQ(pid, -1, "argc > 0 with argv_data_len == 0 rejected");
}

void test_sys_spawn_full_argv_rejects_zero_argc_with_nonzero_data(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    static const char argv_data[] = "x";  // 2 bytes incl trailing \0
    int pid = sys_spawn_full_argv_for_proc(t->proc, "hello", 5,
                                           argv_data, sizeof(argv_data),
                                           /*argc=*/0u,
                                           NULL, 0u, CAP_NONE, 0u);
    TEST_EXPECT_EQ(pid, -1, "argc == 0 with argv_data_len > 0 rejected");
}

// R1 F1 fix: cover the handler's distinctive field-bound + _pad_envp
// + perm_flags-bits validation via the extracted validate_req helper.
// The body's tests above exercise the body's invariants; these tests
// exercise the handler's.

void test_sys_spawn_full_argv_validate_req_golden(void) {
    struct sys_spawn_args req = {
        .name_va       = 0x1000,
        .argv_data_va  = 0,
        .fd_list_va    = 0,
        .name_len      = 5,
        .argv_data_len = 0,
        .argc          = 0,
        .fd_count      = 0,
        .perm_flags    = 0,
        ._pad_envp     = 0,
        .cap_mask      = 0,
    };
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), 0,
        "golden no-argv req passes validate");
}

void test_sys_spawn_full_argv_validate_req_rejects_pad_envp(void) {
    struct sys_spawn_args req = {
        .name_va       = 0x1000,
        .name_len      = 5,
        ._pad_envp     = 0xDEADBEEF,
    };
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), -1,
        "_pad_envp != 0 rejected (forward-compat poison-pattern)");
}

void test_sys_spawn_full_argv_validate_req_rejects_unknown_perm_bits(void) {
    struct sys_spawn_args req = {
        .name_va       = 0x1000,
        .name_len      = 5,
        .perm_flags    = SPAWN_PERM_ALL | (1u << 16),  // unknown high bit
    };
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), -1,
        "perm_flags with bits outside SPAWN_PERM_ALL rejected");
}

void test_sys_spawn_full_argv_validate_req_rejects_oversize_fields(void) {
    struct sys_spawn_args req = {
        .name_va       = 0x1000,
        .name_len      = SYS_SPAWN_NAME_MAX + 1u,
    };
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), -1,
        "name_len > SYS_SPAWN_NAME_MAX rejected");

    req.name_len      = 5;
    req.argv_data_len = SYS_SPAWN_ARGV_DATA_MAX + 1u;
    req.argc          = 1;
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), -1,
        "argv_data_len > SYS_SPAWN_ARGV_DATA_MAX rejected");

    req.argv_data_len = 4;
    req.argc          = SYS_SPAWN_ARGV_MAX + 1u;
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), -1,
        "argc > SYS_SPAWN_ARGV_MAX rejected");

    req.argc          = 1;
    req.fd_count      = SYS_SPAWN_MAX_FDS + 1u;
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), -1,
        "fd_count > SYS_SPAWN_MAX_FDS rejected");

    // R1 F4 fix coverage: argc > 0 with argv_data_len == 0 rejected
    // at the helper.
    struct sys_spawn_args req2 = {
        .name_va       = 0x1000,
        .name_len      = 5,
        .argv_data_len = 0,
        .argc          = 1,
    };
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req2), -1,
        "argc > 0 with argv_data_len == 0 rejected");

    // Symmetric: argc == 0 with argv_data_len > 0.
    struct sys_spawn_args req3 = {
        .name_va       = 0x1000,
        .name_len      = 5,
        .argv_data_len = 4,
        .argc          = 0,
    };
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req3), -1,
        "argc == 0 with argv_data_len > 0 rejected");
}

// R1 F11 fix: SYS_SPAWN_FULL_ARGV has its own copy of the console-
// attached gate (in sys_spawn_full_argv_for_proc); if a future
// refactor accidentally dropped it while leaving the SYS_SPAWN_WITH_
// PERMS one intact, no test in this suite would catch it.
//
// The gate test must work regardless of whether kproc has been marked
// console-attached by an earlier test in the suite. Mirrors
// test_sys_spawn_with_perms_rejects_non_console_attached_parent's
// pattern: allocate a fresh throwaway Proc (which is never console-
// attached by default), invoke the gate from that Proc's context, then
// dispose. This is independent of kproc's flag state.
void test_sys_spawn_full_argv_rejects_non_console_attached_perm_flags(void) {
    drain_zombies();
    struct Proc *unprivileged = proc_alloc();
    TEST_ASSERT(unprivileged != NULL, "proc_alloc fresh unprivileged Proc");
    TEST_ASSERT(!proc_is_console_attached(unprivileged),
        "fresh Proc is not console-attached");

    int rc = sys_spawn_full_argv_for_proc(unprivileged, "hello", 5,
                                          NULL, 0u, 0u,
                                          NULL, 0u,
                                          CAP_NONE,
                                          SPAWN_PERM_MAY_POST_SERVICE);
    TEST_EXPECT_EQ(rc, -1,
        "perm_flags set + parent not console-attached -> -1");

    // Clean up the throwaway Proc.
    unprivileged->state = PROC_STATE_ZOMBIE;
    proc_free(unprivileged);
}
