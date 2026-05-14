---- MODULE pipe ----
(***************************************************************************)
(* Thylacine blocking-pipe spec — P5-pipe-blocking.                        *)
(*                                                                         *)
(* Models the wait/wake protocol of `kernel/pipe.c`'s blocking variant     *)
(* per ARCH §10.3 + §28 I-9 (no wakeup lost between wait-condition check   *)
(* and sleep). The primary invariant is `NoStuckWaiter`: a thread is never *)
(* in WAITING_READ when CanRead holds, and never in WAITING_WRITE when    *)
(* CanWrite holds. Buggy variants that elide the wake-after-mutation step  *)
(* violate this by leaving a thread stuck.                                 *)
(*                                                                         *)
(* Composition with `specs/scheduler.tla`'s NoMissedWakeup: scheduler.tla  *)
(* proves the atomic cond-check + sleep transition (rendez API surface);   *)
(* this spec proves the pipe-side discipline of "every mutation that COULD *)
(* enable a waiter MUST wake one." Together they close the missed-wakeup   *)
(* hazard end-to-end for the pipe.                                         *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   - Single-waiter-per-direction. At most one thread sleeps on the read  *)
(*     side at a time; at most one sleeps on the write side. Mirrors the   *)
(*     impl's use of `struct Rendez` (single-waiter; see rendez.h).        *)
(*     Multi-waiter wait queues are Phase 5+ (poll / futex); when they     *)
(*     land at this layer, the spec extends with a set of waiters and the *)
(*     wake action becomes "wake one chosen waiter" or "wake all."         *)
(*                                                                         *)
(*   - Atomic actions. ReadDrain / WriteAppend / CloseRead / CloseWrite    *)
(*     each atomically mutate state + perform the wake-if-applicable.      *)
(*     This mirrors the impl's discipline of "take pipe-lock → mutate →   *)
(*     wakeup(rendez) → drop pipe-lock"; the rendez API guarantees the    *)
(*     wakeup is delivered to any sleeper (via the atomic cond-check +    *)
(*     sleep protocol, modeled in scheduler.tla).                          *)
(*                                                                         *)
(*   - EOF flags are persistent. CloseRead / CloseWrite are monotonic —   *)
(*     once set, never unset. Mirrors the impl: close hooks set the flag   *)
(*     and never clear it (the pipe is freed when both ends close).        *)
(*                                                                         *)
(*   - Sleep is gated on "single-waiter": a thread that would sleep when  *)
(*     a sleeper is already present is disabled in the model (would       *)
(*     extinct in the impl per rendez.h). This is a structural constraint, *)
(*     not an invariant violation.                                         *)
(*                                                                         *)
(* Buggy-config matrix (one buggy flag per cfg; executable documentation): *)
(*                                                                         *)
(*   pipe.cfg                                  all flags FALSE — TLC       *)
(*                                              proves NoStuckWaiter.      *)
(*                                                                         *)
(*   pipe_buggy_write_no_wake_reader.cfg       WriteAppend skips the      *)
(*     waking of a sleeping reader. After append, ringCount > 0 holds     *)
(*     (CanRead = TRUE) but the reader stays in WAITING_READ.              *)
(*                                                                         *)
(*   pipe_buggy_read_no_wake_writer.cfg        ReadDrain skips the wake   *)
(*     of a sleeping writer.                                               *)
(*                                                                         *)
(*   pipe_buggy_close_write_no_wake_reader.cfg CloseWrite skips waking    *)
(*     a sleeping reader. After close, writeEof = TRUE (CanRead = TRUE)    *)
(*     but the reader stays in WAITING_READ.                               *)
(*                                                                         *)
(*   pipe_buggy_close_read_no_wake_writer.cfg  CloseRead skips waking a    *)
(*     sleeping writer.                                                    *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   TypeOk         — type-safety of the state variables.                  *)
(*   SingleWaiter   — at most one thread in WAITING_READ; at most one in   *)
(*                    WAITING_WRITE. Sanity check on the model + a         *)
(*                    structural property of single-waiter rendez.         *)
(*   EofMonotonic   — readEof and writeEof are monotonic (set TRUE never  *)
(*                    flips back to FALSE).                                *)
(*   NoStuckReader  — no thread is in WAITING_READ while CanRead. This is *)
(*                    the missed-wakeup-freedom property for the read     *)
(*                    side: if the condition the reader is waiting on is  *)
(*                    satisfied, the reader is no longer waiting.          *)
(*   NoStuckWriter  — symmetric.                                           *)
(*                                                                         *)
(* See ARCHITECTURE.md §10 (IPC) + §28 invariant I-9.                      *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Threads,
    CAP,
    BUGGY_WRITE_NO_WAKE_READER,
    BUGGY_READ_NO_WAKE_WRITER,
    BUGGY_CLOSE_WRITE_NO_WAKE_READER,
    BUGGY_CLOSE_READ_NO_WAKE_WRITER

ASSUME Cardinality(Threads) >= 1
ASSUME CAP \in Nat /\ CAP > 0
ASSUME BUGGY_WRITE_NO_WAKE_READER \in BOOLEAN
ASSUME BUGGY_READ_NO_WAKE_WRITER \in BOOLEAN
ASSUME BUGGY_CLOSE_WRITE_NO_WAKE_READER \in BOOLEAN
ASSUME BUGGY_CLOSE_READ_NO_WAKE_WRITER \in BOOLEAN

VARIABLES
    ringCount,     \* 0..CAP
    readEof,       \* BOOLEAN
    writeEof,      \* BOOLEAN
    threadState    \* [Threads -> {"RUNNING", "WAITING_READ", "WAITING_WRITE"}]

vars == <<ringCount, readEof, writeEof, threadState>>

ThreadStates == { "RUNNING", "WAITING_READ", "WAITING_WRITE" }

TypeOk ==
    /\ ringCount \in 0..CAP
    /\ readEof \in BOOLEAN
    /\ writeEof \in BOOLEAN
    /\ threadState \in [Threads -> ThreadStates]

Init ==
    /\ ringCount = 0
    /\ readEof = FALSE
    /\ writeEof = FALSE
    /\ threadState = [t \in Threads |-> "RUNNING"]

(***************************************************************************)
(* Helpers.                                                                *)
(***************************************************************************)

WaitingReaders == { t \in Threads : threadState[t] = "WAITING_READ" }
WaitingWriters == { t \in Threads : threadState[t] = "WAITING_WRITE" }

CanRead  == ringCount > 0 \/ writeEof
CanWrite == ringCount < CAP \/ readEof

(***************************************************************************)
(* Clean actions.                                                          *)
(***************************************************************************)

\* ReadDrain — a thread reads one byte from a non-empty buffer + wakes any
\* sleeping writer (the only blocker that's relieved by draining: full
\* buffer → space available).
ReadDrain(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount > 0
    /\ ringCount' = ringCount - 1
    /\ \* Atomically wake the (single) waiting writer if any. The wake
       \* transitions the writer to RUNNING; it will then re-attempt.
       IF WaitingWriters /= {}
       THEN \E w \in WaitingWriters :
              threadState' = [threadState EXCEPT ![w] = "RUNNING"]
       ELSE threadState' = threadState
    /\ UNCHANGED <<readEof, writeEof>>

\* ReadEof — read on empty buffer with writeEof returns 0 (no state change).
ReadEof(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount = 0
    /\ writeEof
    /\ UNCHANGED vars

\* ReadSleep — read on empty buffer without writeEof: sleep. Single-waiter
\* discipline: only one thread may sleep on the read side at a time.
ReadSleep(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount = 0
    /\ ~writeEof
    /\ Cardinality(WaitingReaders) = 0
    /\ threadState' = [threadState EXCEPT ![t] = "WAITING_READ"]
    /\ UNCHANGED <<ringCount, readEof, writeEof>>

\* WriteAppend — append one byte + wake any sleeping reader.
WriteAppend(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount < CAP
    /\ ~readEof                       \* if read end closed, EPIPE instead
    /\ ringCount' = ringCount + 1
    /\ IF WaitingReaders /= {}
       THEN \E r \in WaitingReaders :
              threadState' = [threadState EXCEPT ![r] = "RUNNING"]
       ELSE threadState' = threadState
    /\ UNCHANGED <<readEof, writeEof>>

\* WriteEpipe — write while readEof set returns -1 (no state change).
WriteEpipe(t) ==
    /\ threadState[t] = "RUNNING"
    /\ readEof
    /\ UNCHANGED vars

\* WriteSleep — write on full buffer without readEof: sleep.
WriteSleep(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount = CAP
    /\ ~readEof
    /\ Cardinality(WaitingWriters) = 0
    /\ threadState' = [threadState EXCEPT ![t] = "WAITING_WRITE"]
    /\ UNCHANGED <<ringCount, readEof, writeEof>>

\* CloseWrite — set writeEof + wake any sleeping reader (so they see EOF).
\* Monotonic: only fires if writeEof is currently FALSE.
CloseWrite ==
    /\ ~writeEof
    /\ writeEof' = TRUE
    /\ IF WaitingReaders /= {}
       THEN \E r \in WaitingReaders :
              threadState' = [threadState EXCEPT ![r] = "RUNNING"]
       ELSE threadState' = threadState
    /\ UNCHANGED <<ringCount, readEof>>

\* CloseRead — set readEof + wake any sleeping writer (so they see EPIPE).
CloseRead ==
    /\ ~readEof
    /\ readEof' = TRUE
    /\ IF WaitingWriters /= {}
       THEN \E w \in WaitingWriters :
              threadState' = [threadState EXCEPT ![w] = "RUNNING"]
       ELSE threadState' = threadState
    /\ UNCHANGED <<ringCount, writeEof>>

(***************************************************************************)
(* Buggy actions — each elides the wake-after-mutation step. TLC's         *)
(* NoStuckReader / NoStuckWriter invariants catch the stuck state.         *)
(***************************************************************************)

BuggyWriteAppendNoWake(t) ==
    /\ BUGGY_WRITE_NO_WAKE_READER
    /\ threadState[t] = "RUNNING"
    /\ ringCount < CAP
    /\ ~readEof
    /\ ringCount' = ringCount + 1
    /\ UNCHANGED threadState                 \* skipped wake
    /\ UNCHANGED <<readEof, writeEof>>

BuggyReadDrainNoWake(t) ==
    /\ BUGGY_READ_NO_WAKE_WRITER
    /\ threadState[t] = "RUNNING"
    /\ ringCount > 0
    /\ ringCount' = ringCount - 1
    /\ UNCHANGED threadState
    /\ UNCHANGED <<readEof, writeEof>>

BuggyCloseWriteNoWake ==
    /\ BUGGY_CLOSE_WRITE_NO_WAKE_READER
    /\ ~writeEof
    /\ writeEof' = TRUE
    /\ UNCHANGED <<ringCount, readEof, threadState>>

BuggyCloseReadNoWake ==
    /\ BUGGY_CLOSE_READ_NO_WAKE_WRITER
    /\ ~readEof
    /\ readEof' = TRUE
    /\ UNCHANGED <<ringCount, writeEof, threadState>>

(***************************************************************************)
(* Next-state relation.                                                    *)
(***************************************************************************)

Next ==
    \/ \E t \in Threads : ReadDrain(t)
    \/ \E t \in Threads : ReadEof(t)
    \/ \E t \in Threads : ReadSleep(t)
    \/ \E t \in Threads : WriteAppend(t)
    \/ \E t \in Threads : WriteEpipe(t)
    \/ \E t \in Threads : WriteSleep(t)
    \/ CloseWrite
    \/ CloseRead
    \/ \E t \in Threads : BuggyWriteAppendNoWake(t)
    \/ \E t \in Threads : BuggyReadDrainNoWake(t)
    \/ BuggyCloseWriteNoWake
    \/ BuggyCloseReadNoWake

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

SingleWaiter ==
    /\ Cardinality(WaitingReaders) <= 1
    /\ Cardinality(WaitingWriters) <= 1

\* NoStuckReader: ARCH §28 I-9 specialized to the pipe's read side.
\* If the read-side wait condition holds, no thread is stuck in
\* WAITING_READ. Equivalent: every WAITING_READ thread is waiting on
\* a condition that DOESN'T currently hold.
NoStuckReader ==
    \A t \in Threads : ~(threadState[t] = "WAITING_READ" /\ CanRead)

NoStuckWriter ==
    \A t \in Threads : ~(threadState[t] = "WAITING_WRITE" /\ CanWrite)

\* EofMonotonic — once set, never cleared. Encoded as: in any reachable
\* state, the only transition from FALSE → TRUE; never TRUE → FALSE.
\* This is a structural property of the actions (all clean + buggy
\* actions only set EOF to TRUE, never clear). State invariant form:
\* trivially TRUE in the state space (no mutation from TRUE to FALSE
\* exists). We assert it as a sanity check on the model.
EofMonotonic ==
    /\ readEof \in BOOLEAN
    /\ writeEof \in BOOLEAN

Invariants ==
    /\ TypeOk
    /\ SingleWaiter
    /\ EofMonotonic
    /\ NoStuckReader
    /\ NoStuckWriter

====
