------------------------------ MODULE poll_walk ------------------------------
(***************************************************************************)
(* The hook list's walk in batches (operator vote 12, 2026-10-27).          *)
(*                                                                         *)
(* kernel/poll.c's poll_waiter_list_wake used to walk a whole hook list    *)
(* under one irqsave hold of the list lock, and nothing bounds how many    *)
(* hooks a list carries: every poll call and every multi-waiter (child,    *)
(* reader, role, CQ, send waiters) of every thread that can reach the      *)
(* object. So a hostile poller set the length of a producer's masked span. *)
(* The walk now reaches at most K hooks per hold. Between holds it leaves  *)
(* a BOOKMARK -- a poll_waiter of its own, on the producer's stack, that   *)
(* no poller owns -- in the list behind the last hook it reached, drops    *)
(* the lock (IRQs back on, the producer preemptible), retakes it and goes  *)
(* on from the bookmark.                                                   *)
(*                                                                         *)
(* This module checks the list mechanics alone: one list, the hooks that   *)
(* come and go on it, and the producers walking it. Whether a reached hook *)
(* wakes its poller in time is poll.tla's question (its Reach action is    *)
(* this module's batch, abstracted).                                       *)
(*                                                                         *)
(* WHAT A WALK OWES. A producer walks the list after its readiness         *)
(* mutation became visible (the poll.h contract). A hook listed by then    *)
(* must be reached; a hook registered later sampled the mutation itself    *)
(* (register-then-observe), so it need not be. A hook that leaves the      *)
(* list (its poller re-arms, returns or dies) is owed nothing: re-listing  *)
(* it is a NEW registration with a fresh sample. So a walk owes exactly    *)
(* the hooks listed when it began and still listed.                        *)
(*                                                                         *)
(* WHY THE WALK TERMINATES. Register pushes at the HEAD and the walk goes  *)
(* head to tail, so every hook the walk meets past its bookmark is older   *)
(* than the walk: the segment it has left only shrinks. Linux's 2017       *)
(* bookmark walk (2554db916586, removed in v6.7-rc1 by b0b598ee08f9) could *)
(* livelock because its waiters re-queued behind the bookmark;             *)
(* BUGGY_TAIL_INSERT is that shape here.                                   *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                    *)
(*  - Each batch is one atomic step: it runs entirely under the list lock, *)
(*    as do Register and Unregister.                                        *)
(*  - A walker is a producer inside one poll_waiter_list_wake call; it may *)
(*    start a new walk any time it is idle. Two walkers show every         *)
(*    interaction of two bookmarks; more add none.                         *)
(*  - A hook is a poller's stack poll_waiter; its identity across an       *)
(*    unregister + register is immaterial (the walk owes neither instance  *)
(*    past the unregister).                                                *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Hooks,               \* the poll_waiter hooks that may be listed
    Walkers,             \* the producers walking the list
    K,                   \* hooks one batch reaches under one hold of the lock
    BUGGY_TAIL_INSERT,   \* TRUE: register appends at the tail
    BUGGY_RESUME_SKIPS,  \* TRUE: a resumed walk starts one element past the
                         \*   bookmark's successor
    BUGGY_MARK_ENDS      \* TRUE: another walker's bookmark is taken for the
                         \*   end of the list

ASSUME Hooks # {} /\ Walkers # {} /\ Hooks \cap Walkers = {}
ASSUME K \in Nat \ {0}
ASSUME BUGGY_TAIL_INSERT  \in BOOLEAN
ASSUME BUGGY_RESUME_SKIPS \in BOOLEAN
ASSUME BUGGY_MARK_ENDS    \in BOOLEAN

VARIABLES
    lst,      \* the list, head first: a sequence of elements (Elems)
    wpc,      \* [Walkers -> {"idle", "parked"}]: parked = between two
              \*   batches, its bookmark in lst
    owe,      \* [Walkers -> SUBSET Hooks]: listed when the walk began and
              \*   neither reached by it nor unregistered since
    strayed   \* [Walkers -> BOOLEAN]: ghost -- the walk reached a hook it
              \*   did not owe (one registered after it began, or one it had
              \*   already reached)

vars == <<lst, wpc, owe, strayed>>

El(kind, id) == [kind |-> kind, id |-> id]
Elems == {El("hook", h) : h \in Hooks} \cup {El("mark", w) : w \in Walkers}

Indices(s)     == 1..Len(s)
In(e)          == \E i \in Indices(lst) : lst[i] = e
Pos(s, e)      == CHOOSE i \in Indices(s) : s[i] = e
Remove(s, i)   == SubSeq(s, 1, i - 1) \o SubSeq(s, i + 1, Len(s))
\* After index i; i = 0 is the head.
Insert(s, i, e) == SubSeq(s, 1, i) \o <<e>> \o SubSeq(s, i + 1, Len(s))
HooksIn(s)     == {s[i].id : i \in {j \in Indices(s) : s[j].kind = "hook"}}

TypeOk ==
    /\ lst \in Seq(Elems)
    /\ \A i, j \in Indices(lst) : i # j => lst[i] # lst[j]
    /\ wpc \in [Walkers -> {"idle", "parked"}]
    /\ owe \in [Walkers -> SUBSET Hooks]
    /\ strayed \in [Walkers -> BOOLEAN]

Init ==
    /\ lst     = <<>>
    /\ wpc     = [w \in Walkers |-> "idle"]
    /\ owe     = [w \in Walkers |-> {}]
    /\ strayed = [w \in Walkers |-> FALSE]

(***************************************************************************)
(* Register -- a poller lists its hook (poll_waiter_list_register).        *)
(***************************************************************************)
Register(h) ==
    /\ ~In(El("hook", h))
    /\ lst' = IF BUGGY_TAIL_INSERT THEN Append(lst, El("hook", h))
              ELSE <<El("hook", h)>> \o lst
    /\ UNCHANGED <<wpc, owe, strayed>>

(***************************************************************************)
(* Unregister -- a poller takes its hook off (poll_waiter_list_unregister),*)
(* wherever it is, bookmarks or not. No walk owes it anything after.       *)
(***************************************************************************)
Unregister(h) ==
    /\ In(El("hook", h))
    /\ lst' = Remove(lst, Pos(lst, El("hook", h)))
    /\ owe' = [w \in Walkers |-> owe[w] \ {h}]
    /\ UNCHANGED <<wpc, strayed>>

(***************************************************************************)
(* Batch -- one hold of the list lock by walker w. An idle walker begins   *)
(* (it owes every hook listed now) at the head; a parked one takes its     *)
(* bookmark out and goes on from the element that followed it. It reaches *)
(* up to K hooks, stepping over other walkers' bookmarks. Past the last    *)
(* hook the walk is over; otherwise its bookmark goes back in behind the   *)
(* K-th hook it reached and the lock drops.                                *)
(***************************************************************************)
Batch(w) ==
    LET resume == wpc[w] = "parked"
        at     == IF resume THEN Pos(lst, El("mark", w)) ELSE 0
        base   == IF resume THEN Remove(lst, at) ELSE lst
        \* In base, the element that followed the bookmark sits at `at`.
        from   == IF ~resume THEN 1
                  ELSE at + (IF BUGGY_RESUME_SKIPS THEN 1 ELSE 0)
        span   == {j \in from..Len(base) : base[j].kind = "mark"}
        stop   == IF BUGGY_MARK_ENDS /\ span # {}
                  THEN (CHOOSE j \in span : \A i \in span : j <= i) - 1
                  ELSE Len(base)
        cands  == {j \in from..stop : base[j].kind = "hook"}
        took   == {j \in cands : Cardinality({i \in cands : i < j}) < K}
        got    == {base[j].id : j \in took}
        owed   == IF resume THEN owe[w] ELSE HooksIn(lst)
        last   == CHOOSE j \in took : \A i \in took : i <= j
        done   == Cardinality(cands) <= K
    IN
    /\ owe'     = [owe EXCEPT ![w] = owed \ got]
    /\ strayed' = [strayed EXCEPT ![w] = strayed[w] \/ ~(got \subseteq owed)]
    /\ IF done
       THEN /\ lst' = base
            /\ wpc' = [wpc EXCEPT ![w] = "idle"]
       ELSE /\ lst' = Insert(base, last, El("mark", w))
            /\ wpc' = [wpc EXCEPT ![w] = "parked"]

Next ==
    \/ \E h \in Hooks : Register(h) \/ Unregister(h)
    \/ \E w \in Walkers : Batch(w)

Spec == Init /\ [][Next]_vars

\* Every walker's batches are taken eventually (I-8: the producer runs). The
\* hooks come and go with no fairness at all: a poller set may churn forever.
Fairness == \A w \in Walkers : WF_vars(Batch(w))
Spec_Live == Spec /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)
\* A walk that is over reached every hook it owed. Violated by
\* BUGGY_RESUME_SKIPS (the hook after the bookmark is passed over) and
\* BUGGY_MARK_ENDS (the hooks behind another walker's bookmark).
Covered == \A w \in Walkers : wpc[w] = "idle" => owe[w] = {}

\* A walk reaches only hooks it owes, each once: never a hook registered
\* after it began, so its length is bounded by the list it began on.
\* Violated by BUGGY_TAIL_INSERT.
NoStray == \A w \in Walkers : ~strayed[w]

\* A parked walker has exactly one bookmark listed; an idle one has none.
MarkIffParked ==
    \A w \in Walkers : (wpc[w] = "parked") <=> In(El("mark", w))

Invariants == TypeOk /\ Covered /\ NoStray /\ MarkIffParked

(***************************************************************************)
(* =============================== LIVENESS =============================== *)
(***************************************************************************)
\* A walk that has started ends, however the pollers churn. Violated by
\* BUGGY_TAIL_INSERT: a hook re-listed behind the bookmark is met again.
WalkEnds == \A w \in Walkers : (wpc[w] = "parked") ~> (wpc[w] = "idle")

=============================================================================
