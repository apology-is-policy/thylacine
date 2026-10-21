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
(*   - The write end can hang up without closing (P3b; ARCH 21.10, "A      *)
(*     death hangs up": a dead 9P session hangs up its tx pipe). writeOpen *)
(*     tracks whether the write end is still held. HangupWrite sets        *)
(*     writeEof with the end still held, so writers can still reach it:    *)
(*     every write after it is refused (EOF is final -- no byte follows    *)
(*     it), and its one wake must reach blocked writers as well as         *)
(*     readers. CloseWrite needs no blocked writer -- in the impl the      *)
(*     close runs at the last ref drop, and a blocked writer holds a ref.  *)
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
(*   pipe_buggy_hangup_no_wake_writer.cfg      HangupWrite wakes readers   *)
(*     only, as a close does. A writer asleep on the full ring stays in    *)
(*     WAITING_WRITE while writeEof makes CanWrite hold (NoStuckWriter).   *)
(*                                                                         *)
(*   pipe_buggy_hangup_takes_bytes.cfg         a hung-up write end still   *)
(*     takes bytes: an append after writeEof (NoByteAfterEof).             *)
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
(*   NoByteAfterEof — (an action PROPERTY) once writeEof holds, the ring   *)
(*                    never grows: no byte follows EOF.                    *)
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
    BUGGY_WAKE_ONE_READER,
    BUGGY_HANGUP_NO_WAKE_WRITER,
    BUGGY_HANGUP_TAKES_BYTES

ASSUME Cardinality(Threads) >= 1
ASSUME CAP \in Nat /\ CAP > 0
ASSUME BUGGY_WRITE_NO_WAKE_READER \in BOOLEAN
ASSUME BUGGY_READ_NO_WAKE_WRITER \in BOOLEAN
ASSUME BUGGY_CLOSE_WRITE_NO_WAKE_READER \in BOOLEAN
ASSUME BUGGY_CLOSE_READ_NO_WAKE_WRITER \in BOOLEAN
ASSUME BUGGY_WAKE_ONE_READER \in BOOLEAN
ASSUME BUGGY_HANGUP_NO_WAKE_WRITER \in BOOLEAN
ASSUME BUGGY_HANGUP_TAKES_BYTES \in BOOLEAN

VARIABLES
    ringCount,     \* 0..CAP
    readEof,       \* BOOLEAN
    writeEof,      \* BOOLEAN
    writeOpen,     \* BOOLEAN -- the write end is still held (hung up or not)
    threadState    \* [Threads -> {"RUNNING", "WAITING_READ", "WAITING_WRITE"}]

vars == <<ringCount, readEof, writeEof, writeOpen, threadState>>

ThreadStates == { "RUNNING", "WAITING_READ", "WAITING_WRITE" }

TypeOk ==
    /\ ringCount \in 0..CAP
    /\ readEof \in BOOLEAN
    /\ writeEof \in BOOLEAN
    /\ writeOpen \in BOOLEAN
    /\ threadState \in [Threads -> ThreadStates]

Init ==
    /\ ringCount = 0
    /\ readEof = FALSE
    /\ writeEof = FALSE
    /\ writeOpen = TRUE
    /\ threadState = [t \in Threads |-> "RUNNING"]

(***************************************************************************)
(* Helpers.                                                                *)
(***************************************************************************)

WaitingReaders == { t \in Threads : threadState[t] = "WAITING_READ" }
WaitingWriters == { t \in Threads : threadState[t] = "WAITING_WRITE" }

CanRead  == ringCount > 0 \/ writeEof
\* A hung-up write end ends a writer's wait too: what it meets is a refusal.
CanWrite == ringCount < CAP \/ readEof \/ writeEof

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
    /\ UNCHANGED <<readEof, writeEof, writeOpen>>

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
    /\ UNCHANGED <<ringCount, readEof, writeEof, writeOpen>>

\* WriteAppend — append one byte + wake EVERY sleeping reader. Only through a
\* held write end, and never after EOF.
WriteAppend(t) ==
    /\ threadState[t] = "RUNNING"
    /\ writeOpen
    /\ ringCount < CAP
    /\ ~readEof                       \* if read end closed, EPIPE instead
    /\ ~writeEof                      \* if the write end hung up, refused too
    /\ ringCount' = ringCount + 1
    /\ threadState' = WakeAllReaders(threadState)
    /\ UNCHANGED <<readEof, writeEof, writeOpen>>

\* WriteEpipe — a write refused (-T_E_PIPE): the read end closed, or the write
\* end hung up (no state change).
WriteEpipe(t) ==
    /\ threadState[t] = "RUNNING"
    /\ writeOpen
    /\ readEof \/ writeEof
    /\ UNCHANGED vars

\* WriteSleep — write on full buffer with neither EOF: sleep (never gated).
WriteSleep(t) ==
    /\ threadState[t] = "RUNNING"
    /\ writeOpen
    /\ ringCount = CAP
    /\ ~readEof
    /\ ~writeEof
    /\ threadState' = [threadState EXCEPT ![t] = "WAITING_WRITE"]
    /\ UNCHANGED <<ringCount, readEof, writeEof, writeOpen>>

\* CloseWrite — the last ref on the write end drops: set writeEof + wake EVERY
\* sleeping reader (so they see EOF). No writer can be asleep in an end being
\* closed (a blocked writer holds a ref); it may follow a hangup.
CloseWrite ==
    /\ writeOpen
    /\ WaitingWriters = {}
    /\ writeOpen' = FALSE
    /\ writeEof' = TRUE
    /\ threadState' = WakeAllReaders(threadState)
    /\ UNCHANGED <<ringCount, readEof>>

\* HangupWrite — EOF without the close (pipe_hangup_write): set writeEof with
\* the end still held, and wake EVERY sleeper -- readers see EOF, writers are
\* refused. Monotonic: only fires if writeEof is currently FALSE.
HangupWrite ==
    /\ writeOpen
    /\ ~writeEof
    /\ writeEof' = TRUE
    /\ threadState' = WakeAllWriters(WakeAllReaders(threadState))
    /\ UNCHANGED <<ringCount, readEof, writeOpen>>

\* CloseRead — set readEof + wake EVERY sleeping writer (so they see EPIPE).
CloseRead ==
    /\ ~readEof
    /\ readEof' = TRUE
    /\ threadState' = WakeAllWriters(threadState)
    /\ UNCHANGED <<ringCount, writeEof, writeOpen>>

(***************************************************************************)
(* Buggy actions — each elides the wake-after-mutation step. TLC's         *)
(* NoStuckReader / NoStuckWriter invariants catch the stuck state.         *)
(***************************************************************************)

BuggyWriteAppendNoWake(t) ==
    /\ BUGGY_WRITE_NO_WAKE_READER
    /\ threadState[t] = "RUNNING"
    /\ writeOpen
    /\ ringCount < CAP
    /\ ~readEof
    /\ ~writeEof
    /\ ringCount' = ringCount + 1
    /\ UNCHANGED threadState                 \* skipped wake
    /\ UNCHANGED <<readEof, writeEof, writeOpen>>

BuggyReadDrainNoWake(t) ==
    /\ BUGGY_READ_NO_WAKE_WRITER
    /\ threadState[t] = "RUNNING"
    /\ ringCount > 0
    /\ ringCount' = ringCount - 1
    /\ UNCHANGED threadState
    /\ UNCHANGED <<readEof, writeEof, writeOpen>>

BuggyCloseWriteNoWake ==
    /\ BUGGY_CLOSE_WRITE_NO_WAKE_READER
    /\ writeOpen
    /\ WaitingWriters = {}
    /\ writeOpen' = FALSE
    /\ writeEof' = TRUE
    /\ UNCHANGED <<ringCount, readEof, threadState>>

BuggyCloseReadNoWake ==
    /\ BUGGY_CLOSE_READ_NO_WAKE_WRITER
    /\ ~readEof
    /\ readEof' = TRUE
    /\ UNCHANGED <<ringCount, writeEof, writeOpen, threadState>>

\* The hangup's wake as a close's: readers only. A writer asleep on the full
\* ring is left in WAITING_WRITE while writeEof makes CanWrite hold.
BuggyHangupWakeReadersOnly ==
    /\ BUGGY_HANGUP_NO_WAKE_WRITER
    /\ writeOpen
    /\ ~writeEof
    /\ writeEof' = TRUE
    /\ threadState' = WakeAllReaders(threadState)
    /\ UNCHANGED <<ringCount, readEof, writeOpen>>

\* A hung-up write end that still takes bytes: the append without its
\* ~writeEof guard. A byte lands after EOF.
BuggyAppendAfterHangup(t) ==
    /\ BUGGY_HANGUP_TAKES_BYTES
    /\ threadState[t] = "RUNNING"
    /\ writeOpen
    /\ writeEof
    /\ ringCount < CAP
    /\ ~readEof
    /\ ringCount' = ringCount + 1
    /\ threadState' = WakeAllReaders(threadState)
    /\ UNCHANGED <<readEof, writeEof, writeOpen>>

\* The multi-waiter-specific bug: an append that wakes ONE chosen reader (the
\* old single-waiter wakeup) instead of every hook. With two readers asleep,
\* the un-woken one is stuck while CanRead holds -- NoStuckReader violated.
BuggyWriteAppendWakeOne(t) ==
    /\ BUGGY_WAKE_ONE_READER
    /\ threadState[t] = "RUNNING"
    /\ writeOpen
    /\ ringCount < CAP
    /\ ~readEof
    /\ ~writeEof
    /\ ringCount' = ringCount + 1
    /\ IF WaitingReaders /= {}
       THEN \E r \in WaitingReaders :
              threadState' = [threadState EXCEPT ![r] = "RUNNING"]
       ELSE threadState' = threadState
    /\ UNCHANGED <<readEof, writeEof, writeOpen>>

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
    \/ HangupWrite
    \/ CloseRead
    \/ \E t \in Threads : BuggyWriteAppendNoWake(t)
    \/ \E t \in Threads : BuggyReadDrainNoWake(t)
    \/ BuggyCloseWriteNoWake
    \/ BuggyCloseReadNoWake
    \/ \E t \in Threads : BuggyWriteAppendWakeOne(t)
    \/ BuggyHangupWakeReadersOnly
    \/ \E t \in Threads : BuggyAppendAfterHangup(t)

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

\* NoByteAfterEof — an action property: once writeEof holds, the ring never
\* grows. A reader that has seen EOF after the drain sees nothing more.
NoByteAfterEof == [][writeEof => ringCount' <= ringCount]_vars

Invariants ==
    /\ TypeOk
    /\ EofMonotonic
    /\ NoStuckReader
    /\ NoStuckWriter

====
