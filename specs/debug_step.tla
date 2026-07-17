---- MODULE debug_step ----
(***************************************************************************)
(* Thylacine Go-IDE Stage 8a-2b-2: the debugger SINGLE-STEP machine and    *)
(* its composition with the death path. Spec-first RE-ENABLED for the debug *)
(* surface (user-voted 2026-07-14). A SIBLING of debug_stop.tla, NOT an      *)
(* extension: debug_stop already proves the stop/park/resume/death handshake *)
(* (its 4 buggy cfgs are the landed pre-commit gate), and a breakpoint-fired *)
(* stop is trigger-agnostic -- it reuses that machinery. The ONLY genuine    *)
(* protocol growth in 8a-2 is the STEP: a `step` resumes a stopped thread for *)
(* EXACTLY ONE EL0 instruction, then re-parks -- so it gets its own focused   *)
(* model (the loom_multishot / loom_order precedent: sibling, not a churn of  *)
(* the audited base's cfgs).                                                 *)
(*                                                                         *)
(* WHAT THIS MODELS.                                                        *)
(*   A stopped Thread is issued `step`. It wakes to the EL0-return TAIL      *)
(*   (where the die-check runs FIRST -- death wins), arms the arm64 SS        *)
(*   machine (MDSCR.SS + SPSR.SS), executes ONE instruction at EL0, takes the *)
(*   step exception (EC 0x32) back to the TAIL, and re-parks. A group         *)
(*   termination (gflag) can race the one-instruction window; the SS EC re-   *)
(*   enters the SAME tail, so the die-check terminates the Thread (death wins  *)
(*   over a step exactly as it wins over a stop).                            *)
(*                                                                         *)
(* THE TAIL is the serialization point (shared with debug_stop): die-check    *)
(*   FIRST, then the step/park decision. A step NEVER bypasses it -- that is  *)
(*   what makes death win and what bounds the step to one instruction.        *)
(*                                                                         *)
(* THE TWO BUG CLASSES (one knob each, each a named buggy cfg).             *)
(*   BUGGY_STEP_RUNS_FREE  -- the SS EC does not return to the tail; the      *)
(*       Thread keeps executing at EL0 (win grows past 1) -> a `step` runs    *)
(*       the target free instead of one instruction (StepExactlyOne), AND a   *)
(*       runaway step never re-enters the tail to die (DeathWinsOverStep).    *)
(*       This is the SPSR.SS=0-while-MDSCR.SS=1 / missing-re-trap family.      *)
(*   BUGGY_STEP_DEATH_LOST -- the tail, handling a step re-entry, SKIPS the   *)
(*       die-check (treats a stepping Thread's checkpoint as never-fatal) ->  *)
(*       a Thread group-terminated mid-step re-parks instead of dying         *)
(*       (DeathWinsOverStep), the step-path twin of debug_stop's              *)
(*       park_before_die.                                                    *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,                 \* the target's Thread ids (>= 1)
    BUGGY_STEP_RUNS_FREE,    \* TRUE = the SS EC does not re-trap to the tail
    BUGGY_STEP_DEATH_LOST    \* TRUE = the tail skips the die-check on a step re-entry

ASSUME Cardinality(Threads) >= 1

\* Thread program counters:
\*   "stopped"  -- parked (the debugger has it stopped; the step's start point)
\*   "tail"     -- at the EL0-return tail (die-check FIRST, then step/park)
\*   "stepping" -- resumed with SS armed, about to execute ONE EL0 instruction
\*   "dead"     -- terminated at a tail die-check (noreturn; never runs at EL0)
PCs == {"stopped", "tail", "stepping", "dead"}

VARIABLES
    pc,        \* [Threads -> PCs]
    gflag,     \* group_exit_msg published (BOOLEAN, set once)
    sarmed,    \* [Threads -> BOOLEAN]  a step is in progress (armed, not yet completed)
    win,       \* [Threads -> Nat]  EL0 instructions executed in the CURRENT step window
    stepped    \* [Threads -> BOOLEAN]  this tail entry is a step re-entry (from the SS EC)

vars == <<pc, gflag, sarmed, win, stepped>>

TypeOk ==
    /\ pc \in [Threads -> PCs]
    /\ gflag \in BOOLEAN
    /\ sarmed \in [Threads -> BOOLEAN]
    /\ win \in [Threads -> Nat]
    /\ stepped \in [Threads -> BOOLEAN]

Init ==
    /\ pc = [t \in Threads |-> "stopped"]
    /\ gflag = FALSE
    /\ sarmed = [t \in Threads |-> FALSE]
    /\ win = [t \in Threads |-> 0]
    /\ stepped = [t \in Threads |-> FALSE]

(***************************************************************************)
(* ============================ DEBUGGER =============================== *)
(***************************************************************************)

(* The `step` verb: arm a step for a stopped Thread and wake it to the tail. *)
(* Resets the per-window instruction count. (Not forced -- the debugger's     *)
(* discretion; the liveness properties hold once it has happened.)           *)
RequestStep(t) ==
    /\ pc[t] = "stopped"
    /\ ~sarmed[t]
    /\ sarmed' = [sarmed EXCEPT ![t] = TRUE]
    /\ win' = [win EXCEPT ![t] = 0]
    /\ stepped' = [stepped EXCEPT ![t] = FALSE]
    /\ pc' = [pc EXCEPT ![t] = "tail"]
    /\ UNCHANGED <<gflag>>

(***************************************************************************)
(* ============================ THREAD ================================== *)
(***************************************************************************)

(* The EL0-return tail. die-check FIRST (death wins), then: if a step is armed *)
(* and the one instruction is not yet done -> arm SS + step; else re-park.     *)
(* BUGGY_STEP_DEATH_LOST models the impl hazard where the SS EC (EC 0x32) is    *)
(* handled by re-arming SS + eret DIRECTLY instead of routing through the tail: *)
(* a step re-entry (stepped[t]) under gflag AUTO-CONTINUES (re-arm, win reset)  *)
(* instead of dying -> an infinite step loop that never reaches a die-check.    *)
Tail(t) ==
    /\ pc[t] = "tail"
    /\ IF gflag /\ BUGGY_STEP_DEATH_LOST /\ stepped[t]
         THEN /\ pc' = [pc EXCEPT ![t] = "stepping"]   \* BUG: auto-continue under death; never dies
              /\ win' = [win EXCEPT ![t] = 0]
              /\ UNCHANGED <<sarmed, stepped>>
         ELSE IF gflag
                THEN /\ pc' = [pc EXCEPT ![t] = "dead"]      \* DEATH WINS (DeathWinsOverStep)
                     /\ UNCHANGED <<sarmed, win, stepped>>
                ELSE IF sarmed[t] /\ win[t] = 0
                       THEN /\ pc' = [pc EXCEPT ![t] = "stepping"]   \* arm SS, run one instruction
                            /\ UNCHANGED <<sarmed, win, stepped>>
                       ELSE /\ pc' = [pc EXCEPT ![t] = "stopped"]    \* window done (win>=1) or no step -> re-park
                            /\ sarmed' = [sarmed EXCEPT ![t] = FALSE]
                            /\ stepped' = [stepped EXCEPT ![t] = FALSE]
                            /\ UNCHANGED <<win>>
    /\ UNCHANGED <<gflag>>

(* Execute the ONE stepped instruction. CORRECT: the SS EC (EC 0x32) returns   *)
(* to the tail after exactly one instruction, marking this a step re-entry.    *)
(* BUGGY_STEP_RUNS_FREE: the SS EC never fires -> the Thread keeps executing    *)
(* (win grows), never returning to the tail.                                   *)
StepExec(t) ==
    /\ pc[t] = "stepping"
    /\ win' = [win EXCEPT ![t] = win[t] + 1]
    /\ IF BUGGY_STEP_RUNS_FREE
         THEN /\ pc' = [pc EXCEPT ![t] = "stepping"]   \* runs free -- no re-trap
              /\ UNCHANGED <<stepped>>
         ELSE /\ pc' = [pc EXCEPT ![t] = "tail"]       \* SS EC -> tail, die-check re-runs
              /\ stepped' = [stepped EXCEPT ![t] = TRUE]
    /\ UNCHANGED <<gflag, sarmed>>

(***************************************************************************)
(* ============================ DEATH PATH ============================== *)
(***************************************************************************)

(* A group termination publishes gflag once (kill / SYS_EXIT_GROUP / an LS-5   *)
(* terminate-interrupt).                                                       *)
SetGflag ==
    /\ ~gflag
    /\ gflag' = TRUE
    /\ UNCHANGED <<pc, sarmed, win, stepped>>

(* The death cascade wakes a parked Thread to the tail, where the die-check     *)
(* terminates it (debug_stop's death-wake, reused so a stopped Thread also dies *)
(* under gflag -- required for DeathWinsOverStep to quantify over ALL Threads). *)
DeathWake(t) ==
    /\ pc[t] = "stopped"
    /\ gflag
    /\ pc' = [pc EXCEPT ![t] = "tail"]
    /\ UNCHANGED <<gflag, sarmed, win, stepped>>

Next ==
    \/ \E t \in Threads : RequestStep(t)
    \/ \E t \in Threads : Tail(t)
    \/ \E t \in Threads : StepExec(t)
    \/ \E t \in Threads : DeathWake(t)
    \/ SetGflag

(* Weak fairness on the mechanical Thread progress (tail, step execute, death   *)
(* wake) so the liveness properties hold; the debugger's `step` and the kill    *)
(* (SetGflag) are discretionary (not forced), but once they happen the Thread   *)
(* must make progress.                                                         *)
Fairness ==
    /\ \A t \in Threads : WF_vars(Tail(t))
    /\ \A t \in Threads : WF_vars(StepExec(t))
    /\ \A t \in Threads : WF_vars(DeathWake(t))

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* ============================ INVARIANTS ============================= *)
(***************************************************************************)

(* StepExactlyOne: a step window executes AT MOST one EL0 instruction. The      *)
(* correct SS EC returns to the tail after one instruction (win = 1, then the   *)
(* tail re-parks); BUGGY_STEP_RUNS_FREE lets StepExec fire repeatedly (win > 1) *)
(* -- the target runs free instead of stepping.                                 *)
StepExactlyOne == \A t \in Threads : win[t] <= 1

Safety ==
    /\ TypeOk
    /\ StepExactlyOne

(* DeathWinsOverStep (liveness): once a group termination is published, every    *)
(* Thread eventually dies -- even one caught mid-step. The die-check-FIRST tail   *)
(* + the SS EC routing back through the tail + the death-wake of parked Threads  *)
(* guarantee it. BUGGY_STEP_RUNS_FREE breaks it (a runaway step never re-enters   *)
(* the tail to die); BUGGY_STEP_DEATH_LOST breaks it (the tail skips the          *)
(* die-check on a step re-entry, so the dying stepped Thread re-parks).           *)
EventuallyAllDead == gflag ~> (\A t \in Threads : pc[t] = "dead")

(* StepEventuallyReparks (liveness): a step that is NOT racing a death completes  *)
(* -- the Thread executes one instruction and returns to "stopped". (Under gflag  *)
(* the Thread dies instead, covered by EventuallyAllDead.) Witnesses that the SS  *)
(* machine advances -- the SPSR.SS/MDSCR.SS stuck-PC bug would strand it.         *)
StepEventuallyReparks ==
    \A t \in Threads :
        (pc[t] = "stepping" /\ ~gflag) ~> (pc[t] \in {"stopped", "dead"})

====
