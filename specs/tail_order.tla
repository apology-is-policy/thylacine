----------------------------- MODULE tail_order -----------------------------
(***************************************************************************)
(* The EL0-return tails' leg order (DEBUG-FS-DESIGN 4.2, 5.5, 5g; the      *)
(* operator's vote of 2026-10-05, "Stop before notes").                    *)
(*                                                                         *)
(* One Thread returns to EL0 through the synchronous tail                  *)
(* (arch/arm64/vectors.S .Lel0_sync_return) or, once, through a held       *)
(* child's birth tail (userland_enter_held). Both run the die-check, then  *)
(* the stop leg (el0_return_stop_check; on the birth tail el0_birth_park,  *)
(* which also holds on the birth hold), then the notes leg                 *)
(* (kernel/notes.c notes_deliver_at_el0_return). A note whose action stops *)
(* the Thread -- an uncaught tty:susp, its job stop applied by the Thread  *)
(* itself (proc_job_stop_self) or discarded by the orphan rule -- asks for *)
(* a re-pass: the die-check and the stop leg again, then the queue afresh. *)
(* One budget of DEPTH (NOTE_QUEUE_DEPTH) bounds the re-passes and the     *)
(* discards together.                                                      *)
(*                                                                         *)
(* What is abstracted:                                                     *)
(*   - The park is one step: it ends in death if the group is dying, holds *)
(*     while either stop owner (the debugger's sflag, the job's jflag) or, *)
(*     on the birth tail, the hold is set, and proceeds otherwise.         *)
(*     debug_stop.tla verifies the park itself (register-then-observe, the *)
(*     single-wake latch, the death re-check); this model checks where the *)
(*     tails call it.                                                      *)
(*   - The queue holds the notes the tail acts on: "plain" (discarded: a   *)
(*     default-ignore or ignored note), "caught" (a handler frame; the leg *)
(*     builds one per tail and returns), "susp" (an uncaught tty:susp),    *)
(*     "term" (a terminating default: the group dies). A note left for the *)
(*     fd reader of a self-managing Proc is not the tail's.                *)
(*   - A note may be posted at any point but one: the masked window after  *)
(*     the notes leg's last decision and before the eret (pc = "eret";     *)
(*     with BUGGY_NOTES_FIRST also the late stop leg's check, but not its  *)
(*     park, which sleeps). A note posted there waits for the Thread's     *)
(*     next checkpoint, the seam every running Thread shares (the IRQ tail *)
(*     delivers no notes; vault seam-el0-irq-tail-no-notes). The IRQ tail  *)
(*     is not modelled.                                                    *)
(*                                                                         *)
(* Properties:                                                             *)
(*   MeetsQueue         -- the Thread erets with no note it could act on   *)
(*       left queued, unless it built a frame or spent the budget: a note  *)
(*       posted during a stop is met as the stop ends (4.2), not at a      *)
(*       later entry that a compute-bound Thread may never make.           *)
(*   NoEretUnderOwnStop -- a stop the notes leg applied is parked for      *)
(*       before the Thread runs at EL0 again (the re-pass).                *)
(*   TailEnds           -- the masked tail is bounded: from every tail     *)
(*       state the Thread reaches EL0, a park, or death, however fast      *)
(*       notes arrive.                                                     *)
(*                                                                         *)
(* Knobs, each a defect the tail had or nearly had:                        *)
(*   BUGGY_NOTES_FIRST  -- the order before the vote: die-check, notes,    *)
(*       stop leg. A note posted during the stop is still queued at the    *)
(*       eret (MeetsQueue). With BIRTH, the birth tail's twin.             *)
(*   BUGGY_NO_REPASS    -- the stop arm asks for no re-pass: the Thread    *)
(*       erets under the stop it applied (NoEretUnderOwnStop).             *)
(*   BUGGY_BUDGET_FIRST -- the budget break runs before the re-pass's      *)
(*       die-check and stop leg: on the budget's last pass the Thread      *)
(*       erets under the stop it applied (NoEretUnderOwnStop).             *)
(*   BUGGY_NO_BUDGET    -- neither the discards nor the re-passes count: a *)
(*       flood of notes holds the Thread in the masked tail (TailEnds).    *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    DEPTH,               \* NOTE_QUEUE_DEPTH: the ring's size and the per-tail budget
    BIRTH,               \* TRUE = the Thread starts as a held child (5f)
    BUGGY_NOTES_FIRST,
    BUGGY_NO_REPASS,
    BUGGY_BUDGET_FIRST,
    BUGGY_NO_BUDGET

Kinds == {"plain", "caught", "susp", "term"}
PCs   == {"run", "die", "stop", "parked", "notes", "eret", "dead"}
Legs  == {"first", "repass", "final"}

VARIABLES
    pc,       \* "run" = at EL0 or in a syscall body (a held child: loading)
    leg,      \* which stop leg "stop"/"parked" is: the tail's own, a re-pass's,
              \* or (BUGGY_NOTES_FIRST) the one after the notes leg
    queue,    \* the notes the tail acts on, oldest first
    passes,   \* the budget this tail has spent
    gflag,    \* group death published (group_exit_msg)
    sflag,    \* the debugger's stop (debug_stop_req)
    jflag,    \* the job stop (job_stop_req)
    hold,     \* the birth hold
    birth,    \* the next tail is the birth tail
    applied,  \* ghost: a job stop this Thread's notes leg applied, not yet continued
    framed    \* ghost: this tail built a handler frame

vars == <<pc, leg, queue, passes, gflag, sflag, jflag, hold, birth, applied, framed>>

Stopped == sflag \/ jflag \/ (birth /\ hold)

TypeOK ==
    /\ pc \in PCs
    /\ leg \in Legs
    /\ queue \in Seq(Kinds) /\ Len(queue) <= DEPTH
    /\ passes \in 0..DEPTH
    /\ gflag \in BOOLEAN /\ sflag \in BOOLEAN /\ jflag \in BOOLEAN
    /\ hold \in BOOLEAN /\ birth \in BOOLEAN
    /\ applied \in BOOLEAN /\ framed \in BOOLEAN

Init ==
    /\ pc = "run"
    /\ leg = "first"
    /\ queue = <<>>
    /\ passes = 0
    /\ gflag = FALSE /\ sflag = FALSE /\ jflag = FALSE
    /\ hold = BIRTH /\ birth = BIRTH
    /\ applied = FALSE /\ framed = FALSE

(***************************************************************************)
(* Helpers. Each assigns pc' and leg' (and Proceed and Repass passes').    *)
(***************************************************************************)
\* The notes loop's end: the eret, or the stop leg BUGGY_NOTES_FIRST moved
\* after the notes leg.
LoopExit ==
    IF BUGGY_NOTES_FIRST THEN pc' = "stop" /\ leg' = "final"
                         ELSE pc' = "eret" /\ leg' = leg

\* The budget after one more pass or discard.
Spend == IF BUGGY_NO_BUDGET THEN passes ELSE passes + 1

\* A stop leg that finds no stop goes on: from the tail's own stop leg to the
\* notes leg; from a re-pass's to the budget check, then the notes leg again;
\* from BUGGY_NOTES_FIRST's late stop leg to the eret.
Proceed ==
    CASE leg = "first"  -> pc' = "notes" /\ leg' = leg /\ passes' = passes
      [] leg = "repass" ->
            IF BUGGY_BUDGET_FIRST
            THEN pc' = "notes" /\ leg' = leg /\ passes' = passes
            ELSE /\ passes' = Spend
                 /\ IF Spend >= DEPTH THEN LoopExit ELSE pc' = "notes" /\ leg' = leg
      [] leg = "final"  -> pc' = "eret" /\ leg' = leg /\ passes' = passes

\* The stop arm's answer: the die-check and the stop leg again, then the
\* queue afresh (notes.c's loop: tail -> die -> stop -> ++passes).
Repass ==
    IF BUGGY_NO_REPASS THEN LoopExit /\ passes' = passes
    ELSE IF BUGGY_BUDGET_FIRST
         THEN /\ passes' = Spend
              /\ IF Spend >= DEPTH THEN LoopExit ELSE pc' = "die" /\ leg' = "repass"
         ELSE pc' = "die" /\ leg' = "repass" /\ passes' = passes

(***************************************************************************)
(* The Thread.                                                             *)
(***************************************************************************)
Enter ==
    /\ pc = "run"
    /\ pc' = "die" /\ leg' = "first" /\ passes' = 0 /\ framed' = FALSE
    /\ UNCHANGED <<queue, gflag, sflag, jflag, hold, birth, applied>>

DieCheck ==
    /\ pc = "die"
    /\ pc' = IF gflag THEN "dead"
             ELSE IF BUGGY_NOTES_FIRST /\ leg = "first" THEN "notes"
             ELSE "stop"
    /\ UNCHANGED <<leg, queue, passes, gflag, sflag, jflag, hold, birth, applied, framed>>

StopLeg ==
    /\ pc = "stop"
    /\ IF Stopped
       THEN /\ pc' = IF gflag THEN "dead" ELSE "parked"
            /\ UNCHANGED <<leg, passes>>
       ELSE Proceed
    /\ UNCHANGED <<queue, gflag, sflag, jflag, hold, birth, applied, framed>>

Park ==
    /\ pc = "parked"
    /\ \/ /\ gflag
          /\ pc' = "dead"
          /\ UNCHANGED <<leg, passes>>
       \/ /\ ~gflag /\ ~Stopped
          /\ Proceed
    /\ UNCHANGED <<queue, gflag, sflag, jflag, hold, birth, applied, framed>>

NotesLeg ==
    /\ pc = "notes"
    /\ IF queue = <<>>
       THEN /\ LoopExit
            /\ UNCHANGED <<queue, passes, gflag, jflag, applied, framed>>
       ELSE LET k == Head(queue) IN
            /\ queue' = Tail(queue)
            /\ CASE k = "plain" ->
                      /\ passes' = Spend
                      /\ IF Spend < DEPTH THEN pc' = "notes" /\ leg' = leg ELSE LoopExit
                      /\ UNCHANGED <<gflag, jflag, applied, framed>>
                 [] k = "caught" ->
                      /\ framed' = TRUE
                      /\ LoopExit
                      /\ UNCHANGED <<passes, gflag, jflag, applied>>
                 [] k = "term" ->
                      /\ gflag' = TRUE /\ pc' = "dead" /\ leg' = leg
                      /\ UNCHANGED <<passes, jflag, applied, framed>>
                 [] k = "susp" ->
                      /\ \/ jflag' = TRUE /\ applied' = TRUE  \* the default stop, applied
                         \/ UNCHANGED <<jflag, applied>>       \* the orphan rule discards it
                      /\ Repass
                      /\ UNCHANGED <<gflag, framed>>
    /\ UNCHANGED <<sflag, hold, birth>>

Eret ==
    /\ pc = "eret"
    /\ pc' = "run" /\ birth' = FALSE /\ leg' = "first"
    /\ UNCHANGED <<queue, passes, gflag, sflag, jflag, hold, applied, framed>>

(***************************************************************************)
(* The rest of the system: posters, the debugger, job control, death, the  *)
(* hold's release.                                                         *)
(***************************************************************************)
\* Not in the masked window after the notes leg's last decision (the seam).
PostWindow == pc \notin {"eret", "dead"} /\ ~(pc = "stop" /\ leg = "final")

Post(k) ==
    /\ PostWindow
    /\ Len(queue) < DEPTH
    /\ queue' = Append(queue, k)
    /\ UNCHANGED <<pc, leg, passes, gflag, sflag, jflag, hold, birth, applied, framed>>

DebugStop ==
    /\ ~sflag /\ pc # "dead" /\ sflag' = TRUE
    /\ UNCHANGED <<pc, leg, queue, passes, gflag, jflag, hold, birth, applied, framed>>

DebugResume ==
    /\ sflag /\ sflag' = FALSE
    /\ UNCHANGED <<pc, leg, queue, passes, gflag, jflag, hold, birth, applied, framed>>

JobStop ==
    /\ ~jflag /\ pc # "dead" /\ jflag' = TRUE
    /\ UNCHANGED <<pc, leg, queue, passes, gflag, sflag, hold, birth, applied, framed>>

JobCont ==
    /\ jflag /\ jflag' = FALSE /\ applied' = FALSE
    /\ UNCHANGED <<pc, leg, queue, passes, gflag, sflag, hold, birth, framed>>

Kill ==
    /\ ~gflag /\ gflag' = TRUE
    /\ UNCHANGED <<pc, leg, queue, passes, sflag, jflag, hold, birth, applied, framed>>

Release ==
    /\ hold /\ hold' = FALSE
    /\ UNCHANGED <<pc, leg, queue, passes, gflag, sflag, jflag, birth, applied, framed>>

ThreadStep == Enter \/ DieCheck \/ StopLeg \/ Park \/ NotesLeg \/ Eret

Next ==
    \/ ThreadStep
    \/ \E k \in Kinds : Post(k)
    \/ DebugStop \/ DebugResume \/ JobStop \/ JobCont \/ Kill \/ Release

Spec == Init /\ [][Next]_vars /\ WF_vars(ThreadStep)

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)
MeetsQueue == pc = "eret" => (queue = <<>> \/ framed \/ passes >= DEPTH)

NoEretUnderOwnStop == pc = "eret" => ~applied

TailEnds == (pc \in {"die", "stop", "notes", "eret"}) ~> (pc \in {"run", "parked", "dead"})
=============================================================================
