-------------------------------- MODULE poll_cpu --------------------------------
(***************************************************************************)
(* poll_cpu -- the CPU half of poll's interrupt-latency bound.             *)
(*                                                                         *)
(* WHY THIS MODULE EXISTS. `poll.tla` models ONE poller, and its           *)
(* `IrqLatencyBounded` is therefore a claim about that poller: it reaches  *)
(* a real sleep, or its preemption point, again and again. Audit round 7   *)
(* F2 measured what that does and does not establish -- and it does NOT    *)
(* establish the thing the preemption point was built for. `[]<>(pc \in    *)
(* RealSleep)` IMPLIES `IrqLatencyBounded` by set inclusion, so a          *)
(* behaviour in which the poller sleeps for ever and never reaches the     *)
(* point satisfies it. That is precisely the shape of round-6 S1. A        *)
(* single-poller model has no CPU to be masked and so cannot see it.       *)
(*                                                                         *)
(* So the obligation moves here. ONE CPU, K pollers, and the one step      *)
(* that carries S1: a poller that blocks hands the CPU DIRECTLY to a       *)
(* runnable peer, because `sched()` switches inside the masked syscall and *)
(* the CPU never reaches its idle loop between them. Under that step,      *)
(* "every poller really sleeps again and again" -- round 5's whole bound,  *)
(* stated here as FAIRNESS, not as a goal -- is satisfied while the CPU    *)
(* takes no interrupt at all.                                              *)
(*                                                                         *)
(* WHAT IS ABSTRACTED. Readiness, hooks, deadlines, death and stop all     *)
(* belong to `poll.tla` and are absent: this module asks one question      *)
(* only, about one CPU. A poller here is any thread looping in an          *)
(* IRQ-masked syscall that an unprivileged producer can keep waking; poll  *)
(* is the instance that has one.                                           *)
(*                                                                         *)
(* THE CPU IS UNMASKED IN EXACTLY TWO PLACES. Idle (the idle loop restores *)
(* the caller's mask around its WFI, so a pending interrupt is taken       *)
(* there) and at a preemption point. Everywhere else a poller is inside a  *)
(* syscall body, which runs masked end to end (ARCHITECTURE.md 8.11; 8.1   *)
(* records that this was never the design). Reaching EL0 would unmask too, *)
(* but a `poll(-1)` under noise never returns, so the model gives the      *)
(* adversary the stronger world in which it does not.                      *)
(*                                                                         *)
(* See ARCHITECTURE.md 23.3 (the preemption point), 8.1 (the masked        *)
(* syscall and the chunk that ends it), and `poll.tla` for everything      *)
(* about a poll call that is not this question.                            *)
(***************************************************************************)
EXTENDS FiniteSets

CONSTANTS
    Pollers,      \* the threads looping in masked syscalls on this ONE CPU.
    SLEEP_ONLY    \* BOOLEAN -- TRUE: round 5's bound alone (a poller yields
                  \*   the CPU only by really sleeping; no preemption point).
                  \*   FALSE: the point, reached on every re-loop.

ASSUME Pollers # {}
ASSUME SLEEP_ONLY \in BOOLEAN

VARIABLES
    pc,           \* [Pollers -> PcStates] -- each poller's own state.
    cur           \* the poller ON the CPU, or Idle.

vars == <<pc, cur>>

\* The loop's ORDER is load-bearing and is modelled, not abstracted: a pass
\* reaches its preemption point BEFORE it can sleep again, because the point
\* sits at a fixed place in the loop body (after the die/stop checks, before
\* the rescan) and the tsleep is below it. A first cut of this module let a
\* poller sleep straight out of "run", which let the adversary hand the CPU
\* off before the point ever fired -- and made the CLEAN cfg fail. The code
\* gives the adversary no such choice.
\*
\* "ready"   -- runnable, off CPU (a producer woke it).
\* "run"     -- on the CPU, inside its masked syscall, BEFORE this pass's
\*              preemption point. Only its own step leaves this state.
\* "atpoint" -- at sched_preempt_point: the CPU is unmasked, pending
\*              interrupts are taken. Not a sleep: the CPU is not yielded.
\* "armed"   -- on the CPU, past the point, at the tsleep: it may now block.
\* "sleep"   -- blocked, off CPU.
PcStates == {"ready", "run", "atpoint", "armed", "sleep"}
Idle      == "idle"

\* On-CPU states. The CPU is occupied iff some poller is in one of them.
OnCpu == {"run", "atpoint", "armed"}

TypeOk ==
    /\ pc  \in [Pollers -> PcStates]
    /\ cur \in Pollers \cup {Idle}
    /\ \A p \in Pollers : (pc[p] \in OnCpu) <=> (cur = p)

Init ==
    /\ pc  = [p \in Pollers |-> "ready"]
    /\ cur = Idle

(***************************************************************************)
(* Open -- the CPU is servicing interrupts. Idle, or at a preemption       *)
(* point. NOT "a poller slept": that is the whole finding.                 *)
(***************************************************************************)
Open == (cur = Idle) \/ (\E p \in Pollers : pc[p] = "atpoint")

(***************************************************************************)
(* Dispatch -- the idle CPU picks up a runnable poller. Entering the       *)
(* syscall masks (hardware exception entry).                               *)
(***************************************************************************)
Dispatch(p) ==
    /\ cur = Idle
    /\ pc[p] = "ready"
    /\ pc'  = [pc EXCEPT ![p] = "run"]
    /\ cur' = p

(***************************************************************************)
(* SleepHandoff -- THE S1 STEP. The running poller blocks and `sched()`    *)
(* dispatches a RUNNABLE peer without the CPU ever idling: the switch      *)
(* happens inside the masked syscall, so nothing unmasks between them.     *)
(* This is why "the thread really slept" does not imply "the CPU took an   *)
(* interrupt", and why round 5's per-thread bound does not compose.        *)
(***************************************************************************)
SleepHandoff(p, q) ==
    /\ cur = p
    /\ pc[p] = "armed"
    /\ q # p
    /\ pc[q] = "ready"
    /\ pc'  = [pc EXCEPT ![p] = "sleep", ![q] = "run"]
    /\ cur' = q

(***************************************************************************)
(* SleepIdle -- the running poller blocks with no runnable peer, so the    *)
(* CPU reaches its idle loop and unmasks. With ONE poller this is the only *)
(* sleep there is, which is exactly why round 5's bound was sound at K=1   *)
(* (poll_cpu_one_poller.cfg is that control).                              *)
(***************************************************************************)
SleepIdle(p) ==
    /\ cur = p
    /\ pc[p] = "armed"
    /\ \A q \in Pollers : (q # p) => (pc[q] # "ready")
    /\ pc'  = [pc EXCEPT ![p] = "sleep"]
    /\ cur' = Idle

\* Either way of really sleeping. Round 5's bound is fairness on THIS.
SleepStep(p) == (\E q \in Pollers : SleepHandoff(p, q)) \/ SleepIdle(p)

(***************************************************************************)
(* Wake -- the noise. A producer on another CPU makes a sleeping poller    *)
(* runnable. The adversary: granted no fairness, never forced to stop.     *)
(***************************************************************************)
Wake(p) ==
    /\ pc[p] = "sleep"
    /\ pc'  = [pc EXCEPT ![p] = "ready"]
    /\ UNCHANGED cur

(***************************************************************************)
(* Point / PointDone -- `sched_preempt_point`. The running poller unmasks, *)
(* the CPU takes every pending interrupt, it re-masks. It does not yield   *)
(* the CPU and it is not a sleep. It is reached on every re-loop whatever  *)
(* any producer does -- that unconditionality is the whole composition     *)
(* argument, and it is what this module checks.                            *)
(***************************************************************************)
Point(p) ==
    /\ ~SLEEP_ONLY
    /\ cur = p
    /\ pc[p] = "run"
    /\ pc' = [pc EXCEPT ![p] = "atpoint"]
    /\ UNCHANGED cur

PointDone(p) ==
    /\ pc[p] = "atpoint"
    /\ pc' = [pc EXCEPT ![p] = "armed"]
    /\ UNCHANGED cur

(***************************************************************************)
(* PassNoPoint -- round 5's loop: the pass reaches its tsleep without ever *)
(* unmasking. The SLEEP_ONLY counterpart of Point + PointDone.             *)
(***************************************************************************)
PassNoPoint(p) ==
    /\ SLEEP_ONLY
    /\ cur = p
    /\ pc[p] = "run"
    /\ pc' = [pc EXCEPT ![p] = "armed"]
    /\ UNCHANGED cur

Next ==
    \/ \E p \in Pollers : Dispatch(p)
    \/ \E p \in Pollers : \E q \in Pollers : SleepHandoff(p, q)
    \/ \E p \in Pollers : SleepIdle(p)
    \/ \E p \in Pollers : Wake(p)
    \/ \E p \in Pollers : Point(p)
    \/ \E p \in Pollers : PointDone(p)
    \/ \E p \in Pollers : PassNoPoint(p)

(***************************************************************************)
(* FAIRNESS. `SleepStep` carries ROUND 5's BOUND: every poller really      *)
(* sleeps again and again, which is what the sleep backstop guaranteed and *)
(* what `poll.tla`'s property is about. It is an assumption here, not a    *)
(* goal -- the point of the module is that granting it in FULL still does  *)
(* not give the CPU its interrupts. Strong fairness, because a poller's    *)
(* chance to sleep is repeatedly enabled and disabled by its peers.        *)
(* The producer (`Wake`) gets nothing.                                     *)
(***************************************************************************)
Fairness ==
    /\ \A p \in Pollers : WF_vars(Dispatch(p))
    /\ \A p \in Pollers : SF_vars(SleepStep(p))
    /\ \A p \in Pollers : WF_vars(Point(p))
    /\ \A p \in Pollers : WF_vars(PointDone(p))
    /\ \A p \in Pollers : WF_vars(PassNoPoint(p))

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* CpuServesIrqs -- THE PROPERTY. This CPU services interrupts again and   *)
(* again, whatever the producers do. `poll.tla`'s IrqLatencyBounded is the *)
(* same sentence about one THREAD; this one is about the CPU, so a sleep   *)
(* that hands the CPU to another masked poller does not satisfy it.        *)
(*                                                                         *)
(* EachPollerSleeps -- round 5's bound, stated as a property so the buggy  *)
(* cfg is self-documenting: it HOLDS there (fairness grants it) while      *)
(* CpuServesIrqs fails. Every poller really sleeps infinitely often, and   *)
(* the CPU still never takes an interrupt. That is round-6 S1, checked.    *)
(***************************************************************************)
CpuServesIrqs    == []<>Open
EachPollerSleeps == \A p \in Pollers : []<>(pc[p] = "sleep")
=================================================================================
