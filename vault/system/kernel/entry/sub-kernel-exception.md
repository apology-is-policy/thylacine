---
id: sub-kernel-exception
type: sub
parent: moc-kernel-entry
title: "Exception entry, the EL0 return tails, and the ways into userspace"
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
updated: 2026-08-18
---
## Purpose

The vector table and the C handlers behind it: every syscall, every interrupt,
and every fault in the system enters the kernel through one of sixteen slots
here, and every return to userspace leaves through one of three `eret`s -- a
fourth path reaches EL0 by branching into the first rather than adding one.

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

### Three `eret`s to EL0, and one rule they all obey

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

### The fourth way in, which is deliberately not a fourth `eret`

A forked child also reaches EL0 for the first time through a trampoline, and it
is the interesting one precisely because it **adds no `eret`**. It lives in the
vector file rather than beside the other trampolines for a single reason: from
there it can branch to the shared return's own local label, handing the child to
the one audited return-to-user path instead of hand-rolling a second.

That is the standing rule being satisfied by refusing to create the situation it
governs. A new hand-rolled return would have owed the masking argument, the
ordering argument, and a fresh review; branching into the existing one owes none
of them, because the child's frame was constructed at exactly the address and
layout that path already expects.

**And it zeroes nothing, which is correct for a reason the other two do not
share.** The other trampolines *construct* an EL0 context out of a kernel
context, so any register they do not overwrite carries kernel residue across the
boundary — hence the sweep. This one *restores* a saved EL0 frame, copied from
the parent's own, so every register already holds a userspace value by
construction. There is no residue to sweep, and sweeping would destroy the fork:
the child continues the parent's C frame, and the frame pointer and the return
address are exactly the state it must keep.

Same invariant, opposite action, because one path's registers come from the
kernel and the other's come from userspace. The sweep is not the rule; *no kernel
state crosses* is the rule.

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

### The descent guard, and the premise it shipped with

A kernel fault whose handler faults on the same bad state recurses, and each
iteration builds a frame on the same stack — so the recursion marches *downward*
through mapped memory, writing frames across physical RAM until the page tables
themselves hold exception frames. That is not hypothetical; it is how one
uninitialized pointer took a whole machine ([[sub-kernel-boot-entry]] owns the
root).

So the kernel synchronous handler counts its own depth per CPU and, at three,
stops trying: it flushes the staged console ring with a bounded try-lock so
already-staged diagnostics still reach the wire, prints **one** raw banner naming
the frame that killed the handler, and parks that CPU with the stack corpse
intact for an external autopsy.

Two deliberate refusals in that sequence. It does **not** run the crash dump,
because the dump machinery is the most likely amplifier — the thing you would
reach for is the thing most likely to fault again. And the banner prints at
*exactly* the threshold, so if the banner itself faults, the next entry parks
silently rather than looping through the print.

**The guard shipped with a false premise, and it was a P1.** Its reasoning was
that legitimate depth is one — that a kernel synchronous handler runs to
completion without yielding. It does not. A kernel-side access to a cold
file-backed page blocks in the filesystem client, so a perfectly healthy handler
*sleeps*, and independent threads time-sharing one CPU can each be asleep inside
one. Three such sleepers reach the threshold, and the guard parks a healthy CPU
and prints a **fabricated extinction line** — the string the entire test harness
reads as "the kernel died" — under nothing more exotic than a parallel build.

The repair is a better discriminator rather than a larger threshold. The
scheduler clears the counter at **every context switch**, because a switch
*proves the handler chain is making forward progress*, while a genuine runaway —
a fault whose handler faults, synchronously, with interrupts masked — never
reaches the scheduler at all, so its count survives to trip.

**Depth alone conflates recursion with interleaving.** Adding "did we yield?"
separates them, and it is the only signal available that distinguishes the two
without knowing anything about what the handlers are doing.

**The runaway's banner is the ABI line, and until 2026-08-18 it was serialized
by nothing.** `el1_sync_runaway` prints `EXTINCTION: el1-sync recursion …`
*without going through* `extinction()`, so the console-word claim added in
2026-08-16 never covered it — and neither did `abi-boot-banner`'s own `mirrors`
set, which is why `quaestor owner` reports this file as matching the literal
from outside that set. It now takes both serializers: the console word (claim,
or confirm this CPU already owns it — the runaway is reachable from a chain that
claimed it at depth 1; a *peer* holding it means a peer is dumping, so this CPU
parks silent and counted) and then the console ring lock, whose miss it reports
after its own banner.

It was found by **deleting the old flush symbol and letting the build fail**,
not by the grep census that ran first and missed it. *A rename is a census that
cannot lie.*

**This path is exercised by no test at all**, and that is a consequence of its
own fix: in a healthy kernel the #806 guard extincts at the *second* kernel
fault, so the depth never reaches the threshold — reaching the runaway requires
the extinction/Halls path itself to fault, which is precisely the defect
(main#244) that was removed. Everything on it is static-audited only; a variant
injecting a fault *inside* `halls_dump` would drive it (main#246).

The reset-on-unwind is a separate mechanism from the reset-at-switch, and both
are needed: unwinding resets to zero rather than decrementing so a handler that
migrated mid-flight cannot strand a foreign CPU's increment.

The residual is documented rather than hidden: a recursive chain that *unmasks*
interrupts partway could be preempt-cleared and evade the count. The observed
class runs interrupts-masked and is still bounded, the terminal path carries its
own re-entrancy guard beneath this one, and the failure mode of a miss is a spin
rather than corruption. **A guard with a known hole and an argued containment is
worth more than one whose hole nobody has looked for** — and this one earned that
posture by having its first premise disproved.

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
- **The register sweep must stay complete** on the paths that *construct* an EL0
  context from a kernel one. A newly-added register that is not zeroed there is a
  kernel-state leak across the privilege boundary. The rule is "no kernel state
  crosses", not "always zero" — a path that *restores* a saved EL0 frame must not
  sweep, because its registers are userspace values and clearing them destroys
  the thing being restored.
- **Prefer branching into the shared return over writing a new one.** A new
  hand-rolled path inherits the masking obligation, the ordering obligation and a
  review; a branch into the audited one inherits its correctness. Building the
  frame at the address that path already expects is what buys this.
- **The descent guard's threshold is not the mechanism — the reset is.** It must
  keep clearing at every context switch, or legitimate sleeping handlers
  accumulate and the guard fabricates a kernel-death report on the tooling ABI.
  Raising the threshold instead would only move the load at which that happens.
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

[[chg-2026-08-02-entry-sweep]], [[chg-2026-08-16-exception-descent-guard]].
