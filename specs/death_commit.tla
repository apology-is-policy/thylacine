---------------------------- MODULE death_commit ----------------------------
(***************************************************************************)
(* ARCH 8.8.2 (operator vote 35, 2026-11-05): a latch's unwind commits the *)
(* death. Code: kernel/notes.c -- thread_die_pending's latch leg and        *)
(* notes_commit_latch (the commit), notes_latch_write_locked (every latch   *)
(* write), notes_tail_yield_or_die_locked (the tail's clear before it hands *)
(* a note to a handler), notes_commit_note_locked (a tail's own death);     *)
(* kernel/proc.c el0_return_die_check.                                      *)
(*                                                                         *)
(* A terminate latch stands for a note whose default ends the Proc, and is *)
(* revocable: a handler installed for the note re-reads it away under      *)
(* q->lock (notes_disposition_reread). A wait that unwinds for the latch    *)
(* answers on the footing that its thread dies -- poll's 0 -- so a revoke   *)
(* between the unwind and the tail left the thread living with an answer   *)
(* meant for a dying one (the poll false-0 class, OPEN-BUGS 2026-11-04).   *)
(* poll.tla models the poller's death as monotonic; this module is why     *)
(* that abstraction is sound for a latch.                                   *)
(*                                                                         *)
(* THE MODEL. One Proc, one note posted with its default, its latch armed. *)
(*   R -- a thread in a wait that reads the death predicate (the poll       *)
(*        loop's check, sleep's register-then-observe), then its EL0 tail:  *)
(*        the die check, then the notes leg.                                *)
(*   P -- a peer already past its die check (it was running at EL0, or its *)
(*        check came first), at its notes leg.                              *)
(*   H -- a thread installing a handler: the disposition's store, then the *)
(*        re-read under q->lock.                                            *)
(* Every step is one atomic action: a compare-and-swap on the word, or a   *)
(* whole q->lock hold. The word is <<latch, committed>>. R does not mask   *)
(* the note; masking only defers a read, and a deferred read commits       *)
(* nothing.                                                                 *)
(*                                                                         *)
(* BUGGY cfgs, each the clean model with one rule taken out:               *)
(*   BUGGY_NO_COMMIT -- the reader commits nothing (the code before the    *)
(*     vote): AnswerIsDeath.                                                *)
(*   BUGGY_PLAIN_CLEAR -- the re-read clears with a plain store, not a     *)
(*     swap refused once committed: CommitNamesNote.                        *)
(*   BUGGY_TAIL_NO_YIELD -- the notes leg hands the note to a handler      *)
(*     without the swap: OneFate.                                           *)
(*   BUGGY_UNCAUGHT_NO_COMMIT -- the notes leg dies of an uncaught note    *)
(*     without committing it, leaving it queued for a peer to hand to a    *)
(*     handler that arrived since: OneFate.                                 *)
(* The control cfg claims R never lives after an EINTR and must FAIL: the  *)
(* handler-first path is open, so the clean model is not vacuous.          *)
(***************************************************************************)
EXTENDS TLC

CONSTANTS BUGGY_NO_COMMIT, BUGGY_PLAIN_CLEAR, BUGGY_TAIL_NO_YIELD,
          BUGGY_UNCAUGHT_NO_COMMIT

ASSUME BUGGY_NO_COMMIT          \in BOOLEAN
ASSUME BUGGY_PLAIN_CLEAR        \in BOOLEAN
ASSUME BUGGY_TAIL_NO_YIELD      \in BOOLEAN
ASSUME BUGGY_UNCAUGHT_NO_COMMIT \in BOOLEAN

VARIABLES
    latch,      \* BOOLEAN -- the note's terminate latch is armed
    committed,  \* BOOLEAN -- PROC_FLAG_TERMINATE_COMMITTED
    handler,    \* BOOLEAN -- the note's disposition is a handler (H's store)
    hpc,        \* H: "store" -> "reread" -> "done"
    queued,     \* BOOLEAN -- the note is on the queue
    rpc,        \* R: "wait" -> "die_check" -> "notes" -> "dead" | "lives"
    ppc,        \* P: "notes" -> "dead" | "lives"
    answer,     \* what R's wait answered: "none" | "death" | "eintr"
    handled,    \* BOOLEAN -- some thread handed the note to the handler
    died        \* BOOLEAN -- some thread died of the note

vars == <<latch, committed, handler, hpc, queued, rpc, ppc, answer, handled, died>>

TypeOK ==
    /\ latch \in BOOLEAN /\ committed \in BOOLEAN /\ handler \in BOOLEAN
    /\ hpc \in {"store", "reread", "done"}
    /\ queued \in BOOLEAN
    /\ rpc \in {"wait", "die_check", "notes", "dead", "lives"}
    /\ ppc \in {"notes", "dead", "lives"}
    /\ answer \in {"none", "death", "eintr"}
    /\ handled \in BOOLEAN /\ died \in BOOLEAN

Init ==
    /\ latch = TRUE /\ committed = FALSE /\ handler = FALSE /\ hpc = "store"
    /\ queued = TRUE /\ rpc = "wait" /\ ppc = "notes" /\ answer = "none"
    /\ handled = FALSE /\ died = FALSE

\* The caught latch the re-read arms for a queued note a handler catches.
Caught == handler /\ hpc = "done" /\ queued

\* --------------------------------- H -----------------------------------

HStore ==
    /\ hpc = "store"
    /\ handler' = TRUE
    /\ hpc' = "reread"
    /\ UNCHANGED <<latch, committed, queued, rpc, ppc, answer, handled, died>>

\* notes_disposition_reread: the latch becomes what the queue says now, and a
\* note the handler catches keeps none -- by a swap refused once committed.
HReread ==
    /\ hpc = "reread"
    /\ latch' = IF committed /\ ~BUGGY_PLAIN_CLEAR THEN latch ELSE FALSE
    /\ hpc' = "done"
    /\ UNCHANGED <<committed, handler, queued, rpc, ppc, answer, handled, died>>

\* --------------------------------- R -----------------------------------

\* The wait reads the death predicate. Reading true for the latch commits.
RWaitDeath ==
    /\ rpc = "wait"
    /\ committed \/ latch
    /\ committed' = (committed \/ ~BUGGY_NO_COMMIT)
    /\ answer' = "death"
    /\ rpc' = "die_check"
    /\ UNCHANGED <<latch, handler, hpc, queued, ppc, handled, died>>

\* The clear came first: the caught note interrupts the wait instead (EINTR).
RWaitEintr ==
    /\ rpc = "wait"
    /\ ~committed /\ ~latch /\ Caught
    /\ answer' = "eintr"
    /\ rpc' = "die_check"
    /\ UNCHANGED <<latch, committed, handler, hpc, queued, ppc, handled, died>>

\* el0_return_die_check: a committed death ends the thread by its note.
RDieCheck ==
    /\ rpc = "die_check"
    /\ IF committed THEN rpc' = "dead" /\ died' = TRUE
                    ELSE rpc' = "notes" /\ died' = died
    /\ UNCHANGED <<latch, committed, handler, hpc, queued, ppc, answer, handled>>

\* ----------------------------- the notes leg ----------------------------
\* One q->lock hold. With no handler the default applies and the thread dies
\* of the note, which stays queued; with one, the leg first clears the latch by
\* the swap (notes_tail_yield_or_die_locked) and dies if it is refused.

Outcome ==
    IF ~queued THEN "lives"
    ELSE IF ~handler THEN "die_uncaught"
    ELSE IF BUGGY_TAIL_NO_YIELD \/ ~committed THEN "deliver"
    ELSE "die_committed"

Leg(o) ==
    /\ committed' = IF o = "die_uncaught" /\ ~BUGGY_UNCAUGHT_NO_COMMIT
                    THEN TRUE ELSE committed
    /\ latch' = CASE o = "die_uncaught" /\ ~BUGGY_UNCAUGHT_NO_COMMIT -> TRUE
                  [] o = "deliver" /\ ~BUGGY_TAIL_NO_YIELD          -> FALSE
                  [] OTHER                                         -> latch
    /\ queued'  = IF o = "deliver" THEN FALSE ELSE queued
    /\ handled' = (handled \/ o = "deliver")
    /\ died'    = (died \/ o \in {"die_uncaught", "die_committed"})

Fate(o) == IF o \in {"die_uncaught", "die_committed"} THEN "dead" ELSE "lives"

RNotes ==
    /\ rpc = "notes"
    /\ rpc' = Fate(Outcome)
    /\ Leg(Outcome)
    /\ UNCHANGED <<handler, hpc, ppc, answer>>

PNotes ==
    /\ ppc = "notes"
    /\ ppc' = Fate(Outcome)
    /\ Leg(Outcome)
    /\ UNCHANGED <<handler, hpc, rpc, answer>>

Next == HStore \/ HReread \/ RWaitDeath \/ RWaitEintr \/ RDieCheck \/ RNotes \/ PNotes

Spec == Init /\ [][Next]_vars

\* ------------------------------ invariants ------------------------------

\* A wait that answered on the footing of death is never followed by life.
AnswerIsDeath == ~(answer = "death" /\ rpc = "lives")

\* No note is both handed to a handler and died of (I-19).
OneFate == ~(handled /\ died)

\* A committed death names its note: the one latch it left stays armed
\* (notes_death_committed stops the kernel on a commit that names nothing).
CommitNamesNote == committed => latch

\* The control's claim, which the clean model must break.
NoEintrLife == ~(answer = "eintr" /\ rpc = "lives")

=============================================================================
