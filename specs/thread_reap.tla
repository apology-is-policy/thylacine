---------------------------- MODULE thread_reap ----------------------------
(***************************************************************************)
(* The thread lifecycle of one Proc (thread-model overhaul; spec-first by  *)
(* operator vote 2026-10-21). This first part models C3, the per-Proc       *)
(* thread lock: thread admission, the EXITING commit, the last thread out,  *)
(* the group claim and wait_pid. C1 extends it with the final switch-out,   *)
(* the reaper and the free.                                                 *)
(*                                                                         *)
(* Every action below is ONE critical section. Admission and the non-last  *)
(* EXITING commit hold p->threads_lock; the claim (group_terminate's CAS)   *)
(* holds g_proc_table_lock AND p->threads_lock; the last-out's close holds  *)
(* g_proc_table_lock with p->threads_lock nested. A buggy cfg splits one    *)
(* critical section into two steps with the lock free between them.        *)
(*                                                                         *)
(* The last thread out decides it is last under threads_lock, then DROPS    *)
(* every lock to close the Proc's handles (that sleeps), and only then      *)
(* takes g_proc_table_lock to commit EXITING and publish ZOMBIE. The pc     *)
(* "closing" is that window. The decision stays true across it because a    *)
(* thread is admitted only by a RUNNING thread of the same Proc, and the    *)
(* only one left is the exiter (CloseStable).                               *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,                    \* every Thread the Proc may ever hold
    Main,                       \* the Thread the Proc starts with
    Cap,                        \* PROC_THREAD_MAX (counts linked Threads)
    BUGGY_ADMIT_SPLIT,          \* the claim/cap check and the link are two holds
    BUGGY_COUNT_SPLIT,          \* the live-count read and its decrement are two holds
    BUGGY_SPAWNER_FREE,         \* a Thread admitted with no RUNNING spawner
    BUGGY_ZOMBIE_BEFORE_COMMIT  \* ZOMBIE published in a hold before the last commit

ASSUME Main \in Threads /\ Cap \in Nat /\ Cap >= 1

(* Thread program counters:
     "unborn"    -- not created yet
     "admit_ok"  -- BUGGY_ADMIT_SPLIT: passed the checks, not linked yet
     "run"       -- linked and live (RUNNING, RUNNABLE or SLEEPING)
     "exit_read" -- BUGGY_COUNT_SPLIT: read the live count, not committed yet
     "closing"   -- the last out, between its decision and its close
     "zclosed"   -- BUGGY_ZOMBIE_BEFORE_COMMIT: ZOMBIE published, EXITING not
     "exiting"   -- committed THREAD_EXITING (never runs at EL0 again)          *)
PCs == {"unborn", "admit_ok", "run", "exit_read", "closing", "zclosed", "exiting"}

VARIABLES
    pc,       \* [Threads -> PCs]
    linked,   \* SUBSET Threads -- on p->threads
    live,     \* p->live_threads
    seen,     \* [Threads -> Nat] -- BUGGY_COUNT_SPLIT's stale read
    gflag,    \* group_exit_msg claimed (set once)
    lastout,  \* SUBSET Threads -- decided they were the last out
    zombie,   \* the Proc published ZOMBIE (set once)
    reaped,   \* wait_pid reaped the Proc
    late      \* history: a Thread was linked while gflag was set

vars == <<pc, linked, live, seen, gflag, lastout, zombie, reaped, late>>

TypeOk ==
    /\ pc      \in [Threads -> PCs]
    /\ linked  \subseteq Threads
    /\ live    \in Nat
    /\ seen    \in [Threads -> Nat]
    /\ gflag   \in BOOLEAN
    /\ lastout \subseteq Threads
    /\ zombie  \in BOOLEAN
    /\ reaped  \in BOOLEAN
    /\ late    \in BOOLEAN

Init ==
    /\ pc      = [t \in Threads |-> IF t = Main THEN "run" ELSE "unborn"]
    /\ linked  = {Main}
    /\ live    = 1
    /\ seen    = [t \in Threads |-> 0]
    /\ gflag   = FALSE
    /\ lastout = {}
    /\ zombie  = FALSE
    /\ reaped  = FALSE
    /\ late    = FALSE

Running == {s \in linked : pc[s] = "run"}

(***************************************************************************)
(* Admission (proc_thread_admit). The spawner s is a RUNNING thread of the *)
(* Proc. One hold: refuse if claimed or at the cap, else link + live++.    *)
(* A refusal changes nothing, so it is not an action.                      *)
(***************************************************************************)
Admit(s, t) ==
    /\ ~BUGGY_ADMIT_SPLIT
    /\ s \in Running
    /\ pc[t] = "unborn"
    /\ ~gflag
    /\ Cardinality(linked) < Cap
    /\ pc'     = [pc EXCEPT ![t] = "run"]
    /\ linked' = linked \cup {t}
    /\ live'   = live + 1
    /\ late'   = gflag
    /\ UNCHANGED <<seen, gflag, lastout, zombie, reaped>>

(* BUGGY_ADMIT_SPLIT: the checks in one hold, the link in another.         *)
AdmitCheck(s, t) ==
    /\ BUGGY_ADMIT_SPLIT
    /\ s \in Running
    /\ pc[t] = "unborn"
    /\ ~gflag
    /\ Cardinality(linked) < Cap
    /\ pc' = [pc EXCEPT ![t] = "admit_ok"]
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>

AdmitLink(t) ==
    /\ pc[t] = "admit_ok"
    /\ pc'     = [pc EXCEPT ![t] = "run"]
    /\ linked' = linked \cup {t}
    /\ live'   = live + 1
    /\ late'   = (late \/ gflag)
    /\ UNCHANGED <<seen, gflag, lastout, zombie, reaped>>

(* BUGGY_SPAWNER_FREE: a creation path that is not a RUNNING thread of the *)
(* Proc (a kernel-side spawn into it). It still checks the claim and the   *)
(* cap, and refuses a ZOMBIE.                                               *)
AdmitFree(t) ==
    /\ BUGGY_SPAWNER_FREE
    /\ pc[t] = "unborn"
    /\ ~gflag
    /\ ~zombie
    /\ Cardinality(linked) < Cap
    /\ pc'     = [pc EXCEPT ![t] = "run"]
    /\ linked' = linked \cup {t}
    /\ live'   = live + 1
    /\ late'   = gflag
    /\ UNCHANGED <<seen, gflag, lastout, zombie, reaped>>

(***************************************************************************)
(* The claim: group_terminate's set-once CAS, under g_proc_table_lock and  *)
(* threads_lock. The death-wake walk that follows is death_wake.tla's;     *)
(* here its effect is that every live thread eventually exits (fairness).  *)
(***************************************************************************)
Claim ==
    /\ ~gflag
    /\ ~zombie
    /\ gflag' = TRUE
    /\ UNCHANGED <<pc, linked, live, seen, lastout, zombie, reaped, late>>

(***************************************************************************)
(* Exit. One threads_lock hold: if another thread is live, commit EXITING  *)
(* and live--; else this is the last out -> "closing" (live stays 1, the   *)
(* commit comes at the close).                                              *)
(***************************************************************************)
ExitDecide(t) ==
    /\ ~BUGGY_COUNT_SPLIT
    /\ pc[t] = "run"
    /\ IF live = 1
         THEN /\ pc'      = [pc EXCEPT ![t] = "closing"]
              /\ lastout' = lastout \cup {t}
              /\ UNCHANGED live
         ELSE /\ pc'      = [pc EXCEPT ![t] = "exiting"]
              /\ live'    = live - 1
              /\ UNCHANGED lastout
    /\ UNCHANGED <<linked, seen, gflag, zombie, reaped, late>>

(* BUGGY_COUNT_SPLIT: the read in one hold, the decision + decrement in    *)
(* another, from the stale read.                                            *)
ExitRead(t) ==
    /\ BUGGY_COUNT_SPLIT
    /\ pc[t] = "run"
    /\ pc'   = [pc EXCEPT ![t] = "exit_read"]
    /\ seen' = [seen EXCEPT ![t] = live]
    /\ UNCHANGED <<linked, live, gflag, lastout, zombie, reaped, late>>

ExitApply(t) ==
    /\ pc[t] = "exit_read"
    /\ IF seen[t] = 1
         THEN /\ pc'      = [pc EXCEPT ![t] = "closing"]
              /\ lastout' = lastout \cup {t}
              /\ UNCHANGED live
         ELSE /\ pc'      = [pc EXCEPT ![t] = "exiting"]
              /\ live'    = live - 1
              /\ UNCHANGED lastout
    /\ UNCHANGED <<linked, seen, gflag, zombie, reaped, late>>

(***************************************************************************)
(* The close: g_proc_table_lock, threads_lock nested for the commit, then  *)
(* the ZOMBIE publish -- one g_proc_table_lock hold, so wait_pid (which     *)
(* holds it) never sees ZOMBIE with a thread not yet EXITING.               *)
(***************************************************************************)
CloseDone(t) ==
    /\ ~BUGGY_ZOMBIE_BEFORE_COMMIT
    /\ pc[t] = "closing"
    /\ pc'     = [pc EXCEPT ![t] = "exiting"]
    /\ live'   = live - 1
    /\ zombie' = TRUE
    /\ UNCHANGED <<linked, seen, gflag, lastout, reaped, late>>

(* BUGGY_ZOMBIE_BEFORE_COMMIT: ZOMBIE in one hold, the commit in a later   *)
(* one.                                                                     *)
CloseZombie(t) ==
    /\ BUGGY_ZOMBIE_BEFORE_COMMIT
    /\ pc[t] = "closing"
    /\ pc'     = [pc EXCEPT ![t] = "zclosed"]
    /\ zombie' = TRUE
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, reaped, late>>

CloseCommit(t) ==
    /\ pc[t] = "zclosed"
    /\ pc'   = [pc EXCEPT ![t] = "exiting"]
    /\ live' = live - 1
    /\ UNCHANGED <<linked, seen, gflag, lastout, zombie, reaped, late>>

(* wait_pid, under g_proc_table_lock: reaps a ZOMBIE.                       *)
WaitPid ==
    /\ zombie
    /\ ~reaped
    /\ reaped' = TRUE
    /\ UNCHANGED <<pc, linked, live, seen, gflag, lastout, zombie, late>>

Next ==
    \/ \E s, t \in Threads : Admit(s, t) \/ AdmitCheck(s, t)
    \/ \E t \in Threads : AdmitLink(t) \/ AdmitFree(t)
    \/ Claim
    \/ \E t \in Threads : ExitDecide(t) \/ ExitRead(t) \/ ExitApply(t)
    \/ \E t \in Threads : CloseDone(t) \/ CloseZombie(t) \/ CloseCommit(t)
    \/ WaitPid

(* Every live thread eventually exits, every split step completes, every   *)
(* close finishes and the parent eventually waits. Spawns and the claim     *)
(* need no fairness.                                                        *)
Fairness ==
    /\ \A t \in Threads : WF_vars(ExitDecide(t))
    /\ \A t \in Threads : WF_vars(ExitRead(t))
    /\ \A t \in Threads : WF_vars(ExitApply(t))
    /\ \A t \in Threads : WF_vars(AdmitLink(t))
    /\ \A t \in Threads : WF_vars(CloseDone(t))
    /\ \A t \in Threads : WF_vars(CloseZombie(t))
    /\ \A t \in Threads : WF_vars(CloseCommit(t))
    /\ WF_vars(WaitPid)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(* p->live_threads counts exactly the linked threads not yet EXITING.      *)
LiveMatches ==
    live = Cardinality({s \in linked : pc[s] \notin {"exiting"}})

(* No thread is ever linked into a Proc whose group claim is set (C0's     *)
(* refusal, exact).                                                         *)
NoAdmitAfterClaim == ~late

(* The cap counts every linked thread and is never exceeded (exact, not a  *)
(* floor).                                                                  *)
CapExact == Cardinality(linked) <= Cap

(* At most one thread ever decides it is the last out.                     *)
OneLastOut == Cardinality(lastout) <= 1

(* A Proc whose every thread has committed EXITING had a last out -- so    *)
(* someone publishes ZOMBIE (the count cannot skip zero).                   *)
NoOrphanedProc ==
    (\A s \in linked : pc[s] = "exiting") => lastout # {}

(* The drop-and-retake's premise: while the last out is closing, it is the *)
(* only live thread and nothing can join it.                                *)
CloseStable ==
    \A t \in Threads : pc[t] = "closing" =>
        /\ live = 1
        /\ \A s \in linked \ {t} : pc[s] = "exiting"

(* I-24 / wait_pid's precondition: a ZOMBIE Proc has no thread that is not *)
(* EXITING (wait_pid_for extincts on one).                                  *)
ZombieImpliesAllExiting ==
    zombie => \A s \in linked : pc[s] = "exiting"

Safety ==
    /\ TypeOk
    /\ LiveMatches
    /\ NoAdmitAfterClaim
    /\ CapExact
    /\ OneLastOut
    /\ NoOrphanedProc
    /\ CloseStable
    /\ ZombieImpliesAllExiting

(* Once claimed, the Proc reaches ZOMBIE and is reaped.                    *)
EventuallyReaped == (gflag ~> reaped)

=============================================================================
