---- MODULE tag_pool ----
(***************************************************************************)
(* The 9P client's tag pool: who may hold a tag, and why a sync op that    *)
(* waits for one gets it (ARCH 21.11, dec-2026-10-07-tag-pool).            *)
(*                                                                         *)
(* One session. A tag is held by an OP (any T-message but Tflush) or by a  *)
(* Tflush. Ops come from two kinds of issuer:                              *)
(*                                                                         *)
(*   Sync  -- a thread that sends one op and waits for its reply. The      *)
(*            server answers it (fairly); a stop may hold the THREAD       *)
(*            forever; a death abandons the op (the #845 Tflush).          *)
(*   Async -- a Loom ring op or a dev9p poll arm: no thread waits on it,   *)
(*            and the server may defer its reply forever.                  *)
(*                                                                         *)
(* The voted rules, each with a buggy cfg that removes it:                 *)
(*                                                                         *)
(*   OpsMax   -- ops may hold at most OpsMax tags, and a Tflush takes any  *)
(*               free tag. Each op has at most one Tflush and keeps its    *)
(*               tag until that Rflush, so Flushes <= Ops, and with        *)
(*               Limit >= 2 * OpsMax a Tflush always finds a tag.          *)
(*               (BUGGY_NO_FLUSH_HEADROOM: ops take any free tag ->        *)
(*               FlushAlwaysFits fails: an abandon finds the pool full.)   *)
(*   AsyncMax -- async ops hold at most AsyncMax < OpsMax of the op share. *)
(*               (BUGGY_NO_ASYNC_CAP: deferred async ops take the whole    *)
(*               share -> SyncProgress fails.)                             *)
(*   D        -- the reader that reads a sync reply applies it, so the tag *)
(*               is free when the reply is read. (BUGGY_WAITER_APPLIES:    *)
(*               the tag stays held until the waiter runs; a stopped       *)
(*               waiter holds it forever -> SyncProgress fails.)           *)
(*                                                                         *)
(* A sync op that finds no op tag waits (rule B); SyncProgress is the      *)
(* claim that the wait ends.                                               *)
(*                                                                         *)
(* Abstractions, said so a green reads no larger:                          *)
(*   - Tags are counted, not named: 9p_client.tla owns tag identity (I-10) *)
(*     and the fid lifecycle.                                              *)
(*   - Sync replies are fair. A sync op the server defers (a read on an    *)
(*     empty pipe) is a thread that waits by design; it holds one tag, so  *)
(*     exhausting the op share beyond AsyncMax takes OpsMax - AsyncMax     *)
(*     such threads on one session (16383 at P9_OPS_MAX / P9_ASYNC_MAX).   *)
(*   - A stop matters only to a thread that holds a tag or a stored        *)
(*     reply: a waiter with no tag parks holding nothing, so Stop is       *)
(*     enabled only from "sent"/"stored"/"done".                           *)
(*   - A caught note's flush(5) holds the same two tags as a death's       *)
(*     abandon (op + Tflush, both until the Rflush); Die models both.      *)
(*   - Take is strongly fair: a waiter woken on every freed tag eventually *)
(*     wins one that stays free long enough to take (the wake-all          *)
(*     send-progress list; no FIFO).                                       *)
(*                                                                         *)
(* Checked by specs/check-tag-pool.sh (counts pinned).                     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Sync, Async, OpsMax, AsyncMax, Limit,
          BUGGY_NO_ASYNC_CAP, BUGGY_WAITER_APPLIES, BUGGY_NO_FLUSH_HEADROOM

ASSUME OpsMax \in Nat \ {0} /\ AsyncMax \in Nat /\ AsyncMax < OpsMax
ASSUME Limit \in Nat /\ (~BUGGY_NO_FLUSH_HEADROOM => Limit >= 2 * OpsMax)

VARIABLES st,       \* per sync thread: where its op is
          stopped,  \* per sync thread: a stop holds the thread
          ast       \* per async issuer: "idle" or "held"

vars == <<st, stopped, ast>>

SyncStates == {"idle", "want", "sent", "stored", "done",
               "flushing", "noflush", "gone"}

\* An op holds its tag from the send until it is answered: a sync op while
\* sent, while its reply waits unapplied (buggy), and after an abandon until
\* the Rflush (or, flush-less, the late original reply).
HoldsOp(s) == st[s] \in {"sent", "stored", "flushing", "noflush"}
SyncOps    == Cardinality({s \in Sync : HoldsOp(s)})
AsyncOps   == Cardinality({a \in Async : ast[a] = "held"})
Ops        == SyncOps + AsyncOps
Flushes    == Cardinality({s \in Sync : st[s] = "flushing"})
Used       == Ops + Flushes

\* Admission for an op. With the headroom rule, Ops < OpsMax already implies
\* Used < Limit (Flushes <= Ops).
OpRoom == IF BUGGY_NO_FLUSH_HEADROOM THEN Used < Limit ELSE Ops < OpsMax

TypeOK ==
    /\ st \in [Sync -> SyncStates]
    /\ stopped \in [Sync -> BOOLEAN]
    /\ ast \in [Async -> {"idle", "held"}]

Init ==
    /\ st = [s \in Sync |-> "idle"]
    /\ stopped = [s \in Sync |-> FALSE]
    /\ ast = [a \in Async |-> "idle"]

Set(s, v) == st' = [st EXCEPT ![s] = v]

Begin(s)   == st[s] = "idle" /\ Set(s, "want") /\ UNCHANGED <<stopped, ast>>

\* Rule B: a sync op with no tag waits; it takes one when the share has room.
Take(s)    == st[s] = "want" /\ OpRoom /\ Set(s, "sent") /\ UNCHANGED <<stopped, ast>>

\* The wait is killable: a killed waiter leaves holding nothing.
Quit(s)    == st[s] = "want" /\ Set(s, "gone") /\ UNCHANGED <<stopped, ast>>

\* Rule D: the reader applies the reply at once; the tag is free. Buggy: the
\* reply is stored and the tag waits for the waiter.
ReplySync(s) ==
    /\ st[s] = "sent"
    /\ Set(s, IF BUGGY_WAITER_APPLIES THEN "stored" ELSE "done")
    /\ UNCHANGED <<stopped, ast>>

Apply(s)   == st[s] = "stored" /\ ~stopped[s] /\ Set(s, "done") /\ UNCHANGED <<stopped, ast>>

Finish(s)  == st[s] = "done" /\ ~stopped[s] /\ Set(s, "idle") /\ UNCHANGED <<stopped, ast>>

\* A death abandons the op: a Tflush on a free tag, or, with none, the
\* flush-less abandon whose tag only the late original reply frees.
Die(s) ==
    /\ st[s] = "sent"
    /\ Set(s, IF Used < Limit THEN "flushing" ELSE "noflush")
    /\ UNCHANGED <<stopped, ast>>

\* The Rflush frees the op's tag and the flush's together.
Rflush(s)    == st[s] = "flushing" /\ Set(s, "gone") /\ UNCHANGED <<stopped, ast>>

\* A flush-less abandon of an op the server defers may never be answered.
LateReply(s) == st[s] = "noflush" /\ Set(s, "gone") /\ UNCHANGED <<stopped, ast>>

Stop(s) ==
    /\ ~stopped[s]
    /\ st[s] \in {"sent", "stored", "done"}
    /\ stopped' = [stopped EXCEPT ![s] = TRUE]
    /\ UNCHANGED <<st, ast>>

Resume(s) ==
    /\ stopped[s]
    /\ stopped' = [stopped EXCEPT ![s] = FALSE]
    /\ UNCHANGED <<st, ast>>

\* An async submit past its share completes with the retryable EAGAIN:
\* nothing is held, so the refusal is no step at all.
Submit(a) ==
    /\ ast[a] = "idle"
    /\ OpRoom
    /\ (BUGGY_NO_ASYNC_CAP \/ AsyncOps < AsyncMax)
    /\ ast' = [ast EXCEPT ![a] = "held"]
    /\ UNCHANGED <<st, stopped>>

ReplyAsync(a) == ast[a] = "held" /\ ast' = [ast EXCEPT ![a] = "idle"] /\ UNCHANGED <<st, stopped>>

Next ==
    \/ \E s \in Sync :
          \/ Begin(s) \/ Take(s) \/ Quit(s) \/ ReplySync(s) \/ Apply(s)
          \/ Finish(s) \/ Die(s) \/ Rflush(s) \/ LateReply(s)
          \/ Stop(s) \/ Resume(s)
    \/ \E a \in Async : Submit(a) \/ ReplyAsync(a)

\* The server answers sync ops and flushes; a waiter keeps trying; a thread
\* that is not stopped runs. No fairness on: async replies (a server may
\* defer them forever), Resume (a stop may last forever), a flush-less op's
\* late reply, Die, Quit, Begin, Submit.
Fairness ==
    \A s \in Sync :
        /\ SF_vars(Take(s))
        /\ WF_vars(ReplySync(s))
        /\ WF_vars(Apply(s))
        /\ WF_vars(Rflush(s))

Spec == Init /\ [][Next]_vars /\ Fairness

----
\* Safety.

TagsFit == Used <= Limit

\* Every abandon finds a tag for its Tflush: no op is left holding a tag that
\* only a reply the server may never send can free.
FlushAlwaysFits == \A s \in Sync : st[s] # "noflush"

Safety == TypeOK /\ TagsFit /\ FlushAlwaysFits

\* Liveness: a sync op waiting for a tag gets one (or is killed).
SyncProgress == \A s \in Sync : st[s] = "want" ~> st[s] # "want"
====
