---- MODULE loom_sq ----
(***************************************************************************)
(* Thylacine Loom -- the SQ slot handoff between a legitimate userspace     *)
(* producer and the kernel consumers (loom_drain_sq, loom_private_submit).  *)
(*                                                                         *)
(* The SQ is a single-producer ring: userspace writes an SQE body and its   *)
(* SQ-index entry at position tail, then release-bumps sq_tail. The kernel  *)
(* reads the index and copies the SQE, then publishes sq_head. A published  *)
(* sq_head is the producer's PERMISSION TO REUSE that position: a           *)
(* well-behaved producer (libthyla-rs Ring::try_submit) acquire-loads       *)
(* sq_head and overwrites any position below it. So the kernel must finish  *)
(* reading a position before it publishes the head past it.                 *)
(*                                                                         *)
(* WHY A SEPARATE MODULE. loom.tla gives every op its own SQE slot for the  *)
(* op's whole life, and its Consume copies atomically, so it cannot express *)
(* a position being handed back and refilled between the kernel's head     *)
(* publication and its copy. Its UserMutateSqe is a HOSTILE writer that     *)
(* I-30 makes harmless; the producer here follows the protocol, and is      *)
(* still harmed by a kernel that releases too early. One focused module per *)
(* mechanism (the loom_order.tla / loom_multishot.tla precedent).           *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   1. FifoFidelity (the SQ half of I-29 no-lost / no-double) -- the       *)
(*      kernel's consume sequence is a prefix of the producer's production  *)
(*      sequence, element for element: every consumed SQE is the one the    *)
(*      producer placed at that position, none is consumed twice, none is   *)
(*      skipped. An out-of-range index (BAD) is consumed as itself (the     *)
(*      kernel counts it in `dropped`); a later valid entry must not be     *)
(*      read in its place.                                                  *)
(*                                                                         *)
(*   2. HeadNeverPassesTail -- the kernel never consumes past what was      *)
(*      produced.                                                           *)
(*                                                                         *)
(*   3. ClaimExclusive -- at most one kernel consumer is between its claim  *)
(*      and its release (l->lock; loom_drain_sq's F1 claim). Checked by     *)
(*      the clean cfg AND violated by BUGGY_UNLOCKED_CLAIM in its own cfg,  *)
(*      so it is not a rule compared with itself.                           *)
(*                                                                         *)
(* THE BUGS THIS PINS (each a BUGGY_* flag, each its own cfg)               *)
(*                                                                         *)
(*   BUGGY_RELEASE_BEFORE_COPY -- the consumer publishes sq_head, THEN      *)
(*     reads the index and copies the SQE. The producer refills the         *)
(*     position in between: the kernel copies the NEW entry for the OLD     *)
(*     position, later consumes the new entry again, and the old one is     *)
(*     never consumed (FifoFidelity cx). This was loom_drain_sq through     *)
(*     a7353c36f.                                                           *)
(*                                                                         *)
(*   BUGGY_UNLOCKED_CLAIM -- two consumers (two ENTERs of a multi-thread    *)
(*     Proc on a non-SQPOLL ring; an SQPOLL ring is drained by its kthread  *)
(*     alone) claim the same head without l->lock: one position is         *)
(*     consumed twice and the next is skipped (FifoFidelity cx, and         *)
(*     ClaimExclusive cx in a second cfg). The control for the two-consumer *)
(*     dimension: without it, a second consumer would be decoration.        *)
(*                                                                         *)
(* ABSTRACTIONS. `head` stands for BOTH the kernel-private l->sq_head and   *)
(* its published mirror h->sq_head: the code advances them together under   *)
(* l->lock, and only a lock holder reads the private one. The producer's    *)
(* body + index writes and its release of sq_tail are one step: its release *)
(* store orders them before the kernel's acquire of sq_tail, and the kernel *)
(* reads only positions below the tail it acquired. A hostile producer that *)
(* ignores sq_head is out of scope here: it can only damage its own         *)
(* submissions, and loom.tla's ArgPinnedToSnapshot covers what the kernel   *)
(* does with whatever it copied.                                            *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    N,                          \* SQ entries
    MAX_OPS,                    \* the producer emits at most this many entries
    Consumers,                  \* kernel consumers sharing one ring
    NONE,                       \* lock is free
    BUGGY_RELEASE_BEFORE_COPY,
    BUGGY_UNLOCKED_CLAIM

ASSUME N \in Nat \ {0}
ASSUME MAX_OPS \in Nat
ASSUME NONE \notin Consumers

BAD == 0                        \* an out-of-range SQ index entry
Contents == {BAD} \cup (1..MAX_OPS)

VARIABLES
    head,       \* consume position (l->sq_head == published h->sq_head)
    tail,       \* produce position (h->sq_tail)
    slot,       \* [0..N-1 -> Contents]: SQ index + SQE body at each position
    produced,   \* ghost: production order
    consumed,   \* ghost: consume order (what the kernel copied, or BAD)
    lock,       \* l->lock holder, or NONE
    pc,         \* [Consumers -> {"idle", "copied", "released"}]
    pos,        \* [Consumers -> 0..N-1]: the position this consumer claimed
    kcopy       \* [Consumers -> Contents]: the kernel copy

vars == <<head, tail, slot, produced, consumed, lock, pc, pos, kcopy>>

Init ==
    /\ head     = 0
    /\ tail     = 0
    /\ slot     = [i \in 0..N-1 |-> BAD]
    /\ produced = <<>>
    /\ consumed = <<>>
    /\ lock     = NONE
    /\ pc       = [c \in Consumers |-> "idle"]
    /\ pos      = [c \in Consumers |-> 0]
    /\ kcopy    = [c \in Consumers |-> BAD]

(***************************************************************************)
(* Produce(v) -- Ring::try_submit: refuse when tail - head >= N (head is    *)
(* acquire-loaded), else write the position and release-bump the tail. The  *)
(* entry is either the next op or an out-of-range index.                    *)
(***************************************************************************)
Produce(v) ==
    /\ Len(produced) < MAX_OPS
    /\ tail - head < N
    /\ v \in {Len(produced) + 1, BAD}
    /\ slot'     = [slot EXCEPT ![tail % N] = v]
    /\ tail'     = tail + 1
    /\ produced' = Append(produced, v)
    /\ UNCHANGED <<head, consumed, lock, pc, pos, kcopy>>

(***************************************************************************)
(* Claim(c) -- take l->lock, see tail != head, claim position head % N.     *)
(* CORRECT: read the index and copy the SQE now, still holding the head.    *)
(* BUGGY_RELEASE_BEFORE_COPY: publish head + 1 first, copy afterwards.      *)
(* BUGGY_UNLOCKED_CLAIM: no lock; another consumer can claim the same head. *)
(***************************************************************************)
Claim(c) ==
    /\ pc[c] = "idle"
    /\ BUGGY_UNLOCKED_CLAIM \/ lock = NONE
    /\ tail # head
    /\ lock' = IF BUGGY_UNLOCKED_CLAIM THEN lock ELSE c
    /\ pos'  = [pos EXCEPT ![c] = head % N]
    /\ IF BUGGY_RELEASE_BEFORE_COPY
         THEN /\ head'  = head + 1
              /\ pc'    = [pc EXCEPT ![c] = "released"]
              /\ UNCHANGED kcopy
         ELSE /\ kcopy' = [kcopy EXCEPT ![c] = slot[head % N]]
              /\ pc'    = [pc EXCEPT ![c] = "copied"]
              /\ UNCHANGED head
    /\ UNCHANGED <<tail, slot, produced, consumed>>

(***************************************************************************)
(* Finish(c) -- CORRECT: publish head + 1 (the reuse permission) after the  *)
(* copy, then drop l->lock. BUGGY_RELEASE_BEFORE_COPY: copy now, from a     *)
(* position the producer may already have refilled.                         *)
(***************************************************************************)
Finish(c) ==
    /\ pc[c] \in {"copied", "released"}
    /\ IF pc[c] = "copied"
         THEN /\ head'     = head + 1
              /\ consumed' = Append(consumed, kcopy[c])
              /\ UNCHANGED kcopy
         ELSE /\ kcopy'    = [kcopy EXCEPT ![c] = slot[pos[c]]]
              /\ consumed' = Append(consumed, slot[pos[c]])
              /\ UNCHANGED head
    /\ lock' = IF BUGGY_UNLOCKED_CLAIM THEN lock ELSE NONE
    /\ pc'   = [pc EXCEPT ![c] = "idle"]
    /\ UNCHANGED <<tail, slot, produced, pos>>

\* The producer has finished and every entry is consumed: stutter, so TLC's
\* deadlock check stays on for every other state.
Terminated ==
    /\ Len(produced) = MAX_OPS
    /\ head = tail
    /\ \A c \in Consumers : pc[c] = "idle"
    /\ UNCHANGED vars

Next ==
    \/ \E v \in Contents : Produce(v)
    \/ \E c \in Consumers : Claim(c) \/ Finish(c)
    \/ Terminated

Spec == Init /\ [][Next]_vars

\* Every entry produced is eventually consumed.
Live == Spec /\ WF_vars(\E v \in Contents : Produce(v))
             /\ \A c \in Consumers : WF_vars(Claim(c)) /\ WF_vars(Finish(c))

(***************************************************************************)
(* Invariants                                                               *)
(***************************************************************************)
TypeOK ==
    /\ head \in Nat /\ tail \in Nat
    /\ slot \in [0..N-1 -> Contents]
    /\ produced \in Seq(Contents) /\ consumed \in Seq(Contents)
    /\ lock \in Consumers \cup {NONE}
    /\ pc \in [Consumers -> {"idle", "copied", "released"}]
    /\ pos \in [Consumers -> 0..N-1]
    /\ kcopy \in [Consumers -> Contents]

FifoFidelity ==
    /\ Len(consumed) <= Len(produced)
    /\ \A i \in 1..Len(consumed) : consumed[i] = produced[i]

HeadNeverPassesTail == head <= tail /\ tail - head <= N

ClaimExclusive ==
    \A c \in Consumers : pc[c] # "idle" => lock = c

Invariants == TypeOK /\ FifoFidelity /\ HeadNeverPassesTail /\ ClaimExclusive

AllConsumed == <>(Len(consumed) = MAX_OPS)
====
