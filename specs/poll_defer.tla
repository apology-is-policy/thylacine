------------------------------ MODULE poll_defer ------------------------------
(***************************************************************************)
(* Walks owed under the 9P client's lock (operator vote 13, 2026-10-27).   *)
(*                                                                         *)
(* Every hook walk the 9P client reaches with its c->lock held -- its own  *)
(* send and role lists, a transport endpoint's (a srvconn's, a pipe        *)
(* ring's), a Loom's CQ list from an async completion -- runs with the     *)
(* lock held: the CPU cannot be preempted, and every other user of the     *)
(* session spins. Some of these run under g_dev9p_poll_lock as well, which *)
(* is taken around the client's lock. The walks move after the OUTERMOST   *)
(* of those locks drops.                                                   *)
(*                                                                         *)
(* THE MECHANISM. A thread inside a deferring lock (the client's or the    *)
(* poll lock) does not walk: it OWES the walk, chaining the list's owed    *)
(* record on its own private chain. The release of its outermost deferring *)
(* lock drains the chain: for each record, under the LIST's lock, it takes *)
(* the record off its chain and lists a bookmark at the head (the walk     *)
(* begins), then walks from the bookmark in batches, holding nothing.      *)
(*                                                                         *)
(* HOW THIS DIFFERS FROM poll_owed.tla. There, one lock (the process       *)
(* table's) serializes every chain and every record, and its holder keeps  *)
(* the owner alive. Here:                                                  *)
(*  - The holders hold DIFFERENT locks (one per client), and one record    *)
(*    can be owed under two of them: a Loom whose ops run on two clients.  *)
(*    So the record's chain field is tested and set under the list's own   *)
(*    lock, and taking it off a chain clears the field and lists the       *)
(*    bookmark in the same hold (BUGGY_SPLIT_OWE, BUGGY_SPLIT_POP). The    *)
(*    chain's links are touched only by the thread that owns the chain.    *)
(*  - Nothing holds the owner alive across the drain, so each owner is     *)
(*    kept alive one of two ways:                                          *)
(*    * a REF owner (a srvconn, a pipe ring) is refcounted: the owe takes  *)
(*      a reference -- the owing thread holds one already, its             *)
(*      transport's, which a close drops under the lock -- and the walk's  *)
(*      end drops it (BUGGY_NO_GET, BUGGY_PUT_AT_POP);                     *)
(*    * a WAIT owner (a Loom, a client) is freed only once it is           *)
(*      unreachable to every poster, and its free waits until its record   *)
(*      is on no chain and no bookmark is on its list                      *)
(*      (BUGGY_FREE_IGNORES_CHAIN).                                        *)
(*  - The deferring locks nest (the poll lock around the client's), and    *)
(*    only the outermost release drains (BUGGY_DRAIN_INNER).               *)
(*                                                                         *)
(* WHY THE OWED HOOKS ARE REACHED is poll_owed.tla's argument: a record    *)
(* still chained has not begun, and its walk will begin with the bookmark  *)
(* at the head; an event on a record whose walk is in flight chains it     *)
(* again (BUGGY_OWE_JOINS_WALK).                                           *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                    *)
(*  - No lock excludes one holder from another: each may hold its own      *)
(*    client's lock. Two holders of one client are serialized in the code; *)
(*    the model lets them interleave, which only adds behaviours.          *)
(*  - The drain holds no deferring lock and posts no owed event. A put     *)
(*    that frees a REF owner walks its (empty) lists inline: poll_walk.tla.*)
(*  - Register, Unregister and each batch are atomic under the list lock   *)
(*    (K = 1 hook per batch; poll_walk.tla checks any K). A hook on a REF  *)
(*    owner holds a reference (the poller's handle).                       *)
(*  - A WAIT owner's free runs in a thread whose own chain is empty (the   *)
(*    code checks: a thread with a non-empty chain holds a spinlock, and   *)
(*    the free may sleep).                                                 *)
(*  - Posts no deferring lock covers (lock-free producers, and the drain's *)
(*    own puts) walk at once: poll_walk.tla.                               *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Holders,                  \* threads that take the deferring locks
    RefOwners,                \* owners kept alive by a reference
    WaitOwners,               \* owners whose free waits out their walks
    H,                        \* hooks per owner's list
    BUGGY_NO_GET,             \* TRUE: an owe on a REF owner takes no ref
    BUGGY_PUT_AT_POP,         \* TRUE: the drain drops the ref when it takes
                              \*   the record, before the walk
    BUGGY_FREE_IGNORES_CHAIN, \* TRUE: a WAIT owner's free waits for the
                              \*   bookmarks only, not for a chained record
    BUGGY_SPLIT_POP,          \* TRUE: the drain clears the record's chain
                              \*   field in one hold of the list lock and
                              \*   lists the bookmark in the next
    BUGGY_SPLIT_OWE,          \* TRUE: an owe tests the chain field in one
                              \*   hold of the list lock and sets it in the
                              \*   next
    BUGGY_DRAIN_INNER,        \* TRUE: every release drains, not only the
                              \*   outermost
    BUGGY_SKIP_DRAIN,         \* TRUE: no release drains
    BUGGY_OWE_JOINS_WALK      \* TRUE: an event on a list whose walk is in
                              \*   flight chains nothing

Owners == RefOwners \cup WaitOwners

ASSUME Holders # {} /\ Owners # {} /\ RefOwners \cap WaitOwners = {}
ASSUME Holders \cap Owners = {}
ASSUME H \in Nat \ {0}
ASSUME \A b \in {BUGGY_NO_GET, BUGGY_PUT_AT_POP, BUGGY_FREE_IGNORES_CHAIN,
                 BUGGY_SPLIT_POP, BUGGY_SPLIT_OWE, BUGGY_DRAIN_INNER,
                 BUGGY_SKIP_DRAIN, BUGGY_OWE_JOINS_WALK} : b \in BOOLEAN

Hooks    == Owners \X (1..H)  \* hook <<o, i>> is listed only on o's list
MaxDepth == 2                 \* the poll lock around a client's
MaxRef   == 1 + H + 1 + Cardinality(Holders)

VARIABLES
    depth,      \* [Holders -> 0..MaxDepth]: deferring locks held
    pc,         \* [Holders -> PcStates]
    owing,      \* [Holders -> Owners \cup {"none"}]: BUGGY_SPLIT_OWE's record
                \*   between the test and the set
    chain,      \* [Holders -> SUBSET Owners]: the records on each private chain
    chainedOn,  \* [Owners -> Holders \cup {"none"}]: each record's chain
                \*   field, under its list's lock
    cur,        \* [Holders -> Owners \cup {"none"}]: the record being walked
    lst,        \* [Owners -> Seq(Elems)]: each list, head first
    rc,         \* [RefOwners -> 0..MaxRef]: references; 0 is freed
    base,       \* [RefOwners -> BOOLEAN]: the transport's reference held
    wst,        \* [WaitOwners -> {"attached", "detached", "freed"}]
    owed,       \* [Hooks -> BOOLEAN]: ghost -- an event happened while the
                \*   hook was listed and no walk has reached it since
    uaf         \* ghost: something touched a freed owner

vars == <<depth, pc, owing, chain, chainedOn, cur, lst, rc, base, wst,
          owed, uaf>>

\* run: posting or not, holding depth locks; owing: BUGGY_SPLIT_OWE between
\* its two holds; drain: taking the next record; popped: taken, no bookmark
\* yet (BUGGY_SPLIT_POP only); walk: walking `cur` from its bookmark.
PcStates == {"run", "owing", "drain", "popped", "walk"}

El(kind, id) == [kind |-> kind, id |-> id]
Elems == {El("hook", k) : k \in Hooks} \cup {El("mark", h) : h \in Holders}

Indices(s)      == 1..Len(s)
In(s, e)        == \E i \in Indices(s) : s[i] = e
Pos(s, e)       == CHOOSE i \in Indices(s) : s[i] = e
Remove(s, i)    == SubSeq(s, 1, i - 1) \o SubSeq(s, i + 1, Len(s))
\* After index i; i = 0 is the head.
Insert(s, i, e) == SubSeq(s, 1, i) \o <<e>> \o SubSeq(s, i + 1, Len(s))
Min(S)          == CHOOSE j \in S : \A i \in S : j <= i

Listed(k)  == In(lst[k[1]], El("hook", k))
Hooked(o)  == {k \in Hooks : k[1] = o /\ Listed(k)}
Marked(o)  == \E h \in Holders : In(lst[o], El("mark", h))
Walking(o) == \E g \in Holders : pc[g] \in {"popped", "walk"} /\ cur[g] = o

Freed(o) == IF o \in RefOwners THEN rc[o] = 0 ELSE wst[o] = "freed"
\* An event can reach o: its transport still holds it, or it is attached.
Live(o)  == IF o \in RefOwners THEN base[o] ELSE wst[o] = "attached"

\* The owe's reference and the walk's put, on a REF owner.
Takes(o) == o \in RefOwners /\ ~BUGGY_NO_GET
Get(o)   == IF Takes(o) THEN [rc EXCEPT ![o] = @ + 1] ELSE rc
Put(o)   == IF Takes(o) THEN [rc EXCEPT ![o] = @ - 1] ELSE rc

TypeOk ==
    /\ depth \in [Holders -> 0..MaxDepth]
    /\ pc \in [Holders -> PcStates]
    /\ owing \in [Holders -> Owners \cup {"none"}]
    /\ chain \in [Holders -> SUBSET Owners]
    /\ chainedOn \in [Owners -> Holders \cup {"none"}]
    /\ cur \in [Holders -> Owners \cup {"none"}]
    /\ \A o \in Owners : lst[o] \in Seq(Elems)
                         /\ \A i, j \in Indices(lst[o]) : i # j => lst[o][i] # lst[o][j]
    /\ rc \in [RefOwners -> 0..MaxRef]
    /\ base \in [RefOwners -> BOOLEAN]
    /\ wst \in [WaitOwners -> {"attached", "detached", "freed"}]
    /\ owed \in [Hooks -> BOOLEAN]
    /\ uaf \in BOOLEAN

Init ==
    /\ depth     = [h \in Holders |-> 0]
    /\ pc        = [h \in Holders |-> "run"]
    /\ owing     = [h \in Holders |-> "none"]
    /\ chain     = [h \in Holders |-> {}]
    /\ chainedOn = [o \in Owners |-> "none"]
    /\ cur       = [h \in Holders |-> "none"]
    /\ lst       = [o \in Owners |-> <<>>]
    /\ rc        = [o \in RefOwners |-> 1]
    /\ base      = [o \in RefOwners |-> TRUE]
    /\ wst       = [o \in WaitOwners |-> "attached"]
    /\ owed      = [k \in Hooks |-> FALSE]
    /\ uaf       = FALSE

(***************************************************************************)
(* The deferring locks. A release that leaves the thread in none drains    *)
(* its chain.                                                              *)
(***************************************************************************)
Lock(h) ==
    /\ pc[h] = "run" /\ depth[h] < MaxDepth
    /\ depth' = [depth EXCEPT ![h] = @ + 1]
    /\ UNCHANGED <<pc, owing, chain, chainedOn, cur, lst, rc, base, wst, owed, uaf>>

Unlock(h) ==
    /\ pc[h] = "run" /\ depth[h] > 0
    /\ depth' = [depth EXCEPT ![h] = @ - 1]
    /\ pc' = [pc EXCEPT ![h] =
                IF ~BUGGY_SKIP_DRAIN /\ chain[h] # {}
                   /\ (depth[h] = 1 \/ BUGGY_DRAIN_INNER)
                THEN "drain" ELSE "run"]
    /\ UNCHANGED <<owing, chain, chainedOn, cur, lst, rc, base, wst, owed, uaf>>

\* An event inside a deferring lock that makes o's hooks' condition true. It
\* owes every hook listed now and, under o's list lock, chains o's record on
\* the poster's own chain unless the record is on a chain already.
Event(h, o) ==
    /\ pc[h] = "run" /\ depth[h] > 0
    /\ Live(o)
    /\ owed' = [k \in Hooks |-> owed[k] \/ (k[1] = o /\ Listed(k))]
    /\ IF BUGGY_SPLIT_OWE
       THEN IF chainedOn[o] = "none"
            THEN /\ pc'    = [pc EXCEPT ![h] = "owing"]
                 /\ owing' = [owing EXCEPT ![h] = o]
                 /\ UNCHANGED <<chain, chainedOn, rc>>
            ELSE UNCHANGED <<pc, owing, chain, chainedOn, rc>>
       ELSE /\ IF chainedOn[o] # "none" \/ (BUGGY_OWE_JOINS_WALK /\ Walking(o))
               THEN UNCHANGED <<chain, chainedOn, rc>>
               ELSE /\ chainedOn' = [chainedOn EXCEPT ![o] = h]
                    /\ chain'     = [chain EXCEPT ![h] = @ \cup {o}]
                    /\ rc'        = Get(o)
            /\ UNCHANGED <<pc, owing>>
    /\ UNCHANGED <<depth, cur, lst, base, wst, uaf>>

\* BUGGY_SPLIT_OWE only: the second hold sets the field it tested.
OweSet(h) ==
    /\ pc[h] = "owing"
    /\ LET o == owing[h] IN
       /\ chainedOn' = [chainedOn EXCEPT ![o] = h]
       /\ chain'     = [chain EXCEPT ![h] = @ \cup {o}]
       /\ rc'        = Get(o)
       /\ uaf'       = (uaf \/ Freed(o))
    /\ pc'    = [pc EXCEPT ![h] = "run"]
    /\ owing' = [owing EXCEPT ![h] = "none"]
    /\ UNCHANGED <<depth, cur, lst, base, wst, owed>>

\* p9_client_close: the transport's close drops its reference on the
\* endpoint, under the client's lock.
Close(h, o) ==
    /\ pc[h] = "run" /\ depth[h] > 0
    /\ o \in RefOwners /\ base[o]
    /\ base' = [base EXCEPT ![o] = FALSE]
    /\ rc'   = [rc EXCEPT ![o] = @ - 1]
    /\ UNCHANGED <<depth, pc, owing, chain, chainedOn, cur, lst, wst, owed, uaf>>

(***************************************************************************)
(* The drain, holding nothing.                                             *)
(***************************************************************************)
\* Take one record off the chain, and -- in the same hold of its list's
\* lock -- clear its chain field and list the bookmark at the head.
Pop(h) ==
    /\ pc[h] = "drain"
    /\ IF chain[h] = {}
       THEN /\ pc' = [pc EXCEPT ![h] = "run"]
            /\ UNCHANGED <<chain, chainedOn, cur, lst, rc, uaf>>
       ELSE \E o \in chain[h] :
            /\ chain'     = [chain EXCEPT ![h] = @ \ {o}]
            /\ chainedOn' = [chainedOn EXCEPT ![o] = "none"]
            /\ cur'       = [cur EXCEPT ![h] = o]
            /\ rc'        = IF BUGGY_PUT_AT_POP THEN Put(o) ELSE rc
            /\ IF BUGGY_SPLIT_POP
               THEN /\ pc' = [pc EXCEPT ![h] = "popped"]
                    /\ uaf' = (uaf \/ Freed(o))
                    /\ UNCHANGED lst
               ELSE /\ pc'  = [pc EXCEPT ![h] = "walk"]
                    /\ lst' = [lst EXCEPT ![o] = <<El("mark", h)>> \o @]
                    /\ uaf' = (uaf \/ Freed(o))
    /\ UNCHANGED <<depth, owing, base, wst, owed>>

\* BUGGY_SPLIT_POP only: the bookmark goes in at the next hold.
Begin(h) ==
    /\ pc[h] = "popped"
    /\ LET o == cur[h] IN
       /\ lst' = [lst EXCEPT ![o] = <<El("mark", h)>> \o @]
       /\ uaf' = (uaf \/ Freed(o))
    /\ pc' = [pc EXCEPT ![h] = "walk"]
    /\ UNCHANGED <<depth, owing, chain, chainedOn, cur, rc, base, wst, owed>>

\* One batch: the bookmark comes out and goes back in behind the next hook
\* (stepping over other walks' bookmarks), or the walk is over, its
\* reference is dropped, and the drain goes on.
Batch(h) ==
    /\ pc[h] = "walk"
    /\ LET o    == cur[h]
           at   == Pos(lst[o], El("mark", h))
           base0 == Remove(lst[o], at)
           nxt  == {j \in at..Len(base0) : base0[j].kind = "hook"}
       IN /\ uaf' = (uaf \/ Freed(o))
          /\ IF nxt = {}
             THEN /\ lst' = [lst EXCEPT ![o] = base0]
                  /\ pc'  = [pc EXCEPT ![h] = "drain"]
                  /\ cur' = [cur EXCEPT ![h] = "none"]
                  /\ rc'  = IF BUGGY_PUT_AT_POP THEN rc ELSE Put(o)
                  /\ UNCHANGED owed
             ELSE LET j == Min(nxt) IN
                  /\ owed' = [owed EXCEPT ![base0[j].id] = FALSE]
                  /\ lst'  = [lst EXCEPT ![o] = Insert(base0, j, El("mark", h))]
                  /\ UNCHANGED <<pc, cur, rc>>
    /\ UNCHANGED <<depth, owing, chain, chainedOn, base, wst>>

(***************************************************************************)
(* Pollers list and unlist their hooks. A fresh registration samples the   *)
(* condition itself (register-then-observe), so no earlier event owes it.  *)
(* A hook on a REF owner holds a reference; a WAIT owner takes hooks only  *)
(* while attached.                                                         *)
(***************************************************************************)
Register(k) ==
    /\ LET o == k[1] IN
       /\ IF o \in RefOwners THEN rc[o] > 0 ELSE wst[o] = "attached"
       /\ ~Listed(k)
       /\ lst' = [lst EXCEPT ![o] = <<El("hook", k)>> \o @]
       /\ rc'  = IF o \in RefOwners THEN [rc EXCEPT ![o] = @ + 1] ELSE rc
    /\ owed' = [owed EXCEPT ![k] = FALSE]
    /\ UNCHANGED <<depth, pc, owing, chain, chainedOn, cur, base, wst, uaf>>

Unregister(k) ==
    /\ Listed(k)
    /\ LET o == k[1] IN
       /\ lst' = [lst EXCEPT ![o] = Remove(@, Pos(@, El("hook", k)))]
       /\ rc'  = IF o \in RefOwners THEN [rc EXCEPT ![o] = @ - 1] ELSE rc
    /\ owed' = [owed EXCEPT ![k] = FALSE]
    /\ UNCHANGED <<depth, pc, owing, chain, chainedOn, cur, base, wst, uaf>>

(***************************************************************************)
(* A WAIT owner becomes unreachable to every poster once its pollers are   *)
(* gone (a Loom: refcount 0 and its async ops abandoned under their        *)
(* clients' locks; a client: closed, no op in flight). Its free waits,     *)
(* under the list lock, until the record is on no chain and no bookmark is *)
(* listed.                                                                 *)
(***************************************************************************)
Detach(o) ==
    /\ o \in WaitOwners /\ wst[o] = "attached"
    /\ Hooked(o) = {}
    /\ wst' = [wst EXCEPT ![o] = "detached"]
    /\ UNCHANGED <<depth, pc, owing, chain, chainedOn, cur, lst, rc, base, owed, uaf>>

Free(o) ==
    /\ o \in WaitOwners /\ wst[o] = "detached"
    /\ BUGGY_FREE_IGNORES_CHAIN \/ chainedOn[o] = "none"
    /\ ~Marked(o)
    /\ wst' = [wst EXCEPT ![o] = "freed"]
    /\ UNCHANGED <<depth, pc, owing, chain, chainedOn, cur, lst, rc, base, owed, uaf>>

Next ==
    \/ \E h \in Holders :
          Lock(h) \/ Unlock(h) \/ OweSet(h) \/ Pop(h) \/ Begin(h) \/ Batch(h)
    \/ \E h \in Holders, o \in Owners : Event(h, o) \/ Close(h, o)
    \/ \E k \in Hooks : Register(k) \/ Unregister(k)
    \/ \E o \in WaitOwners : Detach(o) \/ Free(o)

Spec == Init /\ [][Next]_vars

\* A hold of a lock ends, and a drain makes progress (I-8); a free whose
\* wait is over goes ahead. Nothing else is fair: hooks churn, events, closes
\* and detaches happen or not.
Fairness ==
    /\ \A h \in Holders :
          /\ WF_vars(Unlock(h))
          /\ WF_vars(OweSet(h))
          /\ WF_vars(Pop(h))
          /\ WF_vars(Begin(h))
          /\ WF_vars(Batch(h))
    /\ \A o \in WaitOwners : WF_vars(Free(o))
Spec_Live == Spec /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)
\* Nothing touches a freed owner. Violated by BUGGY_NO_GET and
\* BUGGY_PUT_AT_POP (a REF owner freed under its walk), and by
\* BUGGY_FREE_IGNORES_CHAIN and BUGGY_SPLIT_POP (a WAIT owner freed with a
\* walk still to begin).
NoUAF == ~uaf

\* A record is on at most one chain, the one its field names. Violated by
\* BUGGY_SPLIT_OWE: two posters both find the field clear and both chain the
\* record, and its links now belong to two threads.
OneChain ==
    \A o \in Owners :
        /\ Cardinality({h \in Holders : o \in chain[h]}) <= 1
        /\ \A h \in Holders : o \in chain[h] => chainedOn[o] = h

\* A thread in no deferring lock owes nothing. Violated by BUGGY_SKIP_DRAIN.
ChainDrained == \A h \in Holders : pc[h] = "run" /\ depth[h] = 0 => chain[h] = {}

\* Every walk runs outside every deferring lock. Violated by
\* BUGGY_DRAIN_INNER: the poll lock is still held.
NoWalkUnderLock ==
    \A h \in Holders : pc[h] \in {"drain", "popped", "walk"} => depth[h] = 0

\* Every owed hook is still to be reached: its record is chained (a walk
\* that will begin with its bookmark at the head), or a walk in flight has
\* its bookmark ahead of it. Violated by BUGGY_OWE_JOINS_WALK.
OwedCovered ==
    \A k \in Hooks : owed[k] =>
        \/ chainedOn[k[1]] # "none"
        \/ \E h \in Holders :
              /\ cur[h] = k[1]
              /\ \/ pc[h] = "popped"
                 \/ /\ pc[h] = "walk"
                    /\ Pos(lst[k[1]], El("mark", h)) < Pos(lst[k[1]], El("hook", k))

OwedIsListed == \A k \in Hooks : owed[k] => Listed(k)

\* A walking thread has exactly one bookmark listed, on the list it walks.
MarkIffWalking ==
    \A h \in Holders, o \in Owners :
        In(lst[o], El("mark", h)) <=> (pc[h] = "walk" /\ cur[h] = o)

\* A REF owner's count is exactly its holders: the transport, each hook,
\* the chained record, each walk. No reference leaks, none is missing.
RefExact ==
    \A o \in RefOwners :
        rc[o] = (IF base[o] THEN 1 ELSE 0)
                + Cardinality(Hooked(o))
                + Cardinality({h \in Holders : o \in chain[h]})
                + Cardinality({h \in Holders : pc[h] \in {"popped", "walk"} /\ cur[h] = o})

Invariants ==
    /\ TypeOk /\ NoUAF /\ OneChain /\ ChainDrained /\ NoWalkUnderLock
    /\ OwedCovered /\ OwedIsListed /\ MarkIffWalking /\ RefExact

(***************************************************************************)
(* =============================== LIVENESS =============================== *)
(***************************************************************************)
\* Every owed hook is reached, or leaves the list (I-9).
OwedReached == \A k \in Hooks : owed[k] ~> ~owed[k]

\* A detached WAIT owner's free does not wait forever: every walk owed on
\* it is drained and ends. Violated by BUGGY_SKIP_DRAIN.
FreeEnds == \A o \in WaitOwners : wst[o] = "detached" ~> wst[o] = "freed"

=============================================================================
