---- MODULE loom_devgone ----
(***************************************************************************)
(* Thylacine Loom — the DEVICE-GONE terminal CQE (Menagerie build-arc      *)
(* step 4; docs/MENAGERIE.md §10).                                          *)
(*                                                                         *)
(* When the backing 9P session of an in-flight Loom op DIES — the server / *)
(* driver endpoint vanished (a `DeviceRemoved` group-terminates the driver *)
(* Proc, so its served endpoint tears down and the consumer's rings EOF) — *)
(* the op must complete with a TERMINAL CQE, never hang, AND the CQE's      *)
(* result must distinguish "the backing device disappeared" (a device-gone *)
(* terminal, the §10 `T_E_DEVGONE`-class = POSIX ENODEV) from a generic     *)
(* transport error (the existing -EIO). This is the I-29 (Loom completion   *)
(* integrity) extension §10 calls for: a session death faithfully reflects  *)
(* its REASON in the terminal CQE, completing every in-flight op exactly    *)
(* once.                                                                     *)
(*                                                                         *)
(* WHY A FOCUSED MODULE (not an edit to loom.tla): this is the             *)
(* loom_multishot.tla / loom_order.tla precedent — the audited loom.tla    *)
(* core + its 8 landed cfgs stay UNTOUCHED. loom.tla models the RING        *)
(* teardown (the #898 quiesce — an op is abandoned with NO CQE when the     *)
(* user destroys the ring). A SESSION death is the OTHER teardown: the ring *)
(* is alive but its 9P backing died, so each in-flight op completes WITH an *)
(* error CQE (not abandoned). loom.tla's Teardown is the ring-destroy; this *)
(* module is the session-death-with-reason — a distinct, orthogonal action. *)
(*                                                                         *)
(* HOW THE IMPL REALIZES IT (the mechanism this pins)                       *)
(*                                                                         *)
(*   The transport recv already exposes the distinction — `0` = a clean EOF *)
(*   (the peer/server endpoint torn down = the device/service gone), `< 0`  *)
(*   = a transport error (malformed frame / send fail / deadline). The 9P   *)
(*   client's reader preserves it: a recv of 0 marks the session dead with  *)
(*   the DEVGONE reason, a recv error marks it TRANSPORT. The death fires    *)
(*   each in-flight async op's on_complete with the reason-mapped status     *)
(*   (-ENODEV vs -EIO), which loom_async_complete posts verbatim as the     *)
(*   terminal CQE. exactly-once is the existing discipline: the demux clears *)
(*   inflight[tag] BEFORE completing, so a reply and a death never both      *)
(*   complete one op (a late reply on a death-completed op is dispatched     *)
(*   ownerless = discarded, NO second CQE).                                  *)
(*                                                                         *)
(* SPEC SCOPE (audit F2/F3) — this module models the REASON-THREADING: given *)
(*   a session death of reason R, the terminal CQE faithfully carries R, once *)
(*   per in-flight op. It DELIBERATELY ABSTRACTS the step that DERIVES R --   *)
(*   the `recv 0` (peer-gone EOF) -> devgone / `recv < 0` (error) -> transport *)
(*   classification lives in the byte transport (the srvconn/spoor/loopback    *)
(*   recv contract) below TLA+'s altitude, so it is the PROSE + UNIT-TEST       *)
(*   obligation, NOT a model property: the two legs are pinned by              *)
(*   `9p_client.async_peer_gone_posts_nodev_cqe` (recv 0) +                    *)
(*   `async_session_death_posts_error_cqe` (recv -1). The monolithic           *)
(*   SessionDies likewise does not model the per-reader-site bool, so a         *)
(*   site-local miswiring is caught by the 3-reader (`rr == 0`) / 10-transport  *)
(*   (`false`) site-count discipline in kernel/9p_client.c, not by TLC.         *)
(*                                                                         *)
(* THE OP LIFECYCLE (per op o \in Ops)                                      *)
(*                                                                         *)
(*   "empty"    — no op dispatched on this slot.                            *)
(*   "inflight" — the kernel dispatched the async 9P op (loom_submit_one);  *)
(*                it holds a tag, awaiting either its reply or a session     *)
(*                death.                                                     *)
(*   "cqd"      — a terminal CQE was posted (the reply demux + post is ONE   *)
(*                locked step in the impl, so there is no "completed-but-    *)
(*                not-posted" gap here — an op goes inflight -> cqd). The    *)
(*                CQE carries `result`: ok (reply), err_devgone / err_       *)
(*                transport (session death).                                 *)
(*   "reaped"   — userspace consumed the CQE.                               *)
(*                                                                         *)
(* THE BUGS THIS PINS (each a BUGGY_* flag, each its own cfg)               *)
(*                                                                         *)
(*   BUGGY_DROPS_REASON — SessionDies completes every in-flight op with the *)
(*     GENERIC transport result regardless of the death reason (the pre-    *)
(*     step-4 behavior: client_mark_dead always fired -P9_E_IO). A device-  *)
(*     gone session's ops then surface as -EIO, so the consumer CANNOT tell *)
(*     "device removed" from a transport hiccup (DeathResultFaithful        *)
(*     counterexample). This is the bug step 4 fixes.                       *)
(*                                                                         *)
(*   BUGGY_LEAKS_INFLIGHT — SessionDies leaves an in-flight op UNcompleted  *)
(*     (no terminal CQE). The op hangs forever past the device's removal —  *)
(*     the §10 "must not hang" failure (SessionDeathCompletes counterex.).  *)
(*                                                                         *)
(*   BUGGY_DOUBLE_ON_DEATH — a late reply for an op already death-completed *)
(*     posts a SECOND CQE (the demux-vs-mark_dead race the impl closes by   *)
(*     clearing inflight[tag] before completing). NoDoubleTerminal counterex.*)
(*                                                                         *)
(* CFG MATRIX (executable documentation, CLAUDE.md spec-first policy)        *)
(*                                                                         *)
(*   loom_devgone.cfg                       all BUGGY_* FALSE — every safety *)
(*                                          invariant holds.                 *)
(*   loom_devgone_liveness.cfg              Spec_Live — EventuallyTerminates *)
(*                                          (every in-flight op reaches a    *)
(*                                          terminal: reply or death).       *)
(*   loom_devgone_buggy_drops_reason.cfg    BUGGY_DROPS_REASON —             *)
(*                                          DeathResultFaithful counterex.   *)
(*   loom_devgone_buggy_leaks_inflight.cfg  BUGGY_LEAKS_INFLIGHT —           *)
(*                                          SessionDeathCompletes counterex. *)
(*   loom_devgone_buggy_double.cfg          BUGGY_DOUBLE_ON_DEATH —          *)
(*                                          NoDoubleTerminal counterex.      *)
(*                                                                         *)
(* See docs/MENAGERIE.md §10 (device-gone teardown), docs/LOOM.md §9 (I-29),*)
(* loom.tla (the core; the ring-teardown sibling action), and 9p_client.c   *)
(* (client_mark_dead_locked — the death the reason threads through).        *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Ops,                     \* the set of op slots (op identities).
    BUGGY_DROPS_REASON,      \* BOOLEAN — SessionDies collapses devgone -> the generic.
    BUGGY_LEAKS_INFLIGHT,    \* BOOLEAN — SessionDies leaves an inflight op uncompleted.
    BUGGY_DOUBLE_ON_DEATH    \* BOOLEAN — a late reply double-posts for a death-completed op.

ASSUME Ops # {}
ASSUME BUGGY_DROPS_REASON    \in BOOLEAN
ASSUME BUGGY_LEAKS_INFLIGHT  \in BOOLEAN
ASSUME BUGGY_DOUBLE_ON_DEATH \in BOOLEAN

Phases   == {"empty", "inflight", "cqd", "reaped"}
\* The terminal CQE result. "ok" = a reply; the two err results are the
\* session-death terminals the reason maps to.
Results  == {"none", "ok", "err_transport", "err_devgone"}
\* The 9P session state. A death latches a REASON: "dead_devgone" = the peer
\* endpoint EOF'd (recv 0 = the device/service vanished); "dead_transport" =
\* a recv error / malformed frame / send fail.
Sessions == {"live", "dead_transport", "dead_devgone"}

\* The result a session death of reason `s` SHOULD stamp on its completed ops
\* (the correct reason mapping). A device-gone death -> the device-gone result;
\* any other death -> the generic transport result.
DeathResult(s) == IF s = "dead_devgone" THEN "err_devgone" ELSE "err_transport"

VARIABLES
    phase,        \* [Ops -> Phases] — each op's lifecycle stage.
    result,       \* [Ops -> Results] — the terminal CQE's result (set at cqd).
    via_death,    \* [Ops -> BOOLEAN] — this op's terminal came from a session death.
    cqe_count,    \* [Ops -> 0..2] — CQEs posted for this op (must stay <= 1).
    sess          \* Sessions — the 9P session state + (on death) the reason.

vars == <<phase, result, via_death, cqe_count, sess>>

\* Ops dispatched and awaiting a reply or a death.
Inflight == {o \in Ops : phase[o] = "inflight"}

TypeOk ==
    /\ phase     \in [Ops -> Phases]
    /\ result    \in [Ops -> Results]
    /\ via_death \in [Ops -> BOOLEAN]
    /\ cqe_count \in [Ops -> 0..2]
    /\ sess      \in Sessions

Init ==
    /\ phase     = [o \in Ops |-> "empty"]
    /\ result    = [o \in Ops |-> "none"]
    /\ via_death = [o \in Ops |-> FALSE]
    /\ cqe_count = [o \in Ops |-> 0]
    /\ sess      = "live"

(***************************************************************************)
(* Admit(o) — the kernel dispatches an async Loom op (loom_submit_one ->    *)
(* p9_client_submit_async). Only on a LIVE session: once c->dead, a submit  *)
(* fails closed (an immediate error CQE), so no op goes in-flight after a    *)
(* death — which is why SessionDeathCompletes (no inflight past death) is a  *)
(* safety property, not just liveness.                                       *)
(***************************************************************************)
Admit(o) ==
    /\ sess = "live"
    /\ phase[o] = "empty"
    /\ phase' = [phase EXCEPT ![o] = "inflight"]
    /\ UNCHANGED <<result, via_death, cqe_count, sess>>

(***************************************************************************)
(* ReplyComplete(o) — the #841 elected reader demuxes this op's R-message   *)
(* on a LIVE session and posts its CQE in ONE locked step (demux_frame_     *)
(* locked -> on_complete -> loom_post_cqe). result = ok. Only while         *)
(* inflight: once a death moved the op to cqd, its slot was cleared, so a    *)
(* reply that races in is the ownerless LateReply below, never this.        *)
(***************************************************************************)
ReplyComplete(o) ==
    /\ sess = "live"
    /\ phase[o] = "inflight"
    /\ phase'     = [phase     EXCEPT ![o] = "cqd"]
    /\ result'    = [result    EXCEPT ![o] = "ok"]
    /\ cqe_count' = [cqe_count EXCEPT ![o] = cqe_count[o] + 1]
    /\ UNCHANGED <<via_death, sess>>

(***************************************************************************)
(* SessionDies(reason) — client_mark_dead_locked(c, devgone). Latches the   *)
(* session dead and, ATOMICALLY (the impl's mark_dead loop runs under one   *)
(* c->lock hold), completes EVERY in-flight op with a terminal CQE tagged    *)
(* by the reason. CORRECT: result := DeathResult(reason), via_death set.     *)
(*   BUGGY_DROPS_REASON: result := err_transport regardless of the reason    *)
(*     (devgone is masked as a generic error).                               *)
(*   BUGGY_LEAKS_INFLIGHT: one in-flight op is left UNcompleted (it hangs).  *)
(* The completed_set is the in-flight ops this death terminally completes:   *)
(* all of them (correct) or all-but-one (the leak bug, when there is one to  *)
(* leak — else there is nothing to leak and it degrades to complete-all).    *)
(***************************************************************************)
SessionDies(reason) ==
    /\ sess = "live"
    /\ reason \in {"dead_transport", "dead_devgone"}
    /\ LET stamped == IF BUGGY_DROPS_REASON THEN "err_transport"
                      ELSE DeathResult(reason)
       IN \E completed_set \in
              (IF BUGGY_LEAKS_INFLIGHT /\ Inflight # {}
               THEN {Inflight \ {o} : o \in Inflight}   \* the leak: miss exactly one
               ELSE {Inflight}) :                         \* correct: complete every one
          /\ sess'      = reason
          /\ phase'     = [o \in Ops |->
                              IF o \in completed_set THEN "cqd" ELSE phase[o]]
          /\ result'    = [o \in Ops |->
                              IF o \in completed_set THEN stamped ELSE result[o]]
          /\ via_death' = [o \in Ops |->
                              IF o \in completed_set THEN TRUE ELSE via_death[o]]
          /\ cqe_count' = [o \in Ops |->
                              IF o \in completed_set THEN cqe_count[o] + 1 ELSE cqe_count[o]]

(***************************************************************************)
(* LateReply(o) — a reply arrives for an op the session death already       *)
(* completed (its inflight[tag] was cleared by mark_dead). CORRECT: the      *)
(* reader dispatches it OWNERLESS = discarded, no second CQE. Only enabled   *)
(* under BUGGY_DOUBLE_ON_DEATH, which posts the spurious second CQE — the    *)
(* demux-vs-mark_dead double-completion the impl forecloses.                 *)
(***************************************************************************)
LateReply(o) ==
    /\ BUGGY_DOUBLE_ON_DEATH
    /\ via_death[o]
    /\ phase[o] = "cqd"
    /\ cqe_count[o] < 2
    /\ cqe_count' = [cqe_count EXCEPT ![o] = cqe_count[o] + 1]
    /\ UNCHANGED <<phase, result, via_death, sess>>

(***************************************************************************)
(* Reap(o) — userspace consumes a posted CQE.                               *)
(***************************************************************************)
Reap(o) ==
    /\ phase[o] = "cqd"
    /\ phase' = [phase EXCEPT ![o] = "reaped"]
    /\ UNCHANGED <<result, via_death, cqe_count, sess>>

(***************************************************************************)
(* Done — terminal self-loop (every op drained). With -deadlock checking    *)
(* off this is documentation; it halts the model cleanly at the sink.       *)
(***************************************************************************)
Done ==
    /\ \A o \in Ops : phase[o] \in {"empty", "reaped"}
    /\ ~(sess = "live" /\ \E o \in Ops : phase[o] = "empty")   \* Admit still possible
    /\ UNCHANGED vars

Next ==
    \/ \E o \in Ops : Admit(o)
    \/ \E o \in Ops : ReplyComplete(o)
    \/ \E reason \in {"dead_transport", "dead_devgone"} : SessionDies(reason)
    \/ \E o \in Ops : LateReply(o)
    \/ \E o \in Ops : Reap(o)
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* NoDoubleTerminal (I-29 no-double) — at most one CQE per op, even across a
\* reply racing a session death. Violated by BUGGY_DOUBLE_ON_DEATH.
NoDoubleTerminal ==
    \A o \in Ops : cqe_count[o] <= 1

\* DeathResultFaithful (the I-29 device-gone EXTENSION, §10) — an op whose
\* terminal came from a session death carries that session's reason: a
\* device-gone session's ops are err_devgone, a transport-error session's are
\* err_transport. This is the headline new property — the consumer can tell
\* "device removed" from a transport hiccup. Violated by BUGGY_DROPS_REASON
\* (which stamps err_transport even on a device-gone death).
DeathResultFaithful ==
    \A o \in Ops : via_death[o] => result[o] = DeathResult(sess)

\* DevgoneOnlyFromDevgoneSession (no spurious device-gone) — the device-gone
\* result is posted ONLY for an op completed by a device-gone session death;
\* a reply (ok) or a transport-error death never surfaces as device-gone.
DevgoneOnlyFromDevgoneSession ==
    \A o \in Ops : result[o] = "err_devgone" => (via_death[o] /\ sess = "dead_devgone")

\* SessionDeathCompletes (the §10 "must not hang") — once the session is dead,
\* NO op is left in-flight: the death terminally completed every one (and no
\* new op is admitted on a dead session). Violated by BUGGY_LEAKS_INFLIGHT,
\* which strands an op in-flight past the device's removal.
SessionDeathCompletes ==
    sess # "live" => \A o \in Ops : phase[o] # "inflight"

\* ResultSetIffTerminal — a result is recorded exactly when a CQE was posted
\* (phase reached cqd/reaped). No result without a terminal; no terminal
\* without a result.
ResultSetIffTerminal ==
    \A o \in Ops : (result[o] # "none") <=> (phase[o] \in {"cqd", "reaped"})

\* ViaDeathImpliesDead — an op flagged death-completed implies the session is
\* dead (via_death is set only by SessionDies, which latches sess). Pins the
\* DeathResultFaithful precondition (sess is a death reason when via_death holds).
ViaDeathImpliesDead ==
    \A o \in Ops : via_death[o] => sess # "live"

Invariants ==
    /\ TypeOk
    /\ NoDoubleTerminal
    /\ DeathResultFaithful
    /\ DevgoneOnlyFromDevgoneSession
    /\ SessionDeathCompletes
    /\ ResultSetIffTerminal
    /\ ViaDeathImpliesDead

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* EventuallyTerminates — every in-flight op eventually reaches a terminal  *)
(* CQE (reply or death): it never hangs. Under weak fairness of the         *)
(* forward actions (a live op's reply, and reaping to drain). The §10        *)
(* no-hang is primarily the SessionDeathCompletes SAFETY property (a death   *)
(* atomically completes all in-flight ops); this liveness witness confirms   *)
(* the lifecycle has no stall state independent of a death.                  *)
(***************************************************************************)
Liveness ==
    /\ \A o \in Ops : WF_vars(ReplyComplete(o))
    /\ \A o \in Ops : WF_vars(Reap(o))

Spec_Live == Init /\ [][Next]_vars /\ Liveness

EventuallyTerminates ==
    \A o \in Ops : (phase[o] = "inflight") ~> (phase[o] \in {"cqd", "reaped"})

====
