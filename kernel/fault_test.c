// Deliberate-fault test stubs (P1-I).
//
// One of these provokers is invoked from `boot_main` BEFORE
// "Thylacine boot OK" when the kernel is built with
// `THYLACINE_FAULT_TEST=<variant>`. Each provoker is a deliberate
// trigger of one v1.0 hardening protection so tools/test-fault.sh
// can observe a specific EXTINCTION: message and PASS on that.
//
// In a production build (THYLACINE_FAULT_TEST unset) `fault_test_run`
// is a no-op — the file compiles to a single bare return.
//
// Per ARCHITECTURE.md §24 (hardening) + ROADMAP §4.2 exit criteria
// (deliberate-fault verification of canaries / W^X / BTI).

#include "../arch/arm64/uart.h"
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// ---------------------------------------------------------------------------
// canary_smash — write past stack-array bounds; canary check fires.
//
// `-fstack-protector-strong` arms the canary because `buf` is an array.
// At entry the function reads __stack_chk_guard and stores it just
// below the saved frame pointer; the loop overwrites that slot; at
// exit the function reads __stack_chk_guard again and compares —
// mismatch → __stack_chk_fail → extinction("stack canary mismatch").
//
// `volatile` on the loop counter prevents the compiler from optimising
// the obvious out-of-bounds write away under -O2.
// ---------------------------------------------------------------------------

#ifdef THYLACINE_FAULT_TEST_canary_smash
// Launder the pointer through inline asm so clang loses track of
// `p == buf` and can't reason "writes beyond buf[15] are UB so I
// can elide them." The empty asm with "+r"(p) constraint forces
// a register reload of an "unknown" pointer; subsequent OOB writes
// are emitted faithfully.
//
// `volatile char *` ALONE is insufficient — clang at -O2 happily
// folds the volatile writes when it can prove the underlying object
// is `buf` and the indices exceed bounds. The asm laundering is the
// classic anti-DCE-on-UB pattern (Linux uses it for fault-injection
// in arch/arm64/include/asm/uaccess.h's barriers; we use it for the
// same reason here).
__attribute__((noinline))
static char provoke_canary_smash(void) {
    char buf[16];
    char *p = buf;
    __asm__ __volatile__("" : "+r"(p));    // launder; clang loses track
    for (unsigned i = 0; i < 64; i++) {
        p[i] = (char)0xAA;
    }
    return p[0];
}
#endif

// ---------------------------------------------------------------------------
// wxe_violation — write to .text (mapped RO + executable). The MMU's
// W^X enforcement (PTE_AP_RO_EL1 on PTE_KERN_TEXT) raises a
// permission fault; the existing exception handler recognises FAR
// inside [_kernel_start, _kernel_end) AND fsc_is_permission(fsc) and
// emits extinction("PTE violates W^X (kernel image)").
// ---------------------------------------------------------------------------

#ifdef THYLACINE_FAULT_TEST_wxe_violation
extern char _kernel_start[];

__attribute__((noinline))
__attribute__((no_stack_protector))
static void provoke_wxe_violation(void) {
    volatile char *target = _kernel_start;
    *target = (char)0xCC;
}
#endif

// ---------------------------------------------------------------------------
// bti_fault — indirect branch to a target without a `bti` landing pad.
//
// SCTLR_EL1.BT0=1 (set in start.S), kernel-text pages have PTE_GP=1
// (PTE_KERN_TEXT), and `-mbranch-protection=bti` is on; the compiler
// emits `bti c` at every C function's prologue. We define a hand-
// rolled asm target whose first instruction is `nop` (NOT `bti`).
// Calling it via a function-pointer (which lowers to `blr`) sets
// PSTATE.BTYPE = 01; the target's first instruction is not a matching
// `bti c` / `bti jc`; ARM raises a Branch Target Exception
// (ESR_EL1.EC = 0x0D), which exception_sync_curr_el now recognises
// and emits extinction("BTI fault (...)").
//
// The target lives in `.text` so it inherits PTE_GP=1 from the
// kernel-text mapping. If `.text` ever gets split or some target is
// emitted without GP=1, the test would silently pass (BTI not
// enforced) — which is itself a regression worth catching, but the
// catch would be elsewhere.
// ---------------------------------------------------------------------------

#ifdef THYLACINE_FAULT_TEST_bti_fault
__asm__ (
    ".section .text, \"ax\"\n"
    ".globl _bti_unguarded_target\n"
    ".type _bti_unguarded_target, %function\n"
    "_bti_unguarded_target:\n"
    "    nop\n"          // first instruction is NOT bti j/c/jc
    "    ret\n"
    ".size _bti_unguarded_target, . - _bti_unguarded_target\n"
);

extern void _bti_unguarded_target(void);

__attribute__((noinline))
__attribute__((no_stack_protector))   // canary would fire first otherwise
static void provoke_bti_fault(void) {
    // `volatile` on the function pointer prevents clang from
    // devirtualising the call to a direct `bl _bti_unguarded_target`.
    // BL doesn't set PSTATE.BTYPE, so BTI wouldn't fire — only BLR
    // (indirect call) sets BTYPE=01 at the target. The volatile
    // forces a load+blr sequence.
    void (*volatile fp)(void) = _bti_unguarded_target;
    fp();   // blr → BTYPE=01 → target's `nop` doesn't match → BTI fault
}
#endif

// ---------------------------------------------------------------------------
// kstack_overflow — recurse on a thread_create'd kstack until SP runs
// off the bottom of the 16 KiB usable region into the 16 KiB guard.
// The guard pages are mapped no-access (P2-Dc); the next memory access
// raises a data abort with FAR inside the guard region. The exception
// handler's addr_is_stack_guard() recognizes the FAR is inside the
// current thread's kstack guard and emits "kernel stack overflow".
//
// Each recursion frame allocates a 1 KiB volatile array; ~16 frames
// chew through the 16 KiB usable stack and SP drops into the guard.
// `volatile` blocks any inlining or DCE; touching the array each call
// forces the actual stack store.
// ---------------------------------------------------------------------------

#ifdef THYLACINE_FAULT_TEST_kstack_overflow
__attribute__((noinline))
__attribute__((no_stack_protector))
static void recurse_into_guard(unsigned depth) {
    volatile char buf[1024];
    buf[0] = (char)depth;
    buf[1023] = (char)~depth;
    // Tail-call avoidance: use the local after the recursive call.
    recurse_into_guard(depth + 1);
    buf[0] ^= buf[1023];
    (void)buf[0];
}

static void provoke_kstack_overflow_entry(void *arg) {
    (void)arg;
    uart_puts("  fault-test: invoking kstack_overflow (recursing)...\n");
    recurse_into_guard(0);
    uart_puts("FAIL: recurse_into_guard returned (kstack guard did not fire)\n");
    exits("err");
}

static void provoke_kstack_overflow(void) {
    // Spawn a thread with a fresh guarded kstack and switch into it.
    // We can't recurse on the boot stack — it has its own static guard
    // (from start.S) but no per-thread guard, and the test target is
    // the per-thread P2-Dc mechanism specifically.
    struct Thread *t = thread_create_with_arg(kproc(),
                                              provoke_kstack_overflow_entry,
                                              NULL);
    if (!t) {
        uart_puts("FAIL: thread_create_with_arg returned NULL\n");
        return;
    }
    thread_switch(t);
    // Unreachable: t recurses + faults via the guard. extinction halts
    // the kernel before thread_switch returns.
    uart_puts("FAIL: thread_switch returned (overflow thread did not fault)\n");
}
#endif

// ---------------------------------------------------------------------------
// secondary_stack_guard — write into a secondary CPU's boot-stack guard
// page. P5-secondary-stack-guard maps the leading page of every
// g_secondary_boot_stacks slot no-access (build_page_tables, mmu.c); the
// write raises a data abort with FAR inside the guard, and the exception
// handler's addr_is_stack_guard() recognizes it → extinction("kernel
// stack overflow"). Pre-fix the slot was bare RW BSS and the write
// landed silently → provoker returns → test FAIL.
//
// The guard PTEs are established at mmu_enable time, so this needs no
// secondary CPU to be running; the boot CPU writes via the guard page's
// kernel-image high VA. Targets slot 0 (CPU 1's boot stack).
// ---------------------------------------------------------------------------

#ifdef THYLACINE_FAULT_TEST_secondary_stack_guard
#include <thylacine/smp.h>

__attribute__((noinline))
__attribute__((no_stack_protector))
static void provoke_secondary_stack_guard(void) {
    // Launder the pointer through inline asm so clang cannot prove the
    // store targets a known BSS object and elide it.
    volatile char *g = (volatile char *)&g_secondary_boot_stacks[0].guard[0];
    __asm__ __volatile__("" : "+r"(g));
    *g = (char)0xA5;
}
#endif

// ---------------------------------------------------------------------------
// recursive_kernel_fault — #806 regression. Reproduce the root condition of
// the F-B/#806 saga: a wild current_thread() (TPIDR_EL1). arch_fault_handle's
// stack_guard_overflow_msg dereferences current_thread()->magic, so the first
// kernel data abort below makes that deref re-fault. WITHOUT the #806
// re-entrancy guard the handler recurses one KERNEL_ENTRY frame per fault
// until the boot stack crosses its guard -> "kernel stack overflow
// (boot-stack guard)" (the misleading symptom that cost a year). WITH the
// guard, the second entry to the kernel-fault dispatch extincts ->
// "recursive kernel fault (handler re-entered)". test-fault.sh asserts the
// latter, so this FAILS on the pre-fix code and PASSES on the fix.
// ---------------------------------------------------------------------------

#ifdef THYLACINE_FAULT_TEST_recursive_kernel_fault
__attribute__((noinline))
__attribute__((no_stack_protector))
static void provoke_recursive_kernel_fault(void) {
    // Wild current_thread: an unmapped TTBR1 VA. Any subsequent kernel fault
    // makes stack_guard_overflow_msg's t->magic deref re-fault.
    __asm__ __volatile__("msr tpidr_el1, %0" :: "r"((u64)0xdead000000000000ULL));
    // Deliberate kernel data abort at an unmapped high VA -> the fault
    // dispatch runs with the wild current_thread set above.
    volatile u64 *p = (volatile u64 *)0xffff999900000000ULL;
    __asm__ __volatile__("" : "+r"(p));
    *p = 0xbadu;
}
#endif

// ---------------------------------------------------------------------------
// Public entry point. Called from boot_main before the success line.
//
// In a production build, evaluates to a single return.
// ---------------------------------------------------------------------------

void fault_test_run(void);

void fault_test_run(void) {
#if defined(THYLACINE_FAULT_TEST_canary_smash)
    uart_puts("  fault-test: invoking canary_smash...\n");
    provoke_canary_smash();
    uart_puts("FAIL: provoke_canary_smash returned (canary did not fire)\n");
#elif defined(THYLACINE_FAULT_TEST_wxe_violation)
    uart_puts("  fault-test: invoking wxe_violation...\n");
    provoke_wxe_violation();
    uart_puts("FAIL: provoke_wxe_violation returned (W^X did not fire)\n");
#elif defined(THYLACINE_FAULT_TEST_bti_fault)
    uart_puts("  fault-test: invoking bti_fault...\n");
    provoke_bti_fault();
    uart_puts("FAIL: provoke_bti_fault returned (BTI did not fire)\n");
#elif defined(THYLACINE_FAULT_TEST_kstack_overflow)
    provoke_kstack_overflow();
#elif defined(THYLACINE_FAULT_TEST_secondary_stack_guard)
    uart_puts("  fault-test: invoking secondary_stack_guard...\n");
    provoke_secondary_stack_guard();
    uart_puts("FAIL: provoke_secondary_stack_guard returned (guard did not fire)\n");
#elif defined(THYLACINE_FAULT_TEST_recursive_kernel_fault)
    uart_puts("  fault-test: invoking recursive_kernel_fault...\n");
    provoke_recursive_kernel_fault();
    uart_puts("FAIL: provoke_recursive_kernel_fault returned (no fault fired)\n");
#else
    // No fault test selected — production build.
#endif
}
