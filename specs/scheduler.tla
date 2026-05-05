---- MODULE scheduler ----
(***************************************************************************)
(* Thylacine scheduler — P2-B refinement (in progress).                    *)
(*                                                                         *)
(* Models the thread state machine + per-CPU dispatch + the wait/wake      *)
(* protocol per ARCHITECTURE.md §8.5 (wakeup atomicity, I-9). The earlier  *)
(* P2-A sketch pinned StateConsistency / NoSimultaneousRun /              *)
(* RunnableInQueue / SleepingNotInQueue. P2-B adds wait/wake protocol     *)
(* modeling +                                                              *)
(* NoMissedWakeup invariant — the proof that under the protocol, a thread *)
(* cannot lose a wakeup to the canonical race between cond-check and       *)
(* sleep-transition.                                                       *)
(*                                                                         *)
(* Protocol modeled (correct, atomic — see ARCH §8.5):                     *)
(*                                                                         *)
(*   1. A acquires per-condition lock (modeled implicitly — actions are    *)
(*      atomic).                                                           *)
(*   2. A checks cond; if true, returns.                                   *)
(*   3. A enqueues itself into waiters AND transitions to SLEEPING in a    *)
(*      single atomic step.                                                *)
(*   4. B sets cond := TRUE AND wakes every thread in waiters AND clears   *)
(*      waiters in a single atomic step.                                   *)
(*                                                                         *)
(* The atomic combination of steps 2-3 (cond check + enqueue + sleep) is   *)
(* what defeats the missed-wakeup race. The .cfg pattern:                  *)
(*                                                                         *)
(*   scheduler.cfg       BUGGY=FALSE — TLC proves NoMissedWakeup holds.    *)
(*   scheduler_buggy.cfg BUGGY=TRUE  — TLC produces a counterexample where *)
(*                                     the bug splits step 2 from step 3   *)
(*                                     and a Wake fires between them.      *)
(*                                                                         *)
(* This is the executable-documentation pattern per CLAUDE.md spec-first   *)
(* policy: primary config says "this is how it should work," buggy config  *)
(* says "this is the specific way it could fail."                          *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   StateConsistency       — a thread is RUNNING iff some CPU's current. *)
(*   NoSimultaneousRun      — a thread runs on at most one CPU at a time. *)
(*   RunnableInQueue        — a thread is RUNNABLE iff it sits in some    *)
(*                             CPU's runqueue.                             *)
(*   SleepingNotInQueue     — a SLEEPING thread is in no runqueue and is  *)
(*                             no CPU's current.                           *)
(*   NoMissedWakeup (NEW)   — cond=TRUE ⇒ waiters={}. The wait/wake       *)
(*                             atomicity proof per ARCH §28 I-9, §8.5.     *)
(*                                                                         *)
(* P2-Bc note (preemption mapping):                                        *)
(*                                                                         *)
(*   The C impl adds timer-IRQ-driven preemption: sched_tick() decrements *)
(*   current's slice; on slice expiry, sets need_resched; preempt_check_irq*)
(*   from the IRQ-return path observes need_resched and calls sched().    *)
(*                                                                         *)
(*   In the spec, the involuntary preempt path maps to the existing       *)
(*   Yield(cpu) action — Yield is non-deterministic (TLC explores all     *)
(*   firings), and any cooperative yield is observably indistinguishable  *)
(*   from a preempt-driven yield. The atomicity that matters for          *)
(*   NoMissedWakeup (cond check + sleep transition under r->lock) is      *)
(*   preserved by the spec's atomic-action model: WaitOnCond is a single *)
(*   step; no Yield can fire mid-action. The C impl mirrors this with    *)
(*   spin_lock_irqsave (IRQ mask) bracketing the WaitOnCond body, so a   *)
(*   timer-IRQ-driven preempt cannot fire between cond check and sleep  *)
(*   transition.                                                           *)
(*                                                                         *)
(*   Latency bound (I-17, slice × N) is a liveness property requiring    *)
(*   weak fairness; deferred to a Phase-2-close refinement that adds      *)
(*   weakly-fair Yield + an explicit Slice variable.                      *)
(*                                                                         *)
(* Refined in subsequent sub-chunks:                                       *)
(*                                                                         *)
(*   P2-Bc (later): full EEVDF deadline math (vd_t = ve_t + slice ×      *)
(*                 W_total / w_self; weighted virtual time advance).      *)
(*                 At v1.0 weight=1 always — current g_vd_counter++       *)
(*                 advance is a valid instantiation of the math.          *)
(*                                                                         *)
(*   P2-C: IPI ordering across CPUs (ARCH §28 I-18, §8.7). Send-order     *)
(*         delivery; not yet modeled.                                      *)
(*                                                                         *)
(*   P2-C: Work-stealing fairness (ARCH §8.4). Cross-CPU dequeue with     *)
(*         lock; not yet modeled.                                          *)
(*                                                                         *)
(*   Phase 2 close: Liveness (ARCH §28 I-8) — every runnable thread       *)
(*                  eventually runs. Latency bound (I-17, slice_size × N).*)
(*                  Modeled with weak fairness and TLC liveness checks.   *)
(*                                                                         *)
(* Modeling assumption (closed-universe — P2-A audit R4 F47):              *)
(*                                                                         *)
(*   The set Threads is fixed at Init; the spec doesn't model thread       *)
(*   creation (the C-level thread_create is open-universe — threads are    *)
(*   added dynamically). The closed universe is sound for proving the      *)
(*   state-machine + wait/wake invariants because thread_create's          *)
(*   semantics ("new thread starts RUNNABLE in some runqueue") are         *)
(*   structurally compatible with the existing invariants. P2-B will add   *)
(*   a Spawn(t) action to model the open-universe; the wait/wake proofs    *)
(*   here carry over.                                                      *)
(*                                                                         *)
(* See ARCHITECTURE.md §8 (scheduler design); §28 invariants I-8, I-9,     *)
(* I-17, I-18.                                                             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,        \* set of thread identifiers
    CPUs,           \* set of CPU identifiers
    NULL,           \* sentinel — represents "no thread"
    BUGGY           \* P2-B: BOOLEAN — when TRUE, BuggyCheck + BuggySleep
                    \*   actions fire instead of WaitOnCond, exposing the
                    \*   missed-wakeup race. Set by scheduler_buggy.cfg.

ASSUME NULL \notin Threads
ASSUME Cardinality(Threads) >= 1
ASSUME Cardinality(CPUs)    >= 1
ASSUME BUGGY \in BOOLEAN

VARIABLES
    state,          \* state[t]   ∈ {"RUNNING", "RUNNABLE", "SLEEPING"}
    current,        \* current[c] ∈ Threads ∪ {NULL}
    runq,           \* runq[c]    ⊆ Threads — per-CPU runqueue
    cond,           \* P2-B: BOOLEAN — the wait condition (one shared cond).
    waiters,        \* P2-B: ⊆ Threads — threads sleeping on cond.
    pending_sleep   \* P2-B: [Threads -> BOOLEAN] — buggy intent flag for
                    \*   threads that have observed cond=FALSE but haven't
                    \*   yet transitioned to SLEEPING (modeling the split
                    \*   between cond check and sleep transition).

vars == <<state, current, runq, cond, waiters, pending_sleep>>

States == {"RUNNING", "RUNNABLE", "SLEEPING"}

TypeOk ==
    /\ state          \in [Threads -> States]
    /\ current        \in [CPUs    -> Threads \cup {NULL}]
    /\ runq           \in [CPUs    -> SUBSET Threads]
    /\ cond           \in BOOLEAN
    /\ waiters        \subseteq Threads
    /\ pending_sleep  \in [Threads -> BOOLEAN]

(***************************************************************************)
(* Initial state: pick one CPU as cpu0 and one thread as t0; t0 starts     *)
(* RUNNING on cpu0; all other threads are RUNNABLE in cpu0's runqueue.     *)
(* cond starts FALSE; waiters and pending_sleep start empty.               *)
(***************************************************************************)
Init ==
    LET cpu0 == CHOOSE c \in CPUs    : TRUE
        t0   == CHOOSE t \in Threads : TRUE
    IN  /\ state          = [t \in Threads |-> IF t = t0 THEN "RUNNING" ELSE "RUNNABLE"]
        /\ current        = [c \in CPUs    |-> IF c = cpu0 THEN t0 ELSE NULL]
        /\ runq           = [c \in CPUs    |-> IF c = cpu0 THEN Threads \ {t0} ELSE {}]
        /\ cond           = FALSE
        /\ waiters        = {}
        /\ pending_sleep  = [t \in Threads |-> FALSE]

(***************************************************************************)
(* Yield(cpu): the running thread on `cpu` voluntarily yields. Goes to     *)
(* RUNNABLE, joins the runqueue. The CPU picks next from its runqueue if   *)
(* one exists; otherwise the same thread keeps running.                    *)
(*                                                                         *)
(* Spec-level analogue of cpu_switch_context(prev, next) introduced in     *)
(* P2-A. Disabled mid-buggy-sequence so the buggy split is observable.     *)
(***************************************************************************)
Yield(cpu) ==
    /\ current[cpu] # NULL
    /\ ~pending_sleep[current[cpu]]
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
                  /\ UNCHANGED <<cond, waiters, pending_sleep>>

(***************************************************************************)
(* Block(cpu): voluntary block — thread blocks for a non-cond reason       *)
(* (e.g., explicit kill, postnote stop, etc.). Atomic transition to        *)
(* SLEEPING. Disabled under BUGGY so the buggy state space focuses on      *)
(* the wait/wake split.                                                    *)
(***************************************************************************)
Block(cpu) ==
    /\ ~BUGGY
    /\ current[cpu] # NULL
    /\ ~pending_sleep[current[cpu]]
    /\ LET prev == current[cpu]
           rq   == runq[cpu]
       IN  IF rq = {} THEN
              /\ state'   = [state EXCEPT ![prev] = "SLEEPING"]
              /\ current' = [current EXCEPT ![cpu] = NULL]
              /\ UNCHANGED <<runq, cond, waiters, pending_sleep>>
           ELSE
              LET next == CHOOSE t \in rq : TRUE
              IN  /\ state'   = [state EXCEPT ![prev] = "SLEEPING",
                                              ![next] = "RUNNING"]
                  /\ current' = [current EXCEPT ![cpu] = next]
                  /\ runq'    = [runq EXCEPT ![cpu] = rq \ {next}]
                  /\ UNCHANGED <<cond, waiters, pending_sleep>>

(***************************************************************************)
(* Wake(t): per-thread voluntary wake (e.g., timer expiry, signal). Wakes  *)
(* a SLEEPING thread that's NOT a cond-waiter (those are woken by WakeAll  *)
(* atomically with cond := TRUE). Disabled under BUGGY.                    *)
(***************************************************************************)
Wake(t) ==
    /\ ~BUGGY
    /\ state[t] = "SLEEPING"
    /\ t \notin waiters
    /\ \E cpu \in CPUs :
        /\ state' = [state EXCEPT ![t] = "RUNNABLE"]
        /\ runq'  = [runq  EXCEPT ![cpu] = runq[cpu] \cup {t}]
        /\ UNCHANGED <<current, cond, waiters, pending_sleep>>

(***************************************************************************)
(* Resume(cpu): an idle CPU picks up a runnable thread from its queue.     *)
(***************************************************************************)
Resume(cpu) ==
    /\ current[cpu] = NULL
    /\ runq[cpu]    # {}
    /\ LET next == CHOOSE t \in runq[cpu] : TRUE
       IN  /\ state'   = [state   EXCEPT ![next] = "RUNNING"]
           /\ current' = [current EXCEPT ![cpu]  = next]
           /\ runq'    = [runq    EXCEPT ![cpu]  = runq[cpu] \ {next}]
           /\ UNCHANGED <<cond, waiters, pending_sleep>>

(***************************************************************************)
(* WaitOnCond(cpu) — CORRECT version of the wait/wake protocol.            *)
(*                                                                         *)
(* Atomic: under (modeled-implicit) lock, check cond; if TRUE, fast-path   *)
(* (no sleep). If FALSE, atomically transition current to SLEEPING + add   *)
(* to waiters + pick next. The single atomic step is what defeats the      *)
(* missed-wakeup race.                                                     *)
(*                                                                         *)
(* Disabled under BUGGY so the buggy variant focuses on BuggyCheck +       *)
(* BuggySleep splitting the protocol.                                      *)
(***************************************************************************)
WaitOnCond(cpu) ==
    /\ ~BUGGY
    /\ current[cpu] # NULL
    /\ state[current[cpu]] = "RUNNING"
    /\ ~pending_sleep[current[cpu]]
    /\ LET prev == current[cpu]
           rq   == runq[cpu]
       IN  IF cond THEN
              \* Fast path: cond was TRUE at check; no sleep.
              UNCHANGED vars
           ELSE
              IF rq = {} THEN
                 /\ state'   = [state EXCEPT ![prev] = "SLEEPING"]
                 /\ waiters' = waiters \cup {prev}
                 /\ current' = [current EXCEPT ![cpu] = NULL]
                 /\ UNCHANGED <<runq, cond, pending_sleep>>
              ELSE
                 LET next == CHOOSE t \in rq : TRUE
                 IN  /\ state'   = [state EXCEPT ![prev] = "SLEEPING",
                                                 ![next] = "RUNNING"]
                     /\ waiters' = waiters \cup {prev}
                     /\ current' = [current EXCEPT ![cpu] = next]
                     /\ runq'    = [runq EXCEPT ![cpu] = rq \ {next}]
                     /\ UNCHANGED <<cond, pending_sleep>>

(***************************************************************************)
(* BuggyCheck(cpu) — split step 1 of the buggy variant.                    *)
(*                                                                         *)
(* Models the bug where the cond check is separate from the sleep          *)
(* transition. Thread observes cond=FALSE; sets a pending intent. Between  *)
(* this step and BuggySleep, OTHER actions can fire — including WakeAll,   *)
(* which is the missed-wakeup race.                                        *)
(***************************************************************************)
BuggyCheck(cpu) ==
    /\ BUGGY
    /\ current[cpu] # NULL
    /\ state[current[cpu]] = "RUNNING"
    /\ ~pending_sleep[current[cpu]]
    /\ ~cond                                  \* observed cond=FALSE
    /\ pending_sleep' = [pending_sleep EXCEPT ![current[cpu]] = TRUE]
    /\ UNCHANGED <<state, current, runq, cond, waiters>>

(***************************************************************************)
(* BuggySleep(cpu) — split step 2 of the buggy variant.                    *)
(*                                                                         *)
(* Commits to sleep based on the OBSOLETE cond value observed by           *)
(* BuggyCheck. Doesn't recheck cond — that's the bug. cond may have        *)
(* flipped between Check and Sleep (via WakeAll), but BuggySleep           *)
(* transitions to SLEEPING anyway.                                         *)
(***************************************************************************)
BuggySleep(cpu) ==
    /\ BUGGY
    /\ current[cpu] # NULL
    /\ state[current[cpu]] = "RUNNING"
    /\ pending_sleep[current[cpu]]
    /\ LET prev == current[cpu]
           rq   == runq[cpu]
       IN  IF rq = {} THEN
              /\ state'         = [state EXCEPT ![prev] = "SLEEPING"]
              /\ waiters'       = waiters \cup {prev}
              /\ pending_sleep' = [pending_sleep EXCEPT ![prev] = FALSE]
              /\ current'       = [current EXCEPT ![cpu] = NULL]
              /\ UNCHANGED <<runq, cond>>
           ELSE
              LET next == CHOOSE t \in rq : TRUE
              IN  /\ state'         = [state EXCEPT ![prev] = "SLEEPING",
                                                    ![next] = "RUNNING"]
                  /\ waiters'       = waiters \cup {prev}
                  /\ pending_sleep' = [pending_sleep EXCEPT ![prev] = FALSE]
                  /\ current'       = [current EXCEPT ![cpu] = next]
                  /\ runq'          = [runq EXCEPT ![cpu] = rq \ {next}]
                  /\ UNCHANGED <<cond>>

(***************************************************************************)
(* WakeAll — producer atomic: cond := TRUE; transition every waiter to    *)
(* RUNNABLE; clear waiters.                                                *)
(*                                                                         *)
(* The atomicity is the key: cond := TRUE AND waiters := {} happen in the *)
(* same step. A waiter present at this step's start gets RUNNABLE'd; a    *)
(* waiter added AFTER this step (by a BuggySleep) misses the wakeup       *)
(* because it wasn't in waiters when WakeAll fired.                       *)
(*                                                                         *)
(* Idempotent — only fires when cond was FALSE.                           *)
(***************************************************************************)
WakeAll ==
    /\ ~cond
    /\ cond' = TRUE
    /\ LET cpu0 == CHOOSE c \in CPUs : TRUE
       IN  /\ state' = [t \in Threads |->
                IF t \in waiters THEN "RUNNABLE" ELSE state[t]]
           /\ runq'    = [runq EXCEPT ![cpu0] = runq[cpu0] \cup waiters]
           /\ waiters' = {}
           /\ UNCHANGED <<current, pending_sleep>>

Next ==
    \/ \E cpu \in CPUs    : Yield(cpu)
    \/ \E cpu \in CPUs    : Block(cpu)
    \/ \E t   \in Threads : Wake(t)
    \/ \E cpu \in CPUs    : Resume(cpu)
    \/ \E cpu \in CPUs    : WaitOnCond(cpu)
    \/ \E cpu \in CPUs    : BuggyCheck(cpu)
    \/ \E cpu \in CPUs    : BuggySleep(cpu)
    \/ WakeAll

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

\* NoMissedWakeup — cond=TRUE ⇒ all cond-waiters were woken (waiters set
\* is empty). The wait/wake-atomicity proof per ARCH §28 I-9, §8.5. Holds
\* under WaitOnCond + WakeAll (correct version); violated under BuggyCheck
\* + BuggySleep + WakeAll (buggy version) when WakeAll fires between
\* BuggyCheck and BuggySleep.
NoMissedWakeup ==
    cond => (waiters = {})

\* Composite — checked by the .cfg.
Invariants ==
    /\ TypeOk
    /\ StateConsistency
    /\ NoSimultaneousRun
    /\ RunnableInQueue
    /\ SleepingNotInQueue
    /\ NoMissedWakeup

====
