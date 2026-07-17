-------------------------- MODULE net_poll_teardown --------------------------
(***************************************************************************)
(* #294 -- the dev9p.poll readiness-op TEARDOWN lifetime: the cancel-at-    *)
(* close fix for the permanent netd connection-slot LEAK on the poll-       *)
(* timeout path.                                                            *)
(*                                                                         *)
(* net_poll.tla proves the I-9 readiness invariants (no missed edge) for a  *)
(* LIVE poller. It abstracts away the layer THIS module models: the         *)
(* readiness op's MEMORY/pin lifetime and the delivery of the `ready` fd's  *)
(* Tclunk to netd (which is what frees the connection slot). The leak lives *)
(* entirely in that abstracted-away layer.                                  *)
(*                                                                         *)
(* The scenario: a poller polls a netd `ready` fd; it TIMES OUT; the        *)
(* readiness op is left outstanding; the user closes the fd. The op must be *)
(* torn down AND the `ready`-fd Tclunk delivered to netd (freeing the slot) *)
(* -- exactly once, with no use-after-free of the poll-state.               *)
(*                                                                         *)
(* Two designs, selected by the constant `Fix`:                            *)
(*                                                                         *)
(*  Fix = FALSE -- the CURRENT design (the bug). The readiness op pins the  *)
(*    `ready` Spoor (op->pinned = spoor_ref). The Spoor's close hook        *)
(*    (dev9p_close -> the `ready`-fd Tclunk -> netd slot_unref) runs ONLY   *)
(*    on the LAST drop of the Spoor's two refs {fd-handle, op-pin}. The     *)
(*    op-pin drops only when the kthread GCs the stranded op. So the clunk  *)
(*    delivery DEPENDS on the kthread GC firing for THIS op -- a liveness   *)
(*    assumption. Ground truth (`memory/bug_294_net_session_death.md`): a   *)
(*    real SMP race leaves a stranded op un-GC'd, so the clunk is never     *)
(*    delivered and the slot LEAKS permanently. Modeled as the              *)
(*    no-weak-fairness-on-KthreadGc behaviour.                              *)
(*                                                                         *)
(*  Fix = TRUE -- the cancel-at-close design. The op pins a refcounted      *)
(*    poll-state object + the session, NOT the `ready` Spoor. So the user's *)
(*    fd-close is the Spoor's LAST ref -> dev9p_close runs AT fd-close and  *)
(*    delivers the clunk DETERMINISTICALLY (no kthread dependency), after   *)
(*    Tflush-cancelling the still-outstanding op (a within-dev9p_close      *)
(*    ordering -- Tflush BEFORE Tclunk -- so netd does not orphan the       *)
(*    deferred Tread; that ordering is trivially correct in the code and is *)
(*    not a concurrency property, so it is documented, not modeled). The    *)
(*    clunk delivery becomes a SAFETY consequence of the user's own close,  *)
(*    not a liveness assumption on the kthread.                             *)
(*                                                                         *)
(* INVARIANTS:                                                             *)
(*  - SlotEventuallyFreed (TEMPORAL, the leak): once the poll has ended the *)
(*    `ready`-fd Tclunk is eventually delivered (the slot frees). Holds for *)
(*    Fix=TRUE with NO kthread fairness (UserClose alone delivers it); the  *)
(*    buggy cfg (Fix=FALSE, no WF on KthreadGc) is the LEAK counterexample. *)
(*  - NoUseAfterFreePs (SAFETY): the kthread never touches the poll-state   *)
(*    after it is freed -- the fix's ps-decoupling must not introduce a UAF.*)
(*    The cancel/free coordination is what must prevent it.                 *)
(*  - ClunkAtMostOnce (SAFETY): the `ready`-fd Tclunk is delivered at most  *)
(*    once (the cancel + the close, or the two Spoor-ref drops, must not    *)
(*    double-clunk -> a double slot_unref).                                 *)
(***************************************************************************)
EXTENDS Naturals

CONSTANT Fix    \* BOOLEAN -- TRUE: cancel-at-close; FALSE: the current deferred-pin design.

ASSUME Fix \in BOOLEAN

VARIABLES
    poll,     \* {"parked","ended"} -- the poller; "ended" = it timed out + returned.
    fdref,    \* BOOLEAN -- the user still holds the `ready`-fd handle ref.
    oppin,    \* BOOLEAN -- the op pins the `ready` Spoor (TRUE only in the ~Fix design).
    op,       \* {"live","stranded","torndown"} -- the readiness op.
              \*   live      = outstanding, the poll is parked (a Tread is in flight).
              \*   stranded  = the poll ended; the op awaits teardown (GC or cancel-at-close).
              \*   torndown  = the op was cancelled/unregistered + freed.
    privps,   \* BOOLEAN -- the priv (dev9p_priv) holds the poll-state ref.
    opps,     \* BOOLEAN -- the op holds a poll-state ref (TRUE only in the Fix design).
    clunks,   \* Nat -- count of `ready`-fd Tclunks delivered to netd (the slot frees on the 1st).
    uaf       \* BOOLEAN -- the kthread touched the poll-state after it was freed.

vars == <<poll, fdref, oppin, op, privps, opps, clunks, uaf>>

(* The `ready` Spoor's live refcount: the fd-handle, plus the op-pin in the  *)
(* ~Fix design. The Spoor's close hook (-> the Tclunk) fires when a ref drop *)
(* takes this to 0 -- spoor_clunk's last-drop contract (spoor_unref, the     *)
(* non-hook drop, is not on this path: every holder releases via the hook-   *)
(* running spoor_clunk).                                                     *)
SpoorRefs == (IF fdref THEN 1 ELSE 0) + (IF oppin THEN 1 ELSE 0)

(* The poll-state object's live refcount. In the ~Fix design the op holds no *)
(* separate ps ref (ps IS the priv's aux, kept alive by the Spoor pin), so   *)
(* opps is always FALSE and privps never drops there -- ps is never freed    *)
(* out from under the op (the current no-UAF property, via the Spoor pin).   *)
PsRefs  == (IF privps THEN 1 ELSE 0) + (IF opps THEN 1 ELSE 0)
PsFreed == PsRefs = 0

TypeOk ==
    /\ poll   \in {"parked","ended"}
    /\ fdref  \in BOOLEAN
    /\ oppin  \in BOOLEAN
    /\ op     \in {"live","stranded","torndown"}
    /\ privps \in BOOLEAN
    /\ opps   \in BOOLEAN
    /\ clunks \in Nat
    /\ uaf    \in BOOLEAN

(***************************************************************************)
(* Initial: the poll is parked on a live readiness op; the user holds the   *)
(* fd; the slot is allocated (clunks = 0). In the ~Fix design the op pins    *)
(* the Spoor; in the Fix design it pins the ps (a separate ref) instead.     *)
(***************************************************************************)
Init ==
    /\ poll   = "parked"
    /\ fdref  = TRUE
    /\ oppin  = ~Fix              \* ~Fix: pin the Spoor; Fix: do not.
    /\ op     = "live"
    /\ privps = TRUE
    /\ opps   = Fix               \* Fix: the op holds its own ps ref; ~Fix: no.
    /\ clunks = 0
    /\ uaf    = FALSE

(***************************************************************************)
(* PollTimeout -- the poll times out + returns (sys_poll unregisters the    *)
(* hook). The op is now STRANDED: its poll ended, it awaits teardown.       *)
(***************************************************************************)
PollTimeout ==
    /\ poll = "parked"
    /\ poll' = "ended"
    /\ op = "live"
    /\ op' = "stranded"
    /\ UNCHANGED <<fdref, oppin, privps, opps, clunks, uaf>>

(***************************************************************************)
(* KthreadTouchPs -- the dev9p.poll kthread derefs op->ps (a pump cycle or  *)
(* the completion path reads ps->cached_revents). Legal only while the op   *)
(* is still live/stranded (not torn down). Records a UAF if ps is freed --  *)
(* the safety probe for the fix's ps-decoupling.                            *)
(***************************************************************************)
KthreadTouchPs ==
    /\ op \in {"live","stranded"}
    /\ uaf' = (uaf \/ PsFreed)
    /\ UNCHANGED <<poll, fdref, oppin, op, privps, opps, clunks>>

(***************************************************************************)
(* KthreadGc -- the kthread collects a STRANDED op and tears it down        *)
(* (Tflush + unregister + drop its refs + free). It drops the Spoor pin     *)
(* (~Fix) or the op's ps ref (Fix). If dropping the Spoor pin takes the     *)
(* Spoor to 0 refs (the user already closed the fd), the close hook fires   *)
(* -> the `ready`-fd Tclunk is delivered.                                   *)
(*                                                                         *)
(* THE BUG: in the ~Fix design this is the ONLY thing that drops oppin, so  *)
(* the clunk delivery there hinges on KthreadGc firing for this op. The     *)
(* buggy cfg gives it NO weak fairness -> it can be starved forever -> the  *)
(* slot leaks. (The clean Fix cfg ALSO withholds WF here, to prove the fix  *)
(* frees the slot with NO kthread fairness at all.)                         *)
(***************************************************************************)
KthreadGc ==
    /\ op = "stranded"
    /\ op' = "torndown"
    /\ clunks' = clunks + (IF oppin /\ ~fdref THEN 1 ELSE 0)   \* last Spoor ref -> clunk
    /\ oppin' = FALSE
    /\ opps'  = FALSE
    \* ~Fix: the priv's ps is freed by dev9p_close iff this drop frees the Spoor;
    \* but op is already "torndown" here, so KthreadTouchPs can no longer fire ->
    \* modeling privps as held (never freed) in ~Fix is sound for NoUseAfterFreePs.
    /\ UNCHANGED <<poll, fdref, privps, uaf>>

(***************************************************************************)
(* UserClose -- the user closes the `ready` fd (the poll has ended). Drops  *)
(* the fd-handle ref. The behaviour SPLITS on the design:                   *)
(*                                                                         *)
(*  ~Fix: the op may still pin the Spoor, so dropping the fd ref may NOT    *)
(*    take the Spoor to 0 -> no close hook -> no clunk yet (it waits for    *)
(*    KthreadGc to drop oppin -- the leak window). If the op was ALREADY    *)
(*    GC'd (oppin false), this IS the last drop -> clunk.                   *)
(*                                                                         *)
(*  Fix: the op does NOT pin the Spoor, so this is the LAST Spoor ref ->    *)
(*    dev9p_close runs HERE. It cancels a still-outstanding op (Tflush +    *)
(*    clear inflight -> op "torndown", under c->lock; the kthread can no    *)
(*    longer complete it), drops the priv's ps ref, and delivers the clunk  *)
(*    DETERMINISTICALLY. ps frees iff the op already dropped its ref; else   *)
(*    the op's ref keeps ps alive until KthreadGc/teardown drops it.        *)
(***************************************************************************)
UserClose ==
    /\ poll = "ended"
    /\ fdref
    /\ fdref' = FALSE
    /\ IF Fix
       THEN /\ op'     = "torndown"            \* cancel under c->lock: no late completion.
            /\ privps' = FALSE                 \* the priv drops its ps ref.
            /\ opps'   = FALSE                 \* the op is freed here -> its ps ref drops too.
            /\ oppin'  = oppin                 \* (always FALSE in Fix)
            /\ clunks' = clunks + 1            \* Spoor hits 0 refs (no op-pin) -> clunk.
       ELSE /\ clunks' = clunks + (IF ~oppin THEN 1 ELSE 0)  \* clunk iff the op-pin is already gone.
            /\ UNCHANGED <<oppin, op, privps, opps>>
    /\ UNCHANGED <<poll, uaf>>

Next ==
    \/ PollTimeout
    \/ KthreadTouchPs
    \/ KthreadGc
    \/ UserClose

(* The poll always eventually times out, and the user always eventually      *)
(* closes the fd -- WF on PollTimeout + UserClose. The buggy cfg withholds   *)
(* WF on KthreadGc: that IS the leak -- the slot-free hinges on a kthread     *)
(* step that may never come. The clean (Fix) cfg ALSO withholds it, proving  *)
(* the fix frees the slot without ANY kthread fairness.                      *)
Fairness == WF_vars(PollTimeout) /\ WF_vars(UserClose)

Spec == Init /\ [][Next]_vars /\ Fairness

(* ============================== INVARIANTS ============================== *)

NoUseAfterFreePs == ~uaf            \* the fix's decoupling never reads a freed poll-state.
ClunkAtMostOnce  == clunks <= 1     \* no double slot_unref.

SafetyInvariants ==
    /\ TypeOk
    /\ NoUseAfterFreePs
    /\ ClunkAtMostOnce

(* ============================== LIVENESS ================================ *)

(* THE leak property: once the poll has ended (the op is stranded + the user *)
(* will close the fd), the netd slot is eventually freed -- the `ready`-fd   *)
(* Tclunk is delivered. Fix=TRUE: holds with NO kthread fairness (UserClose  *)
(* delivers it). Fix=FALSE: the buggy cfg (no WF on KthreadGc) violates it   *)
(* -- a stranded op whose GC never fires leaves clunks = 0 forever, the      *)
(* permanent slot leak.                                                      *)
SlotEventuallyFreed == (poll = "ended") ~> (clunks = 1)

Liveness == SlotEventuallyFreed

=============================================================================
