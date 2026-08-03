---
id: sub-kernel-exception
type: sub
parent: moc-kernel-entry
title: "Exception entry, the EL0 return tails, and the three ways into userspace"
code:
  - arch/arm64/vectors.S
  - arch/arm64/exception.c
  - arch/arm64/exception.h
  - arch/arm64/userland.S
audit: hard
guarded-by: [inv-i21, inv-i13, inv-i24, inv-i39]
validated-by: [spec-sched-ctxsw, prose, gate-smp, gate-interactive]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 12"
  - "docs/reference/08-exception.md"
created: 2026-08-02
updated: 2026-08-03
---
## Purpose

The vector table and the C handlers behind it: every syscall, every interrupt,
and every fault in the system enters the kernel through one of sixteen slots
here, and every return to userspace leaves through one of three `eret`s.

## Contract

Hardware vectors to `_exception_vectors + N*0x80` based on the exception's
source and kind. The slot saves the interrupted register state, calls a C
handler, and branches to a return trampoline. A handler either returns —
meaning the exception is resolved and the interrupted instruction resumes — or
it does not return, because it extincted the machine or terminated the Proc.

Four slots are live. Two carry kernel exceptions (synchronous, interrupt), two
carry EL0 exceptions (synchronous, interrupt). The other twelve route to a
diagnostic that names which one fired and halts.

## Mechanism

### Everything is on the thread's own stack, and that is the design

The kernel runs uniformly at `EL1h`, so `sp` is always the running thread's
kernel stack, and the register frame a slot builds lands on that stack. This
sounds like a detail and is actually the load-bearing property: because frames
travel with the thread, a thread can be work-stolen mid-exception and resumed
on another CPU without anything being left behind on a stack the origin CPU
still owns.

The earlier dual-mode kernel could not do that, and the two slots for
"current EL with `SP_EL0`" are the fossil: under the old model they were the
live kernel-exception entries; under this one they are unreachable, so they are
wired to the unexpected-vector diagnostic. An exception arriving there means
the mode bit was somehow cleared — a soundness violation that now announces
itself instead of silently writing the wrong stack pointer.

### The return tails, and the ordering that is not arbitrary

Four things want to happen after a handler returns but before the thread runs
another EL0 instruction:

1. **the preemption check** — the interrupted thread's slice may have expired,
   or a wake may have made something more urgent runnable
2. **the die-check** — the Proc may be group-terminating, in which case this
   thread self-exits and never returns
3. **note delivery** — a queued note may need to be pushed onto the user stack
   as a handler frame, or may default-terminate the Proc
4. **the stop-check** — a debugger or a job-control stop may be pending, in
   which case the thread parks here

The order is load-bearing in two places. The die-check runs **after** the
preempt, so a Proc that is group-terminated *during* the preempt's context
switch is still caught before any EL0 instruction runs. The stop-check runs
**after** the die-check, which is how "death wins over a stop" is made
mechanical rather than aspirational — a thread that is both dying and stopped
takes the death path.

These run at the *vector* level, not inside the C handlers. That matters: by
the time the tail executes, the handler has returned and its crash-dump frame
is closed, so the frame is clean and no C handler is live on the stack. A
thread preempted and stolen here resumes at a clean frame rather than
mid-handler.

### Three ways to reach EL0, and one rule they all obey

The shared return trampoline handles the ordinary case: a thread that entered
via an exception returns the way it came. It is always reached with interrupts
masked — hardware masked them on entry and nothing on the path unmasks — so it
installs the return address and the saved processor state in the same masked
instant that it `eret`s.

The other two are hand-rolled. One takes a kernel thread into EL0 for the first
time after loading an ELF; the other is the initial entry point for a thread
created by the thread-spawn syscall. Both are reached with **interrupts
enabled**, and both must therefore mask explicitly:

> Any hand-rolled `eret` to EL0 that sets the exception link register must mask
> interrupts across the whole set-to-`eret` window.

Without the mask, an interrupt taken in that window re-enters the exception
path and overwrites the link register with the interrupted *kernel* PC. Neither
trampoline re-sets it afterward, so the `eret` lands EL0 at a kernel address —
a rare, timing-dependent instruction-permission fault in a freshly-started
Proc. The `eret` itself restores a cleared processor state, so userspace still
runs with interrupts on; the mask closes only the kernel-side window.

Both hand-rolled paths run their die-check **before** the mask, deliberately —
the die path does not return, so it must never be entered from inside the
masked window.

Both also zero every general-purpose register before the `eret`, so no kernel
register state crosses the boundary. The thread-spawn trampoline zeroes all but
one, which carries the entry argument by calling convention.

### An EL0 fault terminates a Proc; a kernel fault kills the machine

The two synchronous handlers share a fault decoder and diverge on what an
unresolvable fault means. From the kernel, it extincts. From EL0, it terminates
just that Proc, tagged with the fault kind — a bad address, a bad alignment, a
bad indirect branch target, a breakpoint, an unknown exception class. The
kernel does not die for a userspace mistake.

The kernel synchronous handler carries one extra arm, and it is the interesting
one: if the fault came from the kernel but the faulting *address* is in the
user half, it may be a deliberate crossing rather than a corrupted pointer, and
it is handed to [[sub-kernel-uaccess]].

### The hardware-debug exception classes

Three exception classes arrive from EL0 only because the kernel armed something:
a hardware breakpoint, a single-step, a watchpoint. Each is offered to the debug
layer first and terminates the Proc only if it is refused — which is a defensive
backstop rather than a real path, because userspace cannot arm any of these.

## Data structures

One: the saved register frame. It is written by assembly at fixed byte offsets
and read by C as a struct, so the two descriptions are pinned together by
compile-time assertions on the total size and on the offset of every special
register. That pairing is the whole safety argument for the frame — there is no
runtime check that assembly and C agree.

## Concurrency

None owned. Handlers run on the interrupted thread's own stack, and the frames
are per-thread by construction, so there is no shared exception state to
protect. Interrupts are masked on entry by hardware.

The one cross-CPU concern is the crash-dump slot each handler sets for its
duration: a handler that blocks and resumes on another CPU runs its restore
there, leaving the slot pointing somewhere stale. The dump path does not trust
the slot — it gates on plausibility and falls back to capturing the current
frame — so the staleness is absorbed rather than prevented.

## Invariants enforced

**[[inv-i21]]** — the uniform-`EL1h` clause is enforced structurally here: the
two slots that could only be reached from the other mode are wired to a loud
diagnostic, so the invariant's violation is detectable rather than silent.

**[[inv-i24]]** — the die-check in both EL0 return tails, plus the same check at
the head of both hand-rolled entry paths, is what makes "no thread runs at EL0
after its Proc becomes a zombie" hold for a freshly-spawned or freshly-exec'd
thread that would otherwise reach userspace before its next trap.

**[[inv-i39]]** — the stop-check in both tails is the debug surface's park
point, and its position after the die-check is the "death wins" clause.

**[[inv-i13]]** — the register sweep before each `eret` is the crossing half:
no kernel register state reaches EL0.

## Error paths

A kernel-side unresolvable fault, an unexpected vector, and an unknown fault
result all extinct with a specific diagnostic. An EL0-side unresolvable fault
terminates the Proc with a tag naming the fault kind. A spurious or reserved
interrupt identifier is dropped without dispatch and without acknowledgement,
per the interrupt controller's specification.

## Performance

Entry and exit are straight-line register traffic — the save is about
twenty-four instructions, the restore about twenty-three — with no branches and
no memory beyond the thread's own stack. Every syscall, interrupt and fault pays
both.

The structural cost here is not time but **space**: each slot is capped at
`0x80` bytes, or thirty-two instructions, and the save alone is twenty-four.
That budget is why the restore is factored into a shared trampoline rather than
inlined, and it is a live constraint rather than a historical one — the EL0
interrupt slot currently holds **thirty-one** of its thirty-two instructions.

## Prosecution

- **Any new hand-rolled `eret` to EL0 must mask across the link-register-set to
  `eret` window.** The shared trampoline is exempt because it is always reached
  masked; the kernel-to-kernel trampoline is exempt because it does not `eret`
  to EL0 at all. Nothing else is exempt.
- **A noreturn check must run before the mask, not inside it.**
- **The register sweep must stay complete.** A newly-added register that is not
  zeroed is a kernel-state leak across the privilege boundary.
- **The frame layout assertions must be updated with the frame.** Assembly
  writes by offset; only the assertions tie it to the struct.
- **The tail ordering must stay preempt, die, notes, stop.** Moving the
  die-check before the preempt reopens the group-terminate-during-switch
  window; moving the stop-check before the die-check breaks "death wins".
- **A new EL0-return action must be added to both tails**, and see below for
  why that is currently harder than it sounds.
- **The kernel synchronous handler's user-half check must stay narrow.** It is
  the only thing separating a deliberate crossing from a corrupted kernel
  pointer, and widening it would silently absorb real corruption.

## Seams

- **[[seam-el0-irq-tail-no-notes]]** — note delivery runs on only one of the two
  EL0 return tails, so a Proc that takes no syscall and no fault never evaluates
  its note disposition.
- **Kernel stack overflow faults recursively.** The save builds its frame on the
  same overflowing stack, so an overflow into the guard page re-faults rather
  than landing somewhere safe. A dedicated overflow stack is reserved and
  unbuilt.
- **The alignment-fault paths are not fixup-recoverable.** The fixup table
  covers translation, permission and access-flag faults; an unaligned kernel
  access to a user address is not in that set and extincts. Callers that could
  produce one validate alignment themselves.

## Caveats

- **The interrupt slot has one instruction of headroom.** Thirty-one of
  thirty-two are used. The next addition to the EL0 interrupt return path will
  not fit, and the build will fail rather than silently truncate — a loud
  failure, but it means the file sits at a structural cliff, and the fix
  (factoring the tail into its own trampoline, exactly as the synchronous slot
  already does) is a prerequisite rather than a cleanup.
- **The reference document's vector table is stale.** It lists both EL0 slots as
  "unexpected", which was true before userspace existed and has been wrong since
  the EL0 paths went live. The prose beneath it describes a two-live-slot kernel.
  This is the drift-in-the-oldest-summary pattern: the per-slot comments in the
  source are current and unusually thorough, and the summarizing document is
  years behind them.
- **One in-source comment claims note delivery runs on both tails.** It names
  the interrupt slot explicitly. It does not. See the seam.

## Provenance

[[chg-2026-08-02-entry-sweep]].
