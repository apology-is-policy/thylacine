---- MODULE loom_order ----
(***************************************************************************)
(* Thylacine Loom — LINK / DRAIN inter-op ordering (Loom-5).               *)
(*                                                                         *)
(* LINK and DRAIN impose ORDER on otherwise-concurrent ring ops (the SQE   *)
(* flags LOOM_SQE_LINK / LOOM_SQE_DRAIN). Without them, admitted ops run    *)
(* out-of-order (the #841 pipelined engine); these flags are the per-fid   *)
(* dependency mechanism Loom-6's real chains need (walk -> open -> read;    *)
(* write -> read same fid) and Tapestry's present-ordering uses.           *)
(*                                                                         *)
(*   LINK (chain): consecutive linked SQEs form a chain. A linked op does  *)
(*     NOT start until its predecessor COMPLETES SUCCESSFULLY. If a link    *)
(*     member FAILS, the REST of the chain is CANCELLED — each cancelled    *)
(*     op posts EXACTLY ONE terminal CQE (a -ECANCELED), never silently     *)
(*     dropped. The cancel cascades to the chain boundary (a non-linked op  *)
(*     ends the chain).                                                      *)
(*                                                                         *)
(*   DRAIN (barrier): a drain op does NOT start until ALL previously        *)
(*     submitted ops are done; and ops submitted AFTER a drain do not start *)
(*     until the drain itself is done. A full pipeline barrier.             *)
(*                                                                         *)
(* WHY A SEPARATE MODULE. LINK/DRAIN is an ADMISSION-ORDERING concern over  *)
(* a chain of ops — orthogonal to the multishot stream lifecycle           *)
(* (loom_multishot.tla) and to the core transport's CQ representation       *)
(* (loom.tla). Ops here are ORDERED (submission order = SQ-index order), so *)
(* the model uses integer indices + a predecessor relation, a different     *)
(* shape from the unordered op sets of the other two modules. Per CLAUDE.md *)
(* spec-first discipline, one focused module per mechanism (the             *)
(* sched_oncpu.tla + sched_alpha.tla precedent).                            *)
(*                                                                         *)
(* WHAT THIS SPEC PINS                                                      *)
(*                                                                         *)
(*   1. LinkOrdered — a linked successor never EXECUTES (starts / completes *)
(*      with a real result) before its predecessor completed SUCCESSFULLY.  *)
(*      (A cancelled successor is exempt: it never executed.) Violated by   *)
(*      BUGGY_LINK_REORDER.                                                  *)
(*                                                                         *)
(*   2. DrainOrdered — a drain barrier executes only after ALL prior ops    *)
(*      are done; and any op executes only after every earlier drain is     *)
(*      done. Violated by BUGGY_DRAIN_JUMPS_AHEAD.                           *)
(*                                                                         *)
(*   3. EveryDoneOpPosted (I-29 no-lost, ordering form) — every op that     *)
(*      reaches a terminal (ok / fail / cancelled) posts EXACTLY ONE CQE.   *)
(*      A cancelled link member is NOT silently dropped. Violated by        *)
(*      BUGGY_CANCEL_NO_CQE.                                                 *)
(*                                                                         *)
(*   4. EverySubmittedPosts (liveness, no-strand) — every submitted op      *)
(*      eventually posts its CQE: it starts + completes, or it is           *)
(*      cancelled. A failed link must CANCEL its successors so they don't   *)
(*      strand forever waiting on a predecessor that will never succeed.    *)
(*      Violated by BUGGY_CANCEL_SKIPS (the cancel is skipped -> the        *)
(*      successor strands).                                                  *)
(*                                                                         *)
(*   5. NoOrphanCancel — an op is cancelled ONLY because a linked           *)
(*      predecessor failed/was-cancelled (no spurious cancellation).        *)
(*                                                                         *)
(* THE BUGS THIS PINS (each a BUGGY_* flag, each its own cfg)               *)
(*                                                                         *)
(*   BUGGY_LINK_REORDER — Start drops the link gate: a linked successor     *)
(*     starts before its predecessor completed ok (LinkOrdered cx).        *)
(*   BUGGY_DRAIN_JUMPS_AHEAD — Start drops the drain gate: a drain op (or   *)
(*     a post-drain op) starts before the barrier is satisfied              *)
(*     (DrainOrdered cx).                                                    *)
(*   BUGGY_CANCEL_NO_CQE — CancelVictim cancels a successor but posts NO    *)
(*     CQE for it (EveryDoneOpPosted cx) — a lost completion.               *)
(*   BUGGY_CANCEL_SKIPS — CancelVictim never fires: a post-fail successor   *)
(*     strands in "sub" forever (EverySubmittedPosts liveness cx).          *)
(*                                                                         *)
(* CFG MATRIX                                                               *)
(*                                                                         *)
(*   loom_order.cfg                          all BUGGY_* FALSE — every       *)
(*                                            safety invariant holds.        *)
(*   loom_order_liveness.cfg                 Spec_Live — EverySubmittedPosts.*)
(*   loom_order_buggy_link_reorder.cfg       LinkOrdered cx.                 *)
(*   loom_order_buggy_drain_jumps_ahead.cfg  DrainOrdered cx.               *)
(*   loom_order_buggy_cancel_no_cqe.cfg      EveryDoneOpPosted cx.          *)
(*   loom_order_buggy_cancel_skips.cfg       EverySubmittedPosts cx (live). *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                      *)
(*                                                                         *)
(*   Ops = 1..N, submitted in index order (SQ consume is in-order). N = 3   *)
(*   exercises a 3-op chain (fail at the head cancels two; fail in the      *)
(*   middle cancels one), a drain in the middle, and independent ops. The   *)
(*   link[i] / drain[i] flags are chosen NONDETERMINISTICALLY at submit, so *)
(*   TLC explores all 2^N x 2^N topologies. Op completion res (ok / fail)   *)
(*   is nondeterministic. There is no teardown here (teardown-quiesce is    *)
(*   loom_multishot.tla / loom.tla's proof); the focus is admission order   *)
(*   + cancellation completeness. All predecessor/successor access goes via *)
(*   the Pred(i) set (empty for i = 1), so no index ever leaves DOMAIN.     *)
(*                                                                         *)
(* See docs/LOOM.md (the design), loom_multishot.tla (the stream lifecycle),*)
(* loom.tla (the core transport).                                          *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    N,                          \* Nat — number of ops (ordered 1..N).
    BUGGY_LINK_REORDER,         \* BOOLEAN — Start drops the link gate.
    BUGGY_DRAIN_JUMPS_AHEAD,    \* BOOLEAN — Start drops the drain gate.
    BUGGY_CANCEL_NO_CQE,        \* BOOLEAN — a cancelled op posts no CQE.
    BUGGY_CANCEL_SKIPS          \* BOOLEAN — the chain-cancel never fires.

ASSUME N \in Nat /\ N >= 1
ASSUME BUGGY_LINK_REORDER      \in BOOLEAN
ASSUME BUGGY_DRAIN_JUMPS_AHEAD \in BOOLEAN
ASSUME BUGGY_CANCEL_NO_CQE     \in BOOLEAN
ASSUME BUGGY_CANCEL_SKIPS      \in BOOLEAN

Ops == 1..N

Phases  == {"unsub", "sub", "run", "done"}
Results == {"none", "ok", "fail", "cancelled"}

VARIABLES
    phase,    \* [Ops -> Phases] — op lifecycle stage.
    result,   \* [Ops -> Results] — terminal result (none until done).
    link,     \* [Ops -> BOOLEAN] — op i is linked to its successor i+1.
    drain,    \* [Ops -> BOOLEAN] — op i is a drain barrier.
    cqe       \* [Ops -> 0..2] — count of CQEs posted for op i (must stay <= 1).

vars == <<phase, result, link, drain, cqe>>

\* The immediate predecessor of op i, as a 0-or-1-element subset of Ops:
\* {i-1} for i > 1, and {} for i = 1. Quantifying/choosing over Pred(i) keeps
\* every predecessor access inside DOMAIN — no bare phase[i-1] that could
\* index 0 when i = 1.
Pred(i) == {q \in Ops : q + 1 = i}

\* An op "executed" if it actually ran (or completed with a real result) — as
\* opposed to being cancelled without ever running. The link/drain order
\* properties constrain EXECUTION, not cancellation.
Executed(i) == phase[i] = "run" \/ (phase[i] = "done" /\ result[i] \in {"ok", "fail"})

\* The link admission gate for op i: a linked successor (its predecessor links
\* to it) starts only after that predecessor completed SUCCESSFULLY. A chain
\* head or standalone op (no linking predecessor) is unconstrained by links.
LinkAdmits(i) ==
    \A p \in Pred(i) : link[p] => (phase[p] = "done" /\ result[p] = "ok")

\* The drain admission gate for op i: if i is a drain, ALL prior must be done;
\* and regardless, every earlier drain must be done before i starts.
DrainAdmits(i) ==
    /\ (drain[i] => \A j \in 1..(i-1) : phase[j] = "done")
    /\ (\A j \in 1..(i-1) : drain[j] => phase[j] = "done")

TypeOk ==
    /\ phase  \in [Ops -> Phases]
    /\ result \in [Ops -> Results]
    /\ link   \in [Ops -> BOOLEAN]
    /\ drain  \in [Ops -> BOOLEAN]
    /\ cqe    \in [Ops -> 0..2]

Init ==
    /\ phase  = [i \in Ops |-> "unsub"]
    /\ result = [i \in Ops |-> "none"]
    /\ link   = [i \in Ops |-> FALSE]
    /\ drain  = [i \in Ops |-> FALSE]
    /\ cqe    = [i \in Ops |-> 0]

(***************************************************************************)
(* Submit(i, lnk, drn) — consume SQE i (in SQ-index order; the predecessor  *)
(* must already be submitted). The op's link/drain flags are fixed here.   *)
(***************************************************************************)
Submit(i, lnk, drn) ==
    /\ phase[i] = "unsub"
    /\ \A p \in Pred(i) : phase[p] # "unsub"     \* in-order SQ consume
    /\ lnk \in BOOLEAN
    /\ drn \in BOOLEAN
    /\ phase' = [phase EXCEPT ![i] = "sub"]
    /\ link'  = [link  EXCEPT ![i] = lnk]
    /\ drain' = [drain EXCEPT ![i] = drn]
    /\ UNCHANGED <<result, cqe>>

(***************************************************************************)
(* Start(i) — dispatch a submitted op, subject to the ORDERING gates. The   *)
(* CORRECT gates: LinkAdmits (linked-successor waits for predecessor ok) +  *)
(* DrainAdmits (drain barrier). BUGGY_LINK_REORDER / BUGGY_DRAIN_JUMPS_AHEAD *)
(* drop the respective gate.                                                *)
(***************************************************************************)
Start(i) ==
    /\ phase[i] = "sub"
    /\ (BUGGY_LINK_REORDER \/ LinkAdmits(i))
    /\ (BUGGY_DRAIN_JUMPS_AHEAD \/ DrainAdmits(i))
    /\ phase' = [phase EXCEPT ![i] = "run"]
    /\ UNCHANGED <<result, link, drain, cqe>>

(***************************************************************************)
(* Complete(i, res) — a running op finishes (ok or fail) and posts its CQE. *)
(***************************************************************************)
Complete(i, res) ==
    /\ phase[i] = "run"
    /\ res \in {"ok", "fail"}
    /\ phase'  = [phase  EXCEPT ![i] = "done"]
    /\ result' = [result EXCEPT ![i] = res]
    /\ cqe'    = [cqe    EXCEPT ![i] = cqe[i] + 1]
    /\ UNCHANGED <<link, drain>>

(***************************************************************************)
(* CancelVictim(i) — cancel op i because its LINKED predecessor finished    *)
(* NON-ok (fail, or itself cancelled). i posts EXACTLY ONE terminal CQE     *)
(* (-ECANCELED). The cancel cascades: a cancelled link member becomes a     *)
(* non-ok predecessor that cancels ITS successor, to the chain boundary.    *)
(* i is still "sub" (the link gate kept it from starting after the          *)
(* predecessor's non-ok finish).                                           *)
(*                                                                         *)
(* BUGGY_CANCEL_NO_CQE cancels but posts no CQE (a lost completion).        *)
(* BUGGY_CANCEL_SKIPS disables the action entirely -> the victim strands.   *)
(***************************************************************************)
CancelVictim(i) ==
    /\ ~BUGGY_CANCEL_SKIPS
    /\ phase[i] = "sub"
    /\ \E p \in Pred(i) :
         /\ link[p]
         /\ phase[p] = "done"
         /\ result[p] \in {"fail", "cancelled"}
    /\ phase'  = [phase  EXCEPT ![i] = "done"]
    /\ result' = [result EXCEPT ![i] = "cancelled"]
    /\ cqe'    = [cqe    EXCEPT ![i] = IF BUGGY_CANCEL_NO_CQE THEN cqe[i]
                                       ELSE cqe[i] + 1]
    /\ UNCHANGED <<link, drain>>

(***************************************************************************)
(* Done — terminal self-loop once every op is done (the legitimate halt;    *)
(* the explicit stutter avoids a false deadlock + lets liveness be          *)
(* evaluated over the infinite suffix).                                     *)
(***************************************************************************)
Done ==
    /\ \A i \in Ops : phase[i] = "done"
    /\ UNCHANGED vars

Next ==
    \/ \E i \in Ops, lnk \in BOOLEAN, drn \in BOOLEAN : Submit(i, lnk, drn)
    \/ \E i \in Ops : Start(i)
    \/ \E i \in Ops, res \in {"ok", "fail"} : Complete(i, res)
    \/ \E i \in Ops : CancelVictim(i)
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* LinkOrdered — a linked successor executes only after its predecessor
\* completed successfully. A cancelled successor (never executed) is exempt.
\* Violated by BUGGY_LINK_REORDER.
LinkOrdered ==
    \A i \in Ops :
        Executed(i) =>
            \A p \in Pred(i) : link[p] => (phase[p] = "done" /\ result[p] = "ok")

\* DrainOrdered — a drain barrier executes only after all prior are done; any
\* op executes only after every earlier drain is done. Violated by
\* BUGGY_DRAIN_JUMPS_AHEAD.
DrainBarrierWaits ==
    \A i \in Ops : (drain[i] /\ Executed(i)) => \A j \in 1..(i-1) : phase[j] = "done"
PostDrainWaits ==
    \A i \in Ops : Executed(i) => \A j \in 1..(i-1) : drain[j] => phase[j] = "done"
DrainOrdered == DrainBarrierWaits /\ PostDrainWaits

\* EveryDoneOpPosted (I-29 no-lost, ordering form) — every terminal op (ok /
\* fail / cancelled) posted its CQE. Violated by BUGGY_CANCEL_NO_CQE (a
\* cancelled op reaches "done" with cqe = 0).
EveryDoneOpPosted ==
    \A i \in Ops : phase[i] = "done" => cqe[i] >= 1

\* AtMostOneCqe (I-29 no-double, ordering form) — at most one CQE per op.
\* A soundness check; no buggy cfg targets it (the model posts at most once
\* per op by construction), so it must hold in clean AND all buggy runs.
AtMostOneCqe ==
    \A i \in Ops : cqe[i] <= 1

\* NoOrphanCancel — an op is cancelled ONLY because a linked predecessor
\* failed / was itself cancelled. No spurious cancellation. (For i = 1,
\* Pred(1) = {} so the \E is FALSE, asserting op 1 is never cancelled.)
NoOrphanCancel ==
    \A i \in Ops :
        result[i] = "cancelled" =>
            \E p \in Pred(i) : link[p] /\ phase[p] = "done"
                                       /\ result[p] \in {"fail", "cancelled"}

\* ResultConsistent — a result is set exactly when the op is done.
ResultConsistent ==
    \A i \in Ops : (result[i] # "none") <=> (phase[i] = "done")

Invariants ==
    /\ TypeOk
    /\ LinkOrdered
    /\ DrainOrdered
    /\ EveryDoneOpPosted
    /\ AtMostOneCqe
    /\ NoOrphanCancel
    /\ ResultConsistent

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* EverySubmittedPosts — once an op is submitted it always eventually posts *)
(* its CQE: it starts + completes, or a failed linked predecessor cancels   *)
(* it. No op strands. Holds under weak fairness of Submit / Start /         *)
(* Complete / CancelVictim. Violated by BUGGY_CANCEL_SKIPS (a post-fail     *)
(* successor is never cancelled, so it strands in "sub" forever).          *)
(***************************************************************************)
SubmitStep(i)   == \E lnk \in BOOLEAN, drn \in BOOLEAN : Submit(i, lnk, drn)
CompleteStep(i) == \E res \in {"ok", "fail"} : Complete(i, res)

Liveness ==
    /\ \A i \in Ops : WF_vars(SubmitStep(i))
    /\ \A i \in Ops : WF_vars(Start(i))
    /\ \A i \in Ops : WF_vars(CompleteStep(i))
    /\ \A i \in Ops : WF_vars(CancelVictim(i))

Spec_Live == Init /\ [][Next]_vars /\ Liveness

EverySubmittedPosts ==
    \A i \in Ops : (phase[i] = "sub") ~> (cqe[i] >= 1)

====
