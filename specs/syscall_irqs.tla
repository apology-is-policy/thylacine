------------------------------ MODULE syscall_irqs ------------------------------
(***************************************************************************)
(* syscall_irqs -- ARCH 8.12: a syscall body runs with interrupts ON and   *)
(* is still non-preemptible.                                               *)
(*                                                                         *)
(* WHAT THIS MODULE IS FOR. The reconnaissance measured the hazard everyone *)
(* expected -- a plain lock shared between a syscall path and a same-CPU    *)
(* IRQ handler -- and found ZERO sites (ARCHITECTURE.md 8.12). So the lock  *)
(* sweep is not what needs proving. What needs proving is the pair of       *)
(* properties the new mechanism rests on:                                   *)
(*                                                                         *)
(*   1. NO INVOLUNTARY SWITCH INSIDE A SYSCALL BODY. An interrupt taken in  *)
(*      the body runs to completion on the interrupted thread's own kernel  *)
(*      stack and returns to it. A tick may set `need_resched`; the switch  *)
(*      waits for the EL0 return.                                           *)
(*                                                                         *)
(*   2. THE RE-MASK PRECEDES THE ERET WINDOW. `vectors.S` KERNEL_EXIT       *)
(*      installs ELR/SPSR and `eret`s under an INHERITED mask -- the one    *)
(*      surviving #713-class window that does not mask locally. An unmask   *)
(*      that leaks into the return tail resurrects #713 (the year-long      *)
(*      AEGIS corruption: 3-13% of boots, never at -smp 1).                 *)
(*                                                                         *)
(* THE NEGATIVE INVARIANT NEEDS A POSITIVE CONTROL, so this module carries  *)
(* one. "No involuntary switch" is satisfied in full by a model that cannot *)
(* switch at all -- the failure mode where a green run means nothing. The   *)
(* KTHREAD configuration is the control ONE VARIABLE AWAY: a kernel thread  *)
(* sets no marker, so the SAME machinery must produce an involuntary switch *)
(* there (`KthreadGetsPreempted`). Kthreads must STAY preemptible or #810   *)
(* is lost, so this control is a real obligation as well as a check on the  *)
(* check.                                                                   *)
(*                                                                         *)
(* WHAT IS ABSTRACTED. One CPU, one thread. Locks are absent (measured      *)
(* clean). Death, notes, stop and the 9P machinery are elsewhere. The model *)
(* asks only where the mask changes, where the marker is consulted, and     *)
(* what a deferred reschedule does.                                         *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    KTHREAD,                     \* TRUE: a kernel thread -- no marker, preemptible.
    BUGGY_MARKER_IGNORED,        \* preempt_check_irq does not consult the marker.
    BUGGY_UNMASK_BEFORE_MARK,    \* the body unmasks before the marker is set.
    BUGGY_LATE_REMASK,           \* the re-mask lands after ELR/SPSR are installed.
    BUGGY_MARKER_NEVER_CLEARED,  \* the marker survives into the EL0-return tail.
    BUGGY_MASKED_BODY            \* the AS-BUILT model: the body never unmasks.

ASSUME KTHREAD \in BOOLEAN
ASSUME BUGGY_MARKER_IGNORED \in BOOLEAN
ASSUME BUGGY_UNMASK_BEFORE_MARK \in BOOLEAN
ASSUME BUGGY_LATE_REMASK \in BOOLEAN
ASSUME BUGGY_MARKER_NEVER_CLEARED \in BOOLEAN
ASSUME BUGGY_MASKED_BODY \in BOOLEAN

VARIABLES
    pc,          \* where the thread is.
    masked,      \* PSTATE.DAIF.I -- TRUE means interrupts are masked.
    marker,      \* the per-thread in-syscall marker (ARCH 8.12).
    resched,     \* need_resched is pending.
    ctx,         \* ELR/SPSR are installed: the eret window is OPEN.
    ksw,         \* an INVOLUNTARY switch happened while in kernel mode.
    noise        \* an unprivileged producer is keeping the body looping.

vars == <<pc, masked, marker, resched, ctx, ksw, noise>>

(***************************************************************************)
(* "el0"   -- userspace.                                                   *)
(* "entry" -- past the SVC vector: hardware has masked; the marker is not   *)
(*            yet set. THE GAP. Clean code closes it by setting the marker  *)
(*            before it unmasks.                                            *)
(* "body"  -- the syscall body, interrupts ON.                             *)
(* "tail"  -- the EL0-return tail: die check, note delivery, the #107       *)
(*            syscall-return preempt. The deferred reschedule fires here.   *)
(* "eret"  -- ELR/SPSR installed, before the `eret`. #713's window.         *)
(***************************************************************************)
PcStates == {"el0", "entry", "body", "tail", "eret"}

TypeOk ==
    /\ pc \in PcStates
    /\ masked \in BOOLEAN
    /\ marker \in BOOLEAN
    /\ resched \in BOOLEAN
    /\ ctx \in BOOLEAN
    /\ ksw \in BOOLEAN
    /\ noise \in BOOLEAN

Init ==
    /\ pc        = "el0"
    /\ masked    = FALSE       \* EL0 runs unmasked.
    /\ marker    = FALSE
    /\ resched   = FALSE
    /\ ctx       = FALSE
    /\ ksw       = FALSE
    /\ noise     = FALSE

(***************************************************************************)
(* Svc -- EL0 traps. `vectors.S` masks at exception entry (msr daifset).   *)
(* A kthread never does this; it is modelled as entering the body directly. *)
(***************************************************************************)
Svc ==
    /\ pc = "el0"
    /\ pc' = "entry"
    /\ masked' = TRUE
    /\ UNCHANGED <<marker, resched, ctx, ksw, noise>>

(***************************************************************************)
(* EnterBody -- set the marker, then unmask. The ORDER is the mechanism:    *)
(* unmasking first opens a window in which an interrupt sees marker=FALSE   *)
(* and preempts a thread that is already inside its syscall.                *)
(*                                                                         *)
(* A KTHREAD sets no marker -- that is not a bug, it is #810.               *)
(***************************************************************************)
EnterBody ==
    /\ pc = "entry"
    /\ pc' = "body"
    /\ masked' = BUGGY_MASKED_BODY
    /\ marker' = IF KTHREAD THEN FALSE ELSE ~BUGGY_UNMASK_BEFORE_MARK
    /\ UNCHANGED <<resched, ctx, ksw, noise>>

(***************************************************************************)
(* LateMark -- the BUGGY_UNMASK_BEFORE_MARK arm: the marker is set one step *)
(* AFTER the unmask, so an interrupt can land in between.                   *)
(***************************************************************************)
LateMark ==
    /\ BUGGY_UNMASK_BEFORE_MARK
    /\ ~KTHREAD
    /\ pc = "body"
    /\ ~marker
    /\ marker' = TRUE
    /\ UNCHANGED <<pc, masked, resched, ctx, ksw, noise>>

(***************************************************************************)
(* Tick -- the timer lands and sets need_resched. The producer: granted no  *)
(* fairness, never forced to stop. Only possible while UNMASKED, which is   *)
(* the whole point of the chunk.                                            *)
(***************************************************************************)
Tick ==
    /\ ~masked
    /\ ~resched
    /\ resched' = TRUE
    /\ UNCHANGED <<pc, masked, marker, ctx, ksw, noise>>

(***************************************************************************)
(* PreemptCheckIrq -- the IRQ-return gate. It DEFERS (leaves need_resched   *)
(* pending, does not consume it) while the interrupted thread is inside a   *)
(* syscall body; otherwise it switches.                                     *)
(*                                                                         *)
(* `ksw` records an involuntary switch taken in KERNEL mode. For a user     *)
(* thread that is exactly what property 1 forbids; for a kthread it is      *)
(* exactly what #810 REQUIRES. One variable, two cfgs, opposite verdicts -- *)
(* which is what makes the negative invariant worth anything.               *)
(***************************************************************************)
Defers == marker /\ ~BUGGY_MARKER_IGNORED

PreemptCheckIrq ==
    /\ ~masked
    /\ resched
    /\ ~Defers
    /\ resched' = FALSE
    /\ ksw' = (ksw \/ pc \in {"entry", "body", "tail"})
    /\ UNCHANGED <<pc, masked, marker, ctx, noise>>

(***************************************************************************)
(* VoluntarySleep -- the body calls sched() itself. ALLOWED and unchanged   *)
(* by this chunk; modelled so the invariant is not trivially satisfied by a *)
(* model in which no switch of any kind occurs in the body.                 *)
(***************************************************************************)
VoluntarySleep ==
    /\ pc = "body"
    /\ resched
    /\ resched' = FALSE
    /\ UNCHANGED <<pc, masked, marker, ctx, ksw, noise>>

(***************************************************************************)
(* Noise -- an unprivileged producer keeps the body looping (a pipe, one    *)
(* writer, one reader, an events=0 poller). It is never obliged to stop, so *)
(* it gets no fairness in either direction. Under the AS-BUILT model this   *)
(* is what held a CPU's interrupts -- the SAK included -- for as long as it *)
(* lasted, and why poll needed a preemption point at all.                   *)
(***************************************************************************)
NoiseOn ==
    /\ pc = "body"
    /\ ~noise
    /\ noise' = TRUE
    /\ UNCHANGED <<pc, masked, marker, resched, ctx, ksw>>

NoiseOff ==
    /\ noise
    /\ noise' = FALSE
    /\ UNCHANGED <<pc, masked, marker, resched, ctx, ksw>>

(***************************************************************************)
(* LeaveBody -- re-mask and clear the marker BEFORE the return tail, so the *)
(* #107 syscall-return preempt still fires and the eret window is masked.   *)
(***************************************************************************)
LeaveBody ==
    /\ pc = "body"
    /\ ~noise
    /\ pc' = "tail"
    /\ masked' = ~BUGGY_LATE_REMASK
    /\ marker' = IF BUGGY_MARKER_NEVER_CLEARED THEN marker ELSE FALSE
    /\ UNCHANGED <<resched, ctx, ksw, noise>>

(***************************************************************************)
(* TailPreempt -- the deferred reschedule fires here, which is the whole    *)
(* bargain: the switch was not forbidden, only POSTPONED to the boundary.   *)
(* A marker that survives the body (BUGGY_MARKER_NEVER_CLEARED) makes the   *)
(* tail DEFER instead, so the thread returns to EL0 still carrying the      *)
(* reschedule the boundary owed it -- the #107 syscall-return preempt never *)
(* fires. It is not starvation (the next EL0 interrupt takes it), so        *)
(* liveness cannot see it; TailTookItsPreempt is the safety statement.      *)
(***************************************************************************)
TailPreempt ==
    /\ pc = "tail"
    /\ resched
    /\ ~Defers
    /\ resched' = FALSE
    /\ UNCHANGED <<pc, masked, marker, ctx, ksw, noise>>

(***************************************************************************)
(* OpenEretWindow -- KERNEL_EXIT installs ELR/SPSR. From here to the `eret` *)
(* the mask is INHERITED, never set locally. #713 lived exactly here.       *)
(***************************************************************************)
OpenEretWindow ==
    /\ pc = "tail"
    \* The tail's preempt check RUNS, unconditionally, before KERNEL_EXIT.
    \* So the window opens only once that check has had its turn: either it
    \* consumed the reschedule, or it DEFERRED -- which is the bug, and is
    \* what TailTookItsPreempt catches.
    /\ (~resched \/ Defers)
    /\ pc' = "eret"
    /\ ctx' = TRUE
    /\ UNCHANGED <<masked, marker, resched, ksw, noise>>

Eret ==
    /\ pc = "eret"
    /\ pc' = "el0"
    /\ ctx' = FALSE
    /\ masked' = FALSE
    /\ marker' = FALSE
    /\ UNCHANGED <<resched, ksw, noise>>

Next ==
    \/ Svc \/ EnterBody \/ LateMark \/ Tick \/ PreemptCheckIrq
    \/ VoluntarySleep \/ NoiseOn \/ NoiseOff
    \/ LeaveBody \/ TailPreempt \/ OpenEretWindow \/ Eret

Fairness ==
    /\ WF_vars(Svc) /\ WF_vars(EnterBody) /\ WF_vars(LateMark)
    /\ WF_vars(PreemptCheckIrq) /\ WF_vars(LeaveBody)
    /\ WF_vars(TailPreempt) /\ WF_vars(OpenEretWindow) /\ WF_vars(Eret)
    \* Tick, VoluntarySleep and the two Noise steps get NOTHING: the
    \* adversary is never obliged to interrupt, to stop making noise, or to
    \* let a body sleep. A property that needs the producer to cooperate is
    \* not a property.

Spec     == Init /\ [][Next]_vars
SpecLive == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* THE PROPERTIES.                                                         *)
(*                                                                         *)
(* NoInvoluntarySwitchInBody -- property 1. Negative, hence the control.    *)
(* EretWindowMasked          -- property 2. #713's shape.                  *)
(* KthreadGetsPreempted      -- THE CONTROL: with KTHREAD=TRUE an           *)
(*   involuntary switch MUST occur, so a green on the two above is not the  *)
(*   green of a model that cannot switch. Stated as an invariant that is    *)
(*   VIOLATED in the kthread cfg -- the violation IS the pass, and          *)
(*   specs/check-syscall-irqs.sh asserts exactly that.                      *)
(* TailTookItsPreempt -- the #107 syscall-return preempt actually fires at  *)
(*   the boundary, rather than being deferred past it by a leaked marker.   *)
(*   This is the BOUNDARY half of the bargain, and it is SAFETY.            *)
(* CpuGetsItsInterrupts -- what the whole chunk buys, and what              *)
(*   poll_cpu.tla checked before this module replaced it: the CPU is        *)
(*   unmasked again and again, whatever a producer does. Round-6 S1 cannot  *)
(*   recur here, because being unmasked is a property of the CPU's state    *)
(*   and not of any thread having slept. BUGGY_MASKED_BODY is the AS-BUILT  *)
(*   model and violates it under noise -- so this module also reproduces    *)
(*   the defect the preemption point was a stopgap for, which is the        *)
(*   evidence that deleting the point is safe rather than merely tidy.      *)
(***************************************************************************)
NoInvoluntarySwitchInBody == ~ksw
EretWindowMasked          == ctx => masked
TailTookItsPreempt        == (pc = "eret") => ~resched
KthreadGetsPreempted      == ~ksw

CpuGetsItsInterrupts           == []<>(~masked)

(***************************************************************************)
(* WHAT THIS MODEL REFUSES TO CLAIM, and why the refusal is the point.     *)
(*                                                                         *)
(* A first cut asserted `[](resched => <>(~resched))` -- a pending          *)
(* reschedule always eventually fires -- and TLC produced a counterexample  *)
(* in five steps: a tick sets it, the thread enters a syscall, the producer *)
(* keeps the body looping, and the switch never happens.                    *)
(*                                                                         *)
(* That counterexample is CORRECT, and the property was wrong. A            *)
(* non-preemptible kernel defers the switch for as long as the syscall      *)
(* runs; that is what "non-preemptible" MEANS. What 8.1 buys is that        *)
(* INTERRUPTS ARE SERVICED throughout (`CpuGetsItsInterrupts`), not that a  *)
(* reschedule is prompt.                                                    *)
(*                                                                         *)
(* Those are different properties, and conflating them is the SAME category *)
(* error this whole chunk exists to repair: ARCH 8.1 said "defer            *)
(* preemption" and the implementation heard "mask interrupts". The model    *)
(* caught the error repeating at a smaller scale, one layer down, in the    *)
(* spec written to prevent it. So the reschedule guarantee is stated where  *)
(* it is actually true -- at the BOUNDARY, as `TailTookItsPreempt` -- and   *)
(* nowhere else.                                                            *)
(*                                                                         *)
(* The residue is real and is NOT closed by this chunk: a user thread       *)
(* looping in a syscall body still holds its CPU against other runnable     *)
(* threads until it returns or sleeps. Interrupts are served, the SAK       *)
(* arrives, drivers run -- but the scheduler does not switch. Closing THAT  *)
(* is full kernel preemption (ROADMAP Phase 7), which this chunk is the     *)
(* precursor to and not a substitute for.                                   *)
(***************************************************************************)
=================================================================================
