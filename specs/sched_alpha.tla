---- MODULE sched_alpha ----
(***************************************************************************)
(* Thylacine SMP scheduler -- the TARGET design (deep-smp-review alpha +    *)
(* logic-verified HMP foundation). Sibling of specs/sched_oncpu.tla.        *)
(*                                                                         *)
(* WHERE THIS SITS:                                                         *)
(*   - specs/scheduler.tla   : the original (atomic-steal, no on_cpu) model *)
(*                             -- proved blind to the #788/#860 bug class.  *)
(*   - specs/sched_oncpu.tla : the DIAGNOSTIC model. Re-introduced on_cpu + *)
(*                             the multi-step switch + the boot-CPU         *)
(*                             deadlock-dispatch special case; reproduced    *)
(*                             #860 and showed option A vs option B.         *)
(*   - specs/sched_alpha.tla : THIS module -- the FIXED architecture the     *)
(*                             redesign implements. NO boot-CPU special case *)
(*                             at all, plus the HMP placement seam. The      *)
(*                             gating model for the new scheduler.           *)
(*                                                                         *)
(* THE FIX (what this models):                                              *)
(*   1. Every CPU has its OWN idle thread, which is                         *)
(*      (a) PINNED to that CPU -- try_steal never migrates it (generalizes   *)
(*          the secondaries' kstack_base==NULL + retires the g_bootcpu_idle  *)
(*          special-case; impl = a per-Thread `cpu_pinned`), and            *)
(*      (b) IN-TREE -- it lives in its CPU's run tree and is dispatched by    *)
(*          ordinary pick_next, exactly like any other thread.              *)
(*      => NO deadlock-path dispatch, NO off-tree RUNNABLE state, NO racy     *)
(*         guard. A CPU's idle is always reachable on its own CPU            *)
(*         (invariant IdleAvailable), so the old "deadlock" can never arise. *)
(*         This is option B generalized + the special case excised.         *)
(*                                                                         *)
(*   2. The HMP placement SEAM. When a thread becomes RUNNABLE (Place) it is  *)
(*      enqueued on a target CPU chosen by `select_target_cpu` -- modeled    *)
(*      here as NON-DETERMINISTIC choice of target CPU. Proving the safety   *)
(*      invariants under ARBITRARY placement is exactly the composition      *)
(*      result we need: ANY select_target_cpu policy (homogeneous "current   *)
(*      CPU" for v1.0, OR a capacity-aware HMP policy later) is safe, because *)
(*      the on_cpu / migration protocol is placement-agnostic. The capacity  *)
(*      axis is what select_target_cpu reads; the capacity-aware placement   *)
(*      POLICY logic (heavy task -> high-capacity CPU) is NOT a TLA+         *)
(*      invariant -- TLA+ proves SAFETY here, and the placement heuristic is *)
(*      LOGIC-verified by a kernel unit test against a synthetic asymmetric  *)
(*      DTB (deterministic; no real perf asymmetry needed). EAS empirical    *)
(*      tuning (PELT decay, energy model, schedutil, misfit push) is         *)
(*      deferred to real heterogeneous hardware -- it cannot be verified on  *)
(*      QEMU (homogeneous DTB + the host floats vCPUs), and unverifiable     *)
(*      complexity is forbidden (CLAUDE.md "complexity only where verified").*)
(*                                                                         *)
(* ENCODING: each CPU value doubles as its own pinned idle-thread id (so the *)
(* idle of CPU c is the value c). This keeps the model TLC-trivial (no       *)
(* function-valued constants) while faithfully capturing "one pinned in-tree *)
(* idle per CPU". Workers and CPUs are disjoint value sets.                  *)
(*                                                                         *)
(* The on_cpu protocol + the per-CPU lock held across the multi-step switch  *)
(* + FinishSwitch are carried over UNCHANGED from sched_oncpu.tla (the        *)
(* load-bearing correctness machinery the redesign keeps).                  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Workers,   \* ordinary thread ids (>= 1)
    CPUs,      \* cpu ids (>= 2); each CPU value also IS its own pinned idle thread
    NULL       \* "no thread" sentinel

\* The idle thread of CPU c is the value c itself. IdleSet = CPUs.
Threads == Workers \cup CPUs

ASSUME Cardinality(CPUs) >= 2
ASSUME Workers \cap CPUs = {}      \* worker ids and cpu/idle ids are disjoint
ASSUME NULL \notin Threads

\* Unstealable iff it is a CPU's pinned idle (i.e. a CPU value). Impl: cpu_pinned.
IsPinned(t) == t \in CPUs
\* The home CPU of an idle thread is the CPU value itself.
HomeOf(t) == t

VARIABLES
    tstate,     \* [Threads -> {"RUNNING","RUNNABLE","SLEEPING"}]
    on_cpu,     \* [Threads -> BOOLEAN]
    current,    \* [CPUs -> Threads]  (every CPU always runs SOMETHING -- never NULL)
    runq,       \* [CPUs -> SUBSET Threads]
    locked,     \* [CPUs -> BOOLEAN] -- cs->lock held across a multi-step switch
    pend_prev   \* [CPUs -> Threads \cup {NULL}] -- prev_to_clear_on_cpu slot

vars == <<tstate, on_cpu, current, runq, locked, pend_prev>>

States == {"RUNNING", "RUNNABLE", "SLEEPING"}

TypeOk ==
    /\ tstate    \in [Threads -> States]
    /\ on_cpu    \in [Threads -> BOOLEAN]
    /\ current   \in [CPUs -> Threads]
    /\ runq      \in [CPUs -> SUBSET Threads]
    /\ locked    \in [CPUs -> BOOLEAN]
    /\ pend_prev \in [CPUs -> Threads \cup {NULL}]

(***************************************************************************)
(* Init: every CPU runs its own idle (current[c] = c); every Worker is      *)
(* RUNNABLE on CPU0's runq (an arbitrary starting placement; Place/Steal     *)
(* redistribute). Idles are current, hence not in any runq yet.             *)
(***************************************************************************)
Init ==
    LET CPU0 == CHOOSE c \in CPUs : TRUE IN
    /\ tstate    = [t \in Threads |-> IF t \in CPUs THEN "RUNNING" ELSE "RUNNABLE"]
    /\ on_cpu    = [t \in Threads |-> t \in CPUs]
    /\ current   = [c \in CPUs |-> c]
    /\ runq      = [c \in CPUs |-> IF c = CPU0 THEN Workers ELSE {}]
    /\ locked    = [c \in CPUs |-> FALSE]
    /\ pend_prev = [c \in CPUs |-> NULL]

RemoveFromRunq(rq, t) == [c \in CPUs |-> rq[c] \ {t}]

\* Steal candidates for cpu c: a NON-PINNED thread in an unlocked peer's runq.
\* Pinned (idle) threads are skipped -> they never migrate (IdleStaysHome).
StealCand(c) ==
    { t \in Threads :
        /\ ~IsPinned(t)
        /\ \E v \in CPUs : v # c /\ ~locked[v] /\ t \in runq[v] }

\* The shared multi-step switch effect (identical discipline to sched_oncpu):
\* lock c, set next RUNNING + on_cpu, dispose prev, stash prev to clear. prev
\* stays on_cpu until FinishSwitch. On yield prev re-enters c's runq IN-TREE
\* (every thread, idles included -- no off-tree exception). On block prev
\* sleeps. `rq2` is the runq with `nxt` already removed.
DoSwitch(c, prev, nxt, kind, rq2) ==
    /\ current'   = [current EXCEPT ![c] = nxt]
    /\ tstate'    = [tstate EXCEPT ![nxt] = "RUNNING",
                                   ![prev] = IF kind = "yield" THEN "RUNNABLE" ELSE "SLEEPING"]
    /\ on_cpu'    = [on_cpu EXCEPT ![nxt] = TRUE]      \* prev stays on_cpu
    /\ runq'      = IF kind = "yield"
                       THEN [rq2 EXCEPT ![c] = rq2[c] \cup {prev}]
                       ELSE rq2
    /\ locked'    = [locked EXCEPT ![c] = TRUE]
    /\ pend_prev' = [pend_prev EXCEPT ![c] = prev]

Intent == {"yield", "block"}

(***************************************************************************)
(* StartSwitch(c): the RUNNING thread on c reaches sched(). It yields or     *)
(* blocks; pick_next selects from c's runq, else try_steal from a peer.      *)
(* An idle (a CPU value) only yields. A worker that blocks ALWAYS has a next  *)
(* -- the idle `c` is in runq[c] while a worker runs on c -- so the deadlock  *)
(* path of the old design is structurally unreachable. If an idle yields with *)
(* no work anywhere, no next exists and the action is disabled (the idle just *)
(* keeps running; modeled as a stutter).                                     *)
(***************************************************************************)
StartSwitch(c) ==
    /\ ~locked[c]
    /\ tstate[current[c]] = "RUNNING"
    /\ \E kind \in Intent :
        /\ (current[c] \in CPUs => kind = "yield")    \* idles never block
        /\ LET prev == current[c]
               localCand == { t \in runq[c] : tstate[t] = "RUNNABLE" /\ ~on_cpu[t] }
               stealCand == { t \in StealCand(c) : tstate[t] = "RUNNABLE" /\ ~on_cpu[t] }
           IN  \/ /\ localCand # {}
                  /\ \E nxt \in localCand : DoSwitch(c, prev, nxt, kind, RemoveFromRunq(runq, nxt))
               \/ /\ localCand = {}
                  /\ stealCand # {}
                  /\ \E nxt \in stealCand : DoSwitch(c, prev, nxt, kind, RemoveFromRunq(runq, nxt))

(***************************************************************************)
(* FinishSwitch(c): the resuming thread runs finish_task_switch -- clears    *)
(* the switched-out prev's on_cpu (RELEASE) then releases the lock. Atomic    *)
(* (clear precedes unlock, so no lock-free window with prev still on_cpu --   *)
(* the property RunqOnCpuSafe depends on).                                   *)
(***************************************************************************)
FinishSwitch(c) ==
    /\ locked[c]
    /\ pend_prev[c] # NULL
    /\ on_cpu'    = [on_cpu EXCEPT ![pend_prev[c]] = FALSE]
    /\ locked'    = [locked EXCEPT ![c] = FALSE]
    /\ pend_prev' = [pend_prev EXCEPT ![c] = NULL]
    /\ UNCHANGED <<tstate, current, runq>>

(***************************************************************************)
(* Place(t): the HMP placement seam. A SLEEPING worker wakes and is enqueued *)
(* on select_target_cpu(t)'s runq -- modeled as a NON-DETERMINISTIC target   *)
(* CPU. Safety under arbitrary target = ANY placement policy (homogeneous or *)
(* capacity-aware) is safe. Idles never sleep, so are never Placed.          *)
(***************************************************************************)
Place(t) ==
    /\ t \in Workers
    /\ tstate[t] = "SLEEPING"
    /\ ~on_cpu[t]
    /\ \E c \in CPUs :          \* select_target_cpu(t) -- nondeterministic
         /\ tstate' = [tstate EXCEPT ![t] = "RUNNABLE"]
         /\ runq'   = [runq EXCEPT ![c] = runq[c] \cup {t}]
         /\ UNCHANGED <<on_cpu, current, locked, pend_prev>>

Next ==
    \/ \E c \in CPUs    : StartSwitch(c)
    \/ \E c \in CPUs    : FinishSwitch(c)
    \/ \E t \in Workers : Place(t)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

NoSimultaneousRun ==
    \A t \in Threads : Cardinality({c \in CPUs : current[c] = t}) <= 1

OwnerUnique ==
    \A t \in Threads :
        Cardinality({c \in CPUs : current[c] = t} \cup
                    {c \in CPUs : pend_prev[c] = t}) <= 1

OnCpuMeansOwned ==
    \A t \in Threads :
        on_cpu[t] => (\E c \in CPUs : current[c] = t \/ pend_prev[c] = t)

RunningImpliesOnCpu ==
    \A t \in Threads : (\E c \in CPUs : current[c] = t) => on_cpu[t]

RunqRunnable ==
    \A t \in Threads :
        (\E c \in CPUs : t \in runq[c]) =>
            /\ tstate[t] = "RUNNABLE"
            /\ \A c \in CPUs : current[c] # t

RunqOnCpuSafe ==
    \A c \in CPUs : ~locked[c] => (\A t \in runq[c] : ~on_cpu[t])

NoDoubleEnqueue ==
    \A t \in Threads : Cardinality({c \in CPUs : t \in runq[c]}) <= 1

\* IdleStaysHome -- a CPU's pinned idle is NEVER associated with any other CPU
\* (not its current, not in its runq, not mid-switch). Validates that the
\* `cpu_pinned` skip keeps each idle on its home CPU (the I-21 boot-stack /
\* idle-migration class the old kstack_base gate protected).
IdleStaysHome ==
    \A t \in CPUs : \A c \in CPUs :
        (current[c] = t \/ t \in runq[c] \/ pend_prev[c] = t) => (c = HomeOf(t))

\* IdleAvailable -- each CPU's idle is ALWAYS reachable on its own CPU
\* (running, in its runq, or mid-switch). The formal proof that the
\* deadlock-path special case is UNNECESSARY: pick_next can always fall back
\* to the in-tree idle, so a CPU is never stuck with "nothing to run".
IdleAvailable ==
    \A c \in CPUs :
        \/ current[c] = c
        \/ c \in runq[c]
        \/ pend_prev[c] = c

\* AlwaysRunning -- every CPU is always executing some thread (never idle-NULL);
\* a corollary of the per-CPU in-tree idle (no NULL-current state to mismanage).
AlwaysRunning ==
    \A c \in CPUs : tstate[current[c]] = "RUNNING"

Safety ==
    /\ TypeOk
    /\ NoSimultaneousRun
    /\ OwnerUnique
    /\ OnCpuMeansOwned
    /\ RunningImpliesOnCpu
    /\ RunqRunnable
    /\ RunqOnCpuSafe
    /\ NoDoubleEnqueue
    /\ IdleStaysHome
    /\ IdleAvailable
    /\ AlwaysRunning

====
