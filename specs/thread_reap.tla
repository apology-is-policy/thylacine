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
(*   - A LIVE Thread of the same Proc reaps at its spawn and at its exit:    *)
(*     under the lock it detaches every SETTLED retired Thread, then frees   *)
(*     them with the lock dropped. Detaching under the lock IS the claim;    *)
(*     it is what keeps two concurrent reapers disjoint.                     *)
(*   - wait_pid reaps a ZOMBIE: it detaches both lists (the last Thread out  *)
(*     stays on the live list; retired Threads no peer reached), spins each  *)
(*     until settled, frees it, then frees the Proc and its address space.   *)
(*   - exec, whose execer is the Proc's only live Thread, DRAINS the retired *)
(*     list (claim all, spin each until settled, free) BEFORE it frees the   *)
(*     old address space: a retired Thread's tail still stores into it.      *)
(*   - the spawner reads the new Thread's tid BEFORE ready(): once ready,    *)
(*     the new Thread can run, exit and be reaped by a peer.                 *)
(*                                                                         *)
(* A live reaper and wait_pid never overlap on one Proc, because a Proc is *)
(* ZOMBIE only once no Thread of it is live. The model checks that instead *)
(* of assuming it: Reap needs a live reaper, Exit makes the Proc ZOMBIE     *)
(* only from the last live Thread, and every free goes through the claim    *)
(* its freer made.                                                          *)
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
(*   BUGGY_CLAIM_UNLOCKED        choose under the lock, detach after it     *)
(*                               -> OneFreerPerThread                       *)
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
    BUGGY_REAP_IGNORES_ONCPU,
    BUGGY_CLAIM_UNLOCKED,
    BUGGY_EXEC_NO_DRAIN,
    BUGGY_WAITPID_SKIPS_RETIRED,
    BUGGY_TID_AFTER_READY

ASSUME Main \in Threads
ASSUME MaxGen \in Nat \ {0}

NoThread == "none"
Wait     == "wait"     \* wait_pid in the parent: a freer that is not a Thread of p

(* Thread program counters:
     "unborn"    -- not yet spawned
     "live"      -- RUNNING / RUNNABLE / SLEEPING: a live Thread
     "spawning"  -- live, inside the spawn after ready() (BUGGY_TID_AFTER_READY)
     "selecting" -- live, candidates chosen, not yet detached (BUGGY_CLAIM_UNLOCKED)
     "reaping"   -- live, freeing the Threads it claimed
     "execing"   -- live, the exec drain
     "tail"      -- EXITING and still on a CPU: the tail, then the switch away
     "settled"   -- EXITING, switched away, on_cpu clear: nothing runs on it
     "freed"     -- its Thread struct and kstack are back in the allocators *)
PCs   == {"unborn", "live", "spawning", "selecting", "reaping", "execing",
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
    freeInFlight,  \* a Thread was freed while it was still in its tail
    doubleFree,    \* a Thread was freed twice
    uafRead        \* the spawner read a freed Thread

vars == <<pc, list, claims, sel, pending, proc, gen, tgen, dead,
          freeInFlight, doubleFree, uafRead>>

Live(t)  == pc[t] \in {"live", "spawning", "selecting", "reaping", "execing"}
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
    /\ UNCHANGED <<claims, sel, proc, gen, dead, freeInFlight, doubleFree, uafRead>>

(* The spawner's `return nt->tid` when it is read after ready(). *)
ReadTid(s) ==
    /\ pc[s] = "spawning"
    /\ uafRead' = (uafRead \/ pc[pending[s]] = "freed")
    /\ pc'      = [pc EXCEPT ![s] = "live"]
    /\ pending' = [pending EXCEPT ![s] = NoThread]
    /\ UNCHANGED <<list, claims, sel, proc, gen, tgen, dead, freeInFlight, doubleFree>>

(* --- exit: thread_exit_self's commit, one g_proc_table_lock hold --- *)
Exit(t) ==
    /\ pc[t] = "live" /\ proc = "alive"
    /\ pc' = [pc EXCEPT ![t] = "tail"]
    /\ IF LiveList = {t}
         THEN /\ proc' = "zombie"              \* the last out keeps its place
              /\ UNCHANGED list
         ELSE /\ list' = [list EXCEPT ![t] = "retired"]
              /\ UNCHANGED proc
    /\ UNCHANGED <<claims, sel, pending, gen, tgen, dead, freeInFlight, doubleFree, uafRead>>

(* The final switch away completes: the destination clears on_cpu. *)
Settle(t) ==
    /\ pc[t] = "tail"
    /\ pc' = [pc EXCEPT ![t] = "settled"]
    /\ UNCHANGED <<list, claims, sel, pending, proc, gen, tgen, dead,
                   freeInFlight, doubleFree, uafRead>>

(* --- the live reaper (proc_reap_retired, at spawn and at exit) --- *)
Reapable(t) ==
    /\ list[t] = "retired"
    /\ \/ pc[t] = "settled"
       \/ BUGGY_REAP_IGNORES_ONCPU /\ pc[t] = "tail"

Reap(r) ==
    /\ ~BUGGY_CLAIM_UNLOCKED
    /\ pc[r] = "live" /\ proc = "alive"
    /\ LET cand == {t \in Threads : Reapable(t)} IN
         /\ cand # {}
         /\ list'   = Detached(cand)
         /\ claims' = ClaimedBy(cand, r)
    /\ pc' = [pc EXCEPT ![r] = "reaping"]
    /\ UNCHANGED <<sel, pending, proc, gen, tgen, dead, freeInFlight, doubleFree, uafRead>>

(* BUGGY_CLAIM_UNLOCKED: chosen under the lock, detached after it, so a second
   reaper can choose the same Threads in between. *)
Select(r) ==
    /\ BUGGY_CLAIM_UNLOCKED
    /\ pc[r] = "live" /\ proc = "alive"
    /\ LET cand == {t \in Threads : Reapable(t)} IN
         /\ cand # {}
         /\ sel' = [sel EXCEPT ![r] = cand]
    /\ pc' = [pc EXCEPT ![r] = "selecting"]
    /\ UNCHANGED <<list, claims, pending, proc, gen, tgen, dead, freeInFlight, doubleFree, uafRead>>

Detach(r) ==
    /\ pc[r] = "selecting"
    /\ list'   = Detached(sel[r])
    /\ claims' = ClaimedBy(sel[r], r)
    /\ sel'    = [sel EXCEPT ![r] = {}]
    /\ pc'     = [pc EXCEPT ![r] = "reaping"]
    /\ UNCHANGED <<pending, proc, gen, tgen, dead, freeInFlight, doubleFree, uafRead>>

(* With the lock dropped. A settled Thread needs no wait; thread_free_retired's
   on_cpu spin is defence for this freer, not a step it relies on. *)
ReapFree(r, t) ==
    /\ pc[r] = "reaping" /\ r \in claims[t]
    /\ FreeBy(r, t)
    /\ UNCHANGED <<list, sel, pending, proc, gen, tgen, dead, uafRead>>

ReapDone(r) ==
    /\ pc[r] = "reaping"
    /\ \A t \in Threads : r \notin claims[t]
    /\ pc' = [pc EXCEPT ![r] = "live"]
    /\ UNCHANGED <<list, claims, sel, pending, proc, gen, tgen, dead,
                   freeInFlight, doubleFree, uafRead>>

(* --- exec (proc_exec_replace): the execer is the only live Thread --- *)
Exec(e) ==
    /\ pc[e] = "live" /\ proc = "alive" /\ LiveList = {e}
    /\ gen < MaxGen
    /\ IF BUGGY_EXEC_NO_DRAIN
         THEN /\ dead' = dead \cup {gen}
              /\ gen'  = gen + 1
              /\ tgen' = [tgen EXCEPT ![e] = gen + 1]
              /\ UNCHANGED <<pc, list, claims>>
         ELSE /\ LET ret == {t \in Threads : list[t] = "retired"} IN
                   /\ list'   = Detached(ret)
                   /\ claims' = ClaimedBy(ret, e)
              /\ pc' = [pc EXCEPT ![e] = "execing"]
              /\ UNCHANGED <<gen, tgen, dead>>
    /\ UNCHANGED <<sel, pending, proc, freeInFlight, doubleFree, uafRead>>

(* The drain's spin: a claimed Thread is freed once its switch has settled. *)
ExecFree(e, t) ==
    /\ pc[e] = "execing" /\ e \in claims[t] /\ pc[t] = "settled"
    /\ FreeBy(e, t)
    /\ UNCHANGED <<list, sel, pending, proc, gen, tgen, dead, uafRead>>

ExecSwap(e) ==
    /\ pc[e] = "execing"
    /\ \A t \in Threads : e \notin claims[t]
    /\ dead' = dead \cup {gen}
    /\ gen'  = gen + 1
    /\ tgen' = [tgen EXCEPT ![e] = gen + 1]
    /\ pc'   = [pc EXCEPT ![e] = "live"]
    /\ UNCHANGED <<list, claims, sel, pending, proc, freeInFlight, doubleFree, uafRead>>

(* --- wait_pid in the parent --- *)
WaitPid ==
    /\ proc = "zombie"
    /\ LET take == {t \in Threads :
                      \/ list[t] = "live"
                      \/ list[t] = "retired" /\ ~BUGGY_WAITPID_SKIPS_RETIRED} IN
         /\ list'   = Detached(take)
         /\ claims' = ClaimedBy(take, Wait)
    /\ proc' = "reaped"
    /\ UNCHANGED <<pc, sel, pending, gen, tgen, dead, freeInFlight, doubleFree, uafRead>>

WaitFree(t) ==
    /\ proc = "reaped" /\ Wait \in claims[t] /\ pc[t] = "settled"
    /\ FreeBy(Wait, t)
    /\ UNCHANGED <<list, sel, pending, proc, gen, tgen, dead, uafRead>>

ProcFree ==
    /\ proc = "reaped"
    /\ \A t \in Threads : Wait \notin claims[t]
    /\ proc' = "freed"
    /\ dead' = dead \cup {gen}
    /\ UNCHANGED <<pc, list, claims, sel, pending, gen, tgen,
                   freeInFlight, doubleFree, uafRead>>

Next ==
    \/ \E s, n \in Threads : Spawn(s, n)
    \/ \E t \in Threads : ReadTid(t) \/ Exit(t) \/ Settle(t)
    \/ \E r \in Threads : Reap(r) \/ Select(r) \/ Detach(r) \/ ReapDone(r)
    \/ \E r, t \in Threads : ReapFree(r, t)
    \/ \E e \in Threads : Exec(e) \/ ExecSwap(e)
    \/ \E e, t \in Threads : ExecFree(e, t)
    \/ WaitPid
    \/ \E t \in Threads : WaitFree(t)
    \/ ProcFree

(* Fairness only on what the kernel guarantees will happen: a switch away
   completes, a spin ends, a reaper finishes what it claimed, and a parent
   that reaps runs to the end. Nothing forces a Thread to exit or to spawn. *)
Fairness ==
    /\ \A t \in Threads : WF_vars(Settle(t))
    /\ \A t \in Threads : WF_vars(ReadTid(t))
    /\ \A r \in Threads : WF_vars(Detach(r)) /\ WF_vars(ReapDone(r))
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

(* --- liveness --- *)
(* A zombie is freed: no spin of wait_pid's waits forever. *)
EventuallyFreed == (proc = "zombie") ~> (proc = "freed")
(* An exec drain completes: no retired Thread holds the execer forever. *)
ExecCompletes == \A e \in Threads : (pc[e] = "execing") ~> (pc[e] = "live")
(* A claim is always freed: no reaper strands what it detached. *)
ClaimsDischarged == \A t \in Threads : (claims[t] # {}) ~> (pc[t] = "freed")
====
