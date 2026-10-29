---
id: sub-kernel-exception
type: sub
parent: moc-kernel-entry
title: "Exception entry, the EL0 return tails, and the ways into userspace"
code:
  - arch/arm64/vectors.S
  - arch/arm64/exception.c
  - arch/arm64/exception.h
audit: hard
guarded-by: [inv-i21, inv-i13, inv-i24, inv-i39]
validated-by: [spec-sched-ctxsw, prose, gate-smp, gate-interactive]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 12"
  - "docs/reference/08-exception.md"
created: 2026-08-02
updated: 2026-10-29
---
## Purpose

The vector table and the C handlers behind it: every syscall, every interrupt,
and every fault in the system enters the kernel through one of sixteen slots
here, and every return to userspace leaves through one `eret`, the shared return
trampoline's. A thread's first entry to EL0 is one more return through it: its
frame is built first and it takes the same EL0-return tail as every later
return (thread-model C7, 2026-10-28).

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
   as a handler frame, or may default-terminate the Proc. On an IRQ return it
   can stop EL0 at any instruction, which the whole-context save already
   covers. Both tails have this leg since #964 (2026-10-12): the IRQ slot
   branches into the synchronous slot's tail, `.Lel0_return`, once its handler
   has acked, dispatched and EOIed with interrupts masked and closed its frame.
   Until then a thread that never trapped never took a note, so `^C` did not
   end `while(1);` ([[seam-el0-irq-tail-no-notes]], closed). Delivery clears
   `SPSR.BTYPE` in the handler's context, because the interrupt can land
   between an indirect branch and its target ([[sub-kernel-notes]])

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

### One `eret` to EL0

The shared return trampoline is the only `eret` to EL0. A thread that entered
via an exception returns the way it came, and a thread entering for the first
time returns through it too (below). It is always reached with interrupts
masked, so it installs the return address and the saved processor state in the
same masked instant that it `eret`s.

**Why it is reached masked changed at ARCH 8.12, and the old reason is no
longer true.** It used to be "hardware masked them on entry and nothing on the
path unmasks". A syscall body now runs with interrupts ON: `syscall_dispatch`
unmasks after setting the per-thread in-syscall marker, and re-masks
UNCONDITIONALLY before returning. So the property is preserved by a re-mask
rather than by an absence, which is a weaker guarantee and is therefore
asserted rather than assumed. Each tail's last C call carries an
interrupt-state assert: the notes leg, on the EL0-return tail both slots take
and on the birth tail. `el0_return_stop_check` keeps its own assert as well,
because it has two callers: that tail's stop leg, and the notes leg's re-pass,
which parks the thread for a stop the leg applied. The birth
park asserts it for the held child's tail.

The unmask is confined to the syscall body. Kernel fault handling shares the
EL0-synchronous slot and is **not** unmasked, so the recursion guard on that
slot keeps its discriminator.

Until thread-model C7 there were two more `eret`s, both hand-rolled: one took
a kernel thread into EL0 after it loaded an ELF (`userland.S`), the other was
a spawned thread's first switch-in (`context.S`). Each `eret`ed straight from registers,
so each owed its own masking argument (an interrupt between setting the link
register and the `eret` overwrote it with a kernel PC), its own die-check placed
before its mask, and a sweep zeroing every general register. And each ran EL0
code before any stop or note could act: a thread spawned into a stopped Proc
ran until its first trap (F-7, [[spec-debug-stop]]'s `BUGGY_SPAWN_AT_EL0`).
Both are deleted. The rule they obeyed -- any `eret` that sets the link
register masks across the set-to-`eret` window -- now has one subject, the
shared trampoline, which is masked by the tails before it.

### A first entry is a return

Every thread's first EL0 instruction is reached the way every later one is:
from a saved EL0 frame, through `.Lel0_return` -- the preempt check, the
die-check, the stop leg and the notes leg -- and the shared return. A thread
born into a stopped Proc therefore parks before its first instruction, one
born into a dying group dies there, and a note pending at its birth is met
there. Three shapes build the frame:

| Entry | Who | The frame |
|---|---|---|
| `thread_user_trampoline` | a spawned thread's and a forked child's first switch-in | built by its creator at the top of its kernel stack: a spawned thread's is zeros with the entry as the link register, the user stack as SP_EL0 and the argument in x0 (`thread_alloc_user`); a forked child's is the parent's frame with x0 = 0 (`thread_alloc_forked`) |
| `userland_enter` | an exec thunk's first `eret` | carved below the caller's stack and built by `el0_birth_frame_init` (`EL0_FIRST_FRAME`) |
| `userland_enter_held` | a held spawn's first entry | the same frame, with the birth park as its stop leg (below) |

They live in the vector file, not beside the context-switch code, because they
end in `.Lel0_return`, a local label: no caller outside the file can return to
EL0 on whatever sits on the stack, and reusing the one audited exit beats a
hand-rolled one on the most load-bearing path in the tree. The trampoline
releases the run-tree lock the switch held (`sched_finish_task_switch`, which
every trampoline runs), masks, and branches into the tail; the two enter
functions are called from C with interrupts on, build the frame unmasked, then
mask for the tail.

**No kernel state crosses, for a reason that differs by shape.** The shared
return loads every general register, SP_EL0 and the saved state
from the frame. A spawned thread's frame and an exec'd image's are zeroed
before their fields are written, so a zero frame is the guarantee the old
sweeps gave. A forked child's frame is copied from the parent's own, so every
register already holds a userspace value, and the child continues the parent's
C frame with the frame pointer and return address it must keep. The rule is
*no kernel state crosses*; zeroing is how a constructed frame meets it, and a
restored one meets it by construction. TPIDR_EL0 is not in the frame: the
switch restores it from the thread's context, where its creator put the
spawn's TLS pointer.

The kernel tests `first_entry.spawn_into_stop_parks` and
`first_entry.exec_into_stop_parks` hold the code to the model: a thread spawned
into a job-stopped Proc, and an image exec'd with a stop made pending after the
exec, each park at the entry on an untouched frame and die of a kill there,
and each has a control leg with no stop that runs. The exec leg reads its
entry page in first: the image is demand-paged, and otherwise its first fetch
faults and the fault's own return parks it at the entry, which is what a
correct first entry does, so the test could not tell them apart (found by the
gate's sabotage of `userland_enter`).

### The held child's birth: the same frame, a different park

(2026-09-29, [[sub-kernel-birth-hold]].) A child spawned with
`SPAWN_DEBUG_HELD` must stay before its first instruction until its launcher
releases it, which is more than a stop: the hold is set by the spawn, not by a
debugger. So a held child's thunk calls `userland_enter_held` where every other
thunk calls `userland_enter`. It builds the same frame -- the entry as the
return address, EL0 with interrupts clear as the saved state, the user stack as
the EL0 stack pointer, every other field zero -- and runs the same sequence over
it -- preempt, die-check, a park, then notes -- before the shared return. It
spells the tail out rather than branching to `.Lel0_return`, because its stop
leg is the **birth park**, which also holds while the hold is set.

The birth tail is straight-line: the park returns only to proceed. Neither
park answers a latched terminate-interrupt (DEBUG-FS-DESIGN 5g, the operator's
vote of 2026-09-30). Both sleep in `sleep_death_only`, which returns only for
group death, so a stopped thread stays stopped and a held child stays held.
The note stays queued, and the thread meets it at its next note checkpoint
once it runs: an EL0-return tail's notes leg (the IRQ tail's too, since #964).
A note latched while the child is still
loading is taken by the birth tail's own delivery before the park, and ends
the child before its first instruction. Three earlier answers are gone. The
tails' park left for the latch and erets, which let a compute-bound stopped
thread run with its stop set, since only the synchronous tail delivers notes.
The birth park ended the held child with the note's name. And a first draft
before that re-ran the checkpoint in place, which spun forever once note
delivery declined a stack pointer a debugger wrote (audit round 1, F1). The
frame sits below the thunk's stack, so after the `eret` the kernel stack
pointer is where the thunk left it, and every later EL0 entry of the thread
lands its frame where this one was.

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

### An EL0 abort's verdict

An EL0 data or instruction abort goes to `arch_fault_handle`
([[sub-kernel-fault]]), and the handler acts on the result. `FAULT_HANDLED`
returns, and the vector tail (die-check, stop check, note delivery) runs before
the faulting instruction re-executes. `FAULT_UNHANDLED_USER` and
`FAULT_USER_BUS` end the Proc through `proc_fault_terminate` with `snare:segv`
or `snare:bus`. `FAULT_USER_DYING` (the thread's death was pending when the
fault went unresolved) splits on the death. A group death returns: the tail's
`el0_return_die_check` acts on `group_exit_msg` unconditionally. A terminate
latch is acted on at the fault, by `notes_fault_dying_latch`
([[sub-kernel-notes]]), because the tail defers notes in two states -- while a
handler runs (the N-3 guard) and while `SP_EL0` is unusable -- and returning
there re-executed the faulting instruction into the same fault forever
(groupstatus audit r1 F1; witnesses viv-pheno-probe L329/L330). In the EL1
switch DYING is unreachable (only `userland_demand_page` produces it, for EL0
faults) and extincts; a uaccess fixup treats it as a failed access, as it
treats every non-HANDLED result.

### An EL0 `brk` names its immediate (gcpark-b, 2026-10-12)

`EC_BRK` from EL0 ends the Proc through `proc_fault_terminate`. The ISS low 16
bits are the `brk` immediate: `SNARE_ABORT_BRK_IMM` (`0xab06`, notes.h) is
Pouch's `abort()` (patch 0011) and ends as `snare:abort` (SIGABRT to a packed
wait); every other immediate -- a compiler's `__builtin_trap`, WTFCrash's
`0xc471`, a debugger's -- stays `snare:brk` (SIGTRAP). The immediate is ABI with
the note name ([[abi-note-names]]); operator vote "snare:abort". Witness:
pouch-hello-signals' deaths legs (abort and a failed assert read SIGABRT,
`brk #1` reads SIGTRAP).

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

**[[inv-i24]]** — the die-check in the EL0-return tail, which every first entry
takes too, and in the birth tail, is what makes "no thread runs at EL0 after its
Proc becomes a zombie" hold for a freshly-spawned or freshly-exec'd thread that
would otherwise reach userspace before its next trap. The park's two death checks, one at the top of each pass
and one after its wake condition, keep it for a thread that was stopped.

**[[inv-i39]]** — the stop-check in both tails is the debug surface's park
point, and its position after the die-check is the "death wins" clause. Every
first entry takes it, so a thread born into a stop parks before its first
instruction ([[spec-debug-stop]] `NoEL0BornStopped`). The
birth tail's park is where a held child waits for its launcher, and it is the
only way a held child reaches EL0.

**[[inv-i13]]** — the shared return loads every register from the frame, and a
first entry's frame is zeroed or copied from userspace: no kernel register
state reaches EL0.

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

- **There is one `eret` to EL0; do not add another.** A new way into userspace
  builds a frame and branches into `.Lel0_return` (or, for a park of its own,
  spells the tail out and ends at the shared return, as the birth tail does). A
  hand-rolled `eret` would owe the masking argument, a die-check before its mask,
  a register sweep, and it would run EL0 code before any stop or note could act
  -- the defect C7 removed.
- **A constructed first-entry frame must be fully initialised.** The shared
  return loads every field, so a field a new frame builder leaves unset is a
  kernel-state leak across the privilege boundary. The rule is "no kernel state
  crosses", not "always zero": a restored frame (the fork's) must not be
  cleared, because its registers are the userspace values being restored.
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

## Provenance

[[chg-2026-08-02-entry-sweep]], [[chg-2026-08-16-exception-descent-guard]].
