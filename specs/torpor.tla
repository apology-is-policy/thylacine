---------------------------- MODULE torpor ----------------------------
(* The wake of kernel/torpor.c (thread-model C8; F-9, F-10, E-7).

   One bucket: a lock and a chain, oldest first. A waiter's record lives on its
   own kernel stack. WAIT compares the word and links under the lock, sleeps with
   the lock dropped, and takes the lock again to leave. WAKE marks under the lock
   and wakes after it, because wakeup() can spin on a thread's on_cpu and a woken
   waiter's first act is to come for the lock. So a wake's batch holds records past
   the lock, and each waiter waits for the wake to let go of its record.

   What is checked:
     NoUseAfterFree  a wake touches a record only while its frame is alive
     CountHonest     a counted waiter returns 0, never EINTR or ETIMEDOUT
     OldestFirst     nothing older stays in the chain past a marked waiter
     NoStranded      a wake that fell short of its count left nobody unmarked
     NoLostWake      a full wake after the word changed leaves nobody sleeping
     MarkedSleeperIsHeld  a waiter asleep with its mark set has a wake still to come

   A waiter holds its rendez's lock from reading the cond through registering on the
   rendez, and a mark takes that lock, so a mark comes before the read or finds the
   waiter asleep. SleepRead and SleepCommit are that hold's two ends.

   A stop parks a waiter off its rendez with its record still in the chain, which is
   how a mark meets a waiter that is not asleep. A deadline and a caught note end a
   sleep from outside, and each asks the cond first, as tsleep does. *)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Waiters,            \* the waiter threads
    Count,              \* the wake's count
    BUGGY_NO_WAIT,      \* a waiter leaves without waiting for the wake to let go
    BUGGY_LIFO,         \* a wake takes the newest
    BUGGY_COUNT_ANY,    \* a wake counts a waiter that was not asleep
    BUGGY_EINTR_ANY,    \* a caught note unwinds without asking the cond
    BUGGY_UNLOCKED_CHECK,   \* the compare runs outside the lock
    BUGGY_STOP_AT_MARK, \* the walk stops at its first mark, counted or not
    BUGGY_UNLOCKED_MARK \* a mark lands between a waiter's read and its registering

VARIABLES
    word,       \* the user word; WAIT expects 0
    lock,       \* NoOne, "wake" or a waiter
    chain,      \* the bucket, oldest at the head
    pc,         \* each waiter: see Init
    awoken,     \* the cond
    timedout,   \* a deadline fired
    rc,         \* what the waiter returned
    held,       \* a wake's batch holds this record
    counted,    \* the wake counted this waiter
    marked,     \* the wake marked this waiter
    order,      \* when each waiter linked
    clock,
    wk,         \* the wake: "idle" "store" "walk" "deliver" "done"
    woken,
    batch,
    left,       \* who the walk left in the chain when it ended
    read        \* what the waiter's cond read, between SleepRead and SleepCommit

NoOne == "none"
vars == <<word, lock, chain, pc, awoken, timedout, rc, held, counted, marked,
          order, clock, wk, woken, batch, left, read>>

Init ==
    /\ word = 0 /\ lock = NoOne /\ chain = <<>>
    /\ pc = [w \in Waiters |-> "idle"]
    /\ awoken = [w \in Waiters |-> FALSE]
    /\ timedout = [w \in Waiters |-> FALSE]
    /\ rc = [w \in Waiters |-> "none"]
    /\ held = [w \in Waiters |-> FALSE]
    /\ counted = [w \in Waiters |-> FALSE]
    /\ marked = [w \in Waiters |-> FALSE]
    /\ order = [w \in Waiters |-> 0]
    /\ clock = 0 /\ wk = "idle" /\ woken = 0 /\ batch = <<>> /\ left = {}
    /\ read = [w \in Waiters |-> FALSE]

Remove(s, w) == SelectSeq(s, LAMBDA x: x # w)
Range(s) == {s[i] : i \in 1..Len(s)}

(* WAIT, under the bucket's lock. The word and the link are one step. *)
WaitLink(w) ==
    /\ pc[w] = "idle" /\ lock = NoOne /\ ~BUGGY_UNLOCKED_CHECK
    /\ IF word = 0
       THEN /\ chain' = Append(chain, w)
            /\ pc' = [pc EXCEPT ![w] = "linked"]
            /\ order' = [order EXCEPT ![w] = clock + 1]
            /\ clock' = clock + 1
            /\ UNCHANGED rc
       ELSE /\ pc' = [pc EXCEPT ![w] = "gone"]
            /\ rc' = [rc EXCEPT ![w] = "ok"]
            /\ UNCHANGED <<chain, order, clock>>
    /\ UNCHANGED <<word, lock, awoken, timedout, held, counted, marked, wk, woken, batch, left, read>>

(* The bug: the compare, then the lock. *)
BuggyCheck(w) ==
    /\ BUGGY_UNLOCKED_CHECK /\ pc[w] = "idle"
    /\ pc' = [pc EXCEPT ![w] = IF word = 0 THEN "checked" ELSE "gone"]
    /\ rc' = [rc EXCEPT ![w] = IF word = 0 THEN "none" ELSE "ok"]
    /\ UNCHANGED <<word, lock, chain, awoken, timedout, held, counted, marked, order, clock, wk, woken, batch, left, read>>

BuggyLink(w) ==
    /\ BUGGY_UNLOCKED_CHECK /\ pc[w] = "checked" /\ lock = NoOne
    /\ chain' = Append(chain, w)
    /\ pc' = [pc EXCEPT ![w] = "linked"]
    /\ order' = [order EXCEPT ![w] = clock + 1]
    /\ clock' = clock + 1
    /\ UNCHANGED <<word, lock, awoken, timedout, rc, held, counted, marked, wk, woken, batch, left, read>>

(* tsleep's entry and its loop on resume: the rendez's lock, then the cond. *)
SleepRead(w) ==
    /\ pc[w] \in {"linked", "running"}
    /\ pc' = [pc EXCEPT ![w] = "reading"]
    /\ read' = [read EXCEPT ![w] = awoken[w]]
    /\ UNCHANGED <<word, lock, chain, awoken, timedout, rc, held, counted, marked, order, clock, wk, woken, batch, left>>

(* What the cond read, then the deadline, else registering on the rendez. *)
SleepCommit(w) ==
    /\ pc[w] = "reading"
    /\ CASE read[w]     -> /\ pc' = [pc EXCEPT ![w] = "leaving"]
                            /\ rc' = [rc EXCEPT ![w] = "ok"]
         [] timedout[w] -> /\ pc' = [pc EXCEPT ![w] = "leaving"]
                            /\ rc' = [rc EXCEPT ![w] = "timedout"]
         [] OTHER       -> /\ pc' = [pc EXCEPT ![w] = "asleep"]
                            /\ UNCHANGED rc
    /\ UNCHANGED <<word, lock, chain, awoken, timedout, held, counted, marked, order, clock, wk, woken, batch, left, read>>

(* The deadline's wake, off the bucket's lock. *)
Deadline(w) ==
    /\ pc[w] = "asleep"
    /\ timedout' = [timedout EXCEPT ![w] = TRUE]
    /\ pc' = [pc EXCEPT ![w] = "running"]
    /\ UNCHANGED <<word, lock, chain, awoken, rc, held, counted, marked, order, clock, wk, woken, batch, left, read>>

(* A caught note's unwind, under the rendez's lock: the cond first. *)
CaughtNote(w) ==
    /\ pc[w] = "asleep"
    /\ awoken[w] => BUGGY_EINTR_ANY
    /\ pc' = [pc EXCEPT ![w] = "leaving"]
    /\ rc' = [rc EXCEPT ![w] = "eintr"]
    /\ UNCHANGED <<word, lock, chain, awoken, timedout, held, counted, marked, order, clock, wk, woken, batch, left, read>>

(* A stop's wake, then its park: on no rendez, still in the chain. *)
Stop(w) ==
    /\ pc[w] = "asleep"
    /\ pc' = [pc EXCEPT ![w] = "parked"]
    /\ UNCHANGED <<word, lock, chain, awoken, timedout, rc, held, counted, marked, order, clock, wk, woken, batch, left, read>>

Resume(w) ==
    /\ pc[w] = "parked"
    /\ pc' = [pc EXCEPT ![w] = "running"]
    /\ UNCHANGED <<word, lock, chain, awoken, timedout, rc, held, counted, marked, order, clock, wk, woken, batch, left, read>>

(* Take the lock to leave: unlink if still linked. *)
Unlink(w) ==
    /\ pc[w] = "leaving" /\ lock = NoOne
    /\ chain' = Remove(chain, w)
    /\ pc' = [pc EXCEPT ![w] = "draining"]
    /\ UNCHANGED <<word, lock, awoken, timedout, rc, held, counted, marked, order, clock, wk, woken, batch, left, read>>

(* The frame goes once the wake has let go of it. *)
Leave(w) ==
    /\ pc[w] = "draining"
    /\ ~held[w] \/ BUGGY_NO_WAIT
    /\ pc' = [pc EXCEPT ![w] = "gone"]
    /\ UNCHANGED <<word, lock, chain, awoken, timedout, rc, held, counted, marked, order, clock, wk, woken, batch, left, read>>

(* The producer's store comes before its wake. *)
Store ==
    /\ wk = "idle"
    /\ word' = 1
    /\ wk' = "store"
    /\ UNCHANGED <<lock, chain, pc, awoken, timedout, rc, held, counted, marked, order, clock, woken, batch, left, read>>

WakeLock ==
    /\ wk = "store" /\ lock = NoOne
    /\ lock' = "wake"
    /\ wk' = "walk"
    /\ UNCHANGED <<word, chain, pc, awoken, timedout, rc, held, counted, marked, order, clock, woken, batch, left, read>>

(* One mark. The rendez's lock makes the read of "asleep" and the mark one step, and
   keeps the mark out of a waiter's SleepRead to SleepCommit: it waits, bucket held. *)
WakeMark ==
    /\ wk = "walk" /\ chain # <<>> /\ woken < Count
    /\ LET w == IF BUGGY_LIFO THEN chain[Len(chain)] ELSE chain[1]
           asleep == pc[w] = "asleep"
           count == asleep \/ BUGGY_COUNT_ANY
       IN /\ pc[w] # "reading" \/ BUGGY_UNLOCKED_MARK
          /\ awoken' = [awoken EXCEPT ![w] = TRUE]
          /\ marked' = [marked EXCEPT ![w] = TRUE]
          /\ chain' = Remove(chain, w)
          /\ counted' = [counted EXCEPT ![w] = count]
          /\ woken' = IF count THEN woken + 1 ELSE woken
          /\ batch' = IF asleep THEN Append(batch, w) ELSE batch
          /\ held' = [held EXCEPT ![w] = asleep]
          /\ wk' = IF BUGGY_STOP_AT_MARK THEN "unlock" ELSE "walk"
          /\ left' = IF BUGGY_STOP_AT_MARK THEN Range(Remove(chain, w)) ELSE left
    /\ UNCHANGED <<word, lock, pc, timedout, rc, order, clock, read>>

WakeWalkEnds ==
    /\ wk = "walk" /\ (chain = <<>> \/ woken >= Count)
    /\ wk' = "unlock"
    /\ left' = Range(chain)
    /\ UNCHANGED <<word, lock, chain, pc, awoken, timedout, rc, held, counted, marked, order, clock, woken, batch, read>>

WakeUnlock ==
    /\ wk = "unlock"
    /\ lock' = NoOne
    /\ wk' = "deliver"
    /\ UNCHANGED <<word, chain, pc, awoken, timedout, rc, held, counted, marked, order, clock, woken, batch, left, read>>

(* wakeup(), then the clear. wakeup() no-ops on a waiter already off its rendez. *)
WakeDeliver ==
    /\ wk = "deliver" /\ batch # <<>>
    /\ LET w == batch[1]
       IN /\ pc' = IF pc[w] = "asleep" THEN [pc EXCEPT ![w] = "running"] ELSE pc
          /\ held' = [held EXCEPT ![w] = FALSE]
          /\ batch' = Tail(batch)
    /\ UNCHANGED <<word, lock, chain, awoken, timedout, rc, counted, marked, order, clock, wk, woken, left, read>>

WakeDone ==
    /\ wk = "deliver" /\ batch = <<>>
    /\ wk' = "done"
    /\ UNCHANGED <<word, lock, chain, pc, awoken, timedout, rc, held, counted, marked, order, clock, woken, batch, left, read>>

Next ==
    \/ \E w \in Waiters:
        \/ WaitLink(w) \/ BuggyCheck(w) \/ BuggyLink(w) \/ SleepRead(w) \/ SleepCommit(w)
        \/ Deadline(w) \/ CaughtNote(w) \/ Stop(w) \/ Resume(w)
        \/ Unlink(w) \/ Leave(w)
    \/ Store \/ WakeLock \/ WakeMark \/ WakeWalkEnds \/ WakeUnlock \/ WakeDeliver \/ WakeDone

Spec == Init /\ [][Next]_vars

\* A wake reaches into a record only while its frame is there.
NoUseAfterFree == \A i \in 1..Len(batch): pc[batch[i]] # "gone"

\* A waiter a wake counted returns 0, whatever ended its sleep.
CountHonest == \A w \in Waiters: (counted[w] /\ rc[w] # "none") => rc[w] = "ok"

\* Nothing older than a marked waiter stays behind it.
OldestFirst == \A m, o \in Waiters:
    (marked[m] /\ \E i \in 1..Len(chain): chain[i] = o) => order[o] > order[m]

\* A wake that fell short had nobody left to wake.
NoStranded == woken < Count => left = {}

\* After the store and a wake of everyone, nobody who read the old word sleeps on.
NoLostWake == (wk = "done" /\ Count >= Cardinality(Waiters)) =>
    \A w \in Waiters: pc[w] # "asleep" /\ (pc[w] = "parked" => awoken[w])

\* A mark does not wake anyone. A waiter asleep with its mark set needs a batch to hold it,
\* or nothing is coming, and that is true at every count.
MarkedSleeperIsHeld == \A w \in Waiters: (pc[w] = "asleep" /\ awoken[w]) => held[w]

TypeOK ==
    /\ word \in {0, 1}
    /\ pc \in [Waiters -> {"idle", "checked", "linked", "reading", "asleep", "running", "parked",
                           "leaving", "draining", "gone"}]
    /\ rc \in [Waiters -> {"none", "ok", "eintr", "timedout"}]
    /\ woken \in 0..Cardinality(Waiters)
=============================================================================
