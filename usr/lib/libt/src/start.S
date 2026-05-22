// libt _start — program-entry stub for Thylacine userspace binaries.
//
// The kernel's exec_setup + userland_enter delivers control to the ELF's
// e_entry with:
//   sp           = the System V startup frame's argc word
//                  (EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE; 16-aligned)
//   x0..x30      = unspecified (don't rely on initial values)
//   PSTATE.M    = EL0t (already at user EL)
//
// _start's contract: call main(), then unconditionally exits with main's
// return value. The kernel builds a System V argc/argv/envp/auxv frame
// at sp (P6-pouch-kernel-auxv; see the kernel's exec.h layout block) —
// but libt's _start does not consume it: main() takes no arguments. A
// SysV-aware C runtime (pouch) reads the frame; a libt binary ignores it.
//
// BTI marker on _start: the kernel jumps to e_entry via eret (not bl),
// which doesn't count as a "call" landing pad — so technically no `bti c`
// is required. We emit one anyway to keep the entry uniform with future
// dlopen()-style indirect call paths.

.section .text._start, "ax"
.globl _start
.type _start, %function
_start:
    bti     c                       // landing pad for indirect entry (PAC/BTI)
    bl      main                    // x0 := main()
    mov     x8, #0                  // x8 = T_SYS_EXITS
    svc     #0                      // never returns
    // Defensive — SYS_EXITS doesn't return. If somehow it did, halt.
1:  wfe
    b       1b
.size _start, .-_start

// Stack-canary stubs: -fstack-protector-strong references these even at
// userspace level. Provide minimal definitions so a hello binary doesn't
// drag in a libc. The guard is a fixed sentinel at v1.0 P4-Ia1; the
// userspace CSPRNG-seeded variant lands when the seed-via-syscall surface
// arrives (Phase 5+).
//
// __stack_chk_guard: 8-byte sentinel. The compiler emits comparisons
// against this value at function epilogues for canary-protected functions.
.section .data.__stack_chk_guard, "aw"
.globl __stack_chk_guard
.type __stack_chk_guard, %object
.align 3
__stack_chk_guard:
    .quad   0xdeadbeefcafef00d
.size __stack_chk_guard, .-__stack_chk_guard

// __stack_chk_fail: the compiler calls this when a canary mismatch is
// detected. v1.0 userspace response: exit "fail" via SYS_EXITS(1).
// Marked noreturn at the C level; here we just SVC and infinite-loop.
.section .text.__stack_chk_fail, "ax"
.globl __stack_chk_fail
.type __stack_chk_fail, %function
__stack_chk_fail:
    bti     c
    mov     x0, #1                  // status = 1 (fail)
    mov     x8, #0                  // T_SYS_EXITS
    svc     #0
1:  wfe
    b       1b
.size __stack_chk_fail, .-__stack_chk_fail

// PT_GNU_STACK marker: emit it explicitly so the linker doesn't synthesize
// a default. NX stack is enforced via the linker script (no PT_LOAD covers
// the stack region; the kernel maps it RW from exec_map_user_stack).
.section .note.GNU-stack, "", %progbits
