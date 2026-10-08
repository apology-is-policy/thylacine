---- MODULE thread_reap ----
(***************************************************************************)
(* XT-3b (docs/X86-TRANSLATION-DESIGN.md 5.8, XT-K9; I-32, I-24): an       *)
(* exited Thread is reclaimed while its Proc lives on, so PROC_THREAD_MAX   *)
(* counts LIVE Threads instead of every spawn in the Proc's life.           *)
(*                                                                         *)
(* THE PROTOCOL (kernel/proc.c, kernel/thread.c).                           *)
(*   - A Thread that exits while a peer lives commits EXITING and RETIRES:  *)
(*     one g_proc_table_lock hold moves it from the Proc's live list         *)
(*     (Proc.threads) to its retired list (Proc.exited). Its TAIL then runs  *)
(*     -- the clear_child_tid store into the address space and the torpor    *)
(*     wake -- and it switches away for good. The switch's on_cpu clear is   *)
(*     the moment it SETTLES.                                                *)
(*   - A LIVE Thread of the same Proc reaps at its spawn and at its exit.    *)
(*     The CLAIM, under the lock, marks every SETTLED unclaimed retired      *)
(*     Thread; marking under the lock is what keeps two concurrent reapers   *)
(*     disjoint. A claimed Thread stays on the retired list, still counted   *)
(*     by the Proc's totals, until the COMMIT: one hold folds its run time   *)
(*     and stack depth into the Proc and unlinks it, then the free follows   *)
(*     with the lock dropped.                                                *)
(*   - wait_pid reaps a ZOMBIE: it detaches both lists (the last Thread out  *)
(*     stays on the live list; retired Threads no peer reached), spins each  *)
(*     until settled, frees it, then frees the Proc and its address space.   *)
(*     The zombie is unreachable by then, so nothing reads its totals.       *)
(*   - exec, whose execer is the Proc's only live Thread, DRAINS the retired *)
(*     list (claim, wait each out until settled, commit, free) BEFORE it     *)
(*     frees the old address space: a retired Thread's tail still stores     *)
(*     into it.                                                              *)
(*   - the spawner reads the new Thread's tid BEFORE ready(): once ready,    *)
(*     the new Thread can run, exit and be reaped by a peer.                 *)
(*                                                                         *)
(* A live reaper and wait_pid never overlap on one Proc, because a Proc is *)
(* ZOMBIE only once no Thread of it is live. The model checks that instead *)
(* of assuming it: Claim needs a live reaper, Exit makes the Proc ZOMBIE    *)
(* only from the last live Thread, every free goes through the claim its    *)
(* freer made, and ClaimsHeldByLive says no claim outlives its claimer.     *)
(*                                                                         *)
(* ROUNDS. The C claims at most REAP_ROUND Threads per round and repeats   *)
(* while a round comes back full, dropping the lock between rounds, so two  *)
(* reapers can split one settled set. The model does the same at a smaller  *)
(* scale: a round claims Min(RoundMax, |reapable|) of the reapable Threads, *)
(* chosen nondeterministically, and the reaper loops ("reaploop") until a  *)
(* round comes back short. The action property ReapEndsOnShortRound is the  *)
(* C's `while (n == REAP_ROUND)`.                                           *)
(*                                                                         *)
(* COARSENINGS, each a superset of the C's behaviours or argued equal:      *)
(*   - Exit does not have to follow a reap. The C reaps at every exit's     *)
(*     entry; the model lets an exit skip it, so its safety verdicts cover  *)
(*     the C's reap-first exits.                                            *)
(*   - Exec claims every retired Thread at once, settled or not, and frees  *)
(*     each once settled. The C claims settled ones round by round; no      *)
(*     other reaper exists while the execer is alone, so the two orders     *)
(*     cannot be told apart.                                                *)
(*   - One step folds, unlinks and frees a claimed Thread. The C folds and  *)
(*     unlinks a round in one hold and frees after it; between the two the *)
(*     Thread is on no list and held by one claim, so nothing can reach it. *)
(*                                                                         *)
(* NOT MODELLED: CPUs. The bound on retired-but-allocated Threads (about   *)
(* twice the CPU count, kernel/include/thylacine/proc.h proc_reap_retired)  *)
(* rests on a stretch per CPU that is never switched out, which this model  *)
(* has no CPU to state; it lets every Thread sit in its tail at once. The   *)
(* half it can see is ReapEndsOnShortRound: a reap ends only after a round  *)
(* that took every reapable Thread. The churn test's retired_max and        *)
(* /thread-torture witness the bound at runtime.                            *)
(*                                                                         *)
(* AN OBLIGATION ON THE C, not a result: WF(Settle), "every tail reaches a *)
(* switch away". It holds because a tail never sleeps                       *)
(* (seam-exiting-tails-never-sleep) and spins only on bounded waits. A tail *)
(* switched out early by a preemption settles too, its work cut short; that *)
(* lost wake (task #20, closed by preempt_check_irq's EXITING refusal) is   *)
(* below this model, which has no joiners, and                              *)
(* scheduler.preempt_gate_defers_while_exiting is its witness.              *)
(*                                                                         *)
(* NOT MODELLED, and why it needs no step here: walkers of the live list   *)
(* (the death-wake cascade, devproc, proc_cpu_ns) run entirely under        *)
(* g_proc_table_lock, and a Thread is freed only after a lock hold has      *)
(* detached it, so a walker's whole use of a Thread is one atomic step that *)
(* sees it allocated. A walker that keeps a pointer across the lock drop is *)
(* the BUGGY_TID_AFTER_READY shape below.                                   *)
(*                                                                         *)
(* THE BUG CLASSES (one buggy cfg each, each judged by ONE named invariant):*)
(*   BUGGY_REAP_IGNORES_ONCPU    reap a retired Thread still in its tail    *)
(*                               -> NoFreeInFlight                          *)
(*   BUGGY_CLAIM_UNLOCKED        choose under the lock, mark after it       *)
(*                               -> OneFreerPerThread                       *)
(*   BUGGY_UNLINK_AT_CLAIM       the claim unlinks, the fold comes later    *)
(*                               (audit F1) -> EveryThreadCounted           *)
(*   BUGGY_EXEC_NO_DRAIN         exec frees the old space under a tail      *)
(*                               -> TailsOnLiveSpace                        *)
(*   BUGGY_WAITPID_SKIPS_RETIRED wait_pid frees the live list only          *)
(*                               -> TailsOnLiveSpace                        *)
(*   BUGGY_TID_AFTER_READY       the spawner reads nt->tid after ready()    *)
(*                               -> NoTidReadAfterFree                      *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,                      \* every Thread the Proc will ever have
    Main,                         \* the one alive at the start
    MaxGen,                       \* address-space generations (exec bound)
    RoundMax,                     \* Threads one reap round claims (REAP_ROUND)
    BUGGY_REAP_IGNORES_ONCPU,
    BUGGY_CLAIM_UNLOCKED,
    BUGGY_UNLINK_AT_CLAIM,
    BUGGY_EXEC_NO_DRAIN,
    BUGGY_WAITPID_SKIPS_RETIRED,
    BUGGY_TID_AFTER_READY

ASSUME Main \in Threads
ASSUME MaxGen \in Nat \ {0}
ASSUME RoundMax \in Nat \ {0}

NoThread == "none"
Wait     == "wait"     \* wait_pid in the parent: a freer that is not a Thread of p

(* Thread program counters:
     "unborn"    -- not yet spawned
     "live"      -- RUNNING / RUNNABLE / SLEEPING: a live Thread
     "spawning"  -- live, inside the spawn after ready() (BUGGY_TID_AFTER_READY)
     "selecting" -- live, candidates chosen, not yet marked (BUGGY_CLAIM_UNLOCKED)
     "reaping"   -- live, freeing the Threads its current round claimed
     "reaploop"  -- live, a full round done: it must claim again or end
     "execing"   -- live, the exec drain
     "tail"      -- EXITING and still on a CPU: the tail, then the switch away
     "settled"   -- EXITING, switched away, on_cpu clear: nothing runs on it
     "freed"     -- its Thread struct and kstack are back in the allocators *)
PCs   == {"unborn", "live", "spawning", "selecting", "reaping", "reaploop", "execing",
          "tail", "settled", "freed"}
Lists == {"none", "live", "retired"}

VARIABLES
    pc,            \* [Threads -> PCs]
    list,          \* [Threads -> Lists]   which of the Proc's lists holds it
    claims,        \* [Threads -> SUBSET (Threads \cup {Wait})]  who will free it
    sel,           \* [Threads -> SUBSET Threads]  BUGGY_CLAIM_UNLOCKED's choice
    pending,       \* [Threads -> Threads \cup {NoThread}]  the tid still to read
    proc,          \* "alive" | "zombie" | "reaped" | "freed"
    gen,           \* the Proc's current address-space generation
    tgen,          \* [Threads -> 0..MaxGen]  the space each Thread runs in
    dead,          \* address-space generations already freed
    folded,        \* Threads whose totals were folded into the Proc's
    nround,        \* [Threads -> Nat]  how many the reaper's current round claimed
    freeInFlight,  \* a Thread was freed while it was still in its tail
    doubleFree,    \* a Thread was freed twice
    uafRead        \* the spawner read a freed Thread

vars == <<pc, list, claims, sel, pending, proc, gen, tgen, dead, folded, nround,
          freeInFlight, doubleFree, uafRead>>

Live(t)  == pc[t] \in {"live", "spawning", "selecting", "reaping", "reaploop", "execing"}
LiveList == {t \in Threads : list[t] = "live"}

TypeOk ==
    /\ pc      \in [Threads -> PCs]
    /\ list    \in [Threads -> Lists]
    /\ claims  \in [Threads -> SUBSET (Threads \cup {Wait})]
    /\ sel     \in [Threads -> SUBSET Threads]
    /\ pending \in [Threads -> Threads \cup {NoThread}]
    /\ proc    \in {"alive", "zombie", "reaped", "freed"}
    /\ gen     \in 1..MaxGen
    /\ tgen    \in [Threads -> 0..MaxGen]
    /\ dead    \subseteq 1..MaxGen
    /\ folded  \subseteq Threads
    /\ nround  \in [Threads -> 0..Cardinality(Threads)]
    /\ freeInFlight \in BOOLEAN
    /\ doubleFree   \in BOOLEAN
    /\ uafRead      \in BOOLEAN

Init ==
    /\ pc      = [t \in Threads |-> IF t = Main THEN "live" ELSE "unborn"]
    /\ list    = [t \in Threads |-> IF t = Main THEN "live" ELSE "none"]
    /\ claims  = [t \in Threads |-> {}]
    /\ sel     = [t \in Threads |-> {}]
    /\ pending = [t \in Threads |-> NoThread]
    /\ proc    = "alive"
    /\ gen     = 1
    /\ tgen    = [t \in Threads |-> IF t = Main THEN 1 ELSE 0]
    /\ dead    = {}
    /\ folded  = {}
    /\ nround  = [t \in Threads |-> 0]
    /\ freeInFlight = FALSE
    /\ doubleFree   = FALSE
    /\ uafRead      = FALSE

(* One free of Thread t by claimer c. The flags record what the free did
   instead of refusing it, so an invariant can name the violation. *)
FreeBy(c, t) ==
    /\ freeInFlight' = (freeInFlight \/ pc[t] = "tail")
    /\ doubleFree'   = (doubleFree \/ pc[t] = "freed")
    /\ pc'     = [pc EXCEPT ![t] = "freed"]
    /\ claims' = [claims EXCEPT ![t] = @ \ {c}]

(* The commit of a claimed Thread, then its free: the fold and the unlink in
   one hold (proc_commit_reaped), the free after it. *)
CommitFree(c, t) ==
    /\ list'   = [list EXCEPT ![t] = "none"]
    /\ folded' = folded \cup {t}
    /\ FreeBy(c, t)

Detached(set) == [t \in Threads |-> IF t \in set THEN "none" ELSE list[t]]
ClaimedBy(set, c) ==
    [t \in Threads |-> IF t \in set THEN claims[t] \cup {c} ELSE claims[t]]

(* --- spawn (sys_thread_spawn_handler; the vivarium clone thread arm) --- *)
Spawn(s, n) ==
    /\ pc[s] = "live" /\ proc = "alive" /\ pc[n] = "unborn"
    /\ list' = [list EXCEPT ![n] = "live"]
    /\ tgen' = [tgen EXCEPT ![n] = gen]
    /\ IF BUGGY_TID_AFTER_READY
         THEN /\ pc'      = [pc EXCEPT ![n] = "live", ![s] = "spawning"]
              /\ pending' = [pending EXCEPT ![s] = n]
         ELSE /\ pc'      = [pc EXCEPT ![n] = "live"]
              /\ UNCHANGED pending
    /\ UNCHANGED <<claims, sel, proc, gen, dead, folded, nround, freeInFlight, doubleFree, uafRead>>

(* The spawner's `return nt->tid` when it is read after ready(). *)
ReadTid(s) ==
    /\ pc[s] = "spawning"
    /\ uafRead' = (uafRead \/ pc[pending[s]] = "freed")
    /\ pc'      = [pc EXCEPT ![s] = "live"]
    /\ pending' = [pending EXCEPT ![s] = NoThread]
    /\ UNCHANGED <<list, claims, sel, proc, gen, tgen, dead, folded, nround, freeInFlight, doubleFree>>

(* --- exit: thread_exit_self's commit, one g_proc_table_lock hold --- *)
Exit(t) ==
    /\ pc[t] = "live" /\ proc = "alive"
    /\ pc' = [pc EXCEPT ![t] = "tail"]
    /\ IF LiveList = {t}
         THEN /\ proc' = "zombie"              \* the last out keeps its place
              /\ UNCHANGED list
         ELSE /\ list' = [list EXCEPT ![t] = "retired"]
              /\ UNCHANGED proc
    /\ UNCHANGED <<claims, sel, pending, gen, tgen, dead, folded, nround, freeInFlight, doubleFree, uafRead>>

(* The final switch away completes: the destination clears on_cpu. *)
Settle(t) ==
    /\ pc[t] = "tail"
    /\ pc' = [pc EXCEPT ![t] = "settled"]
    /\ UNCHANGED <<list, claims, sel, pending, proc, gen, tgen, dead, folded, nround,
                   freeInFlight, doubleFree, uafRead>>

(* --- the live reaper (proc_reap_retired, at spawn and at exit) --- *)
Reapable(t) ==
    /\ list[t] = "retired" /\ claims[t] = {}
    /\ \/ pc[t] = "settled"
       \/ BUGGY_REAP_IGNORES_ONCPU /\ pc[t] = "tail"

Min(a, b) == IF a < b THEN a ELSE b

(* One round of the claim (proc_claim_settled_retired): under the lock, mark
   Min(RoundMax, |reapable|) of the reapable Threads -- which ones is the
   list order, here a free choice -- and leave them on the retired list. *)
ClaimRound(r) ==
    /\ proc = "alive"
    /\ LET cand == {t \in Threads : Reapable(t)} IN
         /\ cand # {}
         /\ \E S \in SUBSET cand :
              /\ Cardinality(S) = Min(RoundMax, Cardinality(cand))
              /\ claims' = ClaimedBy(S, r)
              /\ nround' = [nround EXCEPT ![r] = Cardinality(S)]
              /\ IF BUGGY_UNLINK_AT_CLAIM THEN list' = Detached(S)
                                          ELSE UNCHANGED list
    /\ pc' = [pc EXCEPT ![r] = "reaping"]
    /\ UNCHANGED <<sel, pending, proc, gen, tgen, dead, folded, freeInFlight, doubleFree, uafRead>>

(* A reap point (a spawn, an exit's entry) starts a reap; nothing forces one. *)
ClaimStart(r) ==
    /\ ~BUGGY_CLAIM_UNLOCKED
    /\ pc[r] = "live"
    /\ ClaimRound(r)

(* After a full round the reaper claims again (proc_reap_retired's loop). *)
ClaimNext(r) ==
    /\ pc[r] = "reaploop"
    /\ ClaimRound(r)

(* The loop's last claim finds nothing: the reap returns. *)
LoopEnd(r) ==
    /\ pc[r] = "reaploop"
    /\ ~\E t \in Threads : Reapable(t)
    /\ pc'     = [pc EXCEPT ![r] = "live"]
    /\ nround' = [nround EXCEPT ![r] = 0]
    /\ UNCHANGED <<list, claims, sel, pending, proc, gen, tgen, dead, folded,
                   freeInFlight, doubleFree, uafRead>>

(* BUGGY_CLAIM_UNLOCKED: chosen under the lock, marked after it, so a second
   reaper can choose the same Threads in between. *)
Select(r) ==
    /\ BUGGY_CLAIM_UNLOCKED
    /\ pc[r] = "live" /\ proc = "alive"
    /\ LET cand == {t \in Threads : Reapable(t)} IN
         /\ cand # {}
         /\ sel' = [sel EXCEPT ![r] = cand]
    /\ pc' = [pc EXCEPT ![r] = "selecting"]
    /\ UNCHANGED <<list, claims, pending, proc, gen, tgen, dead, folded, nround, freeInFlight, doubleFree, uafRead>>

Mark(r) ==
    /\ pc[r] = "selecting"
    /\ claims' = ClaimedBy(sel[r], r)
    /\ sel'    = [sel EXCEPT ![r] = {}]
    /\ pc'     = [pc EXCEPT ![r] = "reaping"]
    /\ UNCHANGED <<list, pending, proc, gen, tgen, dead, folded, nround, freeInFlight, doubleFree, uafRead>>

(* A settled Thread needs no wait; thread_free_retired's on_cpu spin is
   defence for this freer, not a step it relies on. *)
ReapFree(r, t) ==
    /\ pc[r] = "reaping" /\ r \in claims[t]
    /\ CommitFree(r, t)
    /\ UNCHANGED <<sel, pending, proc, gen, tgen, dead, nround, uafRead>>

(* The round is freed. A full round loops; a short one ends the reap. *)
ReapDone(r) ==
    /\ pc[r] = "reaping"
    /\ \A t \in Threads : r \notin claims[t]
    /\ IF nround[r] = RoundMax
         THEN /\ pc'     = [pc EXCEPT ![r] = "reaploop"]
              /\ UNCHANGED nround
         ELSE /\ pc'     = [pc EXCEPT ![r] = "live"]
              /\ nround' = [nround EXCEPT ![r] = 0]
    /\ UNCHANGED <<list, claims, sel, pending, proc, gen, tgen, dead, folded,
                   freeInFlight, doubleFree, uafRead>>

(* --- exec (proc_exec_replace): the execer is the only live Thread --- *)
Exec(e) ==
    /\ pc[e] = "live" /\ proc = "alive" /\ LiveList = {e}
    /\ gen < MaxGen
    /\ IF BUGGY_EXEC_NO_DRAIN
         THEN /\ dead' = dead \cup {gen}
              /\ gen'  = gen + 1
              /\ tgen' = [tgen EXCEPT ![e] = gen + 1]
              /\ UNCHANGED <<pc, claims>>
         ELSE /\ claims' = ClaimedBy({t \in Threads : list[t] = "retired"}, e)
              /\ pc' = [pc EXCEPT ![e] = "execing"]
              /\ UNCHANGED <<gen, tgen, dead>>
    /\ UNCHANGED <<list, sel, pending, proc, folded, nround, freeInFlight, doubleFree, uafRead>>

(* The drain's wait: a claimed Thread is committed and freed once its switch
   has settled. *)
ExecFree(e, t) ==
    /\ pc[e] = "execing" /\ e \in claims[t] /\ pc[t] = "settled"
    /\ CommitFree(e, t)
    /\ UNCHANGED <<sel, pending, proc, gen, tgen, dead, nround, uafRead>>

ExecSwap(e) ==
    /\ pc[e] = "execing"
    /\ \A t \in Threads : e \notin claims[t]
    /\ dead' = dead \cup {gen}
    /\ gen'  = gen + 1
    /\ tgen' = [tgen EXCEPT ![e] = gen + 1]
    /\ pc'   = [pc EXCEPT ![e] = "live"]
    /\ UNCHANGED <<list, claims, sel, pending, proc, folded, nround, freeInFlight, doubleFree, uafRead>>

(* --- wait_pid in the parent --- *)
WaitPid ==
    /\ proc = "zombie"
    /\ LET take == {t \in Threads :
                      \/ list[t] = "live"
                      \/ list[t] = "retired" /\ ~BUGGY_WAITPID_SKIPS_RETIRED} IN
         /\ list'   = Detached(take)
         /\ claims' = ClaimedBy(take, Wait)
    /\ proc' = "reaped"
    /\ UNCHANGED <<pc, sel, pending, gen, tgen, dead, folded, nround, freeInFlight, doubleFree, uafRead>>

WaitFree(t) ==
    /\ proc = "reaped" /\ Wait \in claims[t] /\ pc[t] = "settled"
    /\ FreeBy(Wait, t)
    /\ UNCHANGED <<list, sel, pending, proc, gen, tgen, dead, folded, nround, uafRead>>

ProcFree ==
    /\ proc = "reaped"
    /\ \A t \in Threads : Wait \notin claims[t]
    /\ proc' = "freed"
    /\ dead' = dead \cup {gen}
    /\ UNCHANGED <<pc, list, claims, sel, pending, gen, tgen, folded, nround,
                   freeInFlight, doubleFree, uafRead>>

Next ==
    \/ \E s, n \in Threads : Spawn(s, n)
    \/ \E t \in Threads : ReadTid(t) \/ Exit(t) \/ Settle(t)
    \/ \E r \in Threads : ClaimStart(r) \/ ClaimNext(r) \/ LoopEnd(r)
                          \/ Select(r) \/ Mark(r) \/ ReapDone(r)
    \/ \E r, t \in Threads : ReapFree(r, t)
    \/ \E e \in Threads : Exec(e) \/ ExecSwap(e)
    \/ \E e, t \in Threads : ExecFree(e, t)
    \/ WaitPid
    \/ \E t \in Threads : WaitFree(t)
    \/ ProcFree

(* Fairness only on what the kernel guarantees will happen: a switch away
   completes, a spin ends, a reaper finishes what it claimed and the loop it
   is in, and a parent that reaps runs to the end. Nothing forces a Thread to
   exit, to spawn, or to start a reap. *)
Fairness ==
    /\ \A t \in Threads : WF_vars(Settle(t))
    /\ \A t \in Threads : WF_vars(ReadTid(t))
    /\ \A r \in Threads : WF_vars(Mark(r)) /\ WF_vars(ReapDone(r))
    /\ \A r \in Threads : WF_vars(ClaimNext(r)) /\ WF_vars(LoopEnd(r))
    /\ \A r, t \in Threads : WF_vars(ReapFree(r, t))
    /\ \A e, t \in Threads : WF_vars(ExecFree(e, t))
    /\ \A e \in Threads : WF_vars(ExecSwap(e))
    /\ WF_vars(WaitPid)
    /\ \A t \in Threads : WF_vars(WaitFree(t))
    /\ WF_vars(ProcFree)

Spec == Init /\ [][Next]_vars /\ Fairness

(* --- safety --- *)
NoFreeInFlight     == ~freeInFlight
OneFreerPerThread  == ~doubleFree /\ \A t \in Threads : Cardinality(claims[t]) <= 1
NoTidReadAfterFree == ~uafRead
TailsOnLiveSpace   == \A t \in Threads : pc[t] = "tail" => tgen[t] \notin dead
LiveOnLiveSpace    == \A t \in Threads : Live(t) => tgen[t] \notin dead
RetiredAreExited   == \A t \in Threads : list[t] = "retired" => pc[t] \in {"tail", "settled"}
LiveListIsLive     == proc = "alive" => \A t \in Threads : list[t] = "live" => Live(t)
NoLiveAfterZombie  == proc # "alive" => \A t \in Threads : ~Live(t)
FreedProcHoldsNone == proc = "freed" => \A t \in Threads : pc[t] # "settled"
(* While the Proc can be read (alive, or a zombie not yet unlinked), every
   Thread it ever had is on one of its lists or folded into its totals: no
   reading of proc_cpu_ns or proc_kstack_peak loses one (audit F1). *)
EveryThreadCounted ==
    proc \in {"alive", "zombie"} =>
        \A t \in Threads : pc[t] # "unborn" => (list[t] # "none" \/ t \in folded)
(* A claim is held only by a live Thread, or by wait_pid: so exec, which runs
   alone, and wait_pid, which runs on a zombie, never meet another's claim. *)
ClaimsHeldByLive ==
    \A t \in Threads : \A c \in claims[t] : c = Wait \/ Live(c)

Safety ==
    /\ TypeOk
    /\ NoFreeInFlight
    /\ OneFreerPerThread
    /\ NoTidReadAfterFree
    /\ TailsOnLiveSpace
    /\ LiveOnLiveSpace
    /\ RetiredAreExited
    /\ LiveListIsLive
    /\ NoLiveAfterZombie
    /\ FreedProcHoldsNone
    /\ EveryThreadCounted
    /\ ClaimsHeldByLive

(* --- liveness --- *)
(* A zombie is freed: no spin of wait_pid's waits forever. *)
EventuallyFreed == (proc = "zombie") ~> (proc = "freed")
(* An exec drain completes: no retired Thread holds the execer forever. *)
ExecCompletes == \A e \in Threads : (pc[e] = "execing") ~> (pc[e] = "live")
(* A claim is always freed: no reaper strands what it claimed. *)
ClaimsDischarged == \A t \in Threads : (claims[t] # {}) ~> (pc[t] = "freed")

(* A reap loop ends: a reaper in mid-loop returns to its own work. *)
ReapLoopEnds == \A r \in Threads : (pc[r] = "reaploop") ~> (pc[r] = "live")

(* --- action property --- *)
(* The C's `while (n == REAP_ROUND)`: a reap returns only after a round that
   came back short, which took every Thread reapable at its claim, or after a
   claim that found none. The half of the retired-list bound this model can
   state (see the header). *)
ReapEndsOnShortRound ==
    [][\A r \in Threads :
         (pc[r] \in {"reaping", "reaploop"} /\ pc'[r] = "live") =>
             (nround[r] < RoundMax \/ ~\E t \in Threads : Reapable(t))]_vars
====
