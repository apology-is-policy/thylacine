---- MODULE death_wake ----
(***************************************************************************)
(* Thylacine universal death-interruptible sleep (#811, ARCH 8.8.1) + the  *)
(* LS-5 interrupt-terminate widening (ARCH 8.8.2). The death-WAKE cascade   *)
(* of group termination -- the most bug-prone lineage in the tree          *)
(* (#788/#806/#807/#808/#860/#809/#811/#926). HOLOTYPE RW-2 SA-1: this is   *)
(* the machine-checked model the deep-smp-review + ASID-rollover precedent  *)
(* says an SMP-race-bearing mechanism that benefits from exploration must   *)
(* carry. (Spec-first RE-ENABLED for this surface, user-voted 2026-06-10.)  *)
(*                                                                         *)
(* WHAT THIS MODELS -- the no-lost-death-wake (I-9 generalized).            *)
(*   A Proc has N Threads. A group termination (kill / SYS_EXIT_GROUP /     *)
(*   exits-with-live-peers / an LS-5 terminate-disposition interrupt) sets  *)
(*   the per-Proc group_exit_msg (`gflag`) ONCE, then walks the Threads     *)
(*   and wakes each SLEEPING peer. Each flagged Thread dies at its          *)
(*   EL0-return die-check. The Proc reaps (ZOMBIE) when the last Thread is  *)
(*   out (I-24: exactly-once; no Thread runs at EL0 after ZOMBIE).          *)
(*                                                                         *)
(* THE CENTRAL GUARD -- REGISTER-THEN-OBSERVE under the per-Thread          *)
(* wait_lock (the Plan 9 p->rlock analog). A sleeper, AFTER registering     *)
(* (becoming findable: rendez_blocked_on set + SLEEPING), re-checks gflag   *)
(* BEFORE it actually sleeps -- BOTH under its own wait_lock. The cascade   *)
(* takes the SAME wait_lock to read rendez_blocked_on and wake. Because the *)
(* two critical sections are mutually exclusive on the wait_lock AND gflag  *)
(* is set BEFORE the walk, every Thread either (a) observes gflag in its    *)
(* reg-obs and dies without sleeping, or (b) is found SLEEPING by the walk  *)
(* and woken. No wake is lost.                                              *)
(*                                                                         *)
(* THE BUG CLASS -- BUGGY_OBSERVE_BEFORE_REGISTER. The sleeper checks gflag *)
(* BEFORE registering and OUTSIDE the wait_lock. Now the cascade can set    *)
(* gflag + walk the Thread (not yet SLEEPING -> skipped) in the window      *)
(* between the check and the registration; the Thread then sleeps with      *)
(* gflag set and is never woken -> it never reaches its checkpoint -> the   *)
(* Proc never drives its live count to zero -> never ZOMBIE. This is the    *)
(* #809-audit F1 non-reaping HANG. The buggy cfg makes NoLostDeathWake fail.*)
(*                                                                         *)
(* The wait_lock is an explicit per-Thread mutex (`wlock`) so the reg/obs   *)
(* steps and the cascade's wake genuinely interleave (or don't) -- modeling *)
(* the lock as held across reg+obs is exactly what closes the window.       *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,                       \* the Proc's Thread ids (>= 2 to race)
    BUGGY_OBSERVE_BEFORE_REGISTER  \* TRUE = the lost-wake bug; FALSE = the fix

ASSUME Cardinality(Threads) >= 1

(* Thread program counters:
     "run"      -- RUNNING (may be at EL0; the cascade target / checkpoint site)
     "acq"      -- about to acquire wait_lock to sleep (correct path)
     "obs_out"  -- observed gflag OUTSIDE the lock, about to register (buggy path)
     "reg"      -- holds wait_lock, about to register + observe (correct path)
     "sleeping" -- registered + asleep on the rendez (findable by the cascade)
     "dead"     -- terminated at the EL0-return die-check (noreturn; never EL0) *)
PCs == {"run", "acq", "obs_out", "reg", "sleeping", "dead"}

VARIABLES
    pc,        \* [Threads -> PCs]
    gflag,     \* group_exit_msg published (BOOLEAN, set ONCE)
    woken,     \* [Threads -> BOOLEAN]  the cascade woke this sleeper
    wlock,     \* [Threads -> BOOLEAN]  this Thread's wait_lock is held
    walked,    \* SUBSET Threads -- the cascade has processed (walked) these
    zombie     \* the Proc reached ZOMBIE (BOOLEAN, set ONCE by the last out)

vars == <<pc, gflag, woken, wlock, walked, zombie>>

TypeOk ==
    /\ pc     \in [Threads -> PCs]
    /\ gflag  \in BOOLEAN
    /\ woken  \in [Threads -> BOOLEAN]
    /\ wlock  \in [Threads -> BOOLEAN]
    /\ walked \subseteq Threads
    /\ zombie \in BOOLEAN

Init ==
    /\ pc     = [t \in Threads |-> "run"]
    /\ gflag  = FALSE
    /\ woken  = [t \in Threads |-> FALSE]
    /\ wlock  = [t \in Threads |-> FALSE]
    /\ walked = {}
    /\ zombie = FALSE

(***************************************************************************)
(* A RUNNING Thread decides to sleep. CORRECT path: acquire the wait_lock  *)
(* FIRST (so the register + observe below are atomic vs the cascade's      *)
(* per-Thread wake). BUGGY path: observe gflag now, OUTSIDE the lock.       *)
(***************************************************************************)
SleepBegin(t) ==
    /\ pc[t] = "run"
    /\ IF BUGGY_OBSERVE_BEFORE_REGISTER
         THEN \* observe gflag with NO lock held -- the lost-wake window opens
              IF gflag
                THEN /\ pc' = [pc EXCEPT ![t] = "dead"]   \* saw it: die, never sleep
                     /\ UNCHANGED <<gflag, woken, wlock, walked, zombie>>
                ELSE /\ pc' = [pc EXCEPT ![t] = "obs_out"] \* saw nothing: WILL register
                     /\ UNCHANGED <<gflag, woken, wlock, walked, zombie>>
         ELSE \* correct: go acquire the lock before touching gflag/registration
              /\ pc' = [pc EXCEPT ![t] = "acq"]
              /\ UNCHANGED <<gflag, woken, wlock, walked, zombie>>

(* CORRECT: acquire the wait_lock (it must be free -- the cascade isn't     *)
(* mid-wake on t).                                                          *)
AcquireLock(t) ==
    /\ pc[t] = "acq"
    /\ ~wlock[t]
    /\ wlock' = [wlock EXCEPT ![t] = TRUE]
    /\ pc'    = [pc EXCEPT ![t] = "reg"]
    /\ UNCHANGED <<gflag, woken, walked, zombie>>

(* CORRECT register-then-observe, UNDER the lock: the Thread is now         *)
(* registered (would be findable), and re-checks gflag atomically. If set,  *)
(* unwind + die (it observed the flag); else sleep. Release the lock.       *)
RegisterObserve(t) ==
    /\ pc[t] = "reg"
    /\ wlock[t]
    /\ wlock' = [wlock EXCEPT ![t] = FALSE]
    /\ IF gflag
         THEN pc' = [pc EXCEPT ![t] = "dead"]      \* register-then-OBSERVE caught it
         ELSE pc' = [pc EXCEPT ![t] = "sleeping"]   \* truly asleep, woken=FALSE
    /\ UNCHANGED <<gflag, woken, walked, zombie>>

(* BUGGY: register AFTER the out-of-lock observe. Acquire the lock + sleep. *)
(* If the cascade set gflag and walked t while t was in "obs_out", t is     *)
(* skipped by the walk and now sleeps with gflag set -> a LOST wake.        *)
RegisterBuggy(t) ==
    /\ pc[t] = "obs_out"
    /\ ~wlock[t]
    /\ pc'    = [pc EXCEPT ![t] = "sleeping"]
    /\ UNCHANGED <<gflag, woken, wlock, walked, zombie>>

(***************************************************************************)
(* The cascade. CascadeSet publishes gflag ONCE (the set-once CAS). Each    *)
(* CascadeWalk(t) processes one Thread under its wait_lock (must be free);  *)
(* a SLEEPING peer is woken. gflag is set BEFORE any walk (the RELEASE      *)
(* before the per-Thread wait_lock acquire).                                *)
(***************************************************************************)
CascadeSet ==
    /\ ~gflag
    /\ gflag' = TRUE
    /\ UNCHANGED <<pc, woken, wlock, walked, zombie>>

CascadeWalk(t) ==
    /\ gflag
    /\ t \notin walked
    /\ ~wlock[t]                       \* can't wake a Thread mid-reg-obs (Option-A pin)
    /\ walked' = walked \cup {t}
    /\ woken'  = IF pc[t] = "sleeping" THEN [woken EXCEPT ![t] = TRUE] ELSE woken
    /\ UNCHANGED <<pc, gflag, wlock, zombie>>

(* A woken sleeper resumes, re-checks gflag (set), and dies at its tail.    *)
Resume(t) ==
    /\ pc[t] = "sleeping"
    /\ woken[t]
    /\ pc' = [pc EXCEPT ![t] = "dead"]
    /\ UNCHANGED <<gflag, woken, wlock, walked, zombie>>

(* A flagged RUNNING / about-to-register Thread reaches its EL0-return      *)
(* die-check (broadcast IPI / timer tick / syscall tail) and dies. This is  *)
(* the non-sleeping leg of the cascade -- and the buggy path's escape: an   *)
(* "obs_out" Thread may die HERE instead of sleeping into a lost wake.      *)
RunCheckpoint(t) ==
    /\ pc[t] \in {"run", "acq", "obs_out"}
    /\ gflag
    /\ ~wlock[t]
    /\ pc' = [pc EXCEPT ![t] = "dead"]
    /\ UNCHANGED <<gflag, woken, wlock, walked, zombie>>

(* The last Thread out drives the Proc to ZOMBIE (set once).               *)
ProcReap ==
    /\ ~zombie
    /\ \A t \in Threads : pc[t] = "dead"
    /\ zombie' = TRUE
    /\ UNCHANGED <<pc, gflag, woken, wlock, walked>>

Next ==
    \/ \E t \in Threads : SleepBegin(t)
    \/ \E t \in Threads : AcquireLock(t)
    \/ \E t \in Threads : RegisterObserve(t)
    \/ \E t \in Threads : RegisterBuggy(t)
    \/ CascadeSet
    \/ \E t \in Threads : CascadeWalk(t)
    \/ \E t \in Threads : Resume(t)
    \/ \E t \in Threads : RunCheckpoint(t)
    \/ ProcReap

(* Weak fairness on every progress action -> the cascade + every Thread     *)
(* eventually act, so the liveness property below is meaningful.            *)
Fairness ==
    /\ \A t \in Threads : WF_vars(AcquireLock(t))
    /\ \A t \in Threads : WF_vars(RegisterObserve(t))
    /\ \A t \in Threads : WF_vars(RegisterBuggy(t))
    /\ \A t \in Threads : WF_vars(SleepBegin(t))
    /\ WF_vars(CascadeSet)
    /\ \A t \in Threads : WF_vars(CascadeWalk(t))
    /\ \A t \in Threads : WF_vars(Resume(t))
    /\ \A t \in Threads : WF_vars(RunCheckpoint(t))
    /\ WF_vars(ProcReap)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(* I-9 generalized (THE crux): once the cascade has published gflag AND     *)
(* walked every Thread, no Thread is left SLEEPING-and-not-woken -- such a  *)
(* Thread would sleep forever (the lost death-wake / #809-audit F1 hang).   *)
(* Holds with register-then-observe; the buggy cfg violates it.            *)
NoLostDeathWake ==
    (gflag /\ walked = Threads) =>
        \A t \in Threads : ~(pc[t] = "sleeping" /\ ~woken[t])

(* A sleeping Thread with gflag set is either already woken or not yet      *)
(* walked (the cascade will reach it). The sharper safety form of the above *)
(* that does not wait for walked = Threads.                                 *)
NoStuckSleeper ==
    \A t \in Threads :
        (gflag /\ pc[t] = "sleeping" /\ ~woken[t]) => (t \notin walked)

(* I-24: the Proc is ZOMBIE only when every Thread is dead -- so no Thread  *)
(* executes at EL0 (pc = "run") after the ZOMBIE transition; exactly-once   *)
(* by the set-once guard.                                                   *)
ZombieImpliesAllDead ==
    zombie => \A t \in Threads : pc[t] = "dead"

Safety ==
    /\ TypeOk
    /\ NoLostDeathWake
    /\ NoStuckSleeper
    /\ ZombieImpliesAllDead

(* Liveness (I-9 + I-24 together): once group termination starts, the Proc  *)
(* eventually reaps. The lost-wake bug breaks this (a stuck sleeper means   *)
(* never-all-dead means never ZOMBIE) -- the temporal witness of the hang.  *)
EventuallyReaps == (gflag ~> zombie)

====
