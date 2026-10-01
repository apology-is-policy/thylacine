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
(* A DYING CLOSER (2026-09-28, FID-LIFECYCLE section 9). A thread whose     *)
(* Proc is dying cannot send, so its close hands the Tclunk to the closer   *)
(* threads (DyingClose), which send it later (CloserSend). CloserSend is    *)
(* weakly fair: a closer runs. That is the one liveness assumption the      *)
(* closer adds; the poll kthread still needs none. NO_CLOSER is the design  *)
(* before it: the dying close's Tclunk was refused and never sent.          *)
(*                                                                         *)
(* INVARIANTS:                                                             *)
(*  - SlotEventuallyFreed (TEMPORAL, the leak): once the poll has ended the *)
(*    `ready`-fd Tclunk is eventually delivered (the slot frees). Holds for *)
(*    Fix=TRUE with no fairness on the poll kthread (a live close delivers  *)
(*    it; a dying close's closer does); the buggy cfgs (Fix=FALSE with no  *)
(*    WF on KthreadGc; NO_CLOSER) are the LEAK counterexamples.             *)
(*  - NoUseAfterFreePs (SAFETY): the kthread never touches the poll-state   *)
(*    after it is freed -- the fix's ps-decoupling must not introduce a UAF.*)
(*    The cancel/free coordination is what must prevent it.                 *)
(*  - ClunkAtMostOnce (SAFETY): the `ready`-fd Tclunk is delivered at most  *)
(*    once (the cancel + the close, or the two Spoor-ref drops, must not    *)
(*    double-clunk -> a double slot_unref).                                 *)
(*                                                                         *)
(* THE COLLECTOR IS ONE STEP (BUGGY_SPLIT_GC, 2026-09-28). KthreadGc takes  *)
(* a stranded op off the registry AND flushes its Tread in one step with    *)
(* respect to the close: both happen under g_dev9p_poll_lock, which the     *)
(* close's cancel takes too. Split them -- unlink under the lock, flush     *)
(* after it, the code from #294 to NP-4c -- and a close between the two     *)
(* finds no op to cancel, while the op's Tread still targets the fid, so    *)
(* the Tclunk is refused (9p_session any_outstanding_on_fid) and nothing    *)
(* clunks the fid afterwards: the slot leaks although the kthread flushes.  *)
(***************************************************************************)
EXTENDS Naturals

CONSTANT Fix    \* BOOLEAN -- TRUE: cancel-at-close; FALSE: the current deferred-pin design.
CONSTANT BUGGY_SPLIT_GC   \* BOOLEAN -- the collector unlinks and flushes in two steps.
CONSTANT NO_CLOSER        \* BOOLEAN -- a dying close's Tclunk is refused, not handed on.

ASSUME Fix \in BOOLEAN
ASSUME BUGGY_SPLIT_GC \in BOOLEAN
ASSUME NO_CLOSER \in BOOLEAN
ASSUME BUGGY_SPLIT_GC => Fix    \* a flaw of the cancel-at-close design's collector

VARIABLES
    poll,     \* {"parked","ended"} -- the poller; "ended" = it timed out + returned.
    fdref,    \* BOOLEAN -- the user still holds the `ready`-fd handle ref.
    oppin,    \* BOOLEAN -- the op pins the `ready` Spoor (TRUE only in the ~Fix design).
    op,       \* {"live","stranded","unlinked","torndown"} -- the readiness op.
              \*   live      = outstanding, the poll is parked (a Tread is in flight).
              \*   stranded  = the poll ended; the op awaits teardown (GC or cancel-at-close).
              \*   unlinked  = BUGGY_SPLIT_GC only: off the registry, ps->op cleared, its
              \*               Tread still in flight (not yet flushed).
              \*   torndown  = the op was cancelled/unregistered + freed.
    privps,   \* BOOLEAN -- the priv (dev9p_priv) holds the poll-state ref.
    opps,     \* BOOLEAN -- the op holds a poll-state ref (TRUE only in the Fix design).
    clunks,   \* Nat -- count of `ready`-fd Tclunks delivered to netd (the slot frees on the 1st).
    uaf,      \* BOOLEAN -- the kthread touched the poll-state after it was freed.
    deferred  \* BOOLEAN -- a dying close handed the Tclunk to the closer threads; unsent.

vars == <<poll, fdref, oppin, op, privps, opps, clunks, uaf, deferred>>

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
    /\ op     \in {"live","stranded","unlinked","torndown"}
    /\ privps \in BOOLEAN
    /\ opps   \in BOOLEAN
    /\ clunks \in Nat
    /\ uaf    \in BOOLEAN
    /\ deferred \in BOOLEAN

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
    /\ deferred = FALSE

(***************************************************************************)
(* PollTimeout -- the poll times out + returns (sys_poll unregisters the    *)
(* hook). The op is now STRANDED: its poll ended, it awaits teardown.       *)
(***************************************************************************)
PollTimeout ==
    /\ poll = "parked"
    /\ poll' = "ended"
    /\ op = "live"
    /\ op' = "stranded"
    /\ UNCHANGED <<fdref, oppin, privps, opps, clunks, uaf, deferred>>

(***************************************************************************)
(* KthreadTouchPs -- the dev9p.poll kthread derefs op->ps (the reap's walk *)
(* of ps->poll_list, or the collector's empty-check). Legal only while the *)
(* op is still live/stranded (not torn down). Records a UAF if ps is freed *)
(* -- the safety probe for the fix's ps-decoupling.                        *)
(***************************************************************************)
KthreadTouchPs ==
    /\ op \in {"live","stranded","unlinked"}
    /\ uaf' = (uaf \/ PsFreed)
    /\ UNCHANGED <<poll, fdref, oppin, op, privps, opps, clunks, deferred>>

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
    /\ ~BUGGY_SPLIT_GC
    /\ op = "stranded"
    /\ op' = "torndown"
    /\ clunks' = clunks + (IF oppin /\ ~fdref THEN 1 ELSE 0)   \* last Spoor ref -> clunk
    /\ oppin' = FALSE
    /\ opps'  = FALSE
    \* ~Fix: the priv's ps is freed by dev9p_close iff this drop frees the Spoor;
    \* but op is already "torndown" here, so KthreadTouchPs can no longer fire ->
    \* modeling privps as held (never freed) in ~Fix is sound for NoUseAfterFreePs.
    /\ UNCHANGED <<poll, fdref, privps, uaf, deferred>>

(***************************************************************************)
(* BUGGY_SPLIT_GC -- the collector in two steps. KthreadGcUnlink takes the  *)
(* stranded op off the registry and clears ps->op under the lock;           *)
(* KthreadGcFlush Tflushes and frees it after the unlock.                   *)
(***************************************************************************)
KthreadGcUnlink ==
    /\ BUGGY_SPLIT_GC
    /\ op = "stranded"
    /\ op' = "unlinked"
    /\ UNCHANGED <<poll, fdref, oppin, privps, opps, clunks, uaf, deferred>>

KthreadGcFlush ==
    /\ op = "unlinked"
    /\ op'   = "torndown"
    /\ opps' = FALSE
    /\ UNCHANGED <<poll, fdref, oppin, privps, clunks, uaf, deferred>>

(***************************************************************************)
(* UserClose / DyingClose -- the user closes the `ready` fd (the poll has  *)
(* ended), from a live thread or from a dying one. Drops the fd-handle ref. *)
(* The behaviour SPLITS on the design:                                      *)
(*                                                                         *)
(*  ~Fix: the op may still pin the Spoor, so dropping the fd ref may NOT    *)
(*    take the Spoor to 0 -> no close hook -> no clunk yet (it waits for    *)
(*    KthreadGc to drop oppin -- the leak window). If the op was ALREADY    *)
(*    GC'd (oppin false), this IS the last drop -> clunk.                   *)
(*                                                                         *)
(*  Fix: the op does NOT pin the Spoor, so this is the LAST Spoor ref ->    *)
(*    dev9p_close runs HERE. It cancels a still-outstanding op (Tflush +    *)
(*    clear inflight -> op "torndown", under c->lock; the kthread can no    *)
(*    longer complete it), drops the priv's ps ref, and delivers the clunk. *)
(*    ps frees iff the op already dropped its ref; else the op's ref keeps  *)
(*    ps alive until KthreadGc/teardown drops it.                           *)
(*    An UNLINKED op (BUGGY_SPLIT_GC) is invisible to the cancel, and its   *)
(*    live Tread makes the session refuse the Tclunk: no clunk, ever.       *)
(*                                                                         *)
(* The clunk a close delivers (Deliver): a live thread sends it; a dying   *)
(* one cannot (client_send_flow refuses it), so it hands the Tclunk to the  *)
(* closer threads -- dev9p_clunk_fid on -P9_E_AGAIN, p9_attached_defer_clunk *)
(* -- or, NO_CLOSER, the Tclunk is refused and lost.                        *)
(***************************************************************************)
Deliver(dying) ==
    IF ~dying      THEN /\ clunks'   = clunks + 1
                        /\ deferred' = deferred
    ELSE IF NO_CLOSER THEN UNCHANGED <<clunks, deferred>>
    ELSE                /\ deferred' = TRUE
                        /\ clunks'   = clunks

Close(dying) ==
    /\ poll = "ended"
    /\ fdref
    /\ fdref' = FALSE
    /\ IF Fix /\ op = "unlinked"
       THEN /\ privps' = FALSE                 \* ps->op is NULL: nothing to cancel,
            /\ UNCHANGED <<oppin, op, opps, clunks, deferred>>   \* and the Tclunk is refused.
       ELSE IF Fix
       THEN /\ op'     = "torndown"            \* cancel under c->lock: no late completion.
            /\ privps' = FALSE                 \* the priv drops its ps ref.
            /\ opps'   = FALSE                 \* the op is freed here -> its ps ref drops too.
            /\ oppin'  = oppin                 \* (always FALSE in Fix)
            /\ Deliver(dying)                  \* Spoor hits 0 refs (no op-pin) -> the clunk.
       ELSE IF ~oppin                          \* ~Fix: the op-pin is already gone ->
       THEN /\ Deliver(dying)                  \* this is the last drop -> the clunk.
            /\ UNCHANGED <<oppin, op, privps, opps>>
       ELSE UNCHANGED <<oppin, op, privps, opps, clunks, deferred>>
    /\ UNCHANGED <<poll, uaf>>

UserClose  == Close(FALSE)
DyingClose == Close(TRUE)

(***************************************************************************)
(* CloserSend -- a closer thread sends the Tclunk a dying close handed it.  *)
(* The hand-off took a session reference, so the session, and the fid on   *)
(* it, live until the closer sends (closer_serve, kernel/9p_attach.c).     *)
(***************************************************************************)
CloserSend ==
    /\ deferred
    /\ deferred' = FALSE
    /\ clunks'   = clunks + 1
    /\ UNCHANGED <<poll, fdref, oppin, op, privps, opps, uaf>>

Next ==
    \/ PollTimeout
    \/ KthreadTouchPs
    \/ KthreadGc
    \/ KthreadGcUnlink
    \/ KthreadGcFlush
    \/ UserClose
    \/ DyingClose
    \/ CloserSend

(* The poll always eventually times out, and the user always eventually      *)
(* closes the fd, from a live thread or a dying one -- WF on PollTimeout and *)
(* on the close. The buggy cfg withholds WF on KthreadGc: that IS the leak   *)
(* -- the slot-free hinges on a kthread step that may never come. The clean  *)
(* (Fix) cfg ALSO withholds it, proving the fix frees the slot without any   *)
(* fairness on the poll kthread. KthreadGcFlush IS fair, so the split        *)
(* collector's leak is not a starved kthread. CloserSend is fair: a closer   *)
(* runs (FID-LIFECYCLE section 9), so NO_CLOSER's leak is not a starved one. *)
Fairness == WF_vars(PollTimeout) /\ WF_vars(UserClose \/ DyingClose)
            /\ WF_vars(KthreadGcFlush) /\ WF_vars(CloserSend)

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
(* Tclunk is delivered. Fix=TRUE: holds with no fairness on the poll kthread *)
(* (a live close delivers it; a dying close's closer does). Fix=FALSE: the   *)
(* buggy cfg (no WF on KthreadGc) violates it -- a stranded op whose GC      *)
(* never fires leaves clunks = 0 forever, the permanent slot leak. NO_CLOSER *)
(* violates it too: a dying close's refused Tclunk is never sent.            *)
SlotEventuallyFreed == (poll = "ended") ~> (clunks = 1)

Liveness == SlotEventuallyFreed

=============================================================================
