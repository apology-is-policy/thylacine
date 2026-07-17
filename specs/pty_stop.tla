---- MODULE pty_stop ----
(***************************************************************************)
(* Thylacine PTY job-control stop OWNERSHIP composed with the debug stop + *)
(* death (PTY-1, I-20 stop leg; round-1 holotype F6, round-2 R2-F7).       *)
(*                                                                         *)
(* `pty.tla` models the master/slave DATA path (rings + cooking + signal + *)
(* teardown). This focused SIBLING models the stop-OWNERSHIP algebra the   *)
(* data-path model abstracts -- the two-owner separation (I-39) + the      *)
(* death-vs-job-stop leg §4 flags "prosecute hard". The debug_stop.tla     *)
(* precedent (a focused sibling for a distinct mechanism).                 *)
(*                                                                         *)
(* SCOPE (round-2 R2-F7 -- honest boundary). This module models exactly    *)
(* the STOP OWNERSHIP: who holds the fg group stopped, and that each       *)
(* resume clears ONLY its own owner (the F3 correctness). It does NOT model:*)
(*   - the PARK PREDICATE + the elected-9P-reader-role release (the fan-out *)
(*     of `debug_stop_req ∨ job_stop_req` through the sched.c/9p_client.c   *)
(*     sites -- round-2 R2-F2). That is the debug_stop.tla / 8c-3 park      *)
(*     domain + the PTY-1 audit, NOT re-modeled here.                       *)
(*   - the cook->stop linkage ("a cooked Ctrl-Z produces a STOP, not a      *)
(*     byte/signal") -- that is pty.tla's data-path domain (a susp char is  *)
(*     neither a data byte nor a fg-signal).                                *)
(*   - CATCHABILITY (a caught SIGTSTP does NOT stop -- round-2 R2-F3). That *)
(*     is the notes-machinery boundary (the LS-5                            *)
(*     `notes_interrupt_should_terminate_locked` gate: the default STOP     *)
(*     fires only if no handler + unmasked), prose + unit-tested, NOT here. *)
(* So `StopJob` here is the ABSTRACT "the job stop took effect" -- the      *)
(* uncaught default path -- the reachability vehicle for the composition.   *)
(*                                                                         *)
(* WHAT THIS PINS                                                           *)
(*   StopCompatI39 -- a debug stop (I-39) stays in effect until ITS OWN     *)
(*     resume; a job-control resume must NEVER clear a debug stop. Exception:*)
(*     death (a group-terminate legitimately clears everything -- grpDead;  *)
(*     the reaped Proc has no EL0 corpse to keep stopped).                  *)
(*   DeathWinsOverJobStop -- once group-terminate is requested, the group   *)
(*     always eventually dies, EVEN from a stop (#811 death-interruptible    *)
(*     sleep; the death-wake reaches a stopped thread).                     *)
(*                                                                         *)
(* THE BUGS                                                                 *)
(*   BUGGY_DOUBLE_STOP -- a job resume clears ALL owners (incl. a live      *)
(*     debug stop) -> StopCompatI39 violated (a debugger-stopped thread     *)
(*     runs). The reap-vs-stop / F3 hazard.                                 *)
(*   BUGGY_DEATH_BLOCKED -- GroupDie gated on the group being unstopped ->  *)
(*     a stopped group's death never completes -> DeathWinsOverJobStop      *)
(*     violated (the #811 hazard: a job/debug-stopped thread a              *)
(*     group-terminate cannot reap).                                       *)
(*                                                                         *)
(* CONFIGS                                                                  *)
(*   pty_stop.cfg                 clean; StopCompatI39. Expected: green.    *)
(*   pty_stop_liveness.cfg        Spec_Live; DeathWinsOverJobStop. Green.   *)
(*   pty_stop_buggy_double_stop.cfg    StopCompatI39 -- VIOLATED.          *)
(*   pty_stop_buggy_death_blocked.cfg  DeathWinsOverJobStop -- VIOLATED.   *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    BUGGY_DOUBLE_STOP,   \* BOOLEAN -- a job resume clears a debug stop too.
    BUGGY_DEATH_BLOCKED  \* BOOLEAN -- death gated on ~stopped (never completes).

ASSUME BUGGY_DOUBLE_STOP   \in BOOLEAN
ASSUME BUGGY_DEATH_BLOCKED \in BOOLEAN

StopKinds == {"job", "debug"}

VARIABLES
    stopOwners,   \* SUBSET StopKinds -- who holds the fg group stopped.
    debugReq,     \* BOOLEAN -- a debug stop was issued (history).
    debugCleared, \* BOOLEAN -- the debug stop's OWN resume ran (history).
    gflag,        \* BOOLEAN -- group-terminate requested (death, set-once).
    grpDead       \* BOOLEAN -- the death completed.

vars == <<stopOwners, debugReq, debugCleared, gflag, grpDead>>

TypeOK ==
    /\ stopOwners   \in SUBSET StopKinds
    /\ debugReq     \in BOOLEAN
    /\ debugCleared \in BOOLEAN
    /\ gflag        \in BOOLEAN
    /\ grpDead      \in BOOLEAN

Init ==
    /\ stopOwners   = {}
    /\ debugReq     = FALSE
    /\ debugCleared = FALSE
    /\ gflag        = FALSE
    /\ grpDead      = FALSE

\* The job-control stop took effect (the uncaught-default path of a cooked
\* Ctrl-Z; the catchability gate + the cook->byte exclusion are elsewhere --
\* see SCOPE). Adds the INDEPENDENT "job" owner. A dying group takes no new stop.
StopJob ==
    /\ ~gflag
    /\ "job" \notin stopOwners
    /\ stopOwners' = stopOwners \cup {"job"}
    /\ UNCHANGED <<debugReq, debugCleared, gflag, grpDead>>

\* The debugger stop entry (I-39; CAP_DEBUG-gated in the impl). Independent
\* owner "debug".
StopDebug ==
    /\ ~gflag
    /\ "debug" \notin stopOwners
    /\ stopOwners' = stopOwners \cup {"debug"}
    /\ debugReq'   = TRUE
    /\ UNCHANGED <<debugCleared, gflag, grpDead>>

\* A job-control resume (SIGCONT). Clean: clears ONLY "job". Buggy double_stop:
\* clears ALL owners (including a live debug stop) -> StopCompatI39 violated.
ResumeJob ==
    /\ "job" \in stopOwners
    /\ stopOwners' = IF BUGGY_DOUBLE_STOP THEN {} ELSE stopOwners \ {"job"}
    /\ UNCHANGED <<debugReq, debugCleared, gflag, grpDead>>

\* The debug stop's OWN resume -- the only clean clearer of "debug".
ResumeDebug ==
    /\ "debug" \in stopOwners
    /\ stopOwners'   = stopOwners \ {"debug"}
    /\ debugCleared' = TRUE
    /\ UNCHANGED <<debugReq, gflag, grpDead>>

\* The group-terminate request (death; set-once). The #811 group_exit_msg.
SetGflag ==
    /\ ~gflag
    /\ gflag' = TRUE
    /\ UNCHANGED <<stopOwners, debugReq, debugCleared, grpDead>>

\* Death WINS over the stop: once requested, the group dies and the stop is
\* cleared -- a stopped thread does NOT stay parked through a group-terminate
\* (#811 death-interruptible sleep). Buggy death_blocked: GroupDie is gated on
\* the group being unstopped, so a stopped group's death never completes.
GroupDie ==
    /\ gflag
    /\ ~grpDead
    /\ (BUGGY_DEATH_BLOCKED => stopOwners = {})
    /\ grpDead'    = TRUE
    /\ stopOwners' = {}
    /\ UNCHANGED <<debugReq, debugCleared, gflag>>

Next ==
    \/ StopJob
    \/ StopDebug
    \/ ResumeJob
    \/ ResumeDebug
    \/ SetGflag
    \/ GroupDie

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants + liveness.                                                  *)
(***************************************************************************)

\* I-39: a debug stop stays in effect until ITS OWN resume -- a job-control
\* resume must never run a debugger-stopped thread. EXCEPTION: a group-terminate
\* (grpDead) legitimately clears everything (death wins; no EL0 corpse).
StopCompatI39 ==
    (debugReq /\ ~debugCleared /\ ~grpDead) => ("debug" \in stopOwners)

Invariants ==
    /\ TypeOK
    /\ StopCompatI39

Fairness == WF_vars(GroupDie)

Spec_Live == Spec /\ Fairness

\* Death always eventually completes, EVEN from a stop (#811). GroupDie is
\* (clean) ungated on the stop, so WF forces it. BUGGY_DEATH_BLOCKED gates it on
\* ~stopped -> a stopped group strands.
DeathWinsOverJobStop == gflag ~> grpDead

====
