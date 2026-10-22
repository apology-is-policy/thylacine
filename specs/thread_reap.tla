---------------------------- MODULE thread_reap ----------------------------
(***************************************************************************)
(* The thread lifecycle of one Proc (thread-model overhaul; spec-first by  *)
(* operator vote 2026-10-21). C3, the per-Proc thread lock: thread          *)
(* admission, the EXITING commit, the last thread out, the group claim.     *)
(* C1, reclamation: the final switch-out, the reaper, the free, exec's wait *)
(* for its dead peers, and wait_pid freeing only the Proc (ARCH 7.3.2,      *)
(* section 28 I-48). C5, exec with threads (LINEAGE L-2b): exec claims the  *)
(* Proc, which dooms every other thread, and swaps once it is alone.        *)
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
(*                                                                         *)
(* Reclamation. A committed thread runs its exit tail on its CPU, then      *)
(* switches out for the last time (it is never re-enqueued and never        *)
(* preempted). The resume path of THAT switch -- the code that runs on the  *)
(* same CPU right after cpu_switch_context, before anything else -- is the  *)
(* thread's one reaper: it holds the thread in a local (inhand), takes      *)
(* threads_lock, unlinks it (thread_count--), drops the lock, and frees the *)
(* stack and the slot. It needs no claim, because no other path frees a     *)
(* user thread. Its unlock is its last touch of the Proc, so wait_pid reads *)
(* the thread count UNDER threads_lock before it frees the Proc: once that  *)
(* read sees zero, the reaper that wrote the zero has left the lock.        *)
(*                                                                         *)
(* Exec with threads. Once exec's load into a detached space has succeeded, *)
(* it claims the Proc in one threads_lock hold: refused if the group claim  *)
(* is set or another exec holds the Proc, else p->exec_task = self (Linux's *)
(* signal->group_exec_task). Every death predicate reads "an exec by        *)
(* another thread" as death, so each other thread -- one linked later too   *)
(* -- dies at its next die-check, without touching group_exit_msg (which is *)
(* set-once: exempting the execer from it would void a later real kill).    *)
(* The execer waits, abandoning the exec only on group death, until it is   *)
(* the only thread on the list, then swaps and clears the claim. That an    *)
(* exec ever ends is the death machinery's to deliver: a doomed thread      *)
(* eventually takes its die-check (death_wake.tla's wake; here, fairness).  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,                    \* every Thread the Proc may ever hold
    Main,                       \* the Thread the Proc starts with
    Cap,                        \* PROC_THREAD_MAX (counts live Threads)
    NoThread,                   \* p->exec_task == NULL
    BUGGY_ADMIT_SPLIT,          \* the claim/cap check and the link are two holds
    BUGGY_COUNT_SPLIT,          \* the live-count read and its decrement are two holds
    BUGGY_SPAWNER_FREE,         \* a Thread admitted with no RUNNING spawner
    BUGGY_ZOMBIE_BEFORE_COMMIT, \* ZOMBIE published in a hold before the last commit
    BUGGY_FREE_ON_CPU,          \* a reaper frees an EXITING thread still on its CPU
    BUGGY_TWO_REAPERS,          \* a sweep frees dead threads beside the resume path
    BUGGY_EXEC_EARLY,           \* exec swaps once it is the only LIVE thread
    BUGGY_WAITPID_UNLOCKED,     \* wait_pid reads the thread count without the lock
    BUGGY_REAP_AT_SPAWN,        \* the interim fix: only a later spawn frees the dead
    BUGGY_CAP_COUNTS_EXITED,    \* the cap counts every linked thread, the dead too
    BUGGY_EXEC_PERTHREAD,       \* exec dooms by per-thread flags, checked and set in two holds
    BUGGY_DOOM_MISSED           \* a doomed thread's waits do not read the exec as death

ASSUME Main \in Threads /\ Cap \in Nat /\ Cap >= 1 /\ NoThread \notin Threads

(* Thread program counters:
     "unborn"    -- not created yet
     "admit_ok"  -- BUGGY_ADMIT_SPLIT: passed the checks, not linked yet
     "run"       -- linked and live (RUNNING, RUNNABLE or SLEEPING)
     "exec_ok"   -- BUGGY_EXEC_PERTHREAD: checked it is not doomed, peers not yet doomed
     "exec_wait" -- claimed the Proc for exec, waiting to be alone
     "exit_read" -- BUGGY_COUNT_SPLIT: read the live count, not committed yet
     "closing"   -- the last out, between its decision and its close
     "zclosed"   -- BUGGY_ZOMBIE_BEFORE_COMMIT: ZOMBIE published, EXITING not
     "exiting"   -- committed THREAD_EXITING, still on its CPU in the exit tail
     "dead"      -- its last switch-out is complete; in its reaper's hand
     "unlock"    -- its reaper has unlinked it and still holds threads_lock
     "gone"      -- freed                                                    *)
PCs == {"unborn", "admit_ok", "run", "exec_ok", "exec_wait", "exit_read",
        "closing", "zclosed", "exiting", "dead", "unlock", "gone"}

VARIABLES
    pc,       \* [Threads -> PCs]
    linked,   \* SUBSET Threads -- on p->threads (thread_count = its size)
    live,     \* p->live_threads
    seen,     \* [Threads -> Nat] -- BUGGY_COUNT_SPLIT's stale read
    gflag,    \* group_exit_msg claimed (set once)
    lastout,  \* SUBSET Threads -- decided they were the last out
    zombie,   \* the Proc published ZOMBIE (set once)
    reaped,   \* wait_pid freed the Proc
    late,     \* history: a Thread was linked while gflag was set
    oncpu,    \* SUBSET Threads -- committed, not yet switched out for the last time
    inhand,   \* SUBSET Threads -- switched out, held by the resume path that did it
    frees,    \* [Threads -> 0..2] -- how many times each Thread was freed
    tlheld,   \* a reaper holds threads_lock between its unlink and its unlock
    execbad,  \* history: exec swapped the space while a peer was linked or on a CPU
    exectask, \* p->exec_task: the thread whose exec holds the Proc, or NoThread
    dset      \* SUBSET Threads -- BUGGY_EXEC_PERTHREAD's per-thread die flags

vars == <<pc, linked, live, seen, gflag, lastout, zombie, reaped, late,
          oncpu, inhand, frees, tlheld, execbad, exectask, dset>>

TypeOk ==
    /\ pc       \in [Threads -> PCs]
    /\ linked   \subseteq Threads
    /\ live     \in Nat
    /\ seen     \in [Threads -> Nat]
    /\ gflag    \in BOOLEAN
    /\ lastout  \subseteq Threads
    /\ zombie   \in BOOLEAN
    /\ reaped   \in BOOLEAN
    /\ late     \in BOOLEAN
    /\ oncpu    \subseteq Threads
    /\ inhand   \subseteq Threads
    /\ frees    \in [Threads -> 0..2]
    /\ tlheld   \in BOOLEAN
    /\ execbad  \in BOOLEAN
    /\ exectask \in Threads \cup {NoThread}
    /\ dset     \subseteq Threads

Init ==
    /\ pc       = [t \in Threads |-> IF t = Main THEN "run" ELSE "unborn"]
    /\ linked   = {Main}
    /\ live     = 1
    /\ seen     = [t \in Threads |-> 0]
    /\ gflag    = FALSE
    /\ lastout  = {}
    /\ zombie   = FALSE
    /\ reaped   = FALSE
    /\ late     = FALSE
    /\ oncpu    = {}
    /\ inhand   = {}
    /\ frees    = [t \in Threads |-> 0]
    /\ tlheld   = FALSE
    /\ execbad  = FALSE
    /\ exectask = NoThread
    /\ dset     = {}

Running == {s \in linked : pc[s] = "run"}

(* What admission charges against the cap: the live count, which the       *)
(* EXITING commit lowers. BUGGY_CAP_COUNTS_EXITED charges every linked      *)
(* thread, so a dead one counts until its reaper unlinks it.                 *)
CapCount == IF BUGGY_CAP_COUNTS_EXITED THEN Cardinality(linked) ELSE live

(* An exec dooms t: the claim names another thread (proc_exec_dooms). Read  *)
(* off the Proc, so a thread linked after the claim is doomed as well.      *)
(* BUGGY_EXEC_PERTHREAD keeps a flag per thread instead.                     *)
Doomed(t) ==
    IF BUGGY_EXEC_PERTHREAD THEN t \in dset
    ELSE exectask # NoThread /\ exectask # t

(* What reaches a thread's die-check: group death, or an exec by another    *)
(* thread. BUGGY_DOOM_MISSED: the exec leg is missing from the predicate.   *)
DeathReaches(t) == gflag \/ (Doomed(t) /\ ~BUGGY_DOOM_MISSED)

(* Every C3 action takes threads_lock, so none runs while a reaper holds it. *)
C3Vars == <<pc, linked, live, seen, gflag, lastout, zombie, reaped, late>>
C1Vars == <<oncpu, inhand, frees, tlheld, execbad>>
C5Vars == <<exectask, dset>>

(***************************************************************************)
(* Admission (proc_thread_admit). The spawner s is a RUNNING thread of the *)
(* Proc. One hold: refuse if claimed, held by an exec, or at the cap, else *)
(* link + live++. A refusal changes nothing, so it is not an action. A     *)
(* thread stops counting at its EXITING commit, so one not yet freed does  *)
(* not count.                                                               *)
(***************************************************************************)
Admit(s, t) ==
    /\ ~BUGGY_ADMIT_SPLIT
    /\ ~tlheld
    /\ s \in Running
    /\ pc[t] = "unborn"
    /\ ~gflag
    /\ exectask = NoThread
    /\ CapCount < Cap
    /\ pc'     = [pc EXCEPT ![t] = "run"]
    /\ linked' = linked \cup {t}
    /\ live'   = live + 1
    /\ late'   = gflag
    /\ UNCHANGED <<seen, gflag, lastout, zombie, reaped>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

(* BUGGY_ADMIT_SPLIT: the checks in one hold, the link in another.         *)
AdmitCheck(s, t) ==
    /\ BUGGY_ADMIT_SPLIT
    /\ ~tlheld
    /\ s \in Running
    /\ pc[t] = "unborn"
    /\ ~gflag
    /\ exectask = NoThread
    /\ CapCount < Cap
    /\ pc' = [pc EXCEPT ![t] = "admit_ok"]
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

AdmitLink(t) ==
    /\ ~tlheld
    /\ pc[t] = "admit_ok"
    /\ pc'     = [pc EXCEPT ![t] = "run"]
    /\ linked' = linked \cup {t}
    /\ live'   = live + 1
    /\ late'   = (late \/ gflag)
    /\ UNCHANGED <<seen, gflag, lastout, zombie, reaped>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

(* BUGGY_SPAWNER_FREE: a creation path that is not a RUNNING thread of the *)
(* Proc (a kernel-side spawn into it). It still checks the claim and the   *)
(* cap, and refuses a ZOMBIE.                                               *)
AdmitFree(t) ==
    /\ BUGGY_SPAWNER_FREE
    /\ ~tlheld
    /\ pc[t] = "unborn"
    /\ ~gflag
    /\ ~zombie
    /\ CapCount < Cap
    /\ pc'     = [pc EXCEPT ![t] = "run"]
    /\ linked' = linked \cup {t}
    /\ live'   = live + 1
    /\ late'   = gflag
    /\ UNCHANGED <<seen, gflag, lastout, zombie, reaped>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

(***************************************************************************)
(* The claim: group_terminate's set-once CAS, under g_proc_table_lock and  *)
(* threads_lock. The death-wake walk that follows is death_wake.tla's;     *)
(* here its effect is that every live thread eventually exits (fairness).  *)
(***************************************************************************)
Claim ==
    /\ ~tlheld
    /\ ~gflag
    /\ ~zombie
    /\ gflag' = TRUE
    /\ UNCHANGED <<pc, linked, live, seen, lastout, zombie, reaped, late>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

(***************************************************************************)
(* Exit. One threads_lock hold: if another thread is live, commit EXITING  *)
(* and live--; else this is the last out -> "closing" (live stays 1, the   *)
(* commit comes at the close). A committed thread is on its CPU until its  *)
(* final switch-out. A thread may exit of its own accord (ExitDecide, no    *)
(* fairness: nothing obliges it) or because a death reached it at its       *)
(* die-check (DieDecide, fair: death_wake.tla's wake delivers it).          *)
(***************************************************************************)
ExitBody(t) ==
    /\ ~tlheld
    /\ pc[t] = "run"
    /\ IF live = 1
         THEN /\ pc'      = [pc EXCEPT ![t] = "closing"]
              /\ lastout' = lastout \cup {t}
              /\ UNCHANGED <<live, oncpu>>
         ELSE /\ pc'      = [pc EXCEPT ![t] = "exiting"]
              /\ live'    = live - 1
              /\ oncpu'   = oncpu \cup {t}
              /\ UNCHANGED lastout
    /\ UNCHANGED <<linked, seen, gflag, zombie, reaped, late>>
    /\ UNCHANGED <<inhand, frees, tlheld, execbad>>
    /\ UNCHANGED C5Vars

ExitDecide(t) ==
    /\ ~BUGGY_COUNT_SPLIT
    /\ ExitBody(t)

DieDecide(t) ==
    /\ ~BUGGY_COUNT_SPLIT
    /\ DeathReaches(t)
    /\ ExitBody(t)

(* BUGGY_COUNT_SPLIT: the read in one hold, the decision + decrement in    *)
(* another, from the stale read.                                            *)
ExitRead(t) ==
    /\ BUGGY_COUNT_SPLIT
    /\ ~tlheld
    /\ pc[t] = "run"
    /\ pc'   = [pc EXCEPT ![t] = "exit_read"]
    /\ seen' = [seen EXCEPT ![t] = live]
    /\ UNCHANGED <<linked, live, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

ExitApply(t) ==
    /\ ~tlheld
    /\ pc[t] = "exit_read"
    /\ IF seen[t] = 1
         THEN /\ pc'      = [pc EXCEPT ![t] = "closing"]
              /\ lastout' = lastout \cup {t}
              /\ UNCHANGED <<live, oncpu>>
         ELSE /\ pc'      = [pc EXCEPT ![t] = "exiting"]
              /\ live'    = live - 1
              /\ oncpu'   = oncpu \cup {t}
              /\ UNCHANGED lastout
    /\ UNCHANGED <<linked, seen, gflag, zombie, reaped, late>>
    /\ UNCHANGED <<inhand, frees, tlheld, execbad>>
    /\ UNCHANGED C5Vars

(***************************************************************************)
(* The close: g_proc_table_lock, threads_lock nested for the commit, then  *)
(* the ZOMBIE publish -- one g_proc_table_lock hold, so wait_pid (which     *)
(* holds it) never sees ZOMBIE with a thread not yet EXITING.               *)
(***************************************************************************)
CloseDone(t) ==
    /\ ~BUGGY_ZOMBIE_BEFORE_COMMIT
    /\ ~tlheld
    /\ pc[t] = "closing"
    /\ pc'     = [pc EXCEPT ![t] = "exiting"]
    /\ live'   = live - 1
    /\ zombie' = TRUE
    /\ oncpu'  = oncpu \cup {t}
    /\ UNCHANGED <<linked, seen, gflag, lastout, reaped, late>>
    /\ UNCHANGED <<inhand, frees, tlheld, execbad>>
    /\ UNCHANGED C5Vars

(* BUGGY_ZOMBIE_BEFORE_COMMIT: ZOMBIE in one hold, the commit in a later   *)
(* one.                                                                     *)
CloseZombie(t) ==
    /\ BUGGY_ZOMBIE_BEFORE_COMMIT
    /\ ~tlheld
    /\ pc[t] = "closing"
    /\ pc'     = [pc EXCEPT ![t] = "zclosed"]
    /\ zombie' = TRUE
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, reaped, late>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

CloseCommit(t) ==
    /\ ~tlheld
    /\ pc[t] = "zclosed"
    /\ pc'    = [pc EXCEPT ![t] = "exiting"]
    /\ live'  = live - 1
    /\ oncpu' = oncpu \cup {t}
    /\ UNCHANGED <<linked, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED <<inhand, frees, tlheld, execbad>>
    /\ UNCHANGED C5Vars

(***************************************************************************)
(* Reclamation (C1).                                                        *)
(***************************************************************************)

(* The final switch-out (sched() of an EXITING prev). The resume path that  *)
(* clears on_cpu keeps the thread in a local: it is that thread's reaper.   *)
(* A thread a buggy reaper already freed still switches out (its CPU does   *)
(* not know).                                                               *)
SwitchOut(t) ==
    /\ t \in oncpu
    /\ oncpu' = oncpu \ {t}
    /\ IF pc[t] = "exiting"
         THEN /\ pc'     = [pc EXCEPT ![t] = "dead"]
              /\ inhand' = inhand \cup {t}
         ELSE UNCHANGED <<pc, inhand>>
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED <<frees, tlheld, execbad>>
    /\ UNCHANGED C5Vars

(* The reaper's hold, first half: take threads_lock, fold the thread's      *)
(* accounting into the Proc, unlink it (thread_count--). No claim: it is    *)
(* the only path that frees this thread.                                    *)
ReapUnlink(t) ==
    /\ ~BUGGY_REAP_AT_SPAWN
    /\ ~tlheld
    /\ t \in inhand
    /\ pc'     = [pc EXCEPT ![t] = "unlock"]
    /\ linked' = linked \ {t}
    /\ inhand' = inhand \ {t}
    /\ frees'  = [frees EXCEPT ![t] = IF @ < 2 THEN @ + 1 ELSE 2]
    /\ tlheld' = TRUE
    /\ UNCHANGED <<live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED <<oncpu, execbad>>
    /\ UNCHANGED C5Vars

(* Second half: drop threads_lock -- the reaper's last touch of the Proc.   *)
(* The stack and slot frees that follow touch only the thread.              *)
ReapUnlock(t) ==
    /\ pc[t] = "unlock"
    /\ pc'     = [pc EXCEPT ![t] = "gone"]
    /\ tlheld' = FALSE
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED <<oncpu, inhand, frees, execbad>>
    /\ UNCHANGED C5Vars

(* BUGGY_FREE_ON_CPU: a reaper that frees on THREAD_EXITING alone, without  *)
(* waiting for the final switch-out (the #788 shape: the thread is still    *)
(* executing on the stack being freed).                                     *)
FreeEarly(t) ==
    /\ BUGGY_FREE_ON_CPU
    /\ ~tlheld
    /\ pc[t] = "exiting"
    /\ t \in linked
    /\ pc'     = [pc EXCEPT ![t] = "gone"]
    /\ linked' = linked \ {t}
    /\ frees'  = [frees EXCEPT ![t] = IF @ < 2 THEN @ + 1 ELSE 2]
    /\ UNCHANGED <<live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED <<oncpu, inhand, tlheld, execbad>>
    /\ UNCHANGED C5Vars

(* BUGGY_TWO_REAPERS: an old reaper left in place (the create-time sweep,   *)
(* exec's reap, wait_pid's walk). It claims by the unlink under the lock,   *)
(* but the resume path does not claim, so both free.                        *)
(* BUGGY_REAP_AT_SPAWN: the same sweep as the ONLY reaper (the 10-19        *)
(* interim fix, proc_reap_exited_threads at each spawn): it has no fairness, *)
(* because nothing obliges the program to spawn again.                      *)
Sweep(t) ==
    /\ BUGGY_TWO_REAPERS \/ BUGGY_REAP_AT_SPAWN
    /\ ~tlheld
    /\ pc[t] = "dead"
    /\ t \in linked
    /\ pc'     = [pc EXCEPT ![t] = "gone"]
    /\ linked' = linked \ {t}
    /\ frees'  = [frees EXCEPT ![t] = IF @ < 2 THEN @ + 1 ELSE 2]
    /\ UNCHANGED <<live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED <<oncpu, inhand, tlheld, execbad>>
    /\ UNCHANGED C5Vars

(***************************************************************************)
(* Exec (C5). The claim is one threads_lock hold, after the new image has  *)
(* loaded: refused under the group claim or another exec's, else            *)
(* exec_task = s. The swap is under g_proc_table_lock with threads_lock     *)
(* nested, and waits until s is the only thread on the list, so every peer  *)
(* has been freed and none is on a CPU; it clears the claim.                *)
(* BUGGY_EXEC_EARLY: it swaps once s is the only LIVE thread (C3's          *)
(* proc_exec_alone), while a committed peer may still run its tail.         *)
(***************************************************************************)
ExecClaim(s) ==
    /\ ~BUGGY_EXEC_PERTHREAD
    /\ ~tlheld
    /\ s \in Running
    /\ ~gflag
    /\ exectask = NoThread
    /\ exectask' = s
    /\ pc'       = [pc EXCEPT ![s] = "exec_wait"]
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED dset

(* BUGGY_EXEC_PERTHREAD (LINEAGE L-2b's first sketch, built naively): each *)
(* exec checks that its own die flag is clear in one hold, then sets every  *)
(* peer's flag in another. Two execs that pass the check before either      *)
(* dooms doom each other.                                                    *)
ExecCheck(s) ==
    /\ BUGGY_EXEC_PERTHREAD
    /\ ~tlheld
    /\ s \in Running
    /\ ~gflag
    /\ s \notin dset
    /\ pc' = [pc EXCEPT ![s] = "exec_ok"]
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

ExecDoom(s) ==
    /\ ~tlheld
    /\ pc[s] = "exec_ok"
    /\ dset'     = dset \cup (linked \ {s})
    /\ exectask' = s
    /\ pc'       = [pc EXCEPT ![s] = "exec_wait"]
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED C1Vars

(* The execer abandons the exec on a death that reaches it: group death    *)
(* (only, in the correct protocol: its own claim never dooms it). It        *)
(* returns to its die-check and dies there.                                 *)
ExecAbort(s) ==
    /\ ~tlheld
    /\ pc[s] = "exec_wait"
    /\ gflag \/ Doomed(s)
    /\ pc'       = [pc EXCEPT ![s] = "run"]
    /\ exectask' = IF exectask = s THEN NoThread ELSE exectask
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED dset

ExecSwap(s) ==
    /\ ~tlheld
    /\ pc[s] = "exec_wait"
    /\ IF BUGGY_EXEC_EARLY THEN live = 1 ELSE linked = {s}
    /\ execbad'  = (execbad \/ (oncpu \ {s}) # {} \/ linked # {s})
    /\ pc'       = [pc EXCEPT ![s] = "run"]
    /\ exectask' = IF exectask = s THEN NoThread ELSE exectask
    /\ UNCHANGED <<linked, live, seen, gflag, lastout, zombie, reaped, late>>
    /\ UNCHANGED <<oncpu, inhand, frees, tlheld>>
    /\ UNCHANGED dset

(* wait_pid, under g_proc_table_lock: once the Proc is ZOMBIE, it waits for *)
(* the thread count to reach zero -- read under the zombie's threads_lock   *)
(* -- and frees only the Proc. BUGGY_WAITPID_UNLOCKED reads the count with  *)
(* a bare load, so it can free the Proc while the last reaper still holds   *)
(* (and is about to release) the Proc's own lock.                           *)
WaitPid ==
    /\ zombie
    /\ ~reaped
    /\ linked = {}
    /\ (BUGGY_WAITPID_UNLOCKED \/ ~tlheld)
    /\ reaped' = TRUE
    /\ UNCHANGED <<pc, linked, live, seen, gflag, lastout, zombie, late>>
    /\ UNCHANGED C1Vars
    /\ UNCHANGED C5Vars

Next ==
    \/ \E s, t \in Threads : Admit(s, t) \/ AdmitCheck(s, t)
    \/ \E t \in Threads : AdmitLink(t) \/ AdmitFree(t)
    \/ Claim
    \/ \E t \in Threads : ExitDecide(t) \/ DieDecide(t) \/ ExitRead(t) \/ ExitApply(t)
    \/ \E t \in Threads : CloseDone(t) \/ CloseZombie(t) \/ CloseCommit(t)
    \/ \E t \in Threads : SwitchOut(t) \/ ReapUnlink(t) \/ ReapUnlock(t)
    \/ \E t \in Threads : FreeEarly(t) \/ Sweep(t)
    \/ \E s \in Threads : ExecClaim(s) \/ ExecCheck(s) \/ ExecDoom(s)
    \/ \E s \in Threads : ExecAbort(s) \/ ExecSwap(s)
    \/ WaitPid

(* A death that reaches a live thread eventually kills it, every split     *)
(* step completes, every close finishes, every committed thread switches    *)
(* out, every reaper finishes, an exec that can swap or must abandon does,  *)
(* and the parent eventually waits. Spawns, voluntary exits, the claim, an  *)
(* exec's claim and the buggy reapers need no fairness -- so reclamation is *)
(* shown NOT to depend on a later spawn (the 10-19 freeze), and an exec is  *)
(* shown to end only because its claim kills its peers.                     *)
Fairness ==
    /\ \A t \in Threads : WF_vars(DieDecide(t))
    /\ \A t \in Threads : WF_vars(ExitRead(t))
    /\ \A t \in Threads : WF_vars(ExitApply(t))
    /\ \A t \in Threads : WF_vars(AdmitLink(t))
    /\ \A t \in Threads : WF_vars(CloseDone(t))
    /\ \A t \in Threads : WF_vars(CloseZombie(t))
    /\ \A t \in Threads : WF_vars(CloseCommit(t))
    /\ \A t \in Threads : WF_vars(SwitchOut(t))
    /\ \A t \in Threads : WF_vars(ReapUnlink(t))
    /\ \A t \in Threads : WF_vars(ReapUnlock(t))
    /\ \A s \in Threads : WF_vars(ExecDoom(s))
    /\ \A s \in Threads : WF_vars(ExecAbort(s))
    /\ \A s \in Threads : WF_vars(ExecSwap(s))
    /\ WF_vars(WaitPid)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

Exited == {"exiting", "dead"}

(* p->live_threads counts exactly the linked threads not yet EXITING.      *)
LiveMatches ==
    live = Cardinality({s \in linked : pc[s] \notin Exited})

(* No thread is ever linked into a Proc whose group claim is set (C0's     *)
(* refusal, exact).                                                         *)
NoAdmitAfterClaim == ~late

(* The cap is never exceeded (exact, not a floor).                          *)
CapExact == CapCount <= Cap

(* An exited thread not yet freed never refuses a create: wherever a        *)
(* RUNNING spawner, an unborn thread, no claim and room by the live count   *)
(* coexist, the cap's guard admits. A joiner is woken after its peer's      *)
(* EXITING commit (the clear-child-tid wake), so this is a join followed by *)
(* a create while the joined thread still awaits its reaper.                *)
ExitedNotCharged ==
    \A s \in Running, t \in Threads :
        (pc[t] = "unborn" /\ ~gflag /\ ~tlheld /\ live < Cap) => CapCount < Cap

(* The list exceeds the cap only by threads still on their CPU or in the   *)
(* hand of the resume path that switched them out. That this is at most    *)
(* one per CPU is the kernel's KERNEL_TESTS assert in                      *)
(* thread_commit_exiting_locked; the model has no CPUs.                    *)
ListBound == Cardinality(linked) <= Cap + Cardinality(oncpu \cup inhand)

(* At most one thread ever decides it is the last out.                     *)
OneLastOut == Cardinality(lastout) <= 1

(* A Proc whose every thread has committed EXITING had a last out -- so    *)
(* someone publishes ZOMBIE (the count cannot skip zero).                   *)
NoOrphanedProc ==
    (\A s \in linked : pc[s] \in Exited) => lastout # {}

(* The drop-and-retake's premise: while the last out is closing, it is the *)
(* only live thread and nothing can join it.                                *)
CloseStable ==
    \A t \in Threads : pc[t] = "closing" =>
        /\ live = 1
        /\ \A s \in linked \ {t} : pc[s] \in Exited

(* I-24 / wait_pid's precondition: a ZOMBIE Proc has no thread that is not *)
(* EXITING (wait_pid_for extincts on one).                                  *)
ZombieImpliesAllExiting ==
    zombie => \A s \in linked : pc[s] \in Exited

(* I-48: a thread is freed only after its last switch-out.                 *)
NoFreeOnCpu == \A t \in Threads : frees[t] > 0 => t \notin oncpu

(* I-48: a thread is freed at most once.                                   *)
ExactlyOnce == \A t \in Threads : frees[t] <= 1

(* I-48: no address space is swapped while a peer is on a CPU (F-1), or    *)
(* while a peer of the old image is still on the list (C5: the new image    *)
(* starts as the execer alone).                                             *)
NoSwapWhileOnCpu == ~execbad

(* I-48: the Proc is freed only after its last thread is freed and its     *)
(* last reaper has left the Proc's lock.                                    *)
ProcFreedLast ==
    reaped => (linked = {} /\ oncpu = {} /\ inhand = {} /\ ~tlheld)

(* I-48's bound: an exited thread still on the list is either still on its *)
(* CPU or in the hand of the resume path that switched it out -- never     *)
(* parked waiting for a later spawn, exec or wait.                         *)
NoParkedDead ==
    \A s \in linked : pc[s] \in Exited => s \in oncpu \cup inhand

(* C5: at most one exec holds the Proc at a time.                          *)
OneExecer == Cardinality({s \in Threads : pc[s] \in {"exec_ok", "exec_wait"}}) <= 1

(* C5: an exec never ends the Proc. A thread its exec doomed is never the  *)
(* last out unless the group itself is dying: the execer stays live until   *)
(* it swaps or a group death reaches it.                                    *)
DoomedNeverLast ==
    \A t \in Threads : pc[t] = "closing" => (gflag \/ ~Doomed(t))

Safety ==
    /\ TypeOk
    /\ LiveMatches
    /\ NoAdmitAfterClaim
    /\ CapExact
    /\ ExitedNotCharged
    /\ ListBound
    /\ OneLastOut
    /\ NoOrphanedProc
    /\ CloseStable
    /\ ZombieImpliesAllExiting
    /\ NoFreeOnCpu
    /\ ExactlyOnce
    /\ NoSwapWhileOnCpu
    /\ ProcFreedLast
    /\ NoParkedDead
    /\ OneExecer
    /\ DoomedNeverLast

(* Once claimed, the Proc reaches ZOMBIE and is freed.                      *)
EventuallyReaped == (gflag ~> reaped)

(* Every committed thread is eventually freed and off the list, with no    *)
(* spawn needed.                                                            *)
ExitedThreadsFreed ==
    \A t \in Threads : (t \in oncpu) ~> (frees[t] = 1 /\ t \notin linked)

(* C5: every exec that claimed the Proc ends -- it swaps, or a group death *)
(* makes it abandon -- because its claim kills every other thread.          *)
ExecResolves ==
    \A s \in Threads : (pc[s] = "exec_wait") ~> (pc[s] # "exec_wait")

=============================================================================
