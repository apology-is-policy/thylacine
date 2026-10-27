------------------------------ MODULE poll_owed ------------------------------
(***************************************************************************)
(* Walks owed under the process table lock (operator vote 13, 2026-10-27). *)
(*                                                                         *)
(* g_proc_table_lock is taken IRQSAVE, so a hook walk run under it keeps   *)
(* the CPU's interrupts masked however poll_waiter_list_wake splits it     *)
(* into batches (poll_walk.tla). Two lists were walked there: a Proc's     *)
(* child_waiters and its notes queue's poll_list. They were walked under   *)
(* the lock for a reason: the lock is what keeps their owner alive (Procs  *)
(* have no refcount; proc.c's LOCK ORDER note, R5-H F75). So the walk      *)
(* cannot simply move after the unlock: the owner could be reaped and      *)
(* freed under it.                                                         *)
(*                                                                         *)
(* THE MECHANISM. An event under the lock does not walk: it OWES a walk,   *)
(* chaining the list's owed record on the holder's own chain (once; a      *)
(* record already chained anywhere stays where it is). Every release of    *)
(* the lock drains the holder's chain: still under the lock it pops one    *)
(* record and lists its bookmark (a stack poll_waiter) at the HEAD of that *)
(* list -- the walk BEGINS under the lock -- then drops the lock and walks *)
(* from the bookmark in batches, IRQs on and preemptible, and retakes the  *)
(* lock for the next record. The bookmark is the PIN: proc_free waits      *)
(* until no bookmark is on the Proc's lists, and the reap that unlinks the *)
(* Proc cancels its records, under the lock, so no walk can begin on it    *)
(* after.                                                                  *)
(*                                                                         *)
(* WHY THE OWED HOOKS ARE REACHED. A walk owes the hooks listed when its   *)
(* event happened. A record still chained has not begun, and its walk will *)
(* begin later with the bookmark at the head: every hook listed by then is *)
(* behind it. A record already popped has a walk in flight that may have   *)
(* passed some of the newly owed hooks, so the event chains the record     *)
(* again, for a second walk of its own. BUGGY_OWE_JOINS_WALK is the        *)
(* tempting shortcut ("a walk is already under way on that list").         *)
(*                                                                         *)
(* WHY THE DRAIN ENDS. A holder drains only its own chain, which grows     *)
(* only while it holds the lock as an event poster: another holder's event *)
(* on a record this one has chained adds nothing to it (BUGGY_DRAIN_ALL,   *)
(* draining everyone's, is unbounded work for others). Each walk ends      *)
(* (poll_walk.tla WalkEnds: register pushes at the head, ahead of every    *)
(* bookmark).                                                              *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                    *)
(*  - One hold of the table lock is one or more atomic steps by its holder;*)
(*    each batch is one atomic step under the list lock (K = 1 hook per    *)
(*    batch: poll_walk.tla checks the batch size and the bookmark-skipping *)
(*    for any K). Register and Unregister are atomic under the list lock.  *)
(*  - An owner's hooks belong to its own threads, which are gone before    *)
(*    the reap unlinks it (wait_pid and SYS_POLL register only on the      *)
(*    caller's own lists; a zombie has no threads).                        *)
(*  - The reap is one hold of the lock that posts no events; the free runs *)
(*    after it, unlocked (wait_pid frees outside the lock).                *)
(*  - A walk the lock's holder does not owe (a lock-free producer's) is    *)
(*    poll_walk.tla's; it needs no pin here because its caller keeps the   *)
(*    owner alive.                                                         *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Holders,                  \* threads that take g_proc_table_lock
    Owners,                   \* Procs, each owning one hook list walked
                              \*   under the lock
    H,                        \* hooks per owner's list
    BUGGY_SKIP_DRAIN,         \* TRUE: a release drops the lock without
                              \*   draining the holder's chain
    BUGGY_NO_CANCEL,          \* TRUE: the reap leaves the Proc's record
                              \*   chained
    BUGGY_FREE_NO_WAIT,       \* TRUE: proc_free does not wait for the
                              \*   bookmarks to leave the Proc's lists
    BUGGY_BEGIN_AFTER_UNLOCK, \* TRUE: the drain pops the record under the
                              \*   lock but lists the bookmark after the drop
    BUGGY_OWE_JOINS_WALK,     \* TRUE: an event on a list whose walk is in
                              \*   flight chains nothing
    BUGGY_DRAIN_ALL           \* TRUE: a release drains every chained
                              \*   record, whoever chained it

ASSUME Holders # {} /\ Owners # {} /\ Holders \cap Owners = {}
ASSUME H \in Nat \ {0}
ASSUME \A b \in {BUGGY_SKIP_DRAIN, BUGGY_NO_CANCEL, BUGGY_FREE_NO_WAIT,
                 BUGGY_BEGIN_AFTER_UNLOCK, BUGGY_OWE_JOINS_WALK,
                 BUGGY_DRAIN_ALL} : b \in BOOLEAN

Hooks == Owners \X (1..H)    \* hook <<o, i>> is listed only on o's list

VARIABLES
    tbl,        \* the table lock's holder, or "none"
    hpc,        \* [Holders -> HolderStates]
    cur,        \* [Holders -> Owners \cup {"none"}]: the record the holder
                \*   popped and is walking
    chainedOn,  \* [Owners -> Holders \cup {"none"}]: whose chain holds the
                \*   owner's record
    lst,        \* [Owners -> Seq(Elems)]: each list, head first
    ost,        \* [Owners -> {"alive", "unlinked", "freed"}]
    owed,       \* [Hooks -> BOOLEAN]: ghost -- an event happened while the
                \*   hook was listed and no walk has reached it since
    uaf         \* ghost: a walk touched a freed owner's list or record

vars == <<tbl, hpc, cur, chainedOn, lst, ost, owed, uaf>>

\* idle: not in the lock; held: holding it, posting events; popped: the
\* record popped, the lock dropped, no bookmark yet (BUGGY_BEGIN_AFTER_UNLOCK
\* only); walk: walking `cur` from its bookmark, unlocked; drain: between two
\* walks, waiting for the lock; reheld: holding it again to pop the next.
HolderStates == {"idle", "held", "popped", "walk", "drain", "reheld"}

El(kind, id) == [kind |-> kind, id |-> id]
Elems == {El("hook", k) : k \in Hooks} \cup {El("mark", h) : h \in Holders}

Indices(s)      == 1..Len(s)
In(s, e)        == \E i \in Indices(s) : s[i] = e
Pos(s, e)       == CHOOSE i \in Indices(s) : s[i] = e
Remove(s, i)    == SubSeq(s, 1, i - 1) \o SubSeq(s, i + 1, Len(s))
\* After index i; i = 0 is the head.
Insert(s, i, e) == SubSeq(s, 1, i) \o <<e>> \o SubSeq(s, i + 1, Len(s))
Min(S)          == CHOOSE j \in S : \A i \in S : j <= i

Listed(k) == In(lst[k[1]], El("hook", k))
Mine(h)   == {o \in Owners : chainedOn[o] = h}
Owes(h)   == IF BUGGY_DRAIN_ALL THEN {o \in Owners : chainedOn[o] # "none"}
             ELSE Mine(h)
Walking(o) == \E g \in Holders : hpc[g] \in {"popped", "walk"} /\ cur[g] = o

TypeOk ==
    /\ tbl \in Holders \cup {"none"}
    /\ hpc \in [Holders -> HolderStates]
    /\ cur \in [Holders -> Owners \cup {"none"}]
    /\ chainedOn \in [Owners -> Holders \cup {"none"}]
    /\ \A o \in Owners : lst[o] \in Seq(Elems)
                         /\ \A i, j \in Indices(lst[o]) : i # j => lst[o][i] # lst[o][j]
    /\ ost \in [Owners -> {"alive", "unlinked", "freed"}]
    /\ owed \in [Hooks -> BOOLEAN]
    /\ uaf \in BOOLEAN

Init ==
    /\ tbl       = "none"
    /\ hpc       = [h \in Holders |-> "idle"]
    /\ cur       = [h \in Holders |-> "none"]
    /\ chainedOn = [o \in Owners |-> "none"]
    /\ lst       = [o \in Owners |-> <<>>]
    /\ ost       = [o \in Owners |-> "alive"]
    /\ owed      = [k \in Hooks |-> FALSE]
    /\ uaf       = FALSE

(***************************************************************************)
(* The table lock. A holder takes it to post events (from idle) or to pop  *)
(* its next owed record (from drain).                                      *)
(***************************************************************************)
Acquire(h) ==
    /\ tbl = "none"
    /\ hpc[h] \in {"idle", "drain"}
    /\ tbl' = h
    /\ hpc' = [hpc EXCEPT ![h] = IF hpc[h] = "idle" THEN "held" ELSE "reheld"]
    /\ UNCHANGED <<cur, chainedOn, lst, ost, owed, uaf>>

\* An event under the lock that makes o's hooks' condition true (a child's
\* death, a note posted): o is reachable, so not yet reaped. It owes every
\* hook listed now, and chains o's record unless it is chained already.
Event(h, o) ==
    /\ tbl = h /\ hpc[h] = "held"
    /\ ost[o] = "alive"
    /\ owed' = [k \in Hooks |-> owed[k] \/ (k[1] = o /\ Listed(k))]
    /\ chainedOn' = IF chainedOn[o] # "none" \/ (BUGGY_OWE_JOINS_WALK /\ Walking(o))
                    THEN chainedOn
                    ELSE [chainedOn EXCEPT ![o] = h]
    /\ UNCHANGED <<tbl, hpc, cur, lst, ost, uaf>>

\* The release wrapper: with nothing owed, the lock drops; otherwise one
\* record is popped and its walk begins (the bookmark at the head) before
\* the lock drops.
Release(h) ==
    /\ tbl = h /\ hpc[h] \in {"held", "reheld"}
    /\ tbl' = "none"
    /\ IF Owes(h) = {} \/ BUGGY_SKIP_DRAIN
       THEN /\ hpc' = [hpc EXCEPT ![h] = "idle"]
            /\ UNCHANGED <<cur, chainedOn, lst, uaf>>
       ELSE \E o \in Owes(h) :
            /\ chainedOn' = [chainedOn EXCEPT ![o] = "none"]
            /\ cur' = [cur EXCEPT ![h] = o]
            /\ IF BUGGY_BEGIN_AFTER_UNLOCK
               THEN /\ hpc' = [hpc EXCEPT ![h] = "popped"]
                    /\ UNCHANGED <<lst, uaf>>
               ELSE /\ hpc' = [hpc EXCEPT ![h] = "walk"]
                    /\ lst' = [lst EXCEPT ![o] = <<El("mark", h)>> \o @]
                    /\ uaf' = (uaf \/ ost[o] = "freed")
    /\ UNCHANGED <<ost, owed>>

\* BUGGY_BEGIN_AFTER_UNLOCK only: the bookmark goes in after the drop.
Begin(h) ==
    /\ hpc[h] = "popped"
    /\ LET o == cur[h] IN
       /\ lst' = [lst EXCEPT ![o] = <<El("mark", h)>> \o @]
       /\ uaf' = (uaf \/ ost[o] = "freed")
    /\ hpc' = [hpc EXCEPT ![h] = "walk"]
    /\ UNCHANGED <<tbl, cur, chainedOn, ost, owed>>

\* One batch of the walk, unlocked: the bookmark comes out and goes back in
\* behind the next hook (stepping over other walks' bookmarks), or the walk
\* is over and the holder goes back for the lock.
Batch(h) ==
    /\ hpc[h] = "walk"
    /\ LET o    == cur[h]
           at   == Pos(lst[o], El("mark", h))
           base == Remove(lst[o], at)
           nxt  == {j \in at..Len(base) : base[j].kind = "hook"}
       IN /\ uaf' = (uaf \/ ost[o] = "freed")
          /\ IF nxt = {}
             THEN /\ lst' = [lst EXCEPT ![o] = base]
                  /\ hpc' = [hpc EXCEPT ![h] = "drain"]
                  /\ cur' = [cur EXCEPT ![h] = "none"]
                  /\ UNCHANGED owed
             ELSE LET j == Min(nxt) IN
                  /\ owed' = [owed EXCEPT ![base[j].id] = FALSE]
                  /\ lst' = [lst EXCEPT ![o] = Insert(base, j, El("mark", h))]
                  /\ UNCHANGED <<hpc, cur>>
    /\ UNCHANGED <<tbl, chainedOn, ost>>

(***************************************************************************)
(* The owner's threads list and unlist their hooks while it lives. A fresh *)
(* registration samples the condition itself (register-then-observe), so   *)
(* no earlier event owes it anything.                                      *)
(***************************************************************************)
Register(k) ==
    /\ ost[k[1]] = "alive"
    /\ ~Listed(k)
    /\ lst' = [lst EXCEPT ![k[1]] = <<El("hook", k)>> \o @]
    /\ owed' = [owed EXCEPT ![k] = FALSE]
    /\ UNCHANGED <<tbl, hpc, cur, chainedOn, ost, uaf>>

Unregister(k) ==
    /\ Listed(k)
    /\ lst' = [lst EXCEPT ![k[1]] = Remove(@, Pos(@, El("hook", k)))]
    /\ owed' = [owed EXCEPT ![k] = FALSE]
    /\ UNCHANGED <<tbl, hpc, cur, chainedOn, ost, uaf>>

(***************************************************************************)
(* The reap: one hold of the lock that unlinks a Proc whose threads are    *)
(* gone, and cancels its owed record. Then the free, unlocked, once no     *)
(* bookmark is on its list.                                                *)
(***************************************************************************)
Unlink(o) ==
    /\ tbl = "none"
    /\ ost[o] = "alive"
    /\ \A i \in 1..H : ~Listed(<<o, i>>)
    /\ ost' = [ost EXCEPT ![o] = "unlinked"]
    /\ chainedOn' = IF BUGGY_NO_CANCEL THEN chainedOn
                    ELSE [chainedOn EXCEPT ![o] = "none"]
    /\ UNCHANGED <<tbl, hpc, cur, lst, owed, uaf>>

Free(o) ==
    /\ ost[o] = "unlinked"
    /\ BUGGY_FREE_NO_WAIT \/ lst[o] = <<>>
    /\ ost' = [ost EXCEPT ![o] = "freed"]
    /\ UNCHANGED <<tbl, hpc, cur, chainedOn, lst, owed, uaf>>

Next ==
    \/ \E h \in Holders : Acquire(h) \/ Release(h) \/ Begin(h) \/ Batch(h)
    \/ \E h \in Holders, o \in Owners : Event(h, o)
    \/ \E k \in Hooks : Register(k) \/ Unregister(k)
    \/ \E o \in Owners : Unlink(o) \/ Free(o)

Spec == Init /\ [][Next]_vars

\* A holder makes progress through its hold and its drain (I-8), and gets
\* the lock back for its next record however others contend for it. Nothing
\* else is fair: hooks churn, events and reaps happen or not.
Fairness ==
    \A h \in Holders :
        /\ WF_vars(Release(h))
        /\ WF_vars(Begin(h))
        /\ WF_vars(Batch(h))
        /\ SF_vars(hpc[h] = "drain" /\ Acquire(h))
Spec_Live == Spec /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)
\* No walk touches a freed Proc's list or record. Violated by
\* BUGGY_NO_CANCEL (a stale record popped), BUGGY_FREE_NO_WAIT (a batch on
\* a freed list) and BUGGY_BEGIN_AFTER_UNLOCK (a bookmark listed on one).
NoUAF == ~uaf

\* A holder out of the lock owes nothing. Violated by BUGGY_SKIP_DRAIN.
ChainDrained == \A h \in Holders : hpc[h] = "idle" => Mine(h) = {}

\* Every owed hook is still to be reached: its list's record is chained (a
\* walk that will begin with its bookmark at the head), or a walk in flight
\* has its bookmark ahead of it. Violated by BUGGY_OWE_JOINS_WALK.
OwedCovered ==
    \A k \in Hooks : owed[k] =>
        \/ chainedOn[k[1]] # "none"
        \/ \E h \in Holders :
              /\ cur[h] = k[1]
              /\ \/ hpc[h] = "popped"
                 \/ /\ hpc[h] = "walk"
                    /\ Pos(lst[k[1]], El("mark", h)) < Pos(lst[k[1]], El("hook", k))

OwedIsListed == \A k \in Hooks : owed[k] => Listed(k)

\* A walking holder has exactly one bookmark listed, on the list it walks.
MarkIffWalking ==
    \A h \in Holders, o \in Owners :
        In(lst[o], El("mark", h)) <=> (hpc[h] = "walk" /\ cur[h] = o)

Invariants ==
    /\ TypeOk /\ NoUAF /\ ChainDrained /\ OwedCovered /\ OwedIsListed
    /\ MarkIffWalking

(***************************************************************************)
(* =============================== LIVENESS =============================== *)
(***************************************************************************)
\* Every owed hook is reached, or leaves the list (I-9).
OwedReached == \A k \in Hooks : owed[k] ~> ~owed[k]

\* Every hold of the lock ends with the holder out of it, its chain drained.
\* Violated by BUGGY_DRAIN_ALL: a holder that takes up other holders' records
\* can be kept draining for as long as they keep posting.
DrainEnds == \A h \in Holders : hpc[h] # "idle" ~> hpc[h] = "idle"

=============================================================================
