# 67 — Uniform-EL1h kernel execution model (P5-el1h-kernel)

## Purpose

Records the as-built **uniform-EL1h** kernel execution model and the
`P5-el1h-kernel` conversion that established it. The kernel executes
entirely at **EL1h** (`PSTATE.SPSel = 1`); the active stack pointer is
always `SP_EL1` and is the running thread's own kernel stack; `SP_EL0`
is exclusively the userspace stack. This is invariant **I-21**
(`ARCHITECTURE.md §12.1`, §28).

This document supersedes the EL/stack-pointer parts of `08-exception.md`,
`01-boot.md`, and `17-smp-bringup.md` for the post-P5 tree.

## Why the model changed

Early SMP bring-up (P2-Cc) ran normal kernel code at **EL1t**
(`SPSel=0`, `sp = SP_EL0`) and entered **EL1h** only transiently inside
exception handlers, with `SP_EL1` a separate per-CPU exception stack
(`g_exception_stacks`). That dual-mode design was unsound:

- `cpu_switch_context` (`arch/arm64/context.S`) saves/restores `SP` and
  the callee-saved set, but **not `SPSel`**. A thread therefore resumed
  in whatever mode the *outgoing* thread left the CPU in — its execution
  mode was a function of scheduling history, not a fixed property.
- Under SMP work-stealing a thread could resume the exception-exit
  trampoline (`KERNEL_EXIT`, `arch/arm64/vectors.S`) at the wrong mode.
  `KERNEL_EXIT` executes `msr SP_EL0, x10`; with `SPSel=0` that targets
  the *currently-selected* stack pointer, which is CONSTRAINED
  UNPREDICTABLE and traps `UNDEFINED` (`ESR_EL1 EC=0`) on the QEMU virt
  target. The faulting CPU's `extinction()` parks only itself
  (`_torpor`), so a secondary CPU silently died on every boot once
  cross-CPU work-stealing was enabled at the joey-launch phase.
- A *per-CPU* exception stack is fundamentally incompatible with
  migrating a mid-exception thread: the thread's exception frame lives
  on the origin CPU's stack and cannot follow it.

The dual-mode design had already accreted three separate band-aids for
this same disease — `userland.S`'s P4-Fix157 `msr SPSel,#0` dance, the
R7-F135 "`msr SP_EL0` QEMU quirk" reclassification, and the P4-Ic6
`g_bootcpu_idle`-kept-off-the-run-tree workaround in `sched.c`.
`P5-el1h-kernel` removes the root cause those all worked around.

## The model

| Property | Value |
|---|---|
| `PSTATE.SPSel` | `1` at all times, kernel-wide |
| Live kernel SP | `SP_EL1` = the running thread's kernel stack |
| `SP_EL0` | exclusively the userspace stack; never a kernel stack |
| Exception entry (kernel) | dispatches to the SP_ELx vector group (`0x200`/`0x280`); frame built on the current thread's kernel stack |
| Exception entry (EL0) | dispatches to the lower-EL group (`0x400`/`0x480`); hardware selects `SP_EL1` |
| `KERNEL_EXIT`'s `msr SP_EL0` | always at EL1h → always a write to the non-current bank → architecturally well-defined |
| Migration | exception frames travel with the thread on its own kstack → cross-CPU work-stealing of a mid-exception thread is sound |

Because there is exactly one SP bank in use, `cpu_switch_context` and
the `KERNEL_ENTRY` / `KERNEL_EXIT` macros are **unchanged** by this
conversion — they were already bank-agnostic and are correct when only
one bank is live.

## Implementation

### Boot (`arch/arm64/start.S`)

- Primary `_real_start`: step 3 asserts `msr SPSel,#1` (defensive — the
  EL2-drop and QEMU-virt direct entry are both EL1h) *before* the first
  `mov sp`, so the write always lands `SP_EL1`. Step 4.6 zeros `SP_EL0`
  (it must not carry a kernel-VA leftover into the first eret-to-EL0).
  Step 8.5 re-anchors the single kernel stack (`SP_EL1` =
  `_boot_stack_top`) to its post-KASLR high VA — one re-anchor replacing
  the former P2-Cc SP_EL1-exception-stack + P3-Bda SP_EL0-boot-stack
  pair.
- Secondary `secondary_entry`: asserts `msr SPSel,#1` before the stack
  write; sets `SP_EL1` = this CPU's `g_secondary_boot_stacks` slot;
  zeros `SP_EL0`; re-anchors `SP_EL1` to high VA after
  `mmu_program_this_cpu`.

### Vector table (`arch/arm64/vectors.S`)

The "Current EL with SP_EL0" group (`0x000` sync, `0x080` IRQ) is
**unreachable** — the kernel is never at EL1t. Both slots route to
`VEC_UNEXPECTED` (extinct with the vector index): an exception arriving
there means `SPSel` was somehow cleared, a soundness violation. Kernel
exceptions enter via `0x200`/`0x280`; userspace via `0x400`/`0x480`.

### EL0 transition (`arch/arm64/userland.S`)

`userland_enter` runs at EL1h and installs the user stack pointer with
`msr sp_el0, user_sp` — at EL1h `SP_EL0` is the non-current bank, so
this is the well-defined write. The former `msr SPSel,#0; mov sp` dance
(P4-Fix157) is removed; the kernel `sp` (`SP_EL1`) is untouched, and the
`eret` to EL0 makes `SP_EL0` active.

### Thread stacks

Every thread created by `thread_create` has its own guarded 32 KiB
kstack (`thread.c`); exception frames are built on it. The bootstrap
threads — `kthread` and the per-CPU idle threads from
`thread_init_per_cpu_idle` — have `kstack_base == NULL` and run on a
CPU's *boot* stack (`_boot_stack_top` for the boot CPU,
`g_secondary_boot_stacks[idx-1]` for secondaries). They are **CPU-pinned**:
`try_steal` (`sched.c`) skips any thread with `kstack_base == NULL`,
because such a thread has no portable kstack and migrating it would run
it on a stack its origin CPU still owns (audit F2).

## State machine

A thread's execution mode is now a fixed constant — EL1h — for its
entire kernel lifetime. There is no mode transition to model at the
per-thread level. The `eret` of `KERNEL_EXIT` restores `SPSR.M`:
`EL1h → EL1h` for a kernel→kernel return, `EL1h → EL0t` for a return to
userspace.

## Spec cross-reference

`specs/sched_ctxsw.tla` pins invariant I-21. `CtxSwitchModeConsistent`
(`cpu_mode = thread_mode[mode_running]`) holds by construction under
Model 1 (`DUAL_MODE = FALSE`, `sched_ctxsw.cfg`); `BuggyModeSwitch`
under Model 2 (`DUAL_MODE = TRUE`, `sched_ctxsw_buggy.cfg`) violates it
— the executable counterexample for the secondary-CPU crash. See
`specs/SPEC-TO-CODE.md`.

## Tests

- `test_smp.exception_stack_smoke` asserts `SPSel == 1` (I-21) and the
  reserved `g_exception_stacks` layout.
- The boot path is the live regression: a clean boot (4/4 CPUs, the
  joey → corvus round-trip with work-stealing enabled, `Thylacine boot
  OK`, **zero `EXTINCTION:` lines**) is the signal the dual-mode crash
  is gone.

## Status

Landed at `P5-el1h-kernel`. Verified: 442/442 PASS × default + UBSan, 0
`EXTINCTION` lines, `sched_ctxsw.tla` clean + buggy verified. Adversarial
audit of the exception-entry + boot + scheduler surfaces: no P0; F2/F3
fixed in-chunk; F1 closed by P5-secondary-stack-guard (see Known
caveats).

## Known caveats

- **Secondary-CPU boot-stack guard pages (audit F1 — CLOSED by
  P5-secondary-stack-guard).** `_boot_stack_top` (the boot CPU's stack)
  has had a 4 KiB guard page since P1; `g_secondary_boot_stacks` did
  not, so a secondary CPU's idle-thread stack overflow corrupted
  adjacent BSS silently rather than faulting. P5-secondary-stack-guard
  closed this: each `g_secondary_boot_stacks` slot is now a 4 KiB guard
  page + 16 KiB usable stack, the guard page mapped no-access by
  `build_page_tables` (mmu.c) in both the kernel-image L3 and the
  direct-map alias. An overflow now faults — `addr_is_stack_guard`
  recognizes the FAR → `extinction("kernel stack overflow")`. See
  `docs/reference/17-smp-bringup.md` §"Secondary boot-stack guard
  pages". (The dedicated stack-overflow / SError handler-stack on
  `g_exception_stacks` remains a separate, still-deferred item — see
  the next caveat.)
- **Stack-overflow recovery.** With one stack bank, a kernel-stack
  overflow into the guard page faults recursively (`KERNEL_ENTRY`
  builds its frame on the same overflowing stack). A dedicated
  overflow / SError handler stack — entered via an `SP_EL0`-based
  check in the `0x200` vector — is a future hardening item.
