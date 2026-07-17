---- MODULE pty_stop ----
(***************************************************************************)
(* Thylacine PTY job-control stop composed with the debug stop + death +   *)
(* the cook trigger (PTY-1, I-20 stop leg; round-1 holotype F6).           *)
(*                                                                         *)
(* `pty.tla` models the master/slave DATA path (rings + cooking + signal + *)
(* teardown) and abstracts the stop as a 4-state `stopOwners` set. This    *)
(* focused SIBLING models the stop COMPOSITION that data-path model defers  *)
(* -- the legs docs/PTY-DESIGN.md §4 flags "prosecute hard": the           *)
(* death-vs-job-stop interaction and the cook->stop linkage. The           *)
(* debug_stop.tla precedent (a focused sibling for a distinct mechanism,   *)
(* like sched_alpha.tla / loom_multishot.tla).                             *)
(*                                                                         *)
(* THE DESIGN (docs/PTY-DESIGN.md §4, round-1 F3/F4/F6): the as-built stop  *)
(* is a SINGLE `debug_stop_req` flag; the job-control stop adds an          *)
(* INDEPENDENT `job_stop_req` owner. A thread parks iff EITHER owner holds; *)
(* each resume clears ONLY its own owner. A cooked Ctrl-Z (susp char) is    *)
(* the job-stop trigger (the cook->stop linkage: a susp char produces a     *)
(* STOP, not a data byte / a signal). Death (group-terminate, the #811      *)
(* death-interruptible-sleep leg) WINS over any stop: a stopped thread does *)
(* NOT stay parked through a group-terminate.                               *)
(*                                                                         *)
(* WHAT THIS PINS                                                           *)
(*   StopCompatI39 -- a debug stop (I-39) stays in effect until ITS OWN     *)
(*     resume; a job-control resume must NEVER clear a debug stop (that     *)
(*     would run a thread the debugger stopped). The exception is death:    *)
(*     a group-terminate legitimately clears everything (grpDead).          *)
(*   DeathWinsOverJobStop -- once group-terminate is requested, the group   *)
(*     always eventually dies, EVEN from a stop (the death-wake reaches a    *)
(*     stopped thread; #811). The temporal proof that a stop cannot wedge a *)
(*     dying group.                                                         *)
(*                                                                         *)
(* THE BUGS THIS PINS                                                       *)
(*   BUGGY_DOUBLE_STOP -- a job resume clears ALL owners (incl. a live      *)
(*     debug stop) instead of only "job" -> StopCompatI39 violated (a       *)
(*     debugger-stopped thread runs). The same bug pty.tla's                *)
(*     double_stop cfg pins, here COMPOSED with the cook trigger + death.   *)
(*   BUGGY_DEATH_BLOCKED -- GroupDie is gated on the group being unstopped  *)
(*     (`stopOwners = {}`), so a stopped group's death never completes ->   *)
(*     DeathWinsOverJobStop violated (the #811 hazard: a debug/job-stopped  *)
(*     thread that a group-terminate cannot reap). The reap-vs-stop leg.    *)
(*                                                                         *)
(* CONFIGS                                                                  *)
(*   pty_stop.cfg                 clean; StopCompatI39. Expected: green.    *)
(*   pty_stop_liveness.cfg        Spec_Live; DeathWinsOverJobStop. Green.   *)
(*   pty_stop_buggy_double_stop.cfg    StopCompatI39 -- VIOLATED.          *)
(*   pty_stop_buggy_death_blocked.cfg  DeathWinsOverJobStop -- VIOLATED.   *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    INPUT,               \* susp chars available to cook (small, e.g. 1).
    BUGGY_DOUBLE_STOP,   \* BOOLEAN -- a job resume clears a debug stop too.
    BUGGY_DEATH_BLOCKED  \* BOOLEAN -- death gated on ~stopped (never completes).

ASSUME INPUT \in Nat /\ INPUT > 0
ASSUME BUGGY_DOUBLE_STOP   \in BOOLEAN
ASSUME BUGGY_DEATH_BLOCKED \in BOOLEAN

StopKinds == {"job", "debug"}

VARIABLES
    stopOwners,   \* SUBSET StopKinds -- who holds the fg group stopped.
    debugReq,     \* BOOLEAN -- a debug stop was issued (history).
    debugCleared, \* BOOLEAN -- the debug stop's OWN resume ran (history).
    gflag,        \* BOOLEAN -- group-terminate requested (death, set-once).
    grpDead,      \* BOOLEAN -- the death completed.
    inputLeft,    \* 0..INPUT -- susp chars not yet cooked.
    suspCount     \* 0..INPUT -- job stops produced by cooking a Ctrl-Z.

vars == <<stopOwners, debugReq, debugCleared, gflag, grpDead, inputLeft,
          suspCount>>

TypeOK ==
    /\ stopOwners   \in SUBSET StopKinds
    /\ debugReq     \in BOOLEAN
    /\ debugCleared \in BOOLEAN
    /\ gflag        \in BOOLEAN
    /\ grpDead      \in BOOLEAN
    /\ inputLeft    \in 0..INPUT
    /\ suspCount    \in 0..INPUT

Init ==
    /\ stopOwners   = {}
    /\ debugReq     = FALSE
    /\ debugCleared = FALSE
    /\ gflag        = FALSE
    /\ grpDead      = FALSE
    /\ inputLeft    = INPUT
    /\ suspCount    = 0

(***************************************************************************)
(* The cook->stop linkage: a cooked Ctrl-Z (susp char) parks the fg group  *)
(* -- NO data byte, NO fg-signal, it produces a STOP (adds the independent  *)
(* "job" owner). A dying group cooks no more.                              *)
(***************************************************************************)

CookSusp ==
    /\ ~gflag
    /\ inputLeft > 0
    /\ inputLeft'  = inputLeft - 1
    /\ suspCount'  = suspCount + 1
    /\ stopOwners' = stopOwners \cup {"job"}
    /\ UNCHANGED <<debugReq, debugCleared, gflag, grpDead>>

\* The debugger stop entry (I-39; CAP_DEBUG-gated in the impl). Independent
\* owner "debug".
StopDebug ==
    /\ ~gflag
    /\ "debug" \notin stopOwners
    /\ stopOwners' = stopOwners \cup {"debug"}
    /\ debugReq'   = TRUE
    /\ UNCHANGED <<debugCleared, gflag, grpDead, inputLeft, suspCount>>

\* A job-control resume (SIGCONT). Clean: clears ONLY "job". Buggy double_stop:
\* clears ALL owners (including a live debug stop) -> StopCompatI39 violated.
ResumeJob ==
    /\ "job" \in stopOwners
    /\ stopOwners' = IF BUGGY_DOUBLE_STOP THEN {} ELSE stopOwners \ {"job"}
    /\ UNCHANGED <<debugReq, debugCleared, gflag, grpDead, inputLeft, suspCount>>

\* The debug stop's OWN resume -- the only clean clearer of "debug".
ResumeDebug ==
    /\ "debug" \in stopOwners
    /\ stopOwners'   = stopOwners \ {"debug"}
    /\ debugCleared' = TRUE
    /\ UNCHANGED <<debugReq, gflag, grpDead, inputLeft, suspCount>>

\* The group-terminate request (death; set-once). The #811 group_exit_msg.
SetGflag ==
    /\ ~gflag
    /\ gflag' = TRUE
    /\ UNCHANGED <<stopOwners, debugReq, debugCleared, grpDead, inputLeft,
                   suspCount>>

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
    /\ UNCHANGED <<debugReq, debugCleared, gflag, inputLeft, suspCount>>

Next ==
    \/ CookSusp
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
\* (grpDead) legitimately clears everything (death wins).
StopCompatI39 ==
    (debugReq /\ ~debugCleared /\ ~grpDead) => ("debug" \in stopOwners)

Invariants ==
    /\ TypeOK
    /\ StopCompatI39

Fairness == WF_vars(GroupDie)

Spec_Live == Spec /\ Fairness

\* Death always eventually completes, EVEN from a stop (the death-wake reaches a
\* stopped thread; #811). GroupDie is (clean) ungated on the stop, so WF forces
\* it. BUGGY_DEATH_BLOCKED gates it on ~stopped -> a stopped group strands.
DeathWinsOverJobStop == gflag ~> grpDead

====
