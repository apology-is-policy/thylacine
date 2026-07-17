---- MODULE pty ----
(***************************************************************************)
(* Thylacine PTY -- pseudoterminal master/slave atomicity (PTY-1, I-20).   *)
(*                                                                         *)
(* Models the master/slave pair + its line discipline (docs/PTY-DESIGN.md).*)
(* Spec-first RE-ENABLED for this surface (P4 -- the 7th instance of       *)
(* re-enabling point (a), after Tapestry's T-1). I-20 is literally         *)
(* "master/slave atomicity" -- the concurrency class the discipline exists *)
(* for.                                                                     *)
(*                                                                         *)
(* THE ARCHITECTURE (docs/PTY-DESIGN.md section 3): the PTY is a userspace  *)
(* ptyfs server owning the two rings + the line discipline; the KERNEL owns *)
(* the session/pgrp/controlling-terminal state + note-to-pgrp delivery +    *)
(* the generalized stop. The server's SOLE signal authority is a           *)
(* pts-scoped SYS_TTY_SIGNAL -- it can never NAME a pgrp, only report a     *)
(* signal-class event on a pts it serves; the kernel routes to that pts's  *)
(* foreground pgrp. This spec models the composed behavior + the four       *)
(* atomicity legs of I-20.                                                  *)
(*                                                                         *)
(* THE TWO DIRECTIONS                                                       *)
(*   m2s -- the master's cooked input toward the slave (the child reads).   *)
(*          A master input char is COOKED: a normal char -> a data byte in  *)
(*          m2s; an ISIG signal char -> a signal to the fg pgrp + NO data   *)
(*          byte; ECHO -> an echo byte into s2m (so the emulator sees it).  *)
(*   s2m -- the slave's output + the echoes toward the master (the emulator *)
(*          reads). Slave writes + cook echoes land here.                   *)
(* The slave reads m2s (EOF when the master closes); the master reads s2m   *)
(* (EOF when the slave closes). One blocked reader per side (single-waiter  *)
(* rendez, the pipe.tla discipline).                                        *)
(*                                                                         *)
(* THE FOUR ATOMICITY LEGS OF I-20 (docs/PTY-DESIGN.md section 6)           *)
(*   (1) No byte lost/torn/duplicated across the cook + at teardown.        *)
(*       Pinned by RingConserved (dataProduced = m2s + slaveRead: every     *)
(*       cooked data byte is either in the ring or was read -- teardown     *)
(*       must not DISCARD the ring) + SignalXorByte (each consumed input    *)
(*       char becomes EXACTLY one of {a signal, a data byte}, never both).  *)
(*   (2) A cooked signal-class char raises exactly one signal to exactly    *)
(*       the controlling terminal's FOREGROUND pgrp -- not another pgrp,    *)
(*       and (ISIG set) NOT also a data byte. Pinned by SignalToFgOnly      *)
(*       (the seam: the server never signals a non-fg pgrp) + SignalXorByte.*)
(*   (3) No wake lost between a write producing data and a blocked read on  *)
(*       the other end (I-9 per endpoint). Pinned by NoStuckSlaveReader +   *)
(*       NoStuckMasterReader (the pipe.tla shape; the generic no-wake bugs  *)
(*       are pipe.tla's cfgs -- composed, not re-modeled here).             *)
(*   (4) Teardown drains-then-EOF and master-close raises SIGHUP to the     *)
(*       session exactly once. Pinned by RingConserved (drain-not-discard)  *)
(*       + HupAtMostOnce + the monotonic close flags.                       *)
(*                                                                         *)
(* THE STOP COMPOSITION (P3 -- I-39). Ctrl-Z generalizes the audited debug  *)
(* stopped-state via a non-CAP_DEBUG entry. stopOwners \subseteq            *)
(* {"job","debug"}: the fg group runs iff stopOwners = {}. A debug stop     *)
(* (I-39) must stay in effect until ITS OWN resume -- a job-control resume  *)
(* must NOT clear a debug stop (that would run a thread the debugger        *)
(* stopped = an I-39 violation). Pinned by StopCompatI39.                   *)
(*                                                                         *)
(* THE BUGS THIS PINS (each a BUGGY_* flag, each its own cfg)               *)
(*   BUGGY_SIGNAL_ALSO_BYTE -- an ISIG signal char produces a signal AND a  *)
(*     data byte (the ldisc failed to swallow the control char). Violates   *)
(*     SignalXorByte (sigCount + dataProduced > consumed).                  *)
(*   BUGGY_LOST_TEARDOWN_BYTE -- master close DISCARDS the m2s ring instead *)
(*     of letting the slave drain it (EOF-before-drain). Violates           *)
(*     RingConserved (a cooked byte vanished: dataProduced > m2s+slaveRead).*)
(*   BUGGY_DOUBLE_STOP -- a job-control resume clears ALL stopOwners        *)
(*     (including a live debug stop) instead of only "job". Violates        *)
(*     StopCompatI39 (the group runs while the debugger stopped it).        *)
(*   BUGGY_SIGNAL_WRONG_PGRP -- the cook delivers the signal to a non-fg    *)
(*     pgrp (the seam violation -- the server escaping its pts's fg group). *)
(*     Violates SignalToFgOnly.                                             *)
(*                                                                         *)
(* CONFIGS                                                                  *)
(*   pty.cfg                       all BUGGY_* FALSE. Expected: TLC-green.   *)
(*   pty_liveness.cfg              Spec_Live; EventuallyDrained (a cooked    *)
(*                                 byte is eventually readable -- the        *)
(*                                 temporal side of no-byte-lost). Green.    *)
(*                                 (Resume-liveness is NOT unconditional --  *)
(*                                 see the NOTE at the liveness section.)    *)
(*   pty_buggy_signal_also_byte.cfg        SignalXorByte -- VIOLATED.       *)
(*   pty_buggy_lost_teardown_byte.cfg      RingConserved -- VIOLATED.       *)
(*   pty_buggy_double_stop.cfg             StopCompatI39 -- VIOLATED.       *)
(*   pty_buggy_signal_wrong_pgrp.cfg       SignalToFgOnly -- VIOLATED.      *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    CAP,                       \* ring capacity (small, e.g. 2).
    INPUT,                     \* master input chars to feed (small, e.g. 2).
    ECHO,                      \* BOOLEAN -- the ECHO termios flag (clean: TRUE).
    BUGGY_SIGNAL_ALSO_BYTE,    \* BOOLEAN -- a signal char also emits a data byte.
    BUGGY_LOST_TEARDOWN_BYTE,  \* BOOLEAN -- master close discards the ring.
    BUGGY_DOUBLE_STOP,         \* BOOLEAN -- job resume clears a debug stop too.
    BUGGY_SIGNAL_WRONG_PGRP    \* BOOLEAN -- signal delivered to a non-fg pgrp.

ASSUME CAP \in Nat /\ CAP > 0
ASSUME INPUT \in Nat /\ INPUT > 0
ASSUME ECHO \in BOOLEAN
ASSUME BUGGY_SIGNAL_ALSO_BYTE   \in BOOLEAN
ASSUME BUGGY_LOST_TEARDOWN_BYTE \in BOOLEAN
ASSUME BUGGY_DOUBLE_STOP        \in BOOLEAN
ASSUME BUGGY_SIGNAL_WRONG_PGRP  \in BOOLEAN

StopKinds == {"job", "debug"}

VARIABLES
    m2s,           \* 0..CAP -- cooked data bytes toward the slave (in ring).
    s2m,           \* 0..CAP -- output + echo bytes toward the master (in ring).
    inputLeft,     \* 0..INPUT -- master input chars not yet cooked.
    dataProduced,  \* 0..INPUT -- cumulative data bytes cooked toward the slave.
    slaveRead,     \* 0..INPUT -- cumulative data bytes the slave drained.
    sigCount,      \* 0..INPUT -- cumulative signals delivered to the fg pgrp.
    wrongPgrp,     \* BOOLEAN -- a signal was delivered to a non-fg pgrp (history).
    stopOwners,    \* SUBSET StopKinds -- who holds the fg group stopped.
    debugReq,      \* BOOLEAN -- a debug stop was issued (history).
    debugCleared,  \* BOOLEAN -- the debug stop's OWN resume ran (history).
    mClosed,       \* BOOLEAN -- the master end closed (monotonic).
    sClosed,       \* BOOLEAN -- the slave end closed (monotonic).
    hupCount,      \* 0..2 -- SIGHUP deliveries on master close (must be <= 1).
    slaveWaiting,  \* BOOLEAN -- the slave is blocked reading m2s.
    masterWaiting  \* BOOLEAN -- the master is blocked reading s2m.

vars == <<m2s, s2m, inputLeft, dataProduced, slaveRead, sigCount, wrongPgrp,
          stopOwners, debugReq, debugCleared, mClosed, sClosed, hupCount,
          slaveWaiting, masterWaiting>>

TypeOK ==
    /\ m2s          \in 0..CAP
    /\ s2m          \in 0..CAP
    /\ inputLeft    \in 0..INPUT
    /\ dataProduced \in 0..INPUT
    /\ slaveRead    \in 0..INPUT
    /\ sigCount     \in 0..(2 * INPUT)
    /\ wrongPgrp    \in BOOLEAN
    /\ stopOwners   \in SUBSET StopKinds
    /\ debugReq     \in BOOLEAN
    /\ debugCleared \in BOOLEAN
    /\ mClosed      \in BOOLEAN
    /\ sClosed      \in BOOLEAN
    /\ hupCount     \in 0..2
    /\ slaveWaiting \in BOOLEAN
    /\ masterWaiting \in BOOLEAN

Init ==
    /\ m2s          = 0
    /\ s2m          = 0
    /\ inputLeft    = INPUT
    /\ dataProduced = 0
    /\ slaveRead    = 0
    /\ sigCount     = 0
    /\ wrongPgrp    = FALSE
    /\ stopOwners   = {}
    /\ debugReq     = FALSE
    /\ debugCleared = FALSE
    /\ mClosed      = FALSE
    /\ sClosed      = FALSE
    /\ hupCount     = 0
    /\ slaveWaiting = FALSE
    /\ masterWaiting = FALSE

Consumed == INPUT - inputLeft

\* The fg group runs iff nobody holds it stopped.
GroupRunning == stopOwners = {}

(***************************************************************************)
(* The line discipline: cook one master input char.                        *)
(***************************************************************************)

\* A NORMAL char -> a data byte in m2s (+ an echo into s2m if ECHO). Needs
\* ring room; wakes the slave reader. The master is not closed (no input
\* after close).
CookData ==
    /\ ~mClosed
    /\ inputLeft > 0
    /\ m2s < CAP
    /\ (ECHO => s2m < CAP)
    /\ m2s'          = m2s + 1
    /\ dataProduced' = dataProduced + 1
    /\ inputLeft'    = inputLeft - 1
    /\ s2m'          = IF ECHO THEN s2m + 1 ELSE s2m
    /\ slaveWaiting' = FALSE                          \* wake the slave reader
    /\ masterWaiting' = IF ECHO THEN FALSE ELSE masterWaiting  \* echo wakes master
    /\ UNCHANGED <<slaveRead, sigCount, wrongPgrp, stopOwners, debugReq,
                   debugCleared, mClosed, sClosed, hupCount>>

\* An ISIG SIGNAL char -> a signal to the FG pgrp + NO data byte (the ldisc
\* swallows the control char). ECHO emits the "^C" echo into s2m (still no
\* slave data byte). The clean path: sigCount + dataProduced stays = consumed.
CookSignal ==
    /\ ~mClosed
    /\ inputLeft > 0
    /\ (ECHO => s2m < CAP)
    /\ (BUGGY_SIGNAL_ALSO_BYTE => m2s < CAP)          \* the bug needs ring room
    /\ inputLeft'    = inputLeft - 1
    /\ sigCount'     = sigCount + 1
    /\ wrongPgrp'    = (wrongPgrp \/ BUGGY_SIGNAL_WRONG_PGRP)
    /\ s2m'          = IF ECHO THEN s2m + 1 ELSE s2m
       \* BUG signal_also_byte: ALSO emit a data byte (sigCount AND dataProduced
       \* both bump -> SignalXorByte violated). Clean: no data byte.
    /\ m2s'          = IF BUGGY_SIGNAL_ALSO_BYTE THEN m2s + 1 ELSE m2s
    /\ dataProduced' = IF BUGGY_SIGNAL_ALSO_BYTE THEN dataProduced + 1
                                                 ELSE dataProduced
    /\ slaveWaiting' = IF BUGGY_SIGNAL_ALSO_BYTE THEN FALSE ELSE slaveWaiting
    /\ masterWaiting' = IF ECHO THEN FALSE ELSE masterWaiting
    /\ UNCHANGED <<slaveRead, stopOwners, debugReq, debugCleared, mClosed,
                   sClosed, hupCount>>

(***************************************************************************)
(* Reads + drains (the two endpoints).                                     *)
(***************************************************************************)

\* The slave drains one cooked byte. The fg group must be RUNNING to read
\* (a stopped group does not run) -- but a drain by the slave is modeled as
\* always allowed once data is present (the child that is reading is the fg
\* group; if it were stopped it would not be mid-read). We gate on m2s>0.
SlaveDrain ==
    /\ m2s > 0
    /\ m2s'       = m2s - 1
    /\ slaveRead' = slaveRead + 1
    /\ UNCHANGED <<s2m, inputLeft, dataProduced, sigCount, wrongPgrp,
                   stopOwners, debugReq, debugCleared, mClosed, sClosed,
                   hupCount, slaveWaiting, masterWaiting>>

\* The slave blocks reading an empty ring (no EOF yet). Single-waiter.
SlaveSleep ==
    /\ m2s = 0
    /\ ~mClosed
    /\ ~slaveWaiting
    /\ slaveWaiting' = TRUE
    /\ UNCHANGED <<m2s, s2m, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, stopOwners, debugReq, debugCleared, mClosed,
                   sClosed, hupCount, masterWaiting>>

\* The master drains one output/echo byte.
MasterDrain ==
    /\ s2m > 0
    /\ s2m'        = s2m - 1
    /\ masterWaiting' = masterWaiting
    /\ UNCHANGED <<m2s, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, stopOwners, debugReq, debugCleared, mClosed,
                   sClosed, hupCount, slaveWaiting>>

\* The master blocks reading an empty ring (slave not closed). Single-waiter.
MasterSleep ==
    /\ s2m = 0
    /\ ~sClosed
    /\ ~masterWaiting
    /\ masterWaiting' = TRUE
    /\ UNCHANGED <<m2s, s2m, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, stopOwners, debugReq, debugCleared, mClosed,
                   sClosed, hupCount, slaveWaiting>>

\* The slave writes one byte toward the master (child output). Wakes the
\* master reader.
SlaveWrite ==
    /\ ~sClosed
    /\ s2m < CAP
    /\ s2m'           = s2m + 1
    /\ masterWaiting' = FALSE
    /\ UNCHANGED <<m2s, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, stopOwners, debugReq, debugCleared, mClosed,
                   sClosed, hupCount, slaveWaiting>>

(***************************************************************************)
(* The job-control / debug stop (P3, I-39 composition).                    *)
(***************************************************************************)

StopJob ==
    /\ "job" \notin stopOwners
    /\ stopOwners' = stopOwners \cup {"job"}
    /\ UNCHANGED <<m2s, s2m, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, debugReq, debugCleared, mClosed, sClosed,
                   hupCount, slaveWaiting, masterWaiting>>

StopDebug ==
    /\ "debug" \notin stopOwners
    /\ stopOwners' = stopOwners \cup {"debug"}
    /\ debugReq'   = TRUE
    /\ UNCHANGED <<m2s, s2m, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, debugCleared, mClosed, sClosed, hupCount,
                   slaveWaiting, masterWaiting>>

\* A job-control resume (SIGCONT). Clean: removes ONLY "job". Buggy
\* double_stop: clears ALL owners (including a live debug stop) -> the group
\* runs while the debugger stopped it (StopCompatI39 violated).
ResumeJob ==
    /\ "job" \in stopOwners
    /\ stopOwners' = IF BUGGY_DOUBLE_STOP THEN {} ELSE stopOwners \ {"job"}
    /\ UNCHANGED <<m2s, s2m, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, debugReq, debugCleared, mClosed, sClosed,
                   hupCount, slaveWaiting, masterWaiting>>

\* The debug stop's OWN resume -- the only clean clearer of "debug".
ResumeDebug ==
    /\ "debug" \in stopOwners
    /\ stopOwners'   = stopOwners \ {"debug"}
    /\ debugCleared' = TRUE
    /\ UNCHANGED <<m2s, s2m, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, debugReq, mClosed, sClosed, hupCount,
                   slaveWaiting, masterWaiting>>

(***************************************************************************)
(* Teardown.                                                               *)
(***************************************************************************)

\* Master close: monotonic; raises SIGHUP to the session EXACTLY once; wakes
\* the blocked slave reader so it sees EOF. Clean: the m2s ring is PRESERVED
\* (the slave drains it, THEN sees EOF). Buggy lost_teardown_byte: DISCARD
\* the ring (m2s -> 0) -> a cooked byte vanishes (RingConserved violated).
CloseMaster ==
    /\ ~mClosed
    /\ mClosed'      = TRUE
    /\ hupCount'     = hupCount + 1
    /\ m2s'          = IF BUGGY_LOST_TEARDOWN_BYTE THEN 0 ELSE m2s
    /\ slaveWaiting' = FALSE
    /\ UNCHANGED <<s2m, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, stopOwners, debugReq, debugCleared, sClosed,
                   masterWaiting>>

\* Slave close: monotonic; wakes the blocked master reader so it sees EOF.
CloseSlave ==
    /\ ~sClosed
    /\ sClosed'       = TRUE
    /\ masterWaiting' = FALSE
    /\ UNCHANGED <<m2s, s2m, inputLeft, dataProduced, slaveRead, sigCount,
                   wrongPgrp, stopOwners, debugReq, debugCleared, mClosed,
                   hupCount, slaveWaiting>>

(***************************************************************************)
(* The next-state relation.                                                *)
(***************************************************************************)

Next ==
    \/ CookData
    \/ CookSignal
    \/ SlaveDrain
    \/ SlaveSleep
    \/ SlaveWrite
    \/ MasterDrain
    \/ MasterSleep
    \/ StopJob
    \/ StopDebug
    \/ ResumeJob
    \/ ResumeDebug
    \/ CloseMaster
    \/ CloseSlave

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants (the four legs of I-20; docs/PTY-DESIGN.md section 6).        *)
(***************************************************************************)

\* Leg (1)+(2): each consumed input char becomes EXACTLY one of {signal, data
\* byte} -- never both (the ldisc swallows a signal char) and never neither.
SignalXorByte == sigCount + dataProduced = Consumed

\* Leg (1)+(4): every cooked data byte is accounted -- in the ring or read.
\* Teardown must DRAIN, not DISCARD (a discarded ring byte breaks this).
RingConserved == dataProduced = m2s + slaveRead

\* Leg (2) -- the seam: the server never signals a pgrp other than its pts's
\* foreground group.
SignalToFgOnly == ~wrongPgrp

\* Leg (3) -- I-9 per endpoint (the pipe.tla shape; generic no-wake bugs are
\* pipe.tla's cfgs). The slave reader can read iff data OR EOF; it must never
\* be parked while readable.
SlaveCanRead == m2s > 0 \/ mClosed
MasterCanRead == s2m > 0 \/ sClosed
NoStuckSlaveReader  == ~(slaveWaiting /\ SlaveCanRead)
NoStuckMasterReader == ~(masterWaiting /\ MasterCanRead)

\* Leg (4): SIGHUP on master close is exactly-once (monotonic close).
HupAtMostOnce == hupCount <= 1

\* P3/I-39: a debug stop stays in effect until ITS OWN resume -- a job-control
\* resume must not run a thread the debugger stopped.
StopCompatI39 == (debugReq /\ ~debugCleared) => ("debug" \in stopOwners)

Invariants ==
    /\ TypeOK
    /\ SignalXorByte
    /\ RingConserved
    /\ SignalToFgOnly
    /\ NoStuckSlaveReader
    /\ NoStuckMasterReader
    /\ HupAtMostOnce
    /\ StopCompatI39

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)

Fairness ==
    /\ WF_vars(SlaveDrain)
    /\ WF_vars(MasterDrain)

Spec_Live == Spec /\ Fairness

\* A cooked data byte in the ring is eventually drained (no stranded byte) --
\* the temporal side of "no byte lost": with the reader running, every cooked
\* byte is read (input is bounded, so the ring cannot refill forever).
EventuallyDrained == (m2s > 0) ~> (m2s = 0 \/ mClosed)

\* NOTE (resume-liveness): "a stopped group is eventually resumed" is NOT an
\* unconditional property -- a job-control stop + a debug stop cover for each
\* other (TLC found the lasso {job}<->{job,debug}<->{debug} where both resumes
\* fire infinitely yet stopOwners is never empty: a user can hold Ctrl-Z, a
\* debugger can re-stop). The load-bearing stop guarantee is the SAFETY
\* StopCompatI39 (no lost debug-stop). The resume's no-lost-WAKE (the
\* generalized stop reuses the debug_rendez) is the death_wake.tla / tsleep.tla
\* register-then-observe family -- composed, not re-modeled here.

====
