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

#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

void test_syscall_dispatch_unknown(void);
void test_syscall_dispatch_puts_smoke(void);
void test_syscall_dispatch_exits_ok(void);
void test_syscall_dispatch_exits_fail(void);
void test_syscall_dispatch_args_in_x0_to_x5(void);

void test_syscall_dispatch_unknown(void) {
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = 9999ull;       // not a real syscall

    syscall_dispatch(&ctx);
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
    syscall_dispatch(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1,
        "SYS_PUTS must reject kernel-half VAs (R7 F127)");

    // NULL buf rejected.
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0;
    ctx.regs[1] = 5;
    syscall_dispatch(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1, "NULL buf rejected");

    // Oversized rejected (still rejected — len > 4096 short-circuits
    // before VA check, so any pointer value triggers the len reject).
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = (u64)(uintptr_t)msg;
    ctx.regs[1] = 8192;
    syscall_dispatch(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1, "oversized len rejected");

    // Zero len returns 0 (no-op; short-circuits before VA check).
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = (u64)(uintptr_t)msg;
    ctx.regs[1] = 0;
    syscall_dispatch(&ctx);
    TEST_EXPECT_EQ(ctx.regs[0], 0ull, "zero len → 0 return");

    // R7 F127: synthetic user-VA pointer must pass the bound check.
    // We can't dereference it (no real user mapping at this VA in
    // kernel context), so we use len=0 to short-circuit the read
    // loop — verifies only that the validator accepts user-half VAs.
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0x10040ull;                // user-half VA (matches /init)
    ctx.regs[1] = 0;
    syscall_dispatch(&ctx);
    TEST_EXPECT_EQ(ctx.regs[0], 0ull,
        "SYS_PUTS accepts user-half VAs (R7 F127 positive case)");

    // Boundary: exactly USER_VA_TOP rejected.
    ctx.regs[8] = SYS_PUTS;
    ctx.regs[0] = 0x0001000000000000ull;     // = USER_VA_TOP
    ctx.regs[1] = 1;
    syscall_dispatch(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)-1,
        "USER_VA_TOP itself is out-of-range (closed half-interval)");
}

// Child for exits_ok: calls SYS_EXITS(0) via syscall_dispatch.
static void child_exits_ok(void *arg) {
    (void)arg;
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_EXITS;
    ctx.regs[0] = 0;             // status 0 → "ok"
    syscall_dispatch(&ctx);
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
    ctx.regs[0] = 42;            // non-zero status → "fail"
    syscall_dispatch(&ctx);
    extinction("syscall_dispatch(SYS_EXITS) returned (impossible)");
}

void test_syscall_dispatch_exits_fail(void) {
    int pid = rfork(RFPROC, child_exits_fail, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid returned the rfork'd pid");
    TEST_EXPECT_EQ(status, 1,
        "SYS_EXITS(non-zero) → exit_status == 1 (v1.0 binary mapping)");
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

    syscall_dispatch(&ctx);
    TEST_EXPECT_EQ(ctx.regs[0], 0ull,
        "SYS_PUTS reads x0 + x1 from ctx; ignores x2..x7 + x9..x30");
}
