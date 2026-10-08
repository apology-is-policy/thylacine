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
updated: 2026-10-08
---
## Purpose

The vector table and the C handlers behind it: every syscall, every interrupt,
and every fault in the system enters the kernel through one of sixteen slots
here, and every return to userspace leaves through one of three `eret`s -- a
fourth and a fifth path reach EL0 by branching into the first rather than
adding one.

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

### Full-width interrupt dispatch

The reserved/spurious range is exactly INTIDs 1020 through 1023. The IRQ entry
must not treat every larger ID as spurious: GICv3 LPIs start at 8192 and reach
`gic_dispatch` and EOI with their full identifier. [[sub-kernel-gic]] owns the
controller distinction and [[sub-kernel-pci-irq]] the endpoint lifetime. The
ITS/TCG DMA/PBA and resident-driver gates exercise this path; dropping larger
IDs would leave those endpoints permanently waiting.

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
3. **the stop-check** — a debugger or a job-control stop may be pending, in
   which case the thread parks here
4. **note delivery** — a queued note may need to be pushed onto the user stack
   as a handler frame, or may default-terminate the Proc. Only the synchronous
   tail has this leg ([[seam-el0-irq-tail-no-notes]])

The order is load-bearing in three places. The die-check runs **after** the
preempt, so a Proc that is group-terminated *during* the preempt's context
switch is still caught before any EL0 instruction runs. The stop-check runs
**after** the die-check, which is how "death wins over a stop" is made
mechanical rather than aspirational — a thread that is both dying and stopped
takes the death path. And the stop-check runs **before** note delivery, so a
stop wins over a note: the debugger sees the interrupted context rather than a
handler frame, and a note that arrives during the stop is taken the moment the
stop clears. Plan 9's `notify` runs `procctl` before it looks at a note, and
Linux's signal-delivery-stop comes before the handler frame (DEBUG-FS-DESIGN
4.2). Until 2026-10-05 the notes ran first.

The notes leg can apply a stop itself: an uncaught `tty:susp` takes its default
action there. The thread must park before it runs another EL0 instruction, so
after a stop it applied, the leg runs the die-check and the stop-check again,
parks, and looks at the queue once more. One budget of `NOTE_QUEUE_DEPTH`
passes, shared with the discard loop, bounds the masked tail against a flooded
queue ([[sub-kernel-notes]], [[spec-tail-order]]).

These run at the *vector* level, not inside the C handlers. That matters: by
the time the tail executes, the handler has returned and its crash-dump frame
is closed, so the frame is clean and no C handler is live on the stack. A
thread preempted and stolen here resumes at a clean frame rather than
mid-handler.

### Three `eret`s to EL0, and one rule they all obey

The shared return trampoline handles the ordinary case: a thread that entered
via an exception returns the way it came. It is always reached with interrupts
masked, so it installs the return address and the saved processor state in the
same masked instant that it `eret`s.

**Why it is reached masked changed at ARCH 8.12, and the old reason is no
longer true.** It used to be "hardware masked them on entry and nothing on the
path unmasks". A syscall body now runs with interrupts ON: `syscall_dispatch`
unmasks after setting the per-thread in-syscall marker, and re-masks
UNCONDITIONALLY before returning. So the property is preserved by a re-mask
rather than by an absence, which is a weaker guarantee and is therefore
asserted rather than assumed. Each tail's last C call carries an
interrupt-state assert: `el0_return_stop_check` on the IRQ tail, and the notes
leg on the synchronous and birth tails. `el0_return_stop_check` keeps its own
assert as well, because it has three callers: the two tails, and the notes
leg's re-pass, which parks the thread for a stop the leg applied. The birth
park asserts it for the held child's tail.

The unmask is confined to the syscall body. Kernel fault handling shares the
EL0-synchronous slot and is **not** unmasked, so the recursion guard on that
slot keeps its discriminator.

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

### The fifth way in: a held child's birth, which builds its frame first

(2026-09-29, [[sub-kernel-birth-hold]].) A child spawned with
`SPAWN_DEBUG_HELD` must be stoppable before its first instruction, and the
spawn trampoline cannot give that: it erets straight from registers, with no
frame to stop on and no stop check. So a held child's thunk calls
`userland_enter_held` instead. It carves an exception frame below its own
stack, zeroes it, and writes the image's entry as the return address, EL0 with
interrupts clear as the saved state, and the user stack as the EL0 stack
pointer. That is the frame the first instruction would have been interrupted
with. It then masks and runs the ordinary return sequence over that frame --
preempt, die-check, a park, then notes -- and leaves through the shared return's
local label, exactly as the fork trampoline does. It lives in this file for the
same reason.

It satisfies each standing rule by the fork trampoline's refusal rather than by
a new argument. There is no hand-rolled `eret`, so the masking rule has nothing
to govern: the frame is built with interrupts on, as a plain call, and
everything is masked before the tail, which is the state the tail requires. The
register rule is met by construction. This path builds an EL0 context from
nothing, so it owes the sweep, and the frame is zeroed before its three fields
are written. The ordering rule is the tail's own, with one difference: the
stop leg is the **birth park**, which also holds while the hold is set.

The birth tail is straight-line: the park returns only to proceed. Neither
park answers a latched terminate-interrupt (DEBUG-FS-DESIGN 5g, the operator's
vote of 2026-09-30). Both sleep in `sleep_death_only`, which returns only for
group death, so a stopped thread stays stopped and a held child stays held.
The note stays queued, and the thread meets it at its next note checkpoint
once it runs: the synchronous tail's `notes_deliver` (the IRQ tail delivers
none, [[seam-el0-irq-tail-no-notes]]). A note latched while the child is still
loading is taken by the birth tail's own delivery before the park, and ends
the child before its first instruction. Three earlier answers are gone. The
tails' park left for the latch and erets, which let a compute-bound stopped
thread run with its stop set, since only the synchronous tail delivers notes.
The birth park ended the held child with the note's name. And a first draft
before that re-ran the checkpoint in place, which spun forever once note
delivery declined a stack pointer a debugger wrote (audit round 1, F1). The
frame sits below the thunk's stack, so after the `eret` the kernel stack
pointer is where the spawn trampoline would have left it.

The park the birth tail shares with both tails also gained a second death
check (2026-09-29). It re-reads the group's termination after its wake
condition passes, because a release can follow a terminate (the debugger's
exitkill release terminates and then clears the stop). Without it, a thread
mid-pass could read the cleared flags and `eret` into a dying group. The spec
found that gap, and it was never specific to the birth tail. The park's sleep
reads group death alone (`thread_group_death_pending`), so no latch can end
the park or let a thread leave it.

### An EL0 fault terminates a Proc; a kernel fault kills the machine

The two synchronous handlers share a fault decoder and diverge on what an
unresolvable fault means. From the kernel, it extincts. From EL0, it terminates
just that Proc, tagged with the fault kind — a bad address, a bad alignment, a
bad indirect branch target, a breakpoint, an unknown exception class. The
kernel does not die for a userspace mistake.

**One EL0 trap is not a fault: a wait (XT-3a, 2026-10-08).** `SCTLR_EL1.nTWI`
is clear ([[sub-kernel-boot-entry]]), so an EL0 `WFI` arrives as `EC_WFX`, and
the arm retires it. ELR advances one instruction, and `SPSR.SS` and
`SPSR.BTYPE` clear, as the PE would have left them; this is Linux's
`arm64_skip_faulting_instruction`. Clearing `SS` completes a single-step over
the retired instruction. Clearing `BTYPE` keeps the next instruction from being
checked as the target of the branch that reached the wait. A wait hint that
completes at once is a valid implementation, and retiring it keeps every idle
decision the scheduler's. `WFE` never traps (`nTWE` is set); any trapped wait
retires the same way. `/hint-probe` is the device witness: 64 `WFI` and 64
`WFE`, then a clean exit.

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

One bit of the saved processor state is not the program's: `SPSR_EL1_SS` (bit
21), which `exception.h` names, is the software-step state the `eret` installs.
It belongs to the kernel's step machine. A note's delivery clears it before it
saves the interrupted context, so no saved user context carries it, and a
return through `SYS_NOTED` or `rt_sigreturn` comes back without it
(DEBUG-FS-DESIGN 5.5).

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
the head of both hand-rolled entry paths and in the birth tail, is what makes
"no thread runs at EL0 after its Proc becomes a zombie" hold for a
freshly-spawned or freshly-exec'd thread that would otherwise reach userspace
before its next trap. The park's two death checks, one at the top of each pass
and one after its wake condition, keep it for a thread that was stopped.

**[[inv-i39]]** — the stop-check in both tails is the debug surface's park
point, and its position after the die-check is the "death wins" clause. The
birth tail's park is where a held child waits for its launcher, and it is the
only way a held child reaches EL0.

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
- **The tail ordering must stay preempt, die, notes, stop**, in the birth tail
  too. Moving the die-check before the preempt reopens the
  group-terminate-during-switch window; moving the stop-check before the
  die-check breaks "death wins".
- **The birth tail must never `eret` while its child is held, and no park may
  leave for a latched interrupt.** Both parks return only to proceed, and group
  death alone ends a thread inside them (`ParkEndsOnlyInDeath`). A re-run of
  the checkpoint would spin masked on a frame note delivery declines.
- **The park must re-check death after its wake condition**, not only at the top
  of each pass. A release that follows a terminate is legal.
- **A new EL0-return action must be added to both tails and to the birth
  tail**, and see below for why that is currently harder than it sounds.
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
