---------------------------- MODULE sched_rebalance ----------------------------
(***************************************************************************)
(* Push-on-overload rebalance -- work-conservation under tickless idle      *)
(* (TI-4; the I-9 / work-conservation generalization).                      *)
(*                                                                         *)
(* Model-first for TI-4 (docs/TICKLESS-IDLE.md TI-4; ARCH 8.6 / 8.10).      *)
(* Surfaced by the TI-3 regression: NO_HZ_IDLE stopped the 1 kHz tick that   *)
(* was silently the work-stealing RE-POLL -- an idle CPU re-ran try_steal     *)
(* every tick, so any work-arrival the best-effort single-kick missed was     *)
(* pulled within 1 ms. Tickless removed that re-poll; queued work stranded    *)
(* on a busy CPU until the 100 ms backstop -> a 2.4x boot slowdown.           *)
(*                                                                         *)
(* The SOTA fix (Linux NO_HZ_IDLE push model + FreeBSD ULE + Zircon, all      *)
(* primary-source-confirmed): a BUSY CPU -- which is STILL ticking, since      *)
(* tickless only stops the IDLE tick -- detects it has surplus queued work    *)
(* AND an idle peer, and PUSHES (kicks) the peer to take the work. This       *)
(* sibling models exactly that one new mechanism: the busy-side overload      *)
(* kick + its register-then-observe obligation (the kick must lift a parked   *)
(* peer's park, or the work is lost -- the I-9 leg).                          *)
(*                                                                         *)
(* WHERE THIS SITS (the sched_oncpu / sched_alpha / sched_tickless sibling    *)
(* precedent -- a focused new module, NOT a mutation of a landed spec whose   *)
(* cfgs would ripple):                                                        *)
(*   - sched_alpha.tla   : proves SAFETY of placement under an ARBITRARY      *)
(*                         target CPU (Place picks the target                 *)
(*                         non-deterministically) -- so push-PLACEMENT (place *)
(*                         a waking thread on an IDLE CPU) is already inside   *)
(*                         its proven envelope; TI-4 adds no placement model. *)
(*   - sched_tickless.tla : proves the idle-enter arm-race (register-then-     *)
(*                         observe: no work-ARRIVAL wake lost across the park).*)
(*   - sched_rebalance.tla: THIS -- the busy-side push of ALREADY-QUEUED work  *)
(*                         to an idle peer, the leg the periodic tick used to  *)
(*                         cover by pull-polling. The new liveness mechanism.  *)
(*                                                                         *)
(* The 100 ms backstop (defense-in-depth vs a dropped kick) is, as in         *)
(* sched_tickless, deliberately NOT modeled -- so BUGGY_NO_KICK is a clean     *)
(* stranded-work counterexample (the regression in model form) rather than a  *)
(* masked latency hiccup that a modeled backstop would eventually heal.       *)
(*                                                                         *)
(* CONSTANTS                                                               *)
(*   CPUs               -- the set of CPUs (>= 2). Init designates one as the  *)
(*                         producer (running, holding surplus queued work);    *)
(*                         the rest start idle.                               *)
(*   BUGGY_NO_KICK      -- TRUE disables Overload (no busy-side kick) -> the    *)
(*                         surplus strands: the EventuallyParallelized         *)
(*                         liveness counterexample (= the TI-3 regression).    *)
(*   BUGGY_KICK_NO_LIFT -- TRUE makes Overload migrate work to a peer WITHOUT  *)
(*                         lifting its park -> NoLostWake violated (the kick    *)
(*                         that forgets register-then-observe).                *)
(*                                                                         *)
(* SAFETY                                                                  *)
(*   NoLostWake             -- ~(parked /\ pending): a CPU with work pushed    *)
(*                             onto it is never left parked. The kick's I-9    *)
(*                             obligation (the BUGGY_KICK_NO_LIFT target).     *)
(*   PendingImpliesNotRunning-- pushed-but-not-yet-run work implies the target *)
(*                             is not already running (migration sanity).      *)
(*   SurplusImpliesRunning  -- surplus exists only on a running CPU (sanity).  *)
(*   ParkedImpliesRegistered-- a parked CPU registered first (carried over).   *)
(* LIVENESS                                                                *)
(*   EventuallyParallelized -- surplus[c] ~> ~surplus[c]: queued surplus on a  *)
(*                             busy CPU, with an idle peer, is eventually       *)
(*                             taken off it -- work-conservation. The property  *)
(*                             the busy-side kick restores (the BUGGY_NO_KICK   *)
(*                             target).                                        *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS CPUs, BUGGY_NO_KICK, BUGGY_KICK_NO_LIFT

VARIABLES
    running,     \* [CPUs -> BOOLEAN] -- executing a (non-idle) work thread
    surplus,     \* [CPUs -> BOOLEAN] -- has queued runnable work beyond what it runs
    registered,  \* [CPUs -> BOOLEAN] -- idle_in_wfi announced (the "register")
    parked,      \* [CPUs -> BOOLEAN] -- in WFI, one-shot armed
    pending      \* [CPUs -> BOOLEAN] -- work pushed here, runnable, awaiting dispatch

vars == <<running, surplus, registered, parked, pending>>

TypeOK ==
    /\ running    \in [CPUs -> BOOLEAN]
    /\ surplus    \in [CPUs -> BOOLEAN]
    /\ registered \in [CPUs -> BOOLEAN]
    /\ parked     \in [CPUs -> BOOLEAN]
    /\ pending    \in [CPUs -> BOOLEAN]

(* The producer: one CPU that starts running WITH surplus queued work. The   *)
(* rest start idle. The only ways the producer's surplus clears are Overload *)
(* (the kick -> migrate to an idle peer) or DrainLocally (it runs the work    *)
(* itself -- UNFAIR, a CPU-bound producer need not yield). No re-production:  *)
(* surplus is set only at Init, so EventuallyParallelized is an unconditional *)
(* leads-to in the clean case (no saturation where every peer is busy).       *)
Producer == CHOOSE c \in CPUs : TRUE

Init ==
    /\ running    = [c \in CPUs |-> c = Producer]
    /\ surplus    = [c \in CPUs |-> c = Producer]
    /\ registered = [c \in CPUs |-> FALSE]
    /\ parked     = [c \in CPUs |-> FALSE]
    /\ pending    = [c \in CPUs |-> FALSE]

(* Block(c): a running CPU with no surplus finishes/blocks -> goes idle.     *)
Block(c) ==
    /\ running[c]
    /\ ~surplus[c]
    /\ running' = [running EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<surplus, registered, parked, pending>>

(* DrainLocally(c): a running CPU runs its OWN queued surplus (no migration). *)
(* UNFAIR -- a CPU-bound producer need not yield, so this models "the surplus  *)
(* MIGHT drain locally" without guaranteeing it. The worst case the kick must  *)
(* cover is the producer that never drains -- so NO fairness on this action.   *)
DrainLocally(c) ==
    /\ running[c]
    /\ surplus[c]
    /\ surplus' = [surplus EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<running, registered, parked, pending>>

(* Register(c) / Park(c): the tickless idle-enter (carried from              *)
(* sched_tickless). The ~pending guard is the "observe" half -- a CPU with     *)
(* work pushed onto it (pending) cannot park, so an UNregistered idle target   *)
(* never sleeps through a push.                                               *)
Register(c) ==
    /\ ~running[c] /\ ~registered[c] /\ ~parked[c] /\ ~pending[c]
    /\ registered' = [registered EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<running, surplus, parked, pending>>

Park(c) ==
    /\ ~running[c] /\ registered[c] /\ ~parked[c] /\ ~pending[c]
    /\ parked' = [parked EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<running, surplus, registered, pending>>

(* Overload(b, i): the BUSY-side push -- the TI-4 mechanism. A running CPU b   *)
(* with surplus queued work kicks an idle peer i, migrating the work. The     *)
(* kick lifts i's park exactly as sched_tickless's PlaceWork does             *)
(* (register-then-observe): if i registered (idle_in_wfi), the kick IPI is    *)
(* observed and the park lifts; if i is not yet registered, the ~pending      *)
(* guard on Park keeps it from sleeping. BUGGY_KICK_NO_LIFT drops the lift ->  *)
(* a parked peer keeps pending work asleep (NoLostWake violated).             *)
(* BUGGY_NO_KICK disables the whole action -> the surplus strands             *)
(* (EventuallyParallelized violated).                                         *)
Overload(b, i) ==
    /\ ~BUGGY_NO_KICK
    /\ b # i
    /\ running[b] /\ surplus[b]
    /\ ~running[i] /\ ~pending[i]
    /\ surplus' = [surplus EXCEPT ![b] = FALSE]
    /\ pending' = [pending EXCEPT ![i] = TRUE]
    /\ parked'  = [parked EXCEPT ![i] =
                     IF (registered[i] /\ ~BUGGY_KICK_NO_LIFT) THEN FALSE ELSE @]
    /\ UNCHANGED <<running, registered>>

(* Dispatch(i): the kicked idle CPU observes the pushed work and runs it      *)
(* (becomes busy). Requires ~parked -- so a BUGGY_KICK_NO_LIFT-parked peer     *)
(* never dispatches (the work is doubly stuck: parked AND pending).           *)
Dispatch(i) ==
    /\ pending[i] /\ ~parked[i] /\ ~running[i]
    /\ running'    = [running    EXCEPT ![i] = TRUE]
    /\ pending'    = [pending    EXCEPT ![i] = FALSE]
    /\ registered' = [registered EXCEPT ![i] = FALSE]
    /\ UNCHANGED <<surplus, parked>>

Next ==
    \/ \E c \in CPUs    : Block(c)
    \/ \E c \in CPUs    : DrainLocally(c)
    \/ \E c \in CPUs    : Register(c)
    \/ \E c \in CPUs    : Park(c)
    \/ \E b, i \in CPUs : Overload(b, i)
    \/ \E c \in CPUs    : Dispatch(c)

Spec == Init /\ [][Next]_vars

(* Liveness: WF on the kick (a busy CPU's still-running tick reliably runs    *)
(* the overload check) and on Dispatch (a non-parked CPU with pending work    *)
(* runs it). NO fairness on DrainLocally / Block -- a producer need not        *)
(* yield, so with an idle peer present the only GUARANTEED way surplus        *)
(* clears is the kick.                                                        *)
Fairness ==
    /\ \A b, i \in CPUs : WF_vars(Overload(b, i))
    /\ \A c \in CPUs    : WF_vars(Dispatch(c))

Spec_Live == Spec /\ Fairness

(* ---- Safety ---- *)
NoLostWake               == \A c \in CPUs : ~(parked[c] /\ pending[c])
PendingImpliesNotRunning == \A c \in CPUs : pending[c] => ~running[c]
SurplusImpliesRunning    == \A c \in CPUs : surplus[c] => running[c]
ParkedImpliesRegistered  == \A c \in CPUs : parked[c] => registered[c]

(* ---- Liveness ---- *)
EventuallyParallelized   == \A c \in CPUs : surplus[c] ~> ~surplus[c]

=============================================================================
