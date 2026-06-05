---- MODULE sched_oncpu ----
(***************************************************************************)
(* Thylacine SMP on_cpu / work-stealing / boot-CPU deadlock-dispatch       *)
(* protocol -- the mechanism the existing scheduler.tla ABSTRACTS AWAY.     *)
(*                                                                         *)
(* WHY THIS MODULE EXISTS (deep-smp-review, 2026-06-05):                    *)
(*                                                                         *)
(*   specs/scheduler.tla models Steal as a SINGLE ATOMIC transfer between   *)
(*   per-CPU runqueues ("no observable intermediate state between unlink    *)
(*   and rebase") and has NO `on_cpu` variable. But the IMPL's whole        *)
(*   difficulty is that dispatch is NOT atomic: a thread is claimed         *)
(*   (on_cpu := TRUE under a peer lock), THEN its ctx is loaded, and the    *)
(*   boot CPU has a SECOND dispatch route (the deadlock-path g_bootcpu_idle *)
(*   pointer) that is independent of any runqueue. Every real SMP bug       *)
(*   (#788 / #806 / #860 + its smp8 sibling) lives in exactly the windows   *)
(*   scheduler.tla smooths over. The old model proved the high-level state  *)
(*   machine sound UNDER an atomicity assumption the impl does not satisfy. *)
(*                                                                         *)
(*   This module re-introduces the abstracted-away mechanism:               *)
(*     - `on_cpu[t]`  -- the cross-CPU "this ctx is claimed / in use" flag. *)
(*     - a per-CPU `locked[c]` held ACROSS the multi-step switch (the impl  *)
(*       cs->lock held from sched() entry until the resuming thread's       *)
(*       finish_task_switch) -- what makes a peer's try_steal (spin_trylock)*)
(*       skip a mid-switch CPU.                                             *)
(*     - the boot-CPU deadlock-path dispatch of `BootIdle` (g_bootcpu_idle) *)
(*       via a global pointer, INDEPENDENT of any runqueue.                 *)
(*     - BootIdle being a REAL-kstack idle thread that is STEALABLE if it   *)
(*       ever lands in a runqueue (the #860 root cause).                    *)
(*                                                                         *)
(* PARAMETERS select the impl variant under test:                          *)
(*   BOOTIDLE_IN_TREE     -- TRUE  : BootIdle inserted into runq on yield    *)
(*                                   (pre-#860, AND option B).               *)
(*                           FALSE : BootIdle stays OFF-tree; the deadlock   *)
(*                                   path is its sole dispatcher (option A). *)
(*   STEAL_SKIP_BOOTIDLE  -- TRUE  : try_steal never steals BootIdle         *)
(*                                   (option A and option B both add this).  *)
(*   DEADLOCK_GUARD       -- TRUE  : the boot-CPU deadlock dispatch only      *)
(*                                   fires if ~on_cpu[BootIdle]; if it WOULD  *)
(*                                   need BootIdle while on_cpu, the impl     *)
(*                                   EXTINCTS -- modeled as guard_fired.      *)
(*                                                                         *)
(* CONFIG MATRIX (the A/B experiment at the spec level):                    *)
(*   sched_oncpu_prefix860.cfg : IN_TREE=T, SKIP=F, GUARD=F                  *)
(*       -> NoSimultaneousRun VIOLATED (the #860 silent ctx corruption:      *)
(*          a secondary steals BootIdle from runq[0] while CPU0's deadlock   *)
(*          path re-dispatches it -> two CPUs run one thread).               *)
(*   sched_oncpu_intree_guard.cfg : IN_TREE=T, SKIP=F, GUARD=T               *)
(*       -> NoSimultaneousRun holds, but NoGuardFire VIOLATED (the guard     *)
(*          converts the silent corruption into a LOUD extinction -- still   *)
(*          broken, just fail-stop; proves the guard ALONE is not the fix).  *)
(*   sched_oncpu_optionB.cfg : IN_TREE=T, SKIP=T, GUARD=T                    *)
(*       -> CLEAN. BootIdle is unstealable, so it is never on_cpu on a peer; *)
(*          the deadlock path is always safe; zero dispatch-dynamics change  *)
(*          vs prefix (BootIdle still in-tree, still pick_next-dispatched).  *)
(*   sched_oncpu_optionA.cfg : IN_TREE=F, SKIP=T, GUARD=T                    *)
(*       -> CLEAN at THIS abstraction level. BootIdle off-tree => never in a *)
(*          runq => never stolen. (Any empirical smp8 regression of option A *)
(*          is therefore BELOW this state-machine abstraction -- a           *)
(*          dispatch-dynamics / memory-ordering / kstack-lifecycle effect    *)
(*          not captured by the high-level invariants. That a point-fix can  *)
(*          be model-clean yet differ empirically IS the patchwork signature.)*)
(*                                                                         *)
(* Sibling module of specs/scheduler.tla + specs/sched_ctxsw.tla.           *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Workers,             \* set of ordinary thread ids (>= 1)
    CPUs,                \* set of cpu ids (>= 2 to exercise steal vs deadlock)
    CPU0,                \* the distinguished boot cpu (owns the deadlock path)
    BootIdle,            \* g_bootcpu_idle -- the real-kstack boot-cpu idle thread
    NULL,                \* "no thread" sentinel
    BOOTIDLE_IN_TREE,    \* see header
    STEAL_SKIP_BOOTIDLE, \* see header
    DEADLOCK_GUARD       \* see header

Threads == Workers \cup {BootIdle}

ASSUME CPU0 \in CPUs
ASSUME Cardinality(CPUs) >= 2
ASSUME BootIdle \notin Workers
ASSUME NULL \notin Threads
ASSUME BOOTIDLE_IN_TREE    \in BOOLEAN
ASSUME STEAL_SKIP_BOOTIDLE \in BOOLEAN
ASSUME DEADLOCK_GUARD      \in BOOLEAN

VARIABLES
    tstate,       \* [Threads -> {"RUNNING","RUNNABLE","SLEEPING"}]
    on_cpu,       \* [Threads -> BOOLEAN] -- ctx claimed / in use
    current,      \* [CPUs -> Threads \cup {NULL}] -- the running thread
    runq,         \* [CPUs -> SUBSET Threads] -- per-CPU run tree (band-agnostic)
    locked,       \* [CPUs -> BOOLEAN] -- cs->lock held across a multi-step switch
    pend_prev,    \* [CPUs -> Threads \cup {NULL}] -- prev_to_clear_on_cpu slot
    guard_fired   \* BOOLEAN -- the boot-cpu deadlock guard tripped an extinction

vars == <<tstate, on_cpu, current, runq, locked, pend_prev, guard_fired>>

States == {"RUNNING", "RUNNABLE", "SLEEPING"}

TypeOk ==
    /\ tstate      \in [Threads -> States]
    /\ on_cpu      \in [Threads -> BOOLEAN]
    /\ current     \in [CPUs -> Threads \cup {NULL}]
    /\ runq        \in [CPUs -> SUBSET Threads]
    /\ locked      \in [CPUs -> BOOLEAN]
    /\ pend_prev   \in [CPUs -> Threads \cup {NULL}]
    /\ guard_fired \in BOOLEAN

(***************************************************************************)
(* Init: every Worker is RUNNABLE in CPU0's runq; BootIdle is RUNNABLE but  *)
(* in NO runq (its off-tree resting state -- it enters play only via the   *)
(* boot-cpu deadlock dispatch). Every CPU is idle (current = NULL),         *)
(* unlocked, nothing on_cpu.                                                *)
(***************************************************************************)
Init ==
    /\ tstate      = [t \in Threads |-> "RUNNABLE"]
    /\ on_cpu      = [t \in Threads |-> FALSE]
    /\ current     = [c \in CPUs |-> NULL]
    /\ runq        = [c \in CPUs |-> IF c = CPU0 THEN Workers ELSE {}]
    /\ locked      = [c \in CPUs |-> FALSE]
    /\ pend_prev   = [c \in CPUs |-> NULL]
    /\ guard_fired = FALSE

(***************************************************************************)
(* Steal candidates for cpu c: a thread sitting in some OTHER cpu's runq    *)
(* whose owner is not mid-switch (~locked), honoring the BootIdle skip.     *)
(* The ~locked[v] requirement is the model's faithful image of the impl's   *)
(* spin_trylock-fails-on-a-mid-switch-peer: it is what protects the         *)
(* yield-insert window (prev in runq[c] with on_cpu still TRUE while         *)
(* locked[c]).                                                              *)
(***************************************************************************)
StealSrc(c, t) ==
    \E v \in CPUs : /\ v # c
                    /\ ~locked[v]
                    /\ t \in runq[v]
StealCand(c) ==
    { t \in Threads : /\ StealSrc(c, t)
                      /\ (STEAL_SKIP_BOOTIDLE => t # BootIdle) }

\* Remove t from whichever runq holds it (it is in at most one).
RemoveFromRunq(rq, t) == [c \in CPUs |-> rq[c] \ {t}]

\* prev's post-switch disposition + whether it re-enters a runq.
PrevDisp(prev, kind) ==
    IF kind = "yield" THEN "RUNNABLE" ELSE "SLEEPING"

\* Does prev get re-inserted into c's runq on this switch?
\*   yield  -> yes, EXCEPT BootIdle when off-tree (the option-A carve-out).
\*   block  -> no (it sleeps).
PrevReinserts(prev, kind) ==
    /\ kind = "yield"
    /\ (prev = BootIdle => BOOTIDLE_IN_TREE)

\* The shared switch effect: lock c, set next RUNNING + on_cpu, stash prev to
\* clear, dispose prev. `rq2` is the runq with `nxt` already removed.
DoSwitch(c, prev, nxt, kind, rq2) ==
    /\ current'  = [current EXCEPT ![c] = nxt]
    /\ tstate'   = [tstate EXCEPT ![nxt] = "RUNNING", ![prev] = PrevDisp(prev, kind)]
    /\ on_cpu'   = [on_cpu EXCEPT ![nxt] = TRUE]          \* prev stays on_cpu
    /\ runq'     = IF PrevReinserts(prev, kind)
                      THEN [rq2 EXCEPT ![c] = rq2[c] \cup {prev}]
                      ELSE rq2
    /\ locked'   = [locked EXCEPT ![c] = TRUE]
    /\ pend_prev'= [pend_prev EXCEPT ![c] = prev]
    /\ UNCHANGED guard_fired

\* Boot-cpu deadlock path: dispatch BootIdle via the global pointer, NOT a
\* runq. With the guard, this is only safe if ~on_cpu[BootIdle]; otherwise
\* the impl EXTINCTS -> guard_fired (a loud crash, modeled distinctly from
\* the silent NoSimultaneousRun corruption). Without the guard, dispatch
\* unconditionally (the pre-fix behavior that yields the #860 double-run).
DeadlockDispatch(c, prev) ==
    IF DEADLOCK_GUARD /\ on_cpu[BootIdle]
       THEN /\ guard_fired' = TRUE
            /\ UNCHANGED <<tstate, on_cpu, current, runq, locked, pend_prev>>
       ELSE /\ current'  = [current EXCEPT ![c] = BootIdle]
            /\ tstate'   = [tstate EXCEPT ![BootIdle] = "RUNNING", ![prev] = "SLEEPING"]
            /\ on_cpu'   = [on_cpu EXCEPT ![BootIdle] = TRUE]
            /\ runq'     = runq
            /\ locked'   = [locked EXCEPT ![c] = TRUE]
            /\ pend_prev'= [pend_prev EXCEPT ![c] = prev]
            /\ UNCHANGED guard_fired

(***************************************************************************)
(* Resume(c): an idle CPU (current = NULL, not mid-switch) picks up work    *)
(* -- from its own runq (pick_next) or by stealing. No prev to save, so     *)
(* this is atomic (no pend_prev). on_cpu[t] is claimed as t comes on-CPU.   *)
(***************************************************************************)
Resume(c) ==
    /\ ~locked[c]
    /\ current[c] = NULL
    /\ LET cand == runq[c] \cup StealCand(c)
       IN  /\ cand # {}
           /\ \E t \in cand :
                /\ tstate[t] = "RUNNABLE"
                /\ ~on_cpu[t]
                /\ current'  = [current EXCEPT ![c] = t]
                /\ tstate'   = [tstate  EXCEPT ![t] = "RUNNING"]
                /\ on_cpu'   = [on_cpu  EXCEPT ![t] = TRUE]
                /\ runq'     = RemoveFromRunq(runq, t)
                /\ UNCHANGED <<locked, pend_prev, guard_fired>>

(***************************************************************************)
(* StartSwitch(c) -- the running thread on c reaches sched() and there IS a *)
(* `next` to switch to. Covers BOTH yield (prev -> RUNNABLE) and block      *)
(* (prev -> SLEEPING), and the boot-cpu deadlock dispatch of BootIdle.      *)
(*                                                                         *)
(* `next` selection mirrors the impl's pick order: own runq first, then     *)
(* steal, then (block-with-nothing on CPU0) the deadlock-path BootIdle      *)
(* pointer. Locks c across the switch; the resuming thread releases via     *)
(* FinishSwitch. on_cpu[prev] stays TRUE until FinishSwitch (the #788       *)
(* "switched-out but ctx-save not yet observed" window).                    *)
(*                                                                         *)
(* The idle thread BootIdle is an idle LOOP -- it yields (sched while       *)
(* RUNNING) but never voluntarily blocks; so when prev = BootIdle the only  *)
(* legal intent is "yield" (faithful to bootcpu_idle_main).                 *)
(***************************************************************************)
Intent == {"yield", "block"}

StartSwitch(c) ==
    /\ ~locked[c]
    /\ current[c] # NULL
    /\ tstate[current[c]] = "RUNNING"
    /\ \E kind \in Intent :
        /\ (current[c] = BootIdle => kind = "yield")
        /\ LET prev == current[c]
               localCand == runq[c]
               stealCand == StealCand(c)
           IN  \/ \* (1) pick_next: a local runq thread.
                  /\ localCand # {}
                  /\ \E nxt \in localCand :
                       /\ tstate[nxt] = "RUNNABLE" /\ ~on_cpu[nxt]
                       /\ DoSwitch(c, prev, nxt, kind, RemoveFromRunq(runq, nxt))
               \/ \* (2) try_steal: a peer's runq thread (local empty).
                  /\ localCand = {}
                  /\ stealCand # {}
                  /\ \E nxt \in stealCand :
                       /\ tstate[nxt] = "RUNNABLE" /\ ~on_cpu[nxt]
                       /\ DoSwitch(c, prev, nxt, kind, RemoveFromRunq(runq, nxt))
               \/ \* (3) boot-cpu deadlock dispatch (block, nothing local/steal).
                  /\ kind = "block"
                  /\ localCand = {}
                  /\ stealCand = {}
                  /\ c = CPU0
                  /\ DeadlockDispatch(c, prev)

(***************************************************************************)
(* FinishSwitch(c) -- the resuming thread on c runs finish_task_switch:     *)
(* clears the switched-out prev's on_cpu (RELEASE) THEN releases the lock.  *)
(* Modeled atomically (the impl clears on_cpu before unlocking, so there is *)
(* never a lock-free window with prev still on_cpu).                        *)
(***************************************************************************)
FinishSwitch(c) ==
    /\ locked[c]
    /\ pend_prev[c] # NULL
    /\ on_cpu'    = [on_cpu EXCEPT ![pend_prev[c]] = FALSE]
    /\ locked'    = [locked EXCEPT ![c] = FALSE]
    /\ pend_prev' = [pend_prev EXCEPT ![c] = NULL]
    /\ UNCHANGED <<tstate, current, runq, guard_fired>>

(***************************************************************************)
(* GoIdle(c) -- a SECONDARY CPU whose current blocks with no work goes idle *)
(* (current := NULL, abstract WFI). The impl switches to the secondary's    *)
(* kstack-NULL idle thread which runs the resume-clear; we abstract that    *)
(* idle thread away and clear prev's on_cpu atomically. CPU0 never GoIdles  *)
(* -- it always has BootIdle (DeadlockDispatch). BootIdle itself never      *)
(* blocks (idle loop), so it is never the `prev` here.                      *)
(***************************************************************************)
GoIdle(c) ==
    /\ ~locked[c]
    /\ c # CPU0
    /\ current[c] # NULL
    /\ current[c] # BootIdle
    /\ tstate[current[c]] = "RUNNING"
    /\ runq[c] = {}
    /\ StealCand(c) = {}
    /\ LET prev == current[c]
       IN  /\ current'  = [current EXCEPT ![c] = NULL]
           /\ tstate'   = [tstate EXCEPT ![prev] = "SLEEPING"]
           /\ on_cpu'   = [on_cpu EXCEPT ![prev] = FALSE]
           /\ UNCHANGED <<runq, locked, pend_prev, guard_fired>>

(***************************************************************************)
(* Wake(t) -- an external event (timer / IPI / wakeup) makes a SLEEPING     *)
(* thread RUNNABLE again and enqueues it on some CPU's runq. Models the     *)
(* regenerative loop so the system does not simply quiesce. A fully         *)
(* switched-out sleeper only (on_cpu = FALSE).                              *)
(***************************************************************************)
Wake(t) ==
    /\ tstate[t] = "SLEEPING"
    /\ ~on_cpu[t]
    /\ \E c \in CPUs :
         /\ tstate' = [tstate EXCEPT ![t] = "RUNNABLE"]
         /\ runq'   = [runq EXCEPT ![c] = runq[c] \cup {t}]
         /\ UNCHANGED <<on_cpu, current, locked, pend_prev, guard_fired>>

Next ==
    \/ \E c \in CPUs : Resume(c)
    \/ \E c \in CPUs : StartSwitch(c)
    \/ \E c \in CPUs : FinishSwitch(c)
    \/ \E c \in CPUs : GoIdle(c)
    \/ \E t \in Threads : Wake(t)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* NoSimultaneousRun (I-21 corollary) -- a thread is `current` on at most one
\* CPU. The #860 silent ctx corruption violates this: CPU0's deadlock path
\* re-dispatches BootIdle while a secondary that stole it still runs it.
NoSimultaneousRun ==
    \A t \in Threads :
        Cardinality({c \in CPUs : current[c] = t}) <= 1

\* OwnerUnique -- a thread is "owned" (running OR mid-switch-out) by at most
\* one CPU. Strengthens NoSimultaneousRun across the on_cpu window.
OwnerUnique ==
    \A t \in Threads :
        Cardinality({c \in CPUs : current[c] = t} \cup
                    {c \in CPUs : pend_prev[c] = t}) <= 1

\* OnCpuMeansOwned -- on_cpu[t] => some CPU is running it or saving it out.
OnCpuMeansOwned ==
    \A t \in Threads :
        on_cpu[t] =>
            (\E c \in CPUs : current[c] = t \/ pend_prev[c] = t)

\* RunningImpliesOnCpu -- a RUNNING thread is on_cpu.
RunningImpliesOnCpu ==
    \A t \in Threads :
        (\E c \in CPUs : current[c] = t) => on_cpu[t]

\* RunqRunnable -- a thread in a runq is RUNNABLE and is no CPU's current.
RunqRunnable ==
    \A t \in Threads :
        (\E c \in CPUs : t \in runq[c]) =>
            /\ tstate[t] = "RUNNABLE"
            /\ \A c \in CPUs : current[c] # t

\* RunqOnCpuSafe -- on a CPU that is NOT mid-switch, every runq thread has
\* on_cpu = FALSE, i.e. its ctx is a complete saved snapshot and a steal of
\* it cannot load a half-saved ctx. This is the property the lock-held-
\* across-the-switch discipline buys: the yield-insert window (prev in
\* runq[c] with on_cpu still TRUE) exists ONLY while locked[c].
RunqOnCpuSafe ==
    \A c \in CPUs :
        ~locked[c] => (\A t \in runq[c] : ~on_cpu[t])

\* NoDoubleEnqueue -- a thread sits in at most one runq.
NoDoubleEnqueue ==
    \A t \in Threads :
        Cardinality({c \in CPUs : t \in runq[c]}) <= 1

\* NoGuardFire -- the boot-cpu deadlock guard never trips an extinction.
\* Holds ONLY when BootIdle can never be on_cpu at the moment CPU0 needs it
\* via the deadlock path -- i.e. when BootIdle is UNSTEALABLE (option A/B).
\* Violated by the in-tree+guard+stealable config: the guard fires (a loud
\* crash) instead of corrupting silently -- proving the guard alone is not
\* the fix.
NoGuardFire == ~guard_fired

\* Composite safety (corruption-class) -- checked by every cfg.
Safety ==
    /\ TypeOk
    /\ NoSimultaneousRun
    /\ OwnerUnique
    /\ OnCpuMeansOwned
    /\ RunningImpliesOnCpu
    /\ RunqRunnable
    /\ RunqOnCpuSafe
    /\ NoDoubleEnqueue

\* Availability (no fail-stop) -- checked separately so the guard-fire
\* configs can demonstrate the loud-crash distinctly from corruption.
Availability == NoGuardFire

====
