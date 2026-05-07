// Userspace syscall dispatch implementation (P3-Ec).
//
// At v1.0 P3-Ec the syscall surface is intentionally tiny — exits +
// puts. Just enough for an EL0 thread to signal "I ran, and here's the
// result" to the kernel test harness. Phase 5+ adds the full syscall
// surface; each syscall lands in its own TU.

#include <thylacine/syscall.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/exception.h"
#include "../arch/arm64/uart.h"

// =============================================================================
// SYS_EXITS — terminate calling process.
// =============================================================================
//
// AArch64 ABI: x0 = exit status (0 → "ok"; non-zero → "fail").
//
// At v1.0 P3-Ec we map the integer status to the existing kernel
// exits() string-based convention:
//
//   x0 == 0  → exits("ok")    → p->exit_status = 0
//   x0 != 0  → exits("fail")  → p->exit_status = 1
//
// Phase 5+ extends to a richer per-Proc exit_status u64 carrying the
// full integer payload.
//
// exits() is __attribute__((noreturn)); this helper inherits the
// no-return semantics. The user thread context is abandoned (its
// kernel stack with the exception_context on it stays around until
// wait_pid reaps via thread_free).
__attribute__((noreturn))
static void sys_exits_handler(u64 status) {
    if (status == 0) {
        exits("ok");
    } else {
        exits("fail");
    }
    // Unreachable — exits() is noreturn.
    extinction("sys_exits returned");
}

// =============================================================================
// SYS_PUTS — write `len` bytes to UART.
// =============================================================================
//
// AArch64 ABI: x0 = pointer to bytes; x1 = length.
//
// v1.0 sanity bounds:
//   - len <= 4096 (one page; reject larger as obvious garbage / reserved
//     for Phase 5+ where userspace uses iovec for larger writes).
//   - buf NULL rejected.
//   - buf + len must lie entirely within the user-VA half (TTBR0 range,
//     low VAs) — see SYS_PUTS_USER_VA_TOP. Closes R7 F127: without this,
//     EL0 can pass a kernel-half VA (TTBR1 range) and the kernel's
//     dereference walks via TTBR1 → reads kernel memory → leaks bytes
//     out the UART. PAN/SPAN are not configured at v1.0; the bound
//     check is the privilege boundary on this surface.
//
// Bytes are written one at a time via uart_putc. v1.0 doesn't validate
// the buffer's VMA presence within user-VA — if buf points outside any
// VMA, the read faults in the demand-paging path; if no VMA covers it,
// userland_demand_page returns FAULT_UNHANDLED_USER and the kernel
// extincts at exception_sync_lower_el. Phase 5+ adds copy_from_user-
// style validators that translate the fault into a -EFAULT return.

// User-VA top bound for syscall pointer validation. ARM64 4-KiB granule
// at v1.0 uses 48-bit VAs; user half is TTBR0-anchored at low VA. The
// EXEC_USER_STACK_TOP (0x8000_0000) sits well below this bound. Any VA
// >= USER_VA_TOP is in the kernel-VA-only region (TTBR1 range or
// reserved hole) and MUST NOT be dereferenced from a syscall handler.
#define SYS_PUTS_USER_VA_TOP    0x0001000000000000ull

static s64 sys_puts_handler(u64 buf_va, u64 len) {
    if (len == 0)            return 0;
    if (len > 4096)          return -1;
    if (buf_va == 0)         return -1;

    // R7 F127 close: reject kernel-half VA arguments. Overflow-safe:
    // if buf_va + len wraps past UINT64_MAX, that's also a reject.
    if (buf_va >= SYS_PUTS_USER_VA_TOP)              return -1;
    if (buf_va + len < buf_va)                        return -1;
    if (buf_va + len > SYS_PUTS_USER_VA_TOP)          return -1;

    const char *buf = (const char *)(uintptr_t)buf_va;
    for (u64 i = 0; i < len; i++) {
        uart_putc(buf[i]);
    }
    return (s64)len;
}

// =============================================================================
// Dispatch entry.
// =============================================================================

void syscall_dispatch(struct exception_context *ctx) {
    u64 nr = ctx->regs[8];

    switch (nr) {
    case SYS_EXITS:
        // Never returns. Kernel exits() → sched() picks another thread.
        // The exception_context stays on the EXITING thread's kstack
        // until wait_pid → thread_free.
        sys_exits_handler(ctx->regs[0]);

    case SYS_PUTS:
        ctx->regs[0] = (u64)sys_puts_handler(ctx->regs[0], ctx->regs[1]);
        return;

    default:
        // Unknown syscall. Phase 5+ delivers SIGSYS-equivalent note;
        // v1.0 returns -1 (ENOSYS) and lets userspace decide.
        ctx->regs[0] = (u64)(s64)-1;
        return;
    }
}
