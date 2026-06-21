---------------------------- MODULE sched_tickless ----------------------------
(***************************************************************************)
(* Tickless idle (NO_HZ_IDLE) -- the idle one-shot ARM-RACE (I-9).         *)
(*                                                                         *)
(* Model-first for the TI arc (docs/TICKLESS-IDLE.md; ARCH 8.6 / 8.10).    *)
(* Surfaced by #299: under HVF the never-stopped 1 kHz tick burns ~332%    *)
(* idle CPU (per-tick VTIMER + emulated-GIC MMIO vmexits + a WFI that      *)
(* never parks). The fix stops the tick on a genuinely-idle CPU and arms a *)
(* one-shot; work-arrival rides the existing IPI_RESCHED.                   *)
(*                                                                         *)
(* The audited scheduler.tla ALREADY proves the tickless WAKE-correctness: *)
(* a wfi'd CPU is woken only by the work-arrival IPI (NotifyWFIPeer ->      *)
(* IPI_Deliver) or a deadline Wake(t) -- the periodic tick is not a        *)
(* modeled wake source, and LatencyBound holds without it. So the model    *)
(* was already tickless-shaped; the impl was the laggard.                  *)
(*                                                                         *)
(* This SIBLING (not a scheduler.tla extension -- the arm-race needs the    *)
(* atomic EnterWFI split into a register step + a park step + a new flag +  *)
(* a NotifyWFIPeer precondition change, rippling through scheduler.tla's    *)
(* 11 cfgs; the sched_oncpu / sched_alpha precedent) models the ONE new    *)
(* thing tickless adds: the idle-enter ordering. The impl orders            *)
(* register-then-observe -- idle_in_wfi := TRUE  BEFORE  arm-one-shot+WFI   *)
(* -- so a work-arrival between "nothing runnable" and the park sees a      *)
(* registered CPU and sends an IPI the park observes (the park returns      *)
(* immediately on the pending IRQ). The buggy variant parks BEFORE          *)
(* registering: a peer placing work reads idle_in_wfi = FALSE, sends no     *)
(* IPI, and the CPU sleeps with runnable work pending -- the lost wake the  *)
(* register-then-observe forbids.                                          *)
(*                                                                         *)
(* The deadline one-shot (the timed-sleeper wake) and the 100 ms backstop   *)
(* (defense-in-depth vs a hypothetical dropped IPI) are ORTHOGONAL to the   *)
(* arm-race and prose-validated (docs/TICKLESS-IDLE.md 3.1 / 4); they are   *)
(* deliberately NOT modeled here, so the buggy cfg is a clean lost-wake     *)
(* counterexample rather than a masked latency hiccup (a modeled backstop   *)
(* would eventually wake even the buggy park).                              *)
(*                                                                         *)
(* CONSTANTS                                                               *)
(*   CPUs       -- the set of CPUs (each idles independently; the work      *)
(*                 producer is abstracted as the PlaceWork environment      *)
(*                 action, so the arm-race is captured at one CPU).         *)
(*   BUGGY_PARK -- TRUE enables BuggyPark (park-before-register); the        *)
(*                 sched_tickless_buggy.cfg counterexample.                 *)
(*                                                                         *)
(* SAFETY                                                                  *)
(*   NoLostWake              -- ~(parked /\ pending): no runnable thread     *)
(*                              sits in a parked CPU's runq. The I-9 safety. *)
(*   ParkedImpliesRegistered -- a parked CPU registered first (the          *)
(*                              register-then-observe ordering).            *)
(*   RunningNotParked        -- a running CPU is not parked (sanity).        *)
(* LIVENESS                                                                *)
(*   EventuallyRuns -- pending[c] ~> running[c]: every runnable thread      *)
(*                     eventually dispatched (the IPI lifts the park).      *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS CPUs, BUGGY_PARK

VARIABLES
    running,     \* [CPUs -> BOOLEAN] -- executing a thread
    registered,  \* [CPUs -> BOOLEAN] -- idle_in_wfi announced (the "register")
    parked,      \* [CPUs -> BOOLEAN] -- in WFI, one-shot armed
    pending      \* [CPUs -> BOOLEAN] -- a thread is runnable for this CPU (in its runq)

vars == <<running, registered, parked, pending>>

TypeOK ==
    /\ running    \in [CPUs -> BOOLEAN]
    /\ registered \in [CPUs -> BOOLEAN]
    /\ parked     \in [CPUs -> BOOLEAN]
    /\ pending    \in [CPUs -> BOOLEAN]

Init ==
    /\ running    = [c \in CPUs |-> TRUE]    \* every CPU starts running a thread
    /\ registered = [c \in CPUs |-> FALSE]
    /\ parked     = [c \in CPUs |-> FALSE]
    /\ pending    = [c \in CPUs |-> FALSE]

(* Block(cpu): the running thread blocks/exits with nothing else runnable;  *)
(* the CPU goes idle (unregistered, unparked).                             *)
Block(cpu) ==
    /\ running[cpu]
    /\ ~pending[cpu]
    /\ running' = [running EXCEPT ![cpu] = FALSE]
    /\ UNCHANGED <<registered, parked, pending>>

(* Register(cpu): the idle CPU sets idle_in_wfi := TRUE -- the "register"    *)
(* half of register-then-observe, done BEFORE the park.                     *)
Register(cpu) ==
    /\ ~running[cpu]
    /\ ~registered[cpu]
    /\ ~parked[cpu]
    /\ ~pending[cpu]
    /\ registered' = [registered EXCEPT ![cpu] = TRUE]
    /\ UNCHANGED <<running, parked, pending>>

(* Park(cpu): arm the one-shot + WFI. CORRECT: requires registered. The      *)
(* ~pending guard is the "observe" half -- the CPU re-checks for work after  *)
(* registering and parks only if still none.                                *)
Park(cpu) ==
    /\ ~running[cpu]
    /\ registered[cpu]
    /\ ~parked[cpu]
    /\ ~pending[cpu]
    /\ parked' = [parked EXCEPT ![cpu] = TRUE]
    /\ UNCHANGED <<running, registered, pending>>

(* BuggyPark(cpu): the arm-before-register bug -- park WITHOUT registering.  *)
(* A peer placing work then reads registered = FALSE and sends no IPI.       *)
BuggyPark(cpu) ==
    /\ BUGGY_PARK
    /\ ~running[cpu]
    /\ ~parked[cpu]
    /\ ~pending[cpu]
    /\ parked' = [parked EXCEPT ![cpu] = TRUE]    \* BUG: no registered guard
    /\ UNCHANGED <<running, registered, pending>>

(* PlaceWork(cpu): a peer readies a thread for cpu (places it in cpu's runq).*)
(* The peer reads idle_in_wfi (registered) to decide IPI_RESCHED:            *)
(*   registered  -> IPI -> the park is lifted (parked' = FALSE).             *)
(*   ~registered -> no IPI (the CPU is assumed not parked / about to         *)
(*                  observe); parked left as-is.                            *)
(* CORRECT: parked => registered, so PlaceWork on a parked CPU always lifts  *)
(* the park -- no lost wake. BUGGY: a CPU can be parked while ~registered,    *)
(* so the work-arrival IPI is lost.                                         *)
PlaceWork(cpu) ==
    /\ ~pending[cpu]
    /\ ~running[cpu]
    /\ pending' = [pending EXCEPT ![cpu] = TRUE]
    /\ parked'  = [parked EXCEPT ![cpu] = IF registered[cpu] THEN FALSE ELSE @]
    /\ UNCHANGED <<running, registered>>

(* Dispatch(cpu): the CPU observes runnable work while NOT parked and runs   *)
(* it -- the "observe" of an un-parked idle CPU, and the post-wake dispatch  *)
(* after an IPI lifted the park (parked = FALSE, pending = TRUE).            *)
Dispatch(cpu) ==
    /\ pending[cpu]
    /\ ~parked[cpu]
    /\ ~running[cpu]
    /\ running'    = [running    EXCEPT ![cpu] = TRUE]
    /\ pending'    = [pending    EXCEPT ![cpu] = FALSE]
    /\ registered' = [registered EXCEPT ![cpu] = FALSE]
    /\ UNCHANGED parked

Next ==
    \E cpu \in CPUs :
        \/ Block(cpu)
        \/ Register(cpu)
        \/ Park(cpu)
        \/ BuggyPark(cpu)
        \/ PlaceWork(cpu)
        \/ Dispatch(cpu)

Spec == Init /\ [][Next]_vars

(* Liveness: weak fairness on Dispatch is sufficient -- once work is pending  *)
(* on an un-parked CPU, it runs. The work-arrival IPI lift is folded into     *)
(* PlaceWork's atomic step (it clears parked when registered), so a pending   *)
(* thread is always on an un-parked CPU in the correct model; no fairness on  *)
(* PlaceWork/Register/Park is needed (the property is that a PENDING thread   *)
(* runs, not that work arrives or that an idle CPU parks).                    *)
Fairness == \A cpu \in CPUs : WF_vars(Dispatch(cpu))

Spec_Live == Spec /\ Fairness

(* ---- Safety invariants ---- *)

NoLostWake              == \A c \in CPUs : ~(parked[c] /\ pending[c])
ParkedImpliesRegistered == \A c \in CPUs : parked[c] => registered[c]
RunningNotParked        == \A c \in CPUs : running[c] => ~parked[c]

(* ---- Liveness ---- *)

EventuallyRuns == \A c \in CPUs : (pending[c] ~> running[c])

=============================================================================
