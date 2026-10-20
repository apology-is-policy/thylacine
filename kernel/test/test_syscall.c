// P3-Ec: SVC syscall dispatcher tests.
//
// Five tests exercise syscall_dispatch directly with synthetic
// exception_context (bypassing the EL0 entry path; the real EL0
// vector → ctx → syscall_dispatch chain is exercised end-to-end at
// P3-Ed once the ELF fixture lands).
//
//   syscall.dispatch_unknown
//     Unknown syscall number → ctx->regs[0] = -1; no extinction.
//
//   syscall.dispatch_puts_smoke
//     SYS_PUTS with valid args writes to UART, returns len. NULL buf,
//     oversized len, and zero len handled correctly.
//
//   syscall.dispatch_exits_ok
//     Child process calls syscall_dispatch with SYS_EXITS(0); parent
//     wait_pid observes exit_status=0.
//
//   syscall.dispatch_exits_fail
//     Child calls SYS_EXITS(42); parent observes exit_status=1
//     (v1.0 binary mapping: status==0 → "ok", non-zero → "fail").
//
//   syscall.dispatch_args_in_x0_to_x5
//     Validates that syscall_dispatch reads nr from x8 (ctx->regs[8])
//     and args from x0..x5 (ctx->regs[0..5]), per the AArch64 ABI.

#include "test.h"

#include "../../arch/arm64/exception.h"

#include <thylacine/dev.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len);
s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf, u64 len);
s64 sys_fstat_for_proc(struct Proc *p, u64 hraw, struct t_stat *out_k);

void test_syscall_dispatch_unknown(void);
void test_syscall_dispatch_puts_smoke(void);
void test_syscall_dispatch_exits_ok(void);
void test_syscall_dispatch_exits_fail(void);
void test_syscall_dispatch_exit_group_code(void);
void test_syscall_dispatch_args_in_x0_to_x5(void);
void test_syscall_dispatch_set_tid_address(void);
void test_syscall_opath_walkonly_no_byte_io(void);
void test_syscall_dispatch_symlink_reads(void);
void test_syscall_fstat_errnos(void);

void test_syscall_dispatch_unknown(void) {
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = 9999ull;       // not a real syscall

    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1,
        "unknown syscall must return -1 (ENOSYS-equivalent)");
}

void test_syscall_dispatch_puts_smoke(void) {
    // R7 F127 close (P3-H): SYS_PUTS rejects kernel-half VAs (>=
    // 0x0001_0000_0000_0000) to prevent EL0 callers from reading
    // kernel memory through TTBR1. v1.0 has no PAN, so the bound
    // check IS the privilege boundary on this surface. In-kernel
    // test callers (this function) pass kernel-rodata strings, so
    // the validation correctly rejects with -1 — the byte-count
    // path is exercised by /init's runtime SYS_PUTS("hello\n", 6)
    // call from EL0 with a user-VA pointer (0x10040). See
    // docs/reference/28-syscall.md for the full validation contract.
    static const char msg[] = "[syscall.puts test channel]\n";

    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = (u64)(uintptr_t)msg;       // kernel-half VA
    ctx.regs[1] = sizeof(msg) - 1;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1,
        "SYS_PUTS must reject kernel-half VAs (R7 F127)");

    // NULL buf rejected.
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0;
    ctx.regs[1] = 5;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1, "NULL buf rejected");

    // Oversized rejected (still rejected — len > 4096 short-circuits
    // before VA check, so any pointer value triggers the len reject).
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = (u64)(uintptr_t)msg;
    ctx.regs[1] = 8192;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1, "oversized len rejected");

    // Zero len returns 0 (no-op; short-circuits before VA check).
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = (u64)(uintptr_t)msg;
    ctx.regs[1] = 0;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ(ctx.regs[0], 0ull, "zero len → 0 return");

    // R7 F127: synthetic user-VA pointer must pass the bound check.
    // We can't dereference it (no real user mapping at this VA in
    // kernel context), so we use len=0 to short-circuit the read
    // loop — verifies only that the validator accepts user-half VAs.
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0x10040ull;                // user-half VA (matches /init)
    ctx.regs[1] = 0;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ(ctx.regs[0], 0ull,
        "SYS_PUTS accepts user-half VAs (R7 F127 positive case)");

    // R12-uaccess F210 close: SYS_PUTS now rejects any buf_va >=
    // UACCESS_USER_VA_TOP (= 1<<47), matching the uaccess fault
    // dispatcher's recovery range and burrow_map's USER_VA_TOP.
    // Pre-F210 the bound was 1<<48 — EL0 could pass buf_va in
    // [2^47, 2^48), pass the syscall's bound check, then take an
    // unrecoverable kernel translation fault inside uaccess_load_u8
    // (because the dispatcher's `fi.vaddr < UACCESS_USER_VA_TOP`
    // check fails at that range), extincting the kernel.

    // Boundary: exactly UACCESS_USER_VA_TOP rejected.
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0x0000800000000000ull;     // = 1<<47 = USER_VA_TOP
    ctx.regs[1] = 1;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1,
        "USER_VA_TOP itself is out-of-range (closed half-interval)");

    // F210 regression: mid-gap VA in [2^47, 2^48) rejected at the
    // syscall layer. Without the fix this passed the bound check
    // and extincted the kernel via the dispatcher's user-VA-half
    // check failing inside uaccess_load_u8's translation fault.
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0x4000000000000000ull;     // mid [2^47, 2^48)
    ctx.regs[1] = 1;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1,
        "F210: buf_va in [2^47, 2^48) must be syscall-layer rejected "
        "(would extinct kernel via uaccess fault pre-fix)");

    // F210 positive corner: the highest legal byte (last byte before
    // USER_VA_TOP) must still be accepted at the syscall layer. We
    // use len=0 so the validator path runs without dereferencing.
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0x00007FFFFFFFFFFFull;     // = USER_VA_TOP - 1
    ctx.regs[1] = 0;                          // zero-len short-circuit
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ(ctx.regs[0], 0ull,
        "F210: highest legal user byte (UACCESS_USER_VA_TOP - 1) "
        "accepted with len=0");
}

// Child for exits_ok: calls SYS_EXITS(0) via syscall_dispatch.
static void child_exits_ok(void *arg) {
    (void)arg;
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_EXITS;
    ctx.regs[0] = 0;             // status 0 → "ok"
    TEST_SYSCALL_DISPATCH(&ctx);
    extinction("syscall_dispatch(SYS_EXITS) returned (impossible)");
}

void test_syscall_dispatch_exits_ok(void) {
    int pid = rfork(RFPROC, child_exits_ok, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid returned the rfork'd pid");
    TEST_EXPECT_EQ(status, 0,
        "SYS_EXITS(0) → exit_status == 0 (\"ok\" mapping)");
}

// Child for exits_fail: calls SYS_EXITS(42) via syscall_dispatch.
static void child_exits_fail(void *arg) {
    (void)arg;
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_EXITS;
    ctx.regs[0] = 42;            // status 42 → exit_status 42 (#91)
    TEST_SYSCALL_DISPATCH(&ctx);
    extinction("syscall_dispatch(SYS_EXITS) returned (impossible)");
}

// #91 regression (single-thread exits_code path): a SYS_EXITS(42) child's
// specific byte survives to the parent's wait_pid, instead of the pre-#91
// collapse to exit_status 1. Fails on the old code (status == 1), passes now.
void test_syscall_dispatch_exits_fail(void) {
    int pid = rfork(RFPROC, child_exits_fail, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid returned the rfork'd pid");
    TEST_EXPECT_EQ(status, 42,
        "SYS_EXITS(42) → exit_status == 42 (#91: the real byte, not 0/1)");
}

// Child for exit_group_code: calls SYS_EXIT_GROUP(42) via syscall_dispatch.
// A single-thread Proc routes exit_group through proc_group_terminate_code
// (which stashes group_exit_code) then thread_exit_self (which reads it) --
// exercising the GROUP code path the exits_code test above does not.
static void child_exit_group_code(void *arg) {
    (void)arg;
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_EXIT_GROUP;
    ctx.regs[0] = 42;            // status 42 → group_exit_code 42 → exit_status 42
    TEST_SYSCALL_DISPATCH(&ctx);
    extinction("syscall_dispatch(SYS_EXIT_GROUP) returned (impossible)");
}

// #91 regression (group_exit_code path): SYS_EXIT_GROUP(42) reaches the
// parent's wait as exit_status 42. Fails on the old code (thread_exit_self
// collapsed group_exit_msg to 0/1 -> status 1), passes now.
void test_syscall_dispatch_exit_group_code(void) {
    int pid = rfork(RFPROC, child_exit_group_code, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid returned the rfork'd pid");
    TEST_EXPECT_EQ(status, 42,
        "SYS_EXIT_GROUP(42) → exit_status == 42 (#91: group_exit_code)");
}

void test_syscall_dispatch_args_in_x0_to_x5(void) {
    // R7 F127 close: validation rejects kernel-VA pointers. To verify
    // the AArch64 ABI register placement (x8=nr, x0..x5=args, x0=ret),
    // we use a user-VA buf with len=0: the validator accepts the user
    // VA, but len=0 short-circuits to a 0 return without dereferencing.
    // This exercises the reads of x0+x1 from ctx and the ignore of
    // x2..x5 + x9..x30, exactly matching the original test's intent.

    struct exception_context ctx;
    // Poison every register with a unique value.
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0xDEADBEEF00ull + (u64)i;
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0x10040ull;                // user-half VA
    ctx.regs[1] = 0;                          // zero-length → 0 return
    // x2..x5 stay poisoned — should be ignored by SYS_PUTS.
    // x9..x30 stay poisoned — must not affect SYS_PUTS.

    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ(ctx.regs[0], 0ull,
        "SYS_PUTS reads x0 + x1 from ctx; ignores x2..x7 + x9..x30");
}

// P6-pouch-kernel-auxv: SYS_SET_TID_ADDRESS returns the calling thread's
// tid — the calling Proc's pid at v1.0 (single-threaded Procs; the Linux
// thread-group-leader convention). The `tidptr` argument is accepted but
// not dereferenced (clear-child-tid semantics deferred — POUCH-DESIGN.md
// §12.4), so even a NULL tidptr neither faults nor changes the result.
void test_syscall_dispatch_set_tid_address(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL,
        "the test runs inside a Proc context");
    s64 expect_tid = (s64)t->proc->pid;

    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_SET_TID_ADDRESS;
    ctx.regs[0] = 0x10040ull;                // a plausible user-VA tidptr
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], expect_tid,
        "SYS_SET_TID_ADDRESS returns the calling thread's tid (== Proc pid)");

    // A NULL tidptr is accepted — set_tid_address never dereferences it.
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_SET_TID_ADDRESS;
    ctx.regs[0] = 0;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], expect_tid,
        "NULL tidptr is accepted (clear-child-tid deferred — §12.4)");
}

// #81: a T_OPATH (O_PATH) navigation handle carries CWALKONLY and must reject
// byte I/O -- sys_read/sys_write return -1 -- so the perm_check-exempt O_PATH
// open cannot be a read-bypass (it once leaked the 0400 /system.key). Drives the
// inner sys_*_for_proc (kernel-buf) directly: a normal devzero handle reads/writes,
// a CWALKONLY one is denied (the flag set on the Spoor before handle_alloc, exactly
// as the T_OPATH walk-open path does). The E2E (t_open(O_PATH)+t_read on
// /system.key -> -1) is the joey boot probe.
void test_syscall_opath_walkonly_no_byte_io(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "test thread has a proc");
    struct Proc *p = t->proc;
    u8 buf[4] = { 0, 0, 0, 0 };

    // Baseline: a normal devzero handle reads + writes (the gate is OFF).
    struct Spoor *z = devzero.attach("");
    TEST_ASSERT(z != NULL, "devzero attach (normal)");
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, z);
    TEST_ASSERT(fd >= 0, "alloc normal handle");
    TEST_EXPECT_EQ(sys_read_for_proc(p, fd, buf, 4), (s64)4, "normal handle reads");
    TEST_EXPECT_EQ(sys_write_for_proc(p, fd, buf, 4), (s64)4, "normal handle writes");
    handle_close(p, fd);

    // #81: a CWALKONLY (T_OPATH) handle rejects read + write + zero-length read.
    struct Spoor *zw = devzero.attach("");
    TEST_ASSERT(zw != NULL, "devzero attach (walkonly)");
    zw->flag |= CWALKONLY;
    hidx_t wfd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, zw);
    TEST_ASSERT(wfd >= 0, "alloc walkonly handle");
    // #100 (ER-3): EBADF -- Linux answers EBADF for read/write on an O_PATH
    // descriptor, and the #81 gate is exactly that condition.
    TEST_EXPECT_EQ(sys_read_for_proc(p, wfd, buf, 4), (s64)(-T_E_BADF),
                   "#81: O_PATH (CWALKONLY) read DENIED with -T_E_BADF");
    TEST_EXPECT_EQ(sys_write_for_proc(p, wfd, buf, 4), (s64)(-T_E_BADF),
                   "#81: O_PATH (CWALKONLY) write DENIED with -T_E_BADF");
    TEST_EXPECT_EQ(sys_read_for_proc(p, wfd, buf, 0), (s64)(-T_E_BADF),
                   "#81: O_PATH zero-length read DENIED in the inner helper "
                   "(the syscall-entry len==0 fast-path is covered by the joey E2E -- #81 F1)");
    handle_close(p, wfd);
}

// Vote 2026-10-20: SYS_READLINK (135) and SYS_LSTAT (136) reach their handlers.
// An unknown number answers -1; each handler answers -T_E_INVAL for an empty
// path before it reads any memory.
void test_syscall_dispatch_symlink_reads(void) {
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_READLINK;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-T_E_INVAL,
        "SYS_READLINK reaches its handler (an empty path is EINVAL)");

    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_LSTAT;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-T_E_INVAL,
        "SYS_LSTAT reaches its handler (an empty path is EINVAL)");
}

// SYS_FSTAT's errnos (ERRORS.md: a bad or wrong-kind handle is -T_E_BADF). The
// Dev's refusal crosses the way stalk's err_code maps it, so stat and fstat
// agree: a -errno in [-4095,-2] as itself, anything else as -T_E_IO.
static int g_fstat_dev_rc;

static int fstat_test_stat_native(struct Spoor *c, struct t_stat *out) {
    (void)c;
    for (u64 i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;
    if (g_fstat_dev_rc != 0) return g_fstat_dev_rc;
    out->size = 42;
    return 0;
}

static struct Dev g_fstat_test_dev = {
    .dc          = 'F',
    .name        = "fstattest",
    .stat_native = fstat_test_stat_native,
};

static const struct { int dev; s64 want; const char *what; } g_fstat_dev_rows[] = {
    { -T_E_ACCES, -(s64)T_E_ACCES, "fstat: a Dev's -errno crosses as itself" },
    { -2,         -2,              "fstat: the band's top, -2, crosses" },
    { -4095,      -4095,           "fstat: the band's floor, -4095, crosses" },
    { -1,         -(s64)T_E_IO,    "fstat: a Dev's flat -1 reads EIO" },
    { -T_E_BADF,  -(s64)T_E_IO,    "fstat: a Dev's EBADF does not pass for a dead handle" },
    { -4096,      -(s64)T_E_IO,    "fstat: a value below the band reads EIO" },
    { 7,          -(s64)T_E_IO,    "fstat: a positive Dev failure reads EIO" },
};

// The assertions, apart from the handles: a failed TEST_ASSERT returns from
// here, and the caller still closes what it opened.
static void fstat_errnos_cases(struct Proc *p, hidx_t live, hidx_t dead,
                               hidx_t other_kind, hidx_t statless) {
    struct t_stat ks;
    g_fstat_dev_rc = 0;
    TEST_EXPECT_EQ(sys_fstat_for_proc(p, (u64)live, &ks), (s64)0,
        "fstat: a live WRITE-only handle stats (no right is required)");
    TEST_EXPECT_EQ(ks.size, (u64)42, "fstat: the Dev's record comes back");
    for (u64 i = 0; i < sizeof(g_fstat_dev_rows) / sizeof(g_fstat_dev_rows[0]); i++) {
        g_fstat_dev_rc = g_fstat_dev_rows[i].dev;
        TEST_EXPECT_EQ(sys_fstat_for_proc(p, (u64)live, &ks),
                       g_fstat_dev_rows[i].want, g_fstat_dev_rows[i].what);
    }
    g_fstat_dev_rc = 0;

    TEST_EXPECT_EQ(sys_fstat_for_proc(p, (u64)dead, &ks), -(s64)T_E_BADF,
        "fstat: a closed handle is EBADF");
    TEST_EXPECT_EQ(sys_fstat_for_proc(p, (u64)-1, &ks), -(s64)T_E_BADF,
        "fstat: handle -1 is EBADF");
    TEST_EXPECT_EQ(sys_fstat_for_proc(p, (u64)1 << 20, &ks), -(s64)T_E_BADF,
        "fstat: an index past the table is EBADF");
    TEST_EXPECT_EQ(sys_fstat_for_proc(p, (u64)other_kind, &ks), -(s64)T_E_BADF,
        "fstat: a handle of another kind is EBADF");
    TEST_EXPECT_EQ(sys_fstat_for_proc(p, (u64)statless, &ks), -(s64)T_E_IO,
        "fstat: a Dev with no stat_native reads EIO");

    // Through the dispatcher: the handler's own refusals. The handle lookup
    // runs before any store, so a closed handle is EBADF with a valid-range VA.
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_FSTAT;
    ctx.regs[0] = (u64)live;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], -(s64)T_E_FAULT,
        "SYS_FSTAT: a NULL stat_va is EFAULT");
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_FSTAT;
    ctx.regs[0] = (u64)dead;
    ctx.regs[1] = 0x10000;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], -(s64)T_E_BADF,
        "SYS_FSTAT: a closed handle is EBADF");

    // kproc's user half is empty, so a valid-range destination passes the
    // range check and faults at the first store: the copy-out arm.
    TEST_ASSERT(p == kproc(), "fstat: the copy-out case runs on kproc");
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_FSTAT;
    ctx.regs[0] = (u64)live;
    ctx.regs[1] = 0x10000000ull;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], -(s64)T_E_FAULT,
        "SYS_FSTAT: a fault in the copy-out is EFAULT");
}

void test_syscall_fstat_errnos(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "test thread has a proc");
    struct Proc *p = t->proc;

    hidx_t live = -1, other_kind = -1, statless = -1;
    struct Spoor *s = spoor_alloc(&g_fstat_test_dev);
    if (s) {
        s->qid.type = QTFILE;
        live = handle_alloc(p, KOBJ_SPOOR, RIGHT_WRITE, s);
        if (live < 0) spoor_clunk(s);
    }
    other_kind = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ, NULL);
    struct Spoor *z = devzero.attach("");
    if (z) {
        statless = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, z);
        if (statless < 0) spoor_clunk(z);
    }
    // A freed index: allocated last and closed at once, so nothing reuses it.
    hidx_t dead = -1;
    struct Spoor *d = spoor_alloc(&g_fstat_test_dev);
    if (d) {
        dead = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, d);
        if (dead < 0) spoor_clunk(d);
        else          handle_close(p, dead);
    }

    if (live >= 0 && other_kind >= 0 && statless >= 0 && dead >= 0)
        fstat_errnos_cases(p, live, dead, other_kind, statless);
    else
        test_fail("fstat: fixture allocation failed");

    if (live >= 0)       handle_close(p, live);
    if (other_kind >= 0) handle_close(p, other_kind);
    if (statless >= 0)   handle_close(p, statless);
}
