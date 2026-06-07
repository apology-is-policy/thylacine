---- MODULE loom_multishot ----
(***************************************************************************)
(* Thylacine Loom — the MULTISHOT op lifecycle (Loom-5).                   *)
(*                                                                         *)
(* A multishot op is one SQE that produces MANY CQEs: arm once, a CQE per  *)
(* event, until a terminal event (EOF / error / cancel / teardown) ends    *)
(* the stream. It is the I-29 "exactly one terminal completion" property    *)
(* GENERALIZED from a single CQE to a STREAM: a multishot op posts zero or  *)
(* more non-terminal "MORE" CQEs (each `LOOM_CQE_MORE`-set, the op re-arms  *)
(* after each) followed by EXACTLY ONE terminal (MORE-clear) CQE, and       *)
(* NOTHING after the terminal. A single-shot op is the 1-shot special case  *)
(* (its lone CQE is its terminal).                                          *)
(*                                                                         *)
(* WHY A SEPARATE MODULE (not an extension of loom.tla).                    *)
(*   The audited core `specs/loom.tla` (Loom-1..4) models the CQ as         *)
(*   `cq \subseteq Ops` — AT MOST ONE unreaped CQE per op. That single-CQE  *)
(*   representation is load-bearing for its 8 cfgs and is the gate the      *)
(*   landed+audited Loom-1..4 impl was checked against. Multishot           *)
(*   structurally needs a MULTISET CQ (several unreaped CQEs from one op),  *)
(*   so it gets its own module with a COUNT CQ (`cq[o] \in Nat`), leaving   *)
(*   the frozen core untouched. This mirrors the scheduler suite            *)
(*   (sched_oncpu.tla + sched_alpha.tla): one focused module per mechanism. *)
(*   The core transport invariants (TOCTOU arg pin, single-shot             *)
(*   exactly-one, the CQ-waiter wake) remain loom.tla's proof; this module  *)
(*   re-pins the two that multishot changes — the I-30 OBJECT pin (now held *)
(*   ACROSS re-arms, where a clunk+reuse of the handle slot BETWEEN shots   *)
(*   is the novel race) and CQ back-pressure (now per-SHOT) — plus the new  *)
(*   stream-shape properties (exactly-one-terminal, terminal-ends-stream).  *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   1. Exactly one terminal CQE per admitted op (I-29 generalized to a     *)
(*      stream). A stray/duplicate terminal is forbidden. Pinned by         *)
(*      ExactlyOneTerminal.                                                 *)
(*                                                                         *)
(*   2. The terminal ends the stream. Once the terminal (MORE-clear) CQE    *)
(*      is posted, NO further CQE (MORE or terminal) is posted for the op.  *)
(*      This is the Tapestry buffer-recycle-gate property: the present's    *)
(*      terminal CQE means the buffer is free; a later MORE would be a       *)
(*      use-after-recycle. Pinned by TerminalEndsStream + TerminalImpliesDone.*)
(*                                                                         *)
(*   3. The I-30 pin is held across ALL shots. Every shot (MORE or          *)
(*      terminal) acts under the object PINNED at submit, never a           *)
(*      re-resolution of the live registered-handle slot — even though the  *)
(*      op's lifetime now spans many shots and the slot can be repointed    *)
(*      (a clunk+reuse) BETWEEN shots. Pinned by ObjPinnedAcrossShots +     *)
(*      ActedUnderAdmittedRights.                                            *)
(*                                                                         *)
(*   4. CQ back-pressure HOLDS a shot (never drops it). A shot that would   *)
(*      overflow the bounded CQ waits (the op stays "ready") until          *)
(*      userspace reaps a slot — the same all-or-nothing-never-drop         *)
(*      discipline as loom.tla, now applied per shot of a stream. Pinned by *)
(*      CqNeverOverfull.                                                     *)
(*                                                                         *)
(*   5. No stale completion across teardown. Tear-down quiesces every       *)
(*      armed multishot op (the #898 quiesce / #811 death-interruptible     *)
(*      unwind) so no late shot posts into freed/reused CQ memory. Pinned   *)
(*      by NoStaleAfterTeardown.                                            *)
(*                                                                         *)
(*   6. Liveness: an admitted op eventually reaches its terminal CQE (no    *)
(*      stream stalls forever armed). Pinned by EventuallyTerminal.         *)
(*                                                                         *)
(* THE OP LIFECYCLE (per SQ slot o \in Ops)                                 *)
(*                                                                         *)
(*   "empty"     — SQ slot unused.                                          *)
(*   "posted"    — userspace wrote the SQE (multishot[o] chosen here) and   *)
(*                 bumped the SQ tail. Submitted, not consumed.             *)
(*   "armed"     — admitted: the registered handle was resolved + PINNED    *)
(*                 (snap_obj), a 9P tag allocated. The re-entrant steady    *)
(*                 state — a request is in flight, awaiting the next event.  *)
(*   "ready"     — an event/reply (or a cancel) arrived; a completion is    *)
(*                 pending. pending_term[o] records whether THIS one ends    *)
(*                 the stream (a terminal) or is a MORE shot (re-arm).       *)
(*   "cqd"       — the TERMINAL CQE has been posted (terminal for the       *)
(*                 kernel); the op is done arming. Its CQEs await reaping.   *)
(*   "reaped"    — userspace drained all of the op's CQEs (incl. terminal). *)
(*   "abandoned" — teardown quiesced the op while armed (no terminal CQE).  *)
(*                                                                         *)
(* THE BUGS THIS PINS (each a BUGGY_* flag, each its own cfg)               *)
(*                                                                         *)
(*   BUGGY_DOUBLE_TERMINAL — a stray/duplicate reply posts a SECOND         *)
(*     terminal CQE for an already-terminal op (ExactlyOneTerminal          *)
(*     counterexample).                                                     *)
(*                                                                         *)
(*   BUGGY_SHOT_LOST_ON_FULL — PostShot/PostTerminal drop the room check    *)
(*     and post into a FULL CQ, overwriting an unconsumed CQE = a lost      *)
(*     completion (CqNeverOverfull counterexample). The per-shot form of    *)
(*     loom.tla's BUGGY_LOST_ON_FULL_CQ.                                    *)
(*                                                                         *)
(*   BUGGY_RESOLVE_AT_SHOT — a shot RE-RESOLVES the live registered handle  *)
(*     instead of using the submit-time pin. A clunk+reuse repointing the   *)
(*     slot BETWEEN shots makes a later shot act against the WRONG object — *)
(*     the io_uring credential-vs-work race, amplified by the long          *)
(*     multishot lifetime (ObjPinnedAcrossShots counterexample).            *)
(*                                                                         *)
(*   BUGGY_MORE_AFTER_TERMINAL — a MORE shot is posted AFTER the terminal   *)
(*     (the op kept arming after declaring the stream done) — a             *)
(*     use-after-recycle of state userspace freed on the terminal           *)
(*     (TerminalEndsStream counterexample).                                 *)
(*                                                                         *)
(*   BUGGY_STALE_AFTER_TEARDOWN — Teardown sets torn but does NOT quiesce   *)
(*     armed ops, leaving an admitted multishot op alive past tear-down —   *)
(*     the precondition for a late shot posting a stale CQE                  *)
(*     (NoStaleAfterTeardown counterexample).                               *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)     *)
(*                                                                         *)
(*   loom_multishot.cfg                      all BUGGY_* FALSE,             *)
(*                                            ALLOW_TEARDOWN TRUE — every    *)
(*                                            safety invariant holds.       *)
(*   loom_multishot_liveness.cfg             Spec_Live, ALLOW_TEARDOWN      *)
(*                                            FALSE — EventuallyTerminal.    *)
(*   loom_multishot_buggy_double_terminal.cfg     ExactlyOneTerminal cx.    *)
(*   loom_multishot_buggy_shot_lost_on_full.cfg   CqNeverOverfull cx.       *)
(*   loom_multishot_buggy_resolve_at_shot.cfg     ObjPinnedAcrossShots cx.  *)
(*   loom_multishot_buggy_more_after_terminal.cfg TerminalEndsStream cx.    *)
(*   loom_multishot_buggy_stale_after_teardown.cfg NoStaleAfterTeardown cx. *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                      *)
(*                                                                         *)
(*   Ops = the SQ slots (2 is sufficient: two concurrent streams, CQ        *)
(*   back-pressure with CQ_CAP = 1..2, and the shared-handle reuse race).   *)
(*   MAX_SHOTS bounds the non-terminal shots per multishot op (2 exercises  *)
(*   arm -> shot -> arm -> shot -> terminal). The 9P engine below — tag      *)
(*   uniqueness (I-10), out-of-order completion — is 9p_client.tla's proof  *)
(*   and is NOT re-pinned; a multishot op holds ONE tag for its armed       *)
(*   lifetime (InFlight), released at the terminal / abandon. The arg       *)
(*   TOCTOU is loom.tla's proof (the consume snapshot is unchanged); this   *)
(*   module focuses on the OBJECT pin across the multi-shot lifetime, the   *)
(*   stream shape, and per-shot back-pressure.                              *)
(*                                                                         *)
(* See docs/LOOM.md (the design) + docs/TAPESTRY.md (the first consumer —   *)
(* the event-fd multishot READ + the present-recycle gate), loom.tla (the  *)
(* core transport), poll.tla (the register-then-observe wait/wake lineage). *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Ops,                          \* the set of SQ slots (op identities).
    CQ_CAP,                       \* Nat — CQ ring capacity (posted-unreaped bound).
    MAX_INFLIGHT,                 \* Nat — 9P tag-pool bound (armed admitted ops).
    MAX_SHOTS,                    \* Nat — bound on non-terminal shots per op.
    ALLOW_TEARDOWN,               \* BOOLEAN — TRUE: the ring may be torn down.
    BUGGY_DOUBLE_TERMINAL,        \* BOOLEAN — a duplicate terminal CQE is posted.
    BUGGY_SHOT_LOST_ON_FULL,      \* BOOLEAN — a shot posts into a full CQ.
    BUGGY_RESOLVE_AT_SHOT,        \* BOOLEAN — a shot re-resolves the live handle.
    BUGGY_MORE_AFTER_TERMINAL,    \* BOOLEAN — a MORE shot is posted after the terminal.
    BUGGY_STALE_AFTER_TEARDOWN    \* BOOLEAN — teardown does not quiesce armed ops.

ASSUME Ops # {}
ASSUME CQ_CAP \in Nat /\ CQ_CAP >= 1
ASSUME MAX_INFLIGHT \in Nat /\ MAX_INFLIGHT >= 1
ASSUME MAX_SHOTS \in Nat /\ MAX_SHOTS >= 1
ASSUME ALLOW_TEARDOWN            \in BOOLEAN
ASSUME BUGGY_DOUBLE_TERMINAL     \in BOOLEAN
ASSUME BUGGY_SHOT_LOST_ON_FULL   \in BOOLEAN
ASSUME BUGGY_RESOLVE_AT_SHOT     \in BOOLEAN
ASSUME BUGGY_MORE_AFTER_TERMINAL \in BOOLEAN
ASSUME BUGGY_STALE_AFTER_TEARDOWN \in BOOLEAN

\* Registered-handle object domain. AllowedObjs is the I-2/I-6 rights gate at
\* submit; "obj_bad" models the object a clunk + reuse repoints the slot to
\* BETWEEN shots (the multishot amplification of the I-30 race).
Objects     == {"obj_ok", "obj_bad"}
AllowedObjs == {"obj_ok"}
NONE_OBJ    == "obj_none"          \* sentinel: handle unregistered / not yet pinned.

Phases == {"empty", "posted", "armed", "ready", "cqd", "reaped", "abandoned"}

\* The largest a single op's unreaped-CQE count can reach before any reap:
\* MAX_SHOTS non-terminal shots + at most 2 terminals (the double-terminal bug)
\* + at most 1 more-after-terminal. A generous, finite TypeOk bound.
CqMax == MAX_SHOTS + 3

VARIABLES
    phase,          \* [Ops -> Phases] — each op's lifecycle stage.
    snap_obj,       \* [Ops -> Objects \cup {NONE_OBJ}] — object PINNED at submit (I-30).
    acted_obj,      \* [Ops -> Objects \cup {NONE_OBJ}] — object the latest shot acted against.
    multishot,      \* [Ops -> BOOLEAN] — TRUE: re-arms after each non-terminal shot.
    pending_term,   \* [Ops -> BOOLEAN] — in "ready": is the pending completion terminal?
    shots,          \* [Ops -> Nat] — count of non-terminal (MORE) CQEs posted (<= MAX_SHOTS).
    term_count,     \* [Ops -> 0..2] — count of TERMINAL CQEs posted (must stay <= 1).
    cq,             \* [Ops -> Nat] — unreaped CQEs from this op in the ring (multiset CQ).
    more_after_term,\* [Ops -> 0..1] — ghost: a MORE shot was posted after the terminal (bug).
    reg,            \* Objects \cup {NONE_OBJ} — the single registered-handle slot.
    torn            \* BOOLEAN — the ring has been torn down.

vars == <<phase, snap_obj, acted_obj, multishot, pending_term, shots,
          term_count, cq, more_after_term, reg, torn>>

\* Ops holding a 9P tag: admitted (armed) or with a reply pending (ready), i.e.
\* admitted-and-not-yet-terminal. The set teardown must quiesce.
InFlight     == {o \in Ops : phase[o] \in {"armed", "ready"}}
LiveAdmitted == {o \in Ops : phase[o] \in {"armed", "ready"}}

\* Total unreaped CQEs across the ring (the back-pressure level). A recursive
\* sum over the per-op count function — the multiset CQ depth.
RECURSIVE SumDepth(_)
SumDepth(S) == IF S = {} THEN 0
               ELSE LET o == CHOOSE x \in S : TRUE IN cq[o] + SumDepth(S \ {o})
CqTotal == SumDepth(Ops)

TypeOk ==
    /\ phase           \in [Ops -> Phases]
    /\ snap_obj        \in [Ops -> Objects \cup {NONE_OBJ}]
    /\ acted_obj       \in [Ops -> Objects \cup {NONE_OBJ}]
    /\ multishot       \in [Ops -> BOOLEAN]
    /\ pending_term    \in [Ops -> BOOLEAN]
    /\ shots           \in [Ops -> 0..MAX_SHOTS]
    /\ term_count      \in [Ops -> 0..2]
    /\ cq              \in [Ops -> 0..CqMax]
    /\ more_after_term \in [Ops -> 0..1]
    /\ reg             \in Objects \cup {NONE_OBJ}
    /\ torn            \in BOOLEAN

(***************************************************************************)
(* Init: empty ring. No SQE produced, no handle registered, CQ empty.       *)
(***************************************************************************)
Init ==
    /\ phase           = [o \in Ops |-> "empty"]
    /\ snap_obj        = [o \in Ops |-> NONE_OBJ]
    /\ acted_obj       = [o \in Ops |-> NONE_OBJ]
    /\ multishot       = [o \in Ops |-> FALSE]
    /\ pending_term    = [o \in Ops |-> FALSE]
    /\ shots           = [o \in Ops |-> 0]
    /\ term_count      = [o \in Ops |-> 0]
    /\ cq              = [o \in Ops |-> 0]
    /\ more_after_term = [o \in Ops |-> 0]
    /\ reg             = NONE_OBJ
    /\ torn            = FALSE

(***************************************************************************)
(* UserProduce(o, ms) — userspace fills an SQE slot, choosing single-shot   *)
(* (ms = FALSE) or multishot (ms = TRUE), and bumps the SQ tail.            *)
(***************************************************************************)
UserProduce(o, ms) ==
    /\ ~torn
    /\ phase[o] = "empty"
    /\ ms \in BOOLEAN
    /\ phase'     = [phase     EXCEPT ![o] = "posted"]
    /\ multishot' = [multishot EXCEPT ![o] = ms]
    /\ UNCHANGED <<snap_obj, acted_obj, pending_term, shots, term_count, cq,
                   more_after_term, reg, torn>>

(***************************************************************************)
(* UserRegister(obj) — install or REPLACE the registered-handle slot. A     *)
(* replace models a clunk + reuse of the slot's fid. For multishot this is  *)
(* the key amplification: the slot can be repointed BETWEEN an op's shots,  *)
(* and a shot that re-resolves (BUGGY_RESOLVE_AT_SHOT) loses to it.         *)
(***************************************************************************)
UserRegister(obj) ==
    /\ ~torn
    /\ obj \in Objects
    /\ obj # reg
    /\ reg' = obj
    /\ UNCHANGED <<phase, snap_obj, acted_obj, multishot, pending_term, shots,
                   term_count, cq, more_after_term, torn>>

(***************************************************************************)
(* Consume(o) — the kernel admits a submitted SQE: resolve the registered   *)
(* handle + check rights (reg \in AllowedObjs) AT SUBMIT, allocate a 9P tag *)
(* (the I-10 bound), and PIN the resolved object (snap_obj). From here the  *)
(* kernel acts only on the pin — for the op's WHOLE multishot lifetime.     *)
(***************************************************************************)
Consume(o) ==
    /\ ~torn
    /\ phase[o] = "posted"
    /\ reg \in AllowedObjs                          \* resolve + I-2/I-6 check AT SUBMIT
    /\ Cardinality(InFlight) < MAX_INFLIGHT          \* a free 9P tag (I-10 bound)
    /\ phase'    = [phase    EXCEPT ![o] = "armed"]
    /\ snap_obj' = [snap_obj EXCEPT ![o] = reg]      \* pin the object (I-30)
    /\ UNCHANGED <<acted_obj, multishot, pending_term, shots, term_count, cq,
                   more_after_term, reg, torn>>

(***************************************************************************)
(* ReplyArrives(o, term) — an event/reply (or a cancellation) is demuxed    *)
(* for an armed op (#841 elected reader). `term` decides whether THIS one   *)
(* ends the stream: a single-shot op always terminates; a multishot op may  *)
(* re-arm (term FALSE) or terminate (term TRUE = EOF / error / cancel), and *)
(* is FORCED to terminate once it has posted MAX_SHOTS shots (the model      *)
(* bound). The tag stays held (still admitted) until the terminal.          *)
(***************************************************************************)
ReplyArrives(o, term) ==
    /\ ~torn
    /\ phase[o] = "armed"
    /\ term \in BOOLEAN
    /\ (~multishot[o] => term)              \* single-shot: the lone CQE is terminal
    /\ (shots[o] >= MAX_SHOTS => term)      \* bound: force terminate after MAX_SHOTS
    /\ phase'        = [phase        EXCEPT ![o] = "ready"]
    /\ pending_term' = [pending_term EXCEPT ![o] = term]
    /\ UNCHANGED <<snap_obj, acted_obj, multishot, shots, term_count, cq,
                   more_after_term, reg, torn>>

(***************************************************************************)
(* PostShot(o) — post a NON-terminal (LOOM_CQE_MORE-set) CQE and RE-ARM.    *)
(* CORRECT: only with a free CQ slot (back-pressure HOLDS the shot in       *)
(* "ready" until userspace reaps). The object is the submit-time pin, NOT a *)
(* re-resolution. BUGGY_SHOT_LOST_ON_FULL drops the room check; the bug     *)
(* BUGGY_RESOLVE_AT_SHOT re-resolves the live handle here.                  *)
(***************************************************************************)
PostShot(o) ==
    /\ ~torn
    /\ phase[o] = "ready"
    /\ ~pending_term[o]
    /\ (BUGGY_SHOT_LOST_ON_FULL \/ CqTotal < CQ_CAP)
    /\ phase'     = [phase     EXCEPT ![o] = "armed"]               \* re-arm
    /\ shots'     = [shots     EXCEPT ![o] = shots[o] + 1]
    /\ cq'        = [cq        EXCEPT ![o] = cq[o] + 1]
    /\ acted_obj' = [acted_obj EXCEPT ![o] =
                        IF BUGGY_RESOLVE_AT_SHOT THEN reg           \* re-resolve live handle
                        ELSE snap_obj[o]]                          \* use the pinned object
    /\ UNCHANGED <<snap_obj, multishot, pending_term, term_count,
                   more_after_term, reg, torn>>

(***************************************************************************)
(* PostTerminal(o) — post the TERMINAL (MORE-clear) CQE. The stream ends:   *)
(* the op moves to "cqd" and is done arming. Back-pressure HOLDS the        *)
(* terminal too. Same pin discipline as PostShot.                           *)
(***************************************************************************)
PostTerminal(o) ==
    /\ ~torn
    /\ phase[o] = "ready"
    /\ pending_term[o]
    /\ (BUGGY_SHOT_LOST_ON_FULL \/ CqTotal < CQ_CAP)
    /\ phase'      = [phase      EXCEPT ![o] = "cqd"]
    /\ term_count' = [term_count EXCEPT ![o] = term_count[o] + 1]
    /\ cq'         = [cq         EXCEPT ![o] = cq[o] + 1]
    /\ acted_obj'  = [acted_obj  EXCEPT ![o] =
                        IF BUGGY_RESOLVE_AT_SHOT THEN reg
                        ELSE snap_obj[o]]
    /\ UNCHANGED <<snap_obj, multishot, pending_term, shots,
                   more_after_term, reg, torn>>

(***************************************************************************)
(* BuggyDoubleTerminal(o) — a stray/duplicate reply posts a SECOND terminal *)
(* CQE for an already-terminal op. Only reachable under BUGGY_DOUBLE_TERMINAL.*)
(* Caught by ExactlyOneTerminal.                                            *)
(***************************************************************************)
BuggyDoubleTerminal(o) ==
    /\ BUGGY_DOUBLE_TERMINAL
    /\ ~torn
    /\ phase[o] = "cqd"
    /\ term_count[o] < 2
    /\ CqTotal < CQ_CAP                     \* a clean post (isolate ExactlyOneTerminal)
    /\ term_count' = [term_count EXCEPT ![o] = term_count[o] + 1]
    /\ cq'         = [cq         EXCEPT ![o] = cq[o] + 1]
    /\ UNCHANGED <<phase, snap_obj, acted_obj, multishot, pending_term, shots,
                   more_after_term, reg, torn>>

(***************************************************************************)
(* BuggyMoreAfterTerminal(o) — a MORE shot resurrects a terminated stream:  *)
(* a LOOM_CQE_MORE CQE posted AFTER the terminal. Only reachable under       *)
(* BUGGY_MORE_AFTER_TERMINAL. Caught by TerminalEndsStream.                  *)
(***************************************************************************)
BuggyMoreAfterTerminal(o) ==
    /\ BUGGY_MORE_AFTER_TERMINAL
    /\ ~torn
    /\ phase[o] = "cqd"
    /\ term_count[o] >= 1
    /\ more_after_term[o] = 0
    /\ CqTotal < CQ_CAP
    /\ more_after_term' = [more_after_term EXCEPT ![o] = 1]
    /\ cq'              = [cq              EXCEPT ![o] = cq[o] + 1]
    /\ UNCHANGED <<phase, snap_obj, acted_obj, multishot, pending_term, shots,
                   term_count, reg, torn>>

(***************************************************************************)
(* Reap(o) — userspace consumes one of the op's CQEs (bumps the CQ head).   *)
(* Permitted post-teardown (draining ALREADY-posted CQEs is sound). When    *)
(* the LAST CQE of a terminal op drains, the op reaches "reaped".           *)
(***************************************************************************)
Reap(o) ==
    /\ cq[o] > 0
    /\ cq'    = [cq    EXCEPT ![o] = cq[o] - 1]
    /\ phase' = [phase EXCEPT ![o] =
                    IF phase[o] = "cqd" /\ cq[o] = 1 THEN "reaped" ELSE phase[o]]
    /\ UNCHANGED <<snap_obj, acted_obj, multishot, pending_term, shots,
                   term_count, more_after_term, reg, torn>>

(***************************************************************************)
(* Teardown — destroy the ring. CORRECT: QUIESCE every armed/ready op (move *)
(* it to "abandoned" with no CQE) — the #898 quiesce / #811 unwind — so no  *)
(* late shot posts a stale completion. Already-posted CQEs (phase "cqd")    *)
(* remain reapable. BUGGY_STALE_AFTER_TEARDOWN skips the quiesce.           *)
(***************************************************************************)
Teardown ==
    /\ ALLOW_TEARDOWN
    /\ ~torn
    /\ torn' = TRUE
    /\ phase' = IF BUGGY_STALE_AFTER_TEARDOWN
                THEN phase                                  \* THE BUG: no quiesce
                ELSE [o \in Ops |->
                        IF phase[o] \in {"armed", "ready"}
                        THEN "abandoned" ELSE phase[o]]
    /\ UNCHANGED <<snap_obj, acted_obj, multishot, pending_term, shots,
                   term_count, cq, more_after_term, reg>>

(***************************************************************************)
(* Done — terminal self-loop. Once the ring is torn down, the CQ drained,   *)
(* and no op remains in an advanceable phase, the model halts; the explicit *)
(* stutter keeps the legitimate terminal state from reading as a deadlock   *)
(* and lets the liveness props be evaluated over the infinite suffix.       *)
(***************************************************************************)
Done ==
    /\ torn
    /\ \A o \in Ops : phase[o] \notin {"armed", "ready", "cqd"}
    /\ \A o \in Ops : cq[o] = 0
    /\ UNCHANGED vars

Next ==
    \/ \E o \in Ops, ms \in BOOLEAN : UserProduce(o, ms)
    \/ \E obj \in Objects : UserRegister(obj)
    \/ \E o \in Ops : Consume(o)
    \/ \E o \in Ops, term \in BOOLEAN : ReplyArrives(o, term)
    \/ \E o \in Ops : PostShot(o)
    \/ \E o \in Ops : PostTerminal(o)
    \/ \E o \in Ops : BuggyDoubleTerminal(o)
    \/ \E o \in Ops : BuggyMoreAfterTerminal(o)
    \/ \E o \in Ops : Reap(o)
    \/ Teardown
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* ObjPinnedAcrossShots (I-30 across the multishot lifetime) — every shot the
\* op acted on used the submit-time pin, never a re-resolution of the live
\* handle slot. Violated by BUGGY_RESOLVE_AT_SHOT.
ObjPinnedAcrossShots ==
    \A o \in Ops : acted_obj[o] # NONE_OBJ => acted_obj[o] = snap_obj[o]

\* ActedUnderAdmittedRights — every shot acted only against an object whose
\* rights were ALLOWED at submit. The privilege consequence of the pin: a
\* clunk+reuse between shots cannot escalate/confuse a later shot.
ActedUnderAdmittedRights ==
    \A o \in Ops : acted_obj[o] # NONE_OBJ => acted_obj[o] \in AllowedObjs

\* ExactlyOneTerminal (I-29 generalized to a stream) — at most one TERMINAL
\* (MORE-clear) CQE per op. Violated by BUGGY_DOUBLE_TERMINAL.
ExactlyOneTerminal ==
    \A o \in Ops : term_count[o] <= 1

\* TerminalEndsStream — no MORE shot is ever posted after the terminal. The
\* Tapestry recycle-gate property (no use-after-recycle). Violated by
\* BUGGY_MORE_AFTER_TERMINAL.
TerminalEndsStream ==
    \A o \in Ops : more_after_term[o] = 0

\* TerminalImpliesDone — once the terminal CQE is posted, the op is done
\* arming (in cqd or reaped), never re-armed. The structural backing of
\* TerminalEndsStream: there is no correct action that re-arms from a terminal.
TerminalImpliesDone ==
    \A o \in Ops : term_count[o] >= 1 => phase[o] \in {"cqd", "reaped"}

\* ArmedImpliesNotTerminal — while the op is still active (armed/ready), no
\* terminal has been posted yet. The MORE shots all precede the terminal.
ArmedImpliesNotTerminal ==
    \A o \in Ops : phase[o] \in {"armed", "ready"} => term_count[o] = 0

\* CqNeverOverfull (back-pressure, per shot) — the kernel never posts into a
\* full CQ; the total unreaped-CQE count never exceeds capacity. A shot that
\* would overflow waits in "ready". Violated by BUGGY_SHOT_LOST_ON_FULL.
CqNeverOverfull ==
    CqTotal <= CQ_CAP

\* NoStaleAfterTeardown (I-29 no-stale) — after tear-down, no op remains in a
\* postable phase (armed/ready); teardown quiesced them all, so no late shot
\* posts into freed CQ memory. Violated by BUGGY_STALE_AFTER_TEARDOWN.
NoStaleAfterTeardown ==
    torn => \A o \in Ops : phase[o] \notin {"armed", "ready"}

\* PinImpliesAdmitted — whenever an object is pinned, the submit-time rights
\* gate held (the pinned object had allowed rights). loom.tla's SnapImpliesAdmitted.
PinImpliesAdmitted ==
    \A o \in Ops : snap_obj[o] # NONE_OBJ => snap_obj[o] \in AllowedObjs

\* CqAccounted (no phantom CQE) — every unreaped CQE in the ring corresponds
\* to a real posted shot/terminal; reaping only removes. The no-spurious
\* analog for the multishot count CQ.
CqAccounted ==
    \A o \in Ops : cq[o] <= shots[o] + term_count[o] + more_after_term[o]

\* ShotsBounded — the model's shot bound holds: a multishot op posts at most
\* MAX_SHOTS non-terminal shots (the ReplyArrives forcing-logic is correct).
ShotsBounded ==
    \A o \in Ops : shots[o] <= MAX_SHOTS

\* BoundedInflight (I-10 tag pool) — admitted-and-holding-a-tag count is
\* bounded; submission back-pressures on the finite tag pool.
BoundedInflight ==
    Cardinality(InFlight) <= MAX_INFLIGHT

Invariants ==
    /\ TypeOk
    /\ ObjPinnedAcrossShots
    /\ ActedUnderAdmittedRights
    /\ ExactlyOneTerminal
    /\ TerminalEndsStream
    /\ TerminalImpliesDone
    /\ ArmedImpliesNotTerminal
    /\ CqNeverOverfull
    /\ NoStaleAfterTeardown
    /\ PinImpliesAdmitted
    /\ CqAccounted
    /\ ShotsBounded
    /\ BoundedInflight

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* EventuallyTerminal — I-29 no-lost for a stream: once an op is ADMITTED   *)
(* (reaches "armed"), it always eventually posts its TERMINAL CQE (reaches  *)
(* "cqd" / "reaped"). Checked with ALLOW_TEARDOWN FALSE (no teardown to     *)
(* legitimately abandon an armed op). Holds under weak fairness of the      *)
(* kernel's forward actions: ReplyArrives -> PostShot (bounded by MAX_SHOTS,*)
(* which forces a terminal) -> PostTerminal, with Reap draining the CQ so a *)
(* back-pressured shot/terminal always gets a slot.                         *)
(***************************************************************************)
ReplyStep(o)    == \E term \in BOOLEAN : ReplyArrives(o, term)

Liveness ==
    /\ \A o \in Ops : WF_vars(ReplyStep(o))
    /\ \A o \in Ops : WF_vars(PostShot(o))
    /\ \A o \in Ops : WF_vars(PostTerminal(o))
    /\ \A o \in Ops : WF_vars(Reap(o))

Spec_Live == Init /\ [][Next]_vars /\ Liveness

EventuallyTerminal ==
    \A o \in Ops : (phase[o] = "armed") ~> (phase[o] \in {"cqd", "reaped"})

====
