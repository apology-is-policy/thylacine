---- MODULE handle_slots ----
(***************************************************************************)
(* Thylacine handle table -- descriptor NUMBERS under a call that can fail  *)
(* after it knows its number (kernel/handle.c reservations; vault           *)
(* dec-2026-10-14-descriptor-slot-reservation).                             *)
(*                                                                         *)
(* WHY A SEPARATE MODULE. handles.tla models a Proc's handles as a SET of   *)
(* records, with no slot numbers, so it cannot express a number being       *)
(* closed and reused by a peer thread between a call's install and its      *)
(* unwind. This module models only the numbers.                             *)
(*                                                                         *)
(* THE ACTORS. One call (`c`) takes a number for a new object, does work    *)
(* that may fail, and either keeps the descriptor or undoes it. A peer      *)
(* thread of the same Proc closes numbers, allocates the lowest free one,   *)
(* and dup3s onto any number -- everything EL0 can do to the table.         *)
(*                                                                         *)
(* THE TWO SHAPES. Correct: reserve, work, then install or unreserve (Linux *)
(* get_unused_fd_flags / fd_install / put_unused_fd). Buggy                  *)
(* (BUGGY_CLOSE_BY_NUMBER): install first, and undo a failure by closing     *)
(* the number -- the shape the twelve sites had. BUGGY_DUP3_TAKES_RESERVED: *)
(* dup3 overwrites a reserved number instead of answering EBUSY.           *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*   NoForeignClose -- the call's undo never removes an object it did not    *)
(*     install (a peer's file at a reused number).                          *)
(*   FailedPublishesNothing -- a call that failed never had its object      *)
(*     visible as live at its number.                                       *)
(*   InstallFillsOwnReservation -- an install only ever fills this call's   *)
(*     own reservation (never a peer's object, never a free slot).          *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS N, BUGGY_CLOSE_BY_NUMBER, BUGGY_DUP3_TAKES_RESERVED

Slots == 0..(N-1)
States == {"free", "reserved", "mine", "peer"}

\* published: the call's object has been live at its number (set by the
\* writes of "mine", wherever they happen, never by a failure action).
VARIABLES slot, cpc, cnum, foreign_closed, published

vars == <<slot, cpc, cnum, foreign_closed, published>>

LowestFree == IF \E i \in Slots : slot[i] = "free"
              THEN CHOOSE i \in Slots : slot[i] = "free" /\ \A j \in Slots : (j < i => slot[j] # "free")
              ELSE N

TypeOk ==
    /\ slot \in [Slots -> States]
    /\ cpc \in {"idle", "held", "published", "done", "failed"}
    /\ cnum \in Slots \cup {N}
    /\ foreign_closed \in BOOLEAN
    /\ published \in BOOLEAN

Init ==
    /\ slot = [i \in Slots |-> "free"]
    /\ cpc = "idle"
    /\ cnum = N
    /\ foreign_closed = FALSE
    /\ published = FALSE

\* ---- the call ----
CTake ==
    /\ cpc = "idle"
    /\ LowestFree < N
    /\ cnum' = LowestFree
    /\ IF BUGGY_CLOSE_BY_NUMBER
          THEN /\ slot' = [slot EXCEPT ![LowestFree] = "mine"]
               /\ cpc' = "published"
               /\ published' = TRUE
          ELSE /\ slot' = [slot EXCEPT ![LowestFree] = "reserved"]
               /\ cpc' = "held"
               /\ UNCHANGED published
    /\ UNCHANGED foreign_closed

\* Correct shape: the fallible work ends in an install or an unreserve.
CInstall ==
    /\ cpc = "held"
    /\ slot[cnum] = "reserved"
    /\ slot' = [slot EXCEPT ![cnum] = "mine"]
    /\ cpc' = "done"
    /\ published' = TRUE
    /\ UNCHANGED <<cnum, foreign_closed>>

CUnreserve ==
    /\ cpc = "held"
    /\ slot[cnum] = "reserved"
    /\ slot' = [slot EXCEPT ![cnum] = "free"]
    /\ cpc' = "failed"
    /\ UNCHANGED <<cnum, foreign_closed, published>>

\* Buggy shape: the object was live from the take; a failure closes the NUMBER.
CSucceedPublished ==
    /\ cpc = "published"
    /\ cpc' = "done"
    /\ UNCHANGED <<slot, cnum, foreign_closed, published>>

CFailByNumber ==
    /\ cpc = "published"
    /\ foreign_closed' = (slot[cnum] = "peer")
    /\ slot' = [slot EXCEPT ![cnum] = "free"]
    /\ cpc' = "failed"
    /\ UNCHANGED <<cnum, published>>

\* ---- the peer thread (EL0's whole reach over the table) ----
PClose(i) ==
    /\ slot[i] \in {"mine", "peer"}  \* a reserved number answers EBADF
    /\ slot' = [slot EXCEPT ![i] = "free"]
    /\ UNCHANGED <<cpc, cnum, foreign_closed, published>>

PAlloc ==
    /\ LowestFree < N
    /\ slot' = [slot EXCEPT ![LowestFree] = "peer"]
    /\ UNCHANGED <<cpc, cnum, foreign_closed, published>>

PDup3(i) ==
    /\ BUGGY_DUP3_TAKES_RESERVED \/ slot[i] # "reserved"   \* else EBUSY
    /\ slot' = [slot EXCEPT ![i] = "peer"]
    /\ UNCHANGED <<cpc, cnum, foreign_closed, published>>

Next ==
    \/ CTake \/ CInstall \/ CUnreserve \/ CSucceedPublished \/ CFailByNumber
    \/ \E i \in Slots : PClose(i) \/ PDup3(i)
    \/ PAlloc

Spec == Init /\ [][Next]_vars

NoForeignClose == ~foreign_closed
FailedPublishesNothing == (cpc = "failed") => ~published
InstallFillsOwnReservation == (cpc = "held") => slot[cnum] = "reserved"
====
