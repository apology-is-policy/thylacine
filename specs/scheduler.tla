---- MODULE scheduler ----
(***************************************************************************)
(* Thylacine scheduler — P2-Cg refinement (SMP).                            *)
(*                                                                         *)
(* Models the thread state machine + per-CPU dispatch + the wait/wake      *)
(* protocol per ARCHITECTURE.md §8.5 (wakeup atomicity, I-9), and the SMP  *)
(* discipline added in P2-C: cross-CPU work-stealing (`Steal`) and inter-  *)
(* CPU interrupt ordering (`IPI_Send` / `IPI_Deliver`).                    *)
(*                                                                         *)
(* History:                                                                 *)
(*   P2-A  — state machine + per-CPU dispatch.                             *)
(*   P2-Bb — wait/wake protocol; NoMissedWakeup invariant (I-9).           *)
(*   P2-Bc — preemption mapping (Yield is non-deterministic).              *)
(*   P2-Cg — Steal action; per-(src,dst) IPI queues; IPIOrdering invariant *)
(*           (I-18); NoDoubleEnqueue invariant for cross-CPU runq safety.  *)
(*   P2-H  — LatencyBound liveness (I-17). Spec_Live extends Spec with     *)
(*           SF on Resume + Yield + WakeAll. Refines Steal's precondition  *)
(*           to current[stealer] = NULL (only idle CPUs steal — closes a   *)
(*           spurious steal-back-and-forth lasso). Liveness is checked at  *)
(*           a minimal universe (2T × 1C) in `scheduler_liveness.cfg`;     *)
(*           the buggy `scheduler_buggy_starve.cfg` drops fairness and     *)
(*           produces the stuttering counterexample. Per-thread fairness   *)
(*           refinement (Yield(cpu, t) parameterized) deferred to Phase    *)
(*           5+ alongside full EEVDF math.                                 *)
(*                                                                         *)
(* Protocol modeled (correct, atomic — see ARCH §8.4, §8.5, §8.7):         *)
(*                                                                         *)
(*   Wait/wake:                                                            *)
(*     1. A acquires per-condition lock (modeled implicitly — actions are  *)
(*        atomic).                                                         *)
(*     2. A checks cond; if true, returns.                                 *)
(*     3. A enqueues itself into waiters AND transitions to SLEEPING in a  *)
(*        single atomic step.                                              *)
(*     4. B sets cond := TRUE AND wakes every thread in waiters AND clears *)
(*        waiters in a single atomic step.                                 *)
(*                                                                         *)
(*   Steal (P2-Cg, ARCH §8.4):                                             *)
(*     A stealer CPU with empty runq pulls one runnable thread from a      *)
(*     victim CPU's runq. The atomic step models the impl's spin_trylock-  *)
(*     bracketed window in `kernel/sched.c::try_steal` — the per-CPU       *)
(*     run-tree lock guarantees atomicity of unlink + transfer at the     *)
(*     impl level. The buggy variant `BuggySteal` adds the thread to the   *)
(*     stealer's runq WITHOUT removing it from the victim's, modeling      *)
(*     "steal that forgot to drop the source-side reference" — a class of *)
(*     bugs caught by the NoDoubleEnqueue invariant.                       *)
(*                                                                         *)
(*   IPI ordering (P2-Cg, ARCH §8.7, I-18):                                *)
(*     Per-(src, dst) FIFO queue of pending IPIs. Each send appends a      *)
(*     monotonically-increasing per-pair sequence number. Correct          *)
(*     `IPI_Deliver` consumes the head (FIFO discipline, modeling GIC SGI  *)
(*     pend-bit edge ordering and the per-CPU IRQ priority arbitration).   *)
(*     Buggy `BuggyIPI_Deliver` pops an arbitrary element, modeling        *)
(*     "handler that processed a later IPI before an earlier one" — a     *)
(*     class of bugs caught by the IPIOrdering invariant.                  *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation per CLAUDE.md spec-first  *)
(* policy):                                                                 *)
(*                                                                         *)
(*   scheduler.cfg              all flags FALSE — TLC proves all           *)
(*                              SAFETY invariants hold (3T × 2C).         *)
(*   scheduler_buggy.cfg        BUGGY=TRUE — counterexample violates       *)
(*                              NoMissedWakeup at depth ≈ 4.               *)
(*   scheduler_buggy_steal.cfg  BUGGY_STEAL=TRUE — counterexample          *)
(*                              violates NoDoubleEnqueue (thread in two    *)
(*                              runqueues simultaneously).                 *)
(*   scheduler_buggy_ipi.cfg    BUGGY_IPI_ORDER=TRUE — counterexample      *)
(*                              violates IPIOrdering (head of queue ≠      *)
(*                              next-expected delivery seq).               *)
(*   scheduler_liveness.cfg     2T × 1C with Spec_Live + SF fairness —    *)
(*                              TLC proves LatencyBound liveness (I-17).  *)
(*   scheduler_buggy_starve.cfg 2T × 1C with Spec (no fairness) —         *)
(*                              counterexample stutters; LatencyBound     *)
(*                              violated. Demonstrates that without       *)
(*                              fairness assumptions, the scheduler does  *)
(*                              not satisfy I-17.                          *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   StateConsistency   — a thread is RUNNING iff some CPU's current.     *)
(*   NoSimultaneousRun  — a thread runs on at most one CPU at a time.     *)
(*   RunnableInQueue    — a thread is RUNNABLE iff it sits in some        *)
(*                        CPU's runqueue.                                  *)
(*   SleepingNotInQueue — a SLEEPING thread is in no runqueue and is no   *)
(*                        CPU's current.                                   *)
(*   NoMissedWakeup     — cond=TRUE ⇒ waiters={}. Wait/wake atomicity     *)
(*                        proof per ARCH §28 I-9, §8.5.                    *)
(*   NoDoubleEnqueue    — every thread is in at most one runqueue at a    *)
(*                        time. Holds under Steal (atomic transfer);       *)
(*                        violated by BuggySteal. Cross-CPU runq safety    *)
(*                        (P2-Cg, ARCH §8.4).                              *)
(*   IPIOrdering        — for any (src, dst) pair with a non-empty IPI    *)
(*                        queue, the head's sequence number equals the    *)
(*                        next-expected delivery sequence number. Holds   *)
(*                        under FIFO IPI_Deliver; violated by              *)
(*                        BuggyIPI_Deliver. ARCH §28 I-18, §8.7.           *)
(*                                                                         *)
(* Modeling assumptions:                                                    *)
(*                                                                         *)
(*   Closed-universe threads (P2-A audit R4 F47): the set Threads is fixed *)
(*   at Init. The C-level thread_create is open-universe; the closed       *)
(*   universe is sound for proving the state-machine + wait/wake +         *)
(*   steal + IPI invariants because thread_create's semantics ("new        *)
(*   thread starts RUNNABLE in some runqueue") are structurally            *)
(*   compatible with the existing invariants. P2-D / Phase 2 close adds    *)
(*   a Spawn(t) action to model the open-universe; the proofs here carry. *)
(*                                                                         *)
(*   Bounded IPIs (P2-Cg): each (src,dst) pair has at most MaxIPIs in      *)
(*   flight at once, and MaxIPIs total sends. The bound is for state       *)
(*   space tractability; the IPIOrdering invariant is per-pair so no       *)
(*   cross-pair fairness needs the bound relaxed.                          *)
(*                                                                         *)
(*   Self-IPIs excluded: src ≠ dst. The hardware permits self-IPIs but     *)
(*   they're uninteresting for ordering and waste state space.             *)
(*                                                                         *)
(* Deferred (Phase 5+):                                                     *)
(*                                                                         *)
(*   LatencyBound at full universe with per-thread fairness: requires     *)
(*   parameterizing Yield/Block by both (cpu, thread) and adding SF for   *)
(*   each (cpu, t) pair. State space and fairness-clause cardinality      *)
(*   both grow; meaningful when EEVDF weights differ (since equal-weight  *)
(*   round-robin emerges naturally from CHOOSE rotation at minimal        *)
(*   universe). Phase 5+ when sched_setweight is exposed.                  *)
(*                                                                         *)
(*   Full EEVDF math: vd_t = ve_t + slice × W_total / w_self. Meaningful  *)
(*   when weights differ (Phase 5+); v1.0 is weight=1 always.              *)
(*                                                                         *)
(* See ARCHITECTURE.md §8 (scheduler design); §28 invariants I-8, I-9,     *)
(* I-17, I-18.                                                             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
    Threads,         \* set of thread identifiers
    CPUs,            \* set of CPU identifiers
    NULL,            \* sentinel — represents "no thread"
    BUGGY,           \* P2-Bb: BOOLEAN — when TRUE, BuggyCheck + BuggySleep
                     \*   actions fire instead of WaitOnCond, exposing the
                     \*   missed-wakeup race. Set by scheduler_buggy.cfg.
    BUGGY_STEAL,     \* P2-Cg: BOOLEAN — when TRUE, BuggySteal fires
                     \*   alongside Steal, modeling steal-without-source-
                     \*   side-removal. Set by scheduler_buggy_steal.cfg.
    BUGGY_IPI_ORDER, \* P2-Cg: BOOLEAN — when TRUE, BuggyIPI_Deliver fires
                     \*   alongside IPI_Deliver, modeling out-of-order IPI
                     \*   processing. Set by scheduler_buggy_ipi.cfg.
    MaxIPIs          \* P2-Cg: Nat ≥ 1. Per-pair total send cap AND per-
                     \*   pair queue cap. Bounds state space for TLC.

ASSUME NULL \notin Threads
ASSUME Cardinality(Threads) >= 1
ASSUME Cardinality(CPUs)    >= 1
ASSUME BUGGY \in BOOLEAN
ASSUME BUGGY_STEAL \in BOOLEAN
ASSUME BUGGY_IPI_ORDER \in BOOLEAN
ASSUME MaxIPIs \in Nat /\ MaxIPIs >= 1

VARIABLES
    state,           \* state[t]   ∈ {"RUNNING", "RUNNABLE", "SLEEPING"}
    current,         \* current[c] ∈ Threads ∪ {NULL}
    runq,            \* runq[c]    ⊆ Threads — per-CPU runqueue
    cond,            \* P2-Bb: BOOLEAN — the wait condition (one shared cond).
    waiters,         \* P2-Bb: ⊆ Threads — threads sleeping on cond.
    pending_sleep,   \* P2-Bb: [Threads -> BOOLEAN] — buggy intent flag for
                     \*   threads that have observed cond=FALSE but haven't
                     \*   yet transitioned to SLEEPING.
    ipi_queue,       \* P2-Cg: [<<src,dst>> -> Seq(Nat)] — pending IPI
                     \*   sequence numbers (per-pair FIFO).
    ipi_send_seq,    \* P2-Cg: [<<src,dst>> -> Nat] — next sequence number
                     \*   to assign on send. Monotonic per pair.
    ipi_deliver_seq  \* P2-Cg: [<<src,dst>> -> Nat] — next-expected delivery
                     \*   sequence number per pair. Increments by one on
                     \*   each delivery (correct or buggy).

vars == <<state, current, runq, cond, waiters, pending_sleep,
          ipi_queue, ipi_send_seq, ipi_deliver_seq>>

\* P2-Cg: useful tuples for UNCHANGED clauses.
ipi_vars  == <<ipi_queue, ipi_send_seq, ipi_deliver_seq>>
wait_vars == <<cond, waiters, pending_sleep>>

States == {"RUNNING", "RUNNABLE", "SLEEPING"}

\* P2-Cg: ordered pairs of distinct CPUs. Self-IPIs are excluded — the
\* hardware permits them but they're uninteresting for ordering and waste
\* state space.
CpuPair == {<<s, d>> \in CPUs \X CPUs : s # d}

TypeOk ==
    /\ state           \in [Threads -> States]
    /\ current         \in [CPUs    -> Threads \cup {NULL}]
    /\ runq            \in [CPUs    -> SUBSET Threads]
    /\ cond            \in BOOLEAN
    /\ waiters         \subseteq Threads
    /\ pending_sleep   \in [Threads -> BOOLEAN]
    /\ ipi_queue       \in [CpuPair -> Seq(Nat)]
    /\ ipi_send_seq    \in [CpuPair -> Nat]
    /\ ipi_deliver_seq \in [CpuPair -> Nat]

(***************************************************************************)
(* Initial state: pick one CPU as cpu0 and one thread as t0; t0 starts     *)
(* RUNNING on cpu0; all other threads are RUNNABLE in cpu0's runqueue.     *)
(* cond starts FALSE; waiters and pending_sleep start empty. All IPI       *)
(* queues empty; send and deliver seqs at 0.                               *)
(***************************************************************************)
Init ==
    LET cpu0 == CHOOSE c \in CPUs    : TRUE
        t0   == CHOOSE t \in Threads : TRUE
    IN  /\ state           = [t \in Threads |-> IF t = t0 THEN "RUNNING" ELSE "RUNNABLE"]
        /\ current         = [c \in CPUs    |-> IF c = cpu0 THEN t0 ELSE NULL]
        /\ runq            = [c \in CPUs    |-> IF c = cpu0 THEN Threads \ {t0} ELSE {}]
        /\ cond            = FALSE
        /\ waiters         = {}
        /\ pending_sleep   = [t \in Threads |-> FALSE]
        /\ ipi_queue       = [p \in CpuPair |-> <<>>]
        /\ ipi_send_seq    = [p \in CpuPair |-> 0]
        /\ ipi_deliver_seq = [p \in CpuPair |-> 0]

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
                  /\ UNCHANGED <<wait_vars, ipi_vars>>

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
              /\ UNCHANGED <<runq, wait_vars, ipi_vars>>
           ELSE
              LET next == CHOOSE t \in rq : TRUE
              IN  /\ state'   = [state EXCEPT ![prev] = "SLEEPING",
                                              ![next] = "RUNNING"]
                  /\ current' = [current EXCEPT ![cpu] = next]
                  /\ runq'    = [runq EXCEPT ![cpu] = rq \ {next}]
                  /\ UNCHANGED <<wait_vars, ipi_vars>>

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
        /\ UNCHANGED <<current, wait_vars, ipi_vars>>

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
           /\ UNCHANGED <<wait_vars, ipi_vars>>

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
                 /\ UNCHANGED <<runq, cond, pending_sleep, ipi_vars>>
              ELSE
                 LET next == CHOOSE t \in rq : TRUE
                 IN  /\ state'   = [state EXCEPT ![prev] = "SLEEPING",
                                                 ![next] = "RUNNING"]
                     /\ waiters' = waiters \cup {prev}
                     /\ current' = [current EXCEPT ![cpu] = next]
                     /\ runq'    = [runq EXCEPT ![cpu] = rq \ {next}]
                     /\ UNCHANGED <<cond, pending_sleep, ipi_vars>>

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
    /\ UNCHANGED <<state, current, runq, cond, waiters, ipi_vars>>

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
              /\ UNCHANGED <<runq, cond, ipi_vars>>
           ELSE
              LET next == CHOOSE t \in rq : TRUE
              IN  /\ state'         = [state EXCEPT ![prev] = "SLEEPING",
                                                    ![next] = "RUNNING"]
                  /\ waiters'       = waiters \cup {prev}
                  /\ pending_sleep' = [pending_sleep EXCEPT ![prev] = FALSE]
                  /\ current'       = [current EXCEPT ![cpu] = next]
                  /\ runq'          = [runq EXCEPT ![cpu] = rq \ {next}]
                  /\ UNCHANGED <<cond, ipi_vars>>

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
           /\ UNCHANGED <<current, pending_sleep, ipi_vars>>

(***************************************************************************)
(* Steal(stealer, victim) — cross-CPU work-stealing (P2-Cg, ARCH §8.4).    *)
(*                                                                         *)
(* Stealer with empty runq pulls one runnable thread from victim's runq.   *)
(* The atomic step models the impl's spin_trylock-bracketed window in     *)
(* `kernel/sched.c::try_steal` — stealer holds its own per-CPU run-tree   *)
(* lock and acquires victim's via try_lock, unlinks one thread, releases  *)
(* the victim's lock. Modeling as a single atomic step is sound because   *)
(* the impl's two-lock window contains the entire transfer (no            *)
(* observable intermediate state between unlink and rebase).               *)
(*                                                                         *)
(* The thread keeps its RUNNABLE state; only its runqueue location moves. *)
(*                                                                         *)
(* The impl's vd_t rebasing (peer's clock → caller's clock) is omitted —  *)
(* the spec doesn't model vd_t (deferred to Phase 2 close LatencyBound    *)
(* refinement).                                                            *)
(***************************************************************************)
Steal(stealer, victim) ==
    /\ stealer # victim
    /\ current[stealer] = NULL    \* P2-H: only an idle CPU steals.
                                  \* The impl's try_steal is called from
                                  \* pick_next, which runs only when
                                  \* sched() is about to dispatch — i.e.,
                                  \* the calling CPU has no settled
                                  \* current. Without this precondition
                                  \* the spec admits a steal-back-and-
                                  \* forth lasso that never occurs in the
                                  \* impl: two busy CPUs trade a single
                                  \* thread between their runqs forever
                                  \* while neither dispatches it. Adding
                                  \* this precondition closes the
                                  \* spurious LatencyBound counterexample.
    /\ runq[stealer] = {}
    /\ runq[victim]  # {}
    /\ \E t \in runq[victim] :
         /\ runq' = [runq EXCEPT
                ![victim]  = runq[victim] \ {t},
                ![stealer] = runq[stealer] \cup {t}]
         /\ UNCHANGED <<state, current, wait_vars, ipi_vars>>

(***************************************************************************)
(* BuggySteal(stealer, victim) — non-atomic steal modeling a class of      *)
(* bugs where the unlink-from-victim is forgotten.                         *)
(*                                                                         *)
(* The thread is added to stealer's runq WITHOUT being removed from        *)
(* victim's. If the buggy variant fires, the thread appears in two         *)
(* runqueues simultaneously — caught by NoDoubleEnqueue.                   *)
(*                                                                         *)
(* In the impl, this would correspond to e.g. forgetting to call           *)
(* unlink(peer, stolen) before transferring, or a race where the unlink    *)
(* happens after some other CPU has already observed the thread in        *)
(* victim's tree and pulled it again.                                      *)
(***************************************************************************)
BuggySteal(stealer, victim) ==
    /\ BUGGY_STEAL
    /\ stealer # victim
    /\ runq[stealer] = {}
    /\ runq[victim]  # {}
    /\ \E t \in runq[victim] :
         /\ runq' = [runq EXCEPT ![stealer] = runq[stealer] \cup {t}]
         \* BUG: doesn't remove t from victim's runq.
         /\ UNCHANGED <<state, current, wait_vars, ipi_vars>>

(***************************************************************************)
(* IPI_Send(src, dst) — append a new IPI to the (src, dst) FIFO queue.     *)
(*                                                                         *)
(* Each IPI carries a sequence number assigned monotonically per pair.    *)
(* The bound `MaxIPIs` caps both queue length and total sends per pair    *)
(* for state-space tractability.                                           *)
(*                                                                         *)
(* Models `kernel/smp.c::ipi_send_resched` → `arch/arm64/gic.c::          *)
(* gic_send_ipi` → ICC_SGI1R_EL1. The sequence number is conceptual: the  *)
(* GIC SGI pend bit is per-(src, dst, sgi_intid), and edge-trigger        *)
(* delivery in send order maps to the queue's FIFO discipline.            *)
(***************************************************************************)
IPI_Send(src, dst) ==
    /\ src # dst
    /\ Len(ipi_queue[<<src, dst>>]) < MaxIPIs
    /\ ipi_send_seq[<<src, dst>>] < MaxIPIs
    /\ ipi_queue'    = [ipi_queue EXCEPT
            ![<<src, dst>>] = Append(@, ipi_send_seq[<<src, dst>>])]
    /\ ipi_send_seq' = [ipi_send_seq EXCEPT ![<<src, dst>>] = @ + 1]
    /\ UNCHANGED <<state, current, runq, wait_vars, ipi_deliver_seq>>

(***************************************************************************)
(* IPI_Deliver(src, dst) — CORRECT FIFO delivery.                          *)
(*                                                                         *)
(* Pops the head of the (src, dst) queue and increments the per-pair      *)
(* delivery sequence counter. Maintains IPIOrdering structurally: head    *)
(* of queue equals the next-expected delivery seq number.                  *)
(*                                                                         *)
(* Models `kernel/smp.c::ipi_resched_handler` (and any future per-type    *)
(* IPI handler) consuming the head of the per-CPU pending IPI queue.      *)
(* The handler effects (e.g., setting need_resched on dst) are not        *)
(* modeled — only the ordering discipline is.                              *)
(***************************************************************************)
IPI_Deliver(src, dst) ==
    /\ ~BUGGY_IPI_ORDER
    /\ src # dst
    /\ Len(ipi_queue[<<src, dst>>]) > 0
    /\ ipi_queue'       = [ipi_queue EXCEPT ![<<src, dst>>] = Tail(@)]
    /\ ipi_deliver_seq' = [ipi_deliver_seq EXCEPT ![<<src, dst>>] = @ + 1]
    /\ UNCHANGED <<state, current, runq, wait_vars, ipi_send_seq>>

(***************************************************************************)
(* BuggyIPI_Deliver(src, dst) — non-FIFO delivery.                         *)
(*                                                                         *)
(* Pops an arbitrary index from the (src, dst) queue (not necessarily the *)
(* head). The delivery sequence counter still advances by one. If the     *)
(* popped element is not the head, the new head's seq number no longer    *)
(* equals the next-expected delivery seq — IPIOrdering violated.          *)
(*                                                                         *)
(* Models a class of bugs where the IPI handler processes pending IPIs    *)
(* in priority/type order rather than send order, or where the GIC SGI    *)
(* arbitration is misconfigured (e.g., distinct priority for an IPI       *)
(* class breaks per-pair ordering).                                       *)
(***************************************************************************)
BuggyIPI_Deliver(src, dst) ==
    /\ BUGGY_IPI_ORDER
    /\ src # dst
    /\ Len(ipi_queue[<<src, dst>>]) > 0
    /\ \E i \in 1..Len(ipi_queue[<<src, dst>>]) :
         /\ ipi_queue' = [ipi_queue EXCEPT ![<<src, dst>>] =
                SubSeq(@, 1, i - 1) \o SubSeq(@, i + 1, Len(@))]
         /\ ipi_deliver_seq' = [ipi_deliver_seq EXCEPT ![<<src, dst>>] = @ + 1]
    /\ UNCHANGED <<state, current, runq, wait_vars, ipi_send_seq>>

Next ==
    \/ \E cpu \in CPUs    : Yield(cpu)
    \/ \E cpu \in CPUs    : Block(cpu)
    \/ \E t   \in Threads : Wake(t)
    \/ \E cpu \in CPUs    : Resume(cpu)
    \/ \E cpu \in CPUs    : WaitOnCond(cpu)
    \/ \E cpu \in CPUs    : BuggyCheck(cpu)
    \/ \E cpu \in CPUs    : BuggySleep(cpu)
    \/ WakeAll
    \/ \E s, v \in CPUs   : Steal(s, v)
    \/ \E s, v \in CPUs   : BuggySteal(s, v)
    \/ \E s, d \in CPUs   : IPI_Send(s, d)
    \/ \E s, d \in CPUs   : IPI_Deliver(s, d)
    \/ \E s, d \in CPUs   : BuggyIPI_Deliver(s, d)

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

\* P2-Cg: NoDoubleEnqueue — a thread is in at most one runqueue at a time.
\* Strengthens RunnableInQueue (which says "in some runq" but doesn't bound
\* multiplicity). Holds under Steal (atomic transfer); violated by
\* BuggySteal. Cross-CPU runq safety per ARCH §28 I-2 (state consistency)
\* and §8.4 (work-stealing).
NoDoubleEnqueue ==
    \A t \in Threads :
        Cardinality({c \in CPUs : t \in runq[c]}) <= 1

\* P2-Cg: IPIOrdering — for any (src, dst) pair with a non-empty IPI
\* queue, the head's sequence number equals the next-expected delivery
\* seq number. Encodes ARCH §28 I-18 (FIFO IPI ordering per (src, dst)
\* pair). Holds under FIFO IPI_Deliver; violated by BuggyIPI_Deliver
\* when it pops a non-head element.
IPIOrdering ==
    \A pair \in CpuPair :
        Len(ipi_queue[pair]) > 0 =>
            Head(ipi_queue[pair]) = ipi_deliver_seq[pair]

\* Composite — checked by the .cfg.
Invariants ==
    /\ TypeOk
    /\ StateConsistency
    /\ NoSimultaneousRun
    /\ RunnableInQueue
    /\ SleepingNotInQueue
    /\ NoMissedWakeup
    /\ NoDoubleEnqueue
    /\ IPIOrdering

(***************************************************************************)
(* ============================ LIVENESS (P2-H) =========================== *)
(*                                                                         *)
(* LatencyBound liveness — every RUNNABLE thread eventually runs.          *)
(* Encodes ARCH §28 I-17. The spec models latency QUALITATIVELY            *)
(* ("eventually") rather than QUANTITATIVELY ("within slice × N steps")    *)
(* — TLA+ liveness checks fair-trace inclusion over infinite traces, not   *)
(* bounded-step paths. A bound-arithmetic refinement would require an      *)
(* explicit Slice variable + IRQ/Preempt actions firing at bounded         *)
(* intervals + a step counter; deferred post-P2-H if we ever need to      *)
(* prove a numeric bound at the spec level (the impl-side bound is        *)
(* enforced by EEVDF deadline math + sched_tick at slice boundary).       *)
(*                                                                         *)
(* Fairness assumptions:                                                   *)
(*                                                                         *)
(*   WF on Resume(cpu): an idle CPU (current = NULL) with a non-empty      *)
(*   runqueue eventually picks a thread up. Without this, idle CPUs       *)
(*   could trivially halt — runnable threads starve.                       *)
(*                                                                         *)
(*   WF on Yield(cpu): a busy CPU eventually yields, allowing other        *)
(*   RUNNABLE threads on its runqueue to dispatch. Without this, the      *)
(*   running thread could occupy its CPU forever — runnable threads in    *)
(*   the same runqueue starve. The CHOOSE in Yield is deterministic, but  *)
(*   the previous current re-enters the runqueue each step, so successive *)
(*   Yields rotate through threads (round-robin behavior emerges from     *)
(*   the state-driven CHOOSE rather than an explicit FIFO).                *)
(*                                                                         *)
(*   WF on WakeAll: a producer that has set its precondition (cond=FALSE  *)
(*   with non-empty waiters) eventually fires. Without this, all threads  *)
(*   could WaitOnCond indefinitely while a never-firing wakeup leaves     *)
(*   them in SLEEPING — but since SLEEPING is not RUNNABLE, that doesn't  *)
(*   directly violate LatencyBound. The fairness is for the regenerative  *)
(*   loop: WakeAll re-enters threads to RUNNABLE, then LatencyBound       *)
(*   applies again. (BuggyCheck/BuggySleep/BuggySteal/BuggyIPI_Deliver    *)
(*   need NO fairness because their gating flags are FALSE in non-buggy   *)
(*   configs; in buggy configs the safety violation is what we check, not *)
(*   liveness.)                                                            *)
(*                                                                         *)
(* Why we don't promise fairness on Block / Wake(t): Block(cpu) is the    *)
(* "blocked for a non-cond reason" action; if a thread Blocks, recovery   *)
(* is via Wake(t). The spec doesn't promise an external waker fires —     *)
(* that's a caller-side liveness obligation (Phase 5+ note delivery is    *)
(* where Block-style blocking gets a corresponding wake).                 *)
(***************************************************************************)

LatencyBound ==
    \A t \in Threads :
        [](state[t] = "RUNNABLE" => <>(state[t] = "RUNNING"))

Liveness ==
    /\ \A cpu \in CPUs : SF_vars(Resume(cpu))
    /\ \A cpu \in CPUs : SF_vars(Yield(cpu))
    /\ SF_vars(WakeAll)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

====
