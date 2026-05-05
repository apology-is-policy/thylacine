---- MODULE scheduler ----
(***************************************************************************)
(* Thylacine scheduler — P2-A sketch.                                      *)
(*                                                                         *)
(* Models the thread state machine and per-CPU dispatch at the level       *)
(* established by P2-A (struct Proc, struct Thread, context-switch         *)
(* primitive, no actual scheduler yet — direct switches in test code).     *)
(*                                                                         *)
(* The sketch pins these invariants now so subsequent sub-chunks (P2-B's   *)
(* EEVDF scheduler, P2-C's work-stealing) refine into a verified shape     *)
(* rather than discover the framing late.                                  *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   StateConsistency       — a thread is RUNNING iff it is some CPU's    *)
(*                             current.                                    *)
(*   NoSimultaneousRun      — a thread runs on at most one CPU at a time. *)
(*   RunnableInQueue        — a thread is RUNNABLE iff it sits in some    *)
(*                             CPU's runqueue.                             *)
(*   SleepingNotInQueue     — a SLEEPING thread is not in any runqueue    *)
(*                             and is no CPU's current.                    *)
(*                                                                         *)
(* Refined in subsequent sub-chunks (TODO markers in the action bodies):   *)
(*                                                                         *)
(*   P2-B: EEVDF deadline math (vd_t, ve_t advancement). Pick-next is      *)
(*         currently CHOOSE-from-runq; refine to pick-earliest-deadline.   *)
(*                                                                         *)
(*   P2-B: Wait/wake atomicity (ARCH §28 I-9, §8.5). Block is currently    *)
(*         atomic; refine to expose the cond-check / sleep race so the     *)
(*         spec actually proves the wait/wake protocol.                    *)
(*                                                                         *)
(*   P2-B: IPI ordering across CPUs (ARCH §28 I-18, §8.7). Send-order      *)
(*         delivery; not modeled in P2-A.                                  *)
(*                                                                         *)
(*   P2-C: Work-stealing fairness (ARCH §8.4). Cross-CPU dequeue with      *)
(*         lock; not modeled in P2-A.                                      *)
(*                                                                         *)
(*   Phase 2 close: Latency bound (ARCH §28 I-17 — slice_size × N upper   *)
(*         bound). Liveness via weak fairness; not modeled in the sketch. *)
(*                                                                         *)
(* See ARCHITECTURE.md §8 (scheduler design); §28 invariants I-8, I-9,     *)
(* I-17, I-18.                                                             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,        \* set of thread identifiers
    CPUs,           \* set of CPU identifiers
    NULL            \* sentinel — represents "no thread"

ASSUME NULL \notin Threads
ASSUME Cardinality(Threads) >= 1
ASSUME Cardinality(CPUs)    >= 1

VARIABLES
    state,          \* state[t]   ∈ {"RUNNING", "RUNNABLE", "SLEEPING"}
    current,        \* current[c] ∈ Threads ∪ {NULL}
    runq            \* runq[c]    ⊆ Threads — per-CPU runqueue

vars == <<state, current, runq>>

States == {"RUNNING", "RUNNABLE", "SLEEPING"}

TypeOk ==
    /\ state   \in [Threads -> States]
    /\ current \in [CPUs    -> Threads \cup {NULL}]
    /\ runq    \in [CPUs    -> SUBSET Threads]

(***************************************************************************)
(* Initial state: pick one CPU as cpu0 and one thread as t0; t0 starts     *)
(* RUNNING on cpu0; all other threads are RUNNABLE in cpu0's runqueue.     *)
(* Other CPUs are idle (current = NULL, empty runq). Models the boot       *)
(* condition where the kernel thread enters main() and other kthreads      *)
(* are pre-allocated but not yet scheduled.                                *)
(***************************************************************************)
Init ==
    LET cpu0 == CHOOSE c \in CPUs    : TRUE
        t0   == CHOOSE t \in Threads : TRUE
    IN  /\ state   = [t \in Threads |-> IF t = t0 THEN "RUNNING" ELSE "RUNNABLE"]
        /\ current = [c \in CPUs    |-> IF c = cpu0 THEN t0 ELSE NULL]
        /\ runq    = [c \in CPUs    |-> IF c = cpu0 THEN Threads \ {t0} ELSE {}]

(***************************************************************************)
(* Yield(cpu): the running thread on `cpu` voluntarily yields. Goes to     *)
(* RUNNABLE, joins the runqueue. The CPU picks next from its runqueue if   *)
(* one exists; otherwise the same thread keeps running (no idle thread     *)
(* modeled — added in P2-B).                                               *)
(*                                                                         *)
(* This is the spec-level analogue of cpu_switch_context(prev, next)       *)
(* introduced in P2-A.                                                     *)
(***************************************************************************)
Yield(cpu) ==
    /\ current[cpu] # NULL
    /\ LET prev == current[cpu]
           rq   == runq[cpu]
       IN  IF rq = {} THEN
              UNCHANGED vars
           ELSE
              LET next == CHOOSE t \in rq : TRUE
              IN  /\ state'   = [state EXCEPT ![prev] = "RUNNABLE",
                                              ![next] = "RUNNING"]
                  /\ current' = [current EXCEPT ![cpu] = next]
                  /\ runq'    = [runq EXCEPT ![cpu] = (rq \ {next}) \cup {prev}]

(***************************************************************************)
(* Block(cpu): the running thread on `cpu` blocks (transitions to          *)
(* SLEEPING — out of any runqueue). The CPU picks next from its runqueue   *)
(* or goes idle (current = NULL).                                          *)
(*                                                                         *)
(* P2-A models the transition atomically; P2-B refines this to expose      *)
(* the wait/wake race and prove I-9 (no missed wakeups).                   *)
(***************************************************************************)
Block(cpu) ==
    /\ current[cpu] # NULL
    /\ LET prev == current[cpu]
           rq   == runq[cpu]
       IN  IF rq = {} THEN
              /\ state'   = [state EXCEPT ![prev] = "SLEEPING"]
              /\ current' = [current EXCEPT ![cpu] = NULL]
              /\ runq'    = runq
           ELSE
              LET next == CHOOSE t \in rq : TRUE
              IN  /\ state'   = [state EXCEPT ![prev] = "SLEEPING",
                                              ![next] = "RUNNING"]
                  /\ current' = [current EXCEPT ![cpu] = next]
                  /\ runq'    = [runq EXCEPT ![cpu] = rq \ {next}]

(***************************************************************************)
(* Wake(t): a sleeping thread becomes runnable. Inserted into some CPU's   *)
(* runqueue (modeled non-deterministically; a real impl picks based on     *)
(* affinity / work distribution — refined in P2-C work-stealing).          *)
(***************************************************************************)
Wake(t) ==
    /\ state[t] = "SLEEPING"
    /\ \E cpu \in CPUs :
        /\ state' = [state EXCEPT ![t] = "RUNNABLE"]
        /\ runq'  = [runq  EXCEPT ![cpu] = runq[cpu] \cup {t}]
        /\ UNCHANGED current

(***************************************************************************)
(* Resume(cpu): an idle CPU picks up a runnable thread from its queue.     *)
(* Models the scheduler's dispatch-from-idle path.                         *)
(***************************************************************************)
Resume(cpu) ==
    /\ current[cpu] = NULL
    /\ runq[cpu]    # {}
    /\ LET next == CHOOSE t \in runq[cpu] : TRUE
       IN  /\ state'   = [state   EXCEPT ![next] = "RUNNING"]
           /\ current' = [current EXCEPT ![cpu]  = next]
           /\ runq'    = [runq    EXCEPT ![cpu]  = runq[cpu] \ {next}]

Next ==
    \/ \E cpu \in CPUs    : Yield(cpu)
    \/ \E cpu \in CPUs    : Block(cpu)
    \/ \E t   \in Threads : Wake(t)
    \/ \E cpu \in CPUs    : Resume(cpu)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* StateConsistency — a thread is RUNNING iff it is some CPU's current.
StateConsistency ==
    \A t \in Threads :
        (state[t] = "RUNNING") <=>
        (\E cpu \in CPUs : current[cpu] = t)

\* NoSimultaneousRun — a thread runs on at most one CPU at a time.
NoSimultaneousRun ==
    \A t \in Threads :
        Cardinality({cpu \in CPUs : current[cpu] = t}) <= 1

\* RunnableInQueue — a thread is RUNNABLE iff it sits in some runqueue.
RunnableInQueue ==
    \A t \in Threads :
        (state[t] = "RUNNABLE") <=>
        (\E cpu \in CPUs : t \in runq[cpu])

\* SleepingNotInQueue — a SLEEPING thread is in no runqueue and is no
\* CPU's current.
SleepingNotInQueue ==
    \A t \in Threads :
        (state[t] = "SLEEPING") =>
            /\ \A cpu \in CPUs : t \notin runq[cpu]
            /\ \A cpu \in CPUs : current[cpu] # t

\* Composite — checked by the .cfg.
Invariants ==
    /\ TypeOk
    /\ StateConsistency
    /\ NoSimultaneousRun
    /\ RunnableInQueue
    /\ SleepingNotInQueue

====
