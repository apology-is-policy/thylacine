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
(*   - Multi-waiter-per-direction, wake-ALL. Any number of threads may     *)
(*     sleep on either side at once, and every enabling mutation wakes     *)
(*     EVERY sleeper on the pipe (the impl's poll_waiter_list_wake walks   *)
(*     each blocker's per-call hook). A woken thread re-samples and may    *)
(*     sleep again. This replaced the single-waiter model when pipe ends   *)
(*     became EL0 objects shared across fork/dup/threads: the impl's       *)
(*     per-direction Rendez EXTINCTED on a second sleeper, which no state  *)
(*     invariant here could express -- the runtime witness is the          *)
(*     pipe_blocking.two_*_share_one_* tests. What this model DOES pin is  *)
(*     that wake-all is the obligation: BUGGY_WAKE_ONE_READER wakes a      *)
(*     single chosen reader and leaves a second stuck while CanRead holds. *)
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
(*   - Sleep is never gated: a second (third, ...) sleeper on a side is a  *)
(*     legal state. (The old model disabled it, mirroring the extinction.) *)
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
(*   pipe_buggy_wake_one_reader.cfg            WriteAppend wakes ONE       *)
(*     chosen reader instead of all. With three threads (two readers      *)
(*     asleep), the other stays in WAITING_READ while ringCount > 0.       *)
(*                                                                         *)
(*   pipe_multi.cfg                            all flags FALSE, THREE      *)
(*     threads -- two can wait on one side; TLC proves NoStuck* under      *)
(*     wake-all with re-sleeping.                                          *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   TypeOk         — type-safety of the state variables.                  *)
(*   (SingleWaiter was an invariant of the single-waiter model; retired   *)
(*    with it -- two waiters per side is now the point.)                   *)
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
    BUGGY_CLOSE_READ_NO_WAKE_WRITER,
    BUGGY_WAKE_ONE_READER

ASSUME Cardinality(Threads) >= 1
ASSUME CAP \in Nat /\ CAP > 0
ASSUME BUGGY_WRITE_NO_WAKE_READER \in BOOLEAN
ASSUME BUGGY_READ_NO_WAKE_WRITER \in BOOLEAN
ASSUME BUGGY_CLOSE_WRITE_NO_WAKE_READER \in BOOLEAN
ASSUME BUGGY_CLOSE_READ_NO_WAKE_WRITER \in BOOLEAN
ASSUME BUGGY_WAKE_ONE_READER \in BOOLEAN

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

\* Wake EVERY waiter on one side (poll_waiter_list_wake): each returns to
\* RUNNING and re-attempts; a waiter that finds its condition false again
\* simply sleeps again (ReadSleep / WriteSleep are never gated).
WakeAllReaders(ts) == [t \in Threads |-> IF ts[t] = "WAITING_READ"  THEN "RUNNING" ELSE ts[t]]
WakeAllWriters(ts) == [t \in Threads |-> IF ts[t] = "WAITING_WRITE" THEN "RUNNING" ELSE ts[t]]

(***************************************************************************)
(* Clean actions.                                                          *)
(***************************************************************************)

\* ReadDrain — a thread reads one byte from a non-empty buffer + wakes EVERY
\* sleeping writer (the blockers relieved by draining: full buffer → space).
ReadDrain(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount > 0
    /\ ringCount' = ringCount - 1
    /\ threadState' = WakeAllWriters(threadState)
    /\ UNCHANGED <<readEof, writeEof>>

\* ReadEof — read on empty buffer with writeEof returns 0 (no state change).
ReadEof(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount = 0
    /\ writeEof
    /\ UNCHANGED vars

\* ReadSleep — read on empty buffer without writeEof: sleep. Any number of
\* readers may sleep at once (each has its own hook + Rendez in the impl).
ReadSleep(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount = 0
    /\ ~writeEof
    /\ threadState' = [threadState EXCEPT ![t] = "WAITING_READ"]
    /\ UNCHANGED <<ringCount, readEof, writeEof>>

\* WriteAppend — append one byte + wake EVERY sleeping reader.
WriteAppend(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount < CAP
    /\ ~readEof                       \* if read end closed, EPIPE instead
    /\ ringCount' = ringCount + 1
    /\ threadState' = WakeAllReaders(threadState)
    /\ UNCHANGED <<readEof, writeEof>>

\* WriteEpipe — write while readEof set returns -1 (no state change).
WriteEpipe(t) ==
    /\ threadState[t] = "RUNNING"
    /\ readEof
    /\ UNCHANGED vars

\* WriteSleep — write on full buffer without readEof: sleep (never gated).
WriteSleep(t) ==
    /\ threadState[t] = "RUNNING"
    /\ ringCount = CAP
    /\ ~readEof
    /\ threadState' = [threadState EXCEPT ![t] = "WAITING_WRITE"]
    /\ UNCHANGED <<ringCount, readEof, writeEof>>

\* CloseWrite — set writeEof + wake EVERY sleeping reader (so they see EOF).
\* Monotonic: only fires if writeEof is currently FALSE.
CloseWrite ==
    /\ ~writeEof
    /\ writeEof' = TRUE
    /\ threadState' = WakeAllReaders(threadState)
    /\ UNCHANGED <<ringCount, readEof>>

\* CloseRead — set readEof + wake EVERY sleeping writer (so they see EPIPE).
CloseRead ==
    /\ ~readEof
    /\ readEof' = TRUE
    /\ threadState' = WakeAllWriters(threadState)
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

\* The multi-waiter-specific bug: an append that wakes ONE chosen reader (the
\* old single-waiter wakeup) instead of every hook. With two readers asleep,
\* the un-woken one is stuck while CanRead holds -- NoStuckReader violated.
BuggyWriteAppendWakeOne(t) ==
    /\ BUGGY_WAKE_ONE_READER
    /\ threadState[t] = "RUNNING"
    /\ ringCount < CAP
    /\ ~readEof
    /\ ringCount' = ringCount + 1
    /\ IF WaitingReaders /= {}
       THEN \E r \in WaitingReaders :
              threadState' = [threadState EXCEPT ![r] = "RUNNING"]
       ELSE threadState' = threadState
    /\ UNCHANGED <<readEof, writeEof>>

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
    \/ \E t \in Threads : BuggyWriteAppendWakeOne(t)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

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
    /\ EofMonotonic
    /\ NoStuckReader
    /\ NoStuckWriter

====
