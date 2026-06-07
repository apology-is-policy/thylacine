---- MODULE loom ----
(***************************************************************************)
(* Thylacine Loom — the shared-memory ring transport for 9P (Loom-1).      *)
(*                                                                         *)
(* Loom is the io_uring inversion: userspace posts 9P-shaped ops into a    *)
(* submission-queue (SQ) ring living in a shared Burrow; the kernel's 9P   *)
(* client drives them; R-messages return as completion-queue (CQ) entries  *)
(* (docs/LOOM.md). This module models the SQ/CQ op-lifecycle state machine *)
(* and pins the two ARCH §28 invariants Loom reserves, plus the §6         *)
(* soundness obligations of a shared-memory async boundary:                *)
(*                                                                         *)
(*   I-29 — Loom completion integrity. Every ADMITTED SQE produces EXACTLY *)
(*          ONE terminal CQE (no lost, no double); no CQE is posted whose  *)
(*          user_data correlation is stale (an abandoned/torn-down op      *)
(*          never surfaces as a live completion).                          *)
(*                                                                         *)
(*   I-30 — Loom submit-time capability pin. The object + rights governing *)
(*          an op are RESOLVED + SNAPSHOTTED at SUBMIT and held for the    *)
(*          op's lifetime; NEVER re-evaluated at completion (which would   *)
(*          race a clunk/close that frees + reuses the handle slot).       *)
(*                                                                         *)
(* WHAT THIS SPEC PINS (and why our model helps; docs/LOOM.md §6)           *)
(*                                                                         *)
(*   1. Ring TOCTOU. The SQE fields live in a Burrow userspace shares with *)
(*      the kernel and can mutate CONCURRENTLY. The kernel must COPY each  *)
(*      SQE field to kernel memory at consume time and act ONLY on that    *)
(*      snapshot — never re-read the shared slot after the validity check, *)
(*      or a post-check mutation drives an unvalidated (e.g. out-of-bounds *)
(*      buffer) value into the op. Pinned by ArgPinnedToSnapshot +          *)
(*      ActedArgValidated.                                                  *)
(*                                                                         *)
(*   2. Check at submission, pin for the op's lifetime. io_uring's          *)
(*      reputational hole was decoupling the credential check from the      *)
(*      work (run later against possibly-changed state). Loom resolves the *)
(*      registered handle -> (object, rights) and pins it at submit; the   *)
(*      completion path NEVER re-resolves, so a concurrent clunk + reuse of *)
(*      the registered-handle slot cannot make the op act against a         *)
(*      DIFFERENT object than the one it was admitted under. Pinned by      *)
(*      ObjPinnedToSnapshot + ActedUnderAdmittedRights.                     *)
(*                                                                         *)
(*   3. Exactly-one completion. The kernel writes a CQE into the shared CQ *)
(*      at completion; it must post exactly one per admitted op (no double *)
(*      from a stray/duplicate reply, no spurious completion for an        *)
(*      op that was never admitted). Pinned by NoDoubleCompletion +         *)
(*      NoSpuriousCompletion + (liveness) EventuallyCompletes.              *)
(*                                                                         *)
(*   4. CQ back-pressure. The CQ ring is bounded; the kernel must NOT post *)
(*      into a full CQ (which would overwrite an unconsumed CQE = a lost   *)
(*      completion). A reply that arrives while the CQ is full is HELD      *)
(*      (the op waits in "completed") until userspace drains a slot — the  *)
(*      9P-client F3/F5 "all-or-nothing, never silently drop" discipline.  *)
(*      Pinned by CqNeverOverfull.                                          *)
(*                                                                         *)
(*   5. No stale completion across teardown. A ring tear-down must QUIESCE *)
(*      every in-flight op (the #811 death-interruptible unwind) so no     *)
(*      late reply posts a CQE into freed/reused CQ memory that a new ring *)
(*      would read as a live completion. Pinned by NoStaleCompletion.      *)
(*                                                                         *)
(*   6. The CQ-waiter wait/wake (Loom-4 / SQPOLL). A thread in             *)
(*      SYS_LOOM_ENTER (min_complete >= 1) sleeps on the ring's CQ         *)
(*      wait-list until a completion is available; the SQPOLL kthread (or  *)
(*      a peer ENTER) posting a CQE wakes it. This is I-9 (no lost wakeup) *)
(*      specialized to the CQ wait-list, in the poll.tla register-then-    *)
(*      observe lineage: the cross-lock hand-off is a poll_waiter flag set *)
(*      under the wait-list lock; the waiter reads it under its own Rendez *)
(*      lock. Pinned by CqFlagTracksCq + NoMissedCqWake + NoStrandedWaiter *)
(*      (teardown wakes a waiter so none strands). The SQPOLL kthread's    *)
(*      SUBMIT + reader-drive reuse Consume / Dispatch / ReplyArrives /    *)
(*      PostCqe; only the wait/wake is the new surface.                    *)
(*                                                                         *)
(* THE OP LIFECYCLE (per SQ slot o \in Ops)                                 *)
(*                                                                         *)
(*   "empty"     — SQ slot unused; no SQE produced.                        *)
(*   "posted"    — userspace wrote the SQE into the shared ring and bumped *)
(*                 the SQ tail. SUBMITTED but not yet consumed; the shared *)
(*                 SQE bytes are still userspace-mutable (the TOCTOU       *)
(*                 window).                                                 *)
(*   "snap"      — the kernel CONSUMED the SQE: copied the arg field to    *)
(*                 kernel memory (snap_arg), RESOLVED + PINNED the          *)
(*                 registered handle to (snap_obj), validated both, and    *)
(*                 allocated a 9P tag (the I-10 in-flight bound). ADMITTED. *)
(*                 From here the kernel acts ONLY on the snapshot.          *)
(*   "inflight"  — dispatched to the 9P engine (Tmsg issued under the      *)
(*                 pinned object/arg); awaiting the R-message. Holds a tag. *)
(*   "completed" — the reply was demuxed (#841 elected reader); the result *)
(*                 is ready. The 9P tag is freed; the Loom op now awaits a *)
(*                 free CQ slot (back-pressure point).                      *)
(*   "cqd"       — a CQE was posted into the CQ ring (terminal for the     *)
(*                 kernel). Carries the op's user_data + result.            *)
(*   "reaped"    — userspace consumed the CQE (bumped the CQ head).        *)
(*   "abandoned" — the ring was torn down while this op was admitted +      *)
(*                 non-terminal; the op was quiesced WITHOUT a CQE.         *)
(*                                                                         *)
(* THE BUGS THIS PINS (each a BUGGY_* flag, each its own cfg)               *)
(*                                                                         *)
(*   BUGGY_LIVE_SQE_REREAD — Dispatch re-reads the LIVE shared SQE slot     *)
(*     instead of the kernel snapshot. A userspace mutation between consume *)
(*     and dispatch drives an unvalidated value into the op                *)
(*     (ArgPinnedToSnapshot + ActedArgValidated counterexample).           *)
(*                                                                         *)
(*   BUGGY_RECHECK_AT_COMPLETION — PostCqe RE-RESOLVES the registered       *)
(*     handle at completion instead of using the submit-time pin. A clunk + *)
(*     reuse of the slot (UserRegister to a different object) makes the op  *)
(*     act against the WRONG object — the io_uring credential-vs-work race  *)
(*     (ObjPinnedToSnapshot + ActedUnderAdmittedRights counterexample).    *)
(*                                                                         *)
(*   BUGGY_DOUBLE_POST — a stray/duplicate reply (or an inflight slot not   *)
(*     cleared at completion) posts a SECOND CQE for an already-completed   *)
(*     op (NoDoubleCompletion counterexample).                              *)
(*                                                                         *)
(*   BUGGY_LOST_ON_FULL_CQ — PostCqe drops the room check and posts into a  *)
(*     FULL CQ, overwriting an unconsumed CQE = a lost completion           *)
(*     (CqNeverOverfull counterexample).                                    *)
(*                                                                         *)
(*   BUGGY_STALE_AFTER_TEARDOWN — Teardown sets torn but does NOT quiesce   *)
(*     in-flight ops, leaving an admitted op alive past tear-down — the     *)
(*     precondition for a late reply posting a stale CQE                     *)
(*     (NoStaleCompletion counterexample).                                  *)
(*                                                                         *)
(*   BUGGY_CQWAIT_NO_WAKE (Loom-4) — PostCqe / Teardown make a completion   *)
(*     available but do NOT signal the CQ-waiter's wait-list, so a sleeping *)
(*     waiter never re-evaluates (NoMissedCqWake counterexample) — poll's   *)
(*     BUGGY_NO_WAKE analog.                                                 *)
(*                                                                         *)
(*   BUGGY_CQWAIT_CHECK_EARLY (Loom-4) — the CQ-waiter samples the CQ       *)
(*     BEFORE installing its wait-list hook (check, register, sleep). A     *)
(*     completion in the gap reaches no hook, so the stale sample drives    *)
(*     the waiter to sleep on an already-ready CQ (NoMissedCqWake           *)
(*     counterexample) — poll's BUGGY_CHECK_BEFORE_REGISTER analog.         *)
(*                                                                         *)
(* CFG MATRIX (executable documentation per CLAUDE.md spec-first policy)     *)
(*                                                                         *)
(*   loom.cfg                            all BUGGY_* FALSE, ALLOW_TEARDOWN  *)
(*                                        TRUE — every safety invariant     *)
(*                                        holds (incl. correct teardown).   *)
(*   loom_liveness.cfg                   Spec_Live, ALLOW_TEARDOWN FALSE —  *)
(*                                        EventuallyCompletes (no-lost: an  *)
(*                                        admitted op always reaches a CQE  *)
(*                                        when userspace drains the CQ).    *)
(*   loom_buggy_live_sqe_reread.cfg      BUGGY_LIVE_SQE_REREAD —            *)
(*                                        ArgPinnedToSnapshot counterex.    *)
(*   loom_buggy_recheck_at_completion.cfg BUGGY_RECHECK_AT_COMPLETION —     *)
(*                                        ObjPinnedToSnapshot counterex.    *)
(*   loom_buggy_double_post.cfg          BUGGY_DOUBLE_POST —                *)
(*                                        NoDoubleCompletion counterex.     *)
(*   loom_buggy_lost_on_full.cfg         BUGGY_LOST_ON_FULL_CQ —            *)
(*                                        CqNeverOverfull counterex.        *)
(*   loom_buggy_stale_after_teardown.cfg BUGGY_STALE_AFTER_TEARDOWN —       *)
(*                                        NoStaleCompletion counterex.      *)
(*   loom_buggy_cqwait_no_wake.cfg       BUGGY_CQWAIT_NO_WAKE —            *)
(*                                        NoMissedCqWake counterex.         *)
(*   loom_buggy_cqwait_check_early.cfg   BUGGY_CQWAIT_CHECK_EARLY —        *)
(*                                        NoMissedCqWake counterex.         *)
(*                                                                         *)
(* MODELING ASSUMPTIONS                                                      *)
(*                                                                         *)
(*   Ops = the SQ slots (2 is sufficient: it exercises two concurrent ops, *)
(*   CQ back-pressure with CQ_CAP = 1, and the shared-handle reuse race).  *)
(*   Actions are atomic and model the impl's per-client lock discipline    *)
(*   (the #841 elected-reader engine drops the lock only across the        *)
(*   blocking recv; the demux + bookkeeping are locked) — the same         *)
(*   atomic-action choice 9p_client.tla / pipe.tla make.                   *)
(*                                                                         *)
(*   ONE registered-handle slot (reg) shared by all ops: this is the       *)
(*   "fixed file" table cell the I-30 pin protects. UserRegister installs  *)
(*   or REPLACES it (modeling a clunk + reuse), which is exactly the race  *)
(*   a re-resolve-at-completion bug loses to.                              *)
(*                                                                         *)
(*   ONE shared SQE arg field per op (sqe_arg[o]): the userspace-mutable   *)
(*   ring byte the kernel must snapshot. ValidArgs is the admission gate   *)
(*   (a malformed/out-of-bounds descriptor is "arg_bad").                  *)
(*                                                                         *)
(*   The 9P-level invariants Loom rides on — per-session tag uniqueness    *)
(*   (I-10), fid stability (I-11), out-of-order match — are 9p_client.tla's *)
(*   proof and are NOT re-pinned here; Loom adds only the SQ/CQ transport  *)
(*   layer above that engine. The bounded 9P tag pool surfaces here as     *)
(*   BoundedInflight (Consume needs a free tag).                           *)
(*                                                                         *)
(*   ENGINE PLUGGABILITY: this module models the "POST_CQE" completion     *)
(*   front-end (docs/LOOM.md §8.4). The existing synchronous "WAKE_RENDEZ" *)
(*   front-end is scheduler.tla / 9p_client.tla's proof; Loom keeps ONE    *)
(*   engine with two completion actions — only the action modeled here is  *)
(*   new.                                                                   *)
(*                                                                         *)
(* See docs/LOOM.md (the design), ARCHITECTURE.md §21 (the 9P client) +     *)
(* §28 (I-29, I-30 reserved), 9p_client.tla (the engine below this layer), *)
(* poll.tla (the register-then-observe wait/wake lineage).                 *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Ops,                          \* the set of SQ slots (op identities).
    CQ_CAP,                       \* Nat — CQ ring capacity (posted-unreaped bound).
    MAX_INFLIGHT,                 \* Nat — 9P tag-pool bound (in-flight admitted ops).
    ALLOW_TEARDOWN,               \* BOOLEAN — TRUE: the ring may be torn down
                                  \*   mid-flight (safety cfgs); FALSE: no teardown
                                  \*   (the liveness cfg, where no-lost = completion).
    BUGGY_LIVE_SQE_REREAD,        \* BOOLEAN — Dispatch re-reads the live SQE slot.
    BUGGY_RECHECK_AT_COMPLETION,  \* BOOLEAN — PostCqe re-resolves the handle.
    BUGGY_DOUBLE_POST,            \* BOOLEAN — a duplicate CQE is posted.
    BUGGY_LOST_ON_FULL_CQ,        \* BOOLEAN — PostCqe posts into a full CQ.
    BUGGY_STALE_AFTER_TEARDOWN,   \* BOOLEAN — Teardown does not quiesce in-flight ops.
    BUGGY_CQWAIT_NO_WAKE,         \* BOOLEAN (Loom-4) — a CQE post / teardown does
                                  \*   NOT wake the registered CQ-waiter.
    BUGGY_CQWAIT_CHECK_EARLY      \* BOOLEAN (Loom-4) — the CQ-waiter samples the CQ
                                  \*   BEFORE registering on the wait-list.

ASSUME Ops # {}
ASSUME CQ_CAP \in Nat /\ CQ_CAP >= 1
ASSUME MAX_INFLIGHT \in Nat /\ MAX_INFLIGHT >= 1
ASSUME ALLOW_TEARDOWN              \in BOOLEAN
ASSUME BUGGY_LIVE_SQE_REREAD       \in BOOLEAN
ASSUME BUGGY_RECHECK_AT_COMPLETION \in BOOLEAN
ASSUME BUGGY_DOUBLE_POST           \in BOOLEAN
ASSUME BUGGY_LOST_ON_FULL_CQ       \in BOOLEAN
ASSUME BUGGY_STALE_AFTER_TEARDOWN  \in BOOLEAN
ASSUME BUGGY_CQWAIT_NO_WAKE        \in BOOLEAN
ASSUME BUGGY_CQWAIT_CHECK_EARLY    \in BOOLEAN

\* SQE arg field domain. ValidArgs is the admission gate; "arg_bad" models a
\* malformed / out-of-bounds buffer descriptor a malicious userspace can write
\* into the shared SQE slot after the kernel snapshots a good one.
Args      == {"arg_ok", "arg_bad"}
ValidArgs == {"arg_ok"}
NONE_ARG  == "arg_none"          \* sentinel: no snapshot / no acted value yet.

\* Registered-handle object domain. AllowedObjs is the I-2/I-6 rights gate
\* at submit; "obj_bad" models the object a clunk + reuse repoints the slot to.
Objects     == {"obj_ok", "obj_bad"}
AllowedObjs == {"obj_ok"}
NONE_OBJ    == "obj_none"         \* sentinel: handle unregistered / not yet pinned.

Phases == {"empty", "posted", "snap", "inflight", "completed",
           "cqd", "reaped", "abandoned"}

VARIABLES
    phase,        \* [Ops -> Phases] — each op's lifecycle stage.
    sqe_arg,      \* [Ops -> Args] — the SHARED SQE arg slot (userspace-mutable).
    snap_arg,     \* [Ops -> Args \cup {NONE_ARG}] — kernel copy taken at consume.
    acted_arg,    \* [Ops -> Args \cup {NONE_ARG}] — the value the kernel ACTED on.
    snap_obj,     \* [Ops -> Objects \cup {NONE_OBJ}] — the object PINNED at submit.
    acted_obj,    \* [Ops -> Objects \cup {NONE_OBJ}] — the object the op ACTED against.
    cqe_posted,   \* [Ops -> 0..2] — count of CQEs posted for this op (must stay <= 1).
    reg,          \* Objects \cup {NONE_OBJ} — the single registered-handle slot.
    cq,           \* SUBSET Ops — posted-but-unreaped CQEs (bounded by CQ_CAP).
    torn,         \* BOOLEAN — the ring has been torn down.
    \* Loom-4: ONE representative CQ-waiter (a thread in SYS_LOOM_ENTER with
    \* min_complete >= 1, sleeping on the ring's CQ wait-list until a completion
    \* is available). Multiple waiters compose — each has its own poll_waiter on
    \* the same list, woken independently (the poll.tla argument). The headline
    \* new property is I-9 on THIS list: a waiter is never left asleep while an
    \* unreaped CQE is available (the SQPOLL kthread / a peer ENTER posts it).
    cqw,          \* {"idle","checking","registered","sleeping","returned"}.
    cqw_flag      \* BOOLEAN — the waiter's wake flag: a post/teardown set it
                  \*   while the waiter's hook was installed (the cross-lock
                  \*   hand-off the waiter reads under its own Rendez lock).

vars == <<phase, sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj,
          cqe_posted, reg, cq, torn, cqw, cqw_flag>>

\* Ops holding a 9P tag (between admit/dispatch and the reply demux).
InFlight == {o \in Ops : phase[o] \in {"snap", "inflight"}}

\* Admitted but not yet terminal — the set teardown must quiesce.
LiveAdmitted == {o \in Ops : phase[o] \in {"snap", "inflight", "completed"}}

\* Loom-4 CQ-waiter helpers. CqWaitPhases — the waiter lifecycle. The waiter's
\* hook is "installed" once it has registered (registered/sleeping); a post then
\* reaches it. CqAvailable — an unreaped CQE is sitting in the ring for the
\* waiter to reap (the readiness edge it waits on; poll.tla's `ready[f]` analog).
\* CanStillComplete — some admitted op may yet post a CQE (so a waiter that has
\* nothing to reap is not stranded — it sleeps until one arrives). When neither
\* CqAvailable nor CanStillComplete holds, the waiter returns what is posted
\* (the impl's "async_inflight == 0 -> nothing more is coming" early return).
CqWaitPhases    == {"idle", "checking", "registered", "sleeping", "returned"}
CqHookInstalled == cqw \in {"registered", "sleeping"}
CqAvailable     == cq # {}
CanStillComplete == \E o \in Ops : phase[o] \in {"snap", "inflight", "completed"}

TypeOk ==
    /\ phase      \in [Ops -> Phases]
    /\ sqe_arg    \in [Ops -> Args]
    /\ snap_arg   \in [Ops -> Args \cup {NONE_ARG}]
    /\ acted_arg  \in [Ops -> Args \cup {NONE_ARG}]
    /\ snap_obj   \in [Ops -> Objects \cup {NONE_OBJ}]
    /\ acted_obj  \in [Ops -> Objects \cup {NONE_OBJ}]
    /\ cqe_posted \in [Ops -> 0..2]
    /\ reg        \in Objects \cup {NONE_OBJ}
    /\ cq         \subseteq Ops
    /\ torn       \in BOOLEAN
    /\ cqw        \in CqWaitPhases
    /\ cqw_flag   \in BOOLEAN

(***************************************************************************)
(* Init: empty ring. No SQE produced, no handle registered, CQ empty, the  *)
(* ring is live.                                                            *)
(***************************************************************************)
Init ==
    /\ phase      = [o \in Ops |-> "empty"]
    /\ sqe_arg    = [o \in Ops |-> "arg_ok"]   \* slot contents irrelevant until produced
    /\ snap_arg   = [o \in Ops |-> NONE_ARG]
    /\ acted_arg  = [o \in Ops |-> NONE_ARG]
    /\ snap_obj   = [o \in Ops |-> NONE_OBJ]
    /\ acted_obj  = [o \in Ops |-> NONE_OBJ]
    /\ cqe_posted = [o \in Ops |-> 0]
    /\ reg        = NONE_OBJ
    /\ cq         = {}
    /\ torn       = FALSE
    /\ cqw        = "idle"
    /\ cqw_flag   = FALSE

(***************************************************************************)
(* UserProduce(o, a) — userspace fills an SQE slot and bumps the SQ tail.   *)
(* The arg `a` may be valid OR malformed; admission (Consume) gates it.     *)
(***************************************************************************)
UserProduce(o, a) ==
    /\ ~torn
    /\ phase[o] = "empty"
    /\ a \in Args
    /\ phase'   = [phase   EXCEPT ![o] = "posted"]
    /\ sqe_arg' = [sqe_arg EXCEPT ![o] = a]
    /\ UNCHANGED <<snap_arg, acted_arg, snap_obj, acted_obj, cqe_posted, reg, cq, torn, cqw, cqw_flag>>

(***************************************************************************)
(* UserMutateSqe(o, a) — the adversary mutates the SHARED SQE slot          *)
(* concurrently with kernel processing. Legal in any phase where the slot   *)
(* is still alive (posted..completed): models a userspace thread racing the *)
(* kernel's read of the ring. The CORRECT kernel already snapshotted at     *)
(* consume and is immune; a re-reading kernel (BUGGY_LIVE_SQE_REREAD) is    *)
(* not.                                                                      *)
(***************************************************************************)
UserMutateSqe(o, a) ==
    /\ ~torn
    /\ phase[o] \in {"posted", "snap", "inflight", "completed"}
    /\ a \in Args
    /\ a # sqe_arg[o]
    /\ sqe_arg' = [sqe_arg EXCEPT ![o] = a]
    /\ UNCHANGED <<phase, snap_arg, acted_arg, snap_obj, acted_obj, cqe_posted, reg, cq, torn, cqw, cqw_flag>>

(***************************************************************************)
(* UserRegister(obj) — install or REPLACE the registered-handle slot. A     *)
(* replace models LOOM_REGISTER_HANDLES re-install OR a clunk + reuse of    *)
(* the slot's fid. This is exactly the mutation the I-30 submit-time pin    *)
(* protects an in-flight op against.                                        *)
(***************************************************************************)
UserRegister(obj) ==
    /\ ~torn
    /\ obj \in Objects
    /\ obj # reg
    /\ reg' = obj
    /\ UNCHANGED <<phase, sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj, cqe_posted, cq, torn, cqw, cqw_flag>>

(***************************************************************************)
(* Consume(o) — the kernel admits a submitted SQE. The submit-time gate:    *)
(*   - validate the SQE arg (snapshot must be in ValidArgs),                *)
(*   - resolve the registered handle + check rights (reg \in AllowedObjs),  *)
(*   - allocate a 9P tag (the I-10 in-flight bound).                        *)
(* COPIES the arg to kernel memory (snap_arg) and PINS the resolved object  *)
(* (snap_obj). From here the kernel acts only on these snapshots.           *)
(***************************************************************************)
Consume(o) ==
    /\ ~torn
    /\ phase[o] = "posted"
    /\ sqe_arg[o] \in ValidArgs                    \* validate the snapshot (admission)
    /\ reg \in AllowedObjs                          \* resolve + I-2/I-6 check AT SUBMIT
    /\ Cardinality(InFlight) < MAX_INFLIGHT         \* a free 9P tag (I-10 bound)
    /\ phase'    = [phase    EXCEPT ![o] = "snap"]
    /\ snap_arg' = [snap_arg EXCEPT ![o] = sqe_arg[o]]   \* copy the SQE field
    /\ snap_obj' = [snap_obj EXCEPT ![o] = reg]          \* pin the object (I-30)
    /\ UNCHANGED <<sqe_arg, acted_arg, acted_obj, cqe_posted, reg, cq, torn, cqw, cqw_flag>>

(***************************************************************************)
(* Dispatch(o) — the kernel issues the 9P Tmsg, acting on the snapshot.     *)
(* CORRECT: acted_arg := snap_arg (the kernel copy). BUGGY_LIVE_SQE_REREAD: *)
(* acted_arg := sqe_arg (re-reads the shared ring) — a TOCTOU window.       *)
(* The object always comes from the submit-time pin here; the re-resolve    *)
(* bug lives at completion (PostCqe), per "never re-evaluate at completion".*)
(***************************************************************************)
Dispatch(o) ==
    /\ ~torn
    /\ phase[o] = "snap"
    /\ phase'     = [phase     EXCEPT ![o] = "inflight"]
    /\ acted_arg' = [acted_arg EXCEPT ![o] =
                        IF BUGGY_LIVE_SQE_REREAD THEN sqe_arg[o]   \* re-read shared SQE
                        ELSE snap_arg[o]]                          \* act on the snapshot
    /\ acted_obj' = [acted_obj EXCEPT ![o] = snap_obj[o]]          \* use the pinned object
    /\ UNCHANGED <<sqe_arg, snap_arg, snap_obj, cqe_posted, reg, cq, torn, cqw, cqw_flag>>

(***************************************************************************)
(* ReplyArrives(o) — the #841 elected reader demuxes the R-message; the     *)
(* result is ready and the 9P tag is freed. The Loom op now waits for a CQ  *)
(* slot.                                                                     *)
(***************************************************************************)
ReplyArrives(o) ==
    /\ ~torn
    /\ phase[o] = "inflight"
    /\ phase' = [phase EXCEPT ![o] = "completed"]
    /\ UNCHANGED <<sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj, cqe_posted, reg, cq, torn, cqw, cqw_flag>>

(***************************************************************************)
(* PostCqe(o) — write the CQE into the CQ ring. CORRECT: only with a free   *)
(* CQ slot (back-pressure; a full CQ holds the op in "completed"). The       *)
(* object is NOT re-resolved — the pin from Dispatch stands.                *)
(*                                                                         *)
(* BUGGY_LOST_ON_FULL_CQ drops the room check -> posts into a full CQ        *)
(* (overwrites an unconsumed CQE). BUGGY_RECHECK_AT_COMPLETION re-resolves   *)
(* the LIVE registered handle here -> a clunk+reuse makes the op act         *)
(* against the wrong object.                                                 *)
(***************************************************************************)
PostCqe(o) ==
    /\ ~torn
    /\ phase[o] = "completed"
    /\ (BUGGY_LOST_ON_FULL_CQ \/ Cardinality(cq) < CQ_CAP)
    /\ phase'      = [phase      EXCEPT ![o] = "cqd"]
    /\ cqe_posted' = [cqe_posted EXCEPT ![o] = cqe_posted[o] + 1]
    /\ cq'         = cq \cup {o}
    /\ acted_obj'  = [acted_obj EXCEPT ![o] =
                        IF BUGGY_RECHECK_AT_COMPLETION THEN reg    \* re-resolve live handle
                        ELSE acted_obj[o]]                         \* keep the pinned object
    \* Loom-4: posting a CQE makes a completion available -- wake the CQ-waiter
    \* (loom_async_complete signals the ring's wait-list AFTER loom_post_cqe).
    \* Set the wake flag if the waiter's hook is installed, and move a sleeping
    \* waiter back to the evaluate point. BUGGY_CQWAIT_NO_WAKE posts the CQE but
    \* skips the wake -> a sleeping waiter is stranded with cq # {} (NoMissedCqWake
    \* counterexample).
    /\ cqw_flag'   = IF CqHookInstalled /\ ~BUGGY_CQWAIT_NO_WAKE THEN TRUE ELSE cqw_flag
    /\ cqw'        = IF cqw = "sleeping" /\ ~BUGGY_CQWAIT_NO_WAKE THEN "registered" ELSE cqw
    /\ UNCHANGED <<sqe_arg, snap_arg, acted_arg, snap_obj, reg, torn>>

(***************************************************************************)
(* BuggyDoublePost(o) — a stray/duplicate reply (or an inflight slot not    *)
(* cleared at completion) posts a SECOND CQE for an op already completed.   *)
(* Only reachable under BUGGY_DOUBLE_POST. Caught by NoDoubleCompletion.    *)
(***************************************************************************)
BuggyDoublePost(o) ==
    /\ BUGGY_DOUBLE_POST
    /\ ~torn
    /\ phase[o] = "cqd"
    /\ cqe_posted[o] < 2
    /\ cqe_posted' = [cqe_posted EXCEPT ![o] = cqe_posted[o] + 1]
    /\ UNCHANGED <<phase, sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj, reg, cq, torn, cqw, cqw_flag>>

(***************************************************************************)
(* Reap(o) — userspace consumes a posted CQE (bumps the CQ head). Permitted *)
(* even after teardown: draining ALREADY-posted CQEs is sound; no-stale     *)
(* forbids POSTING after teardown, not reaping.                             *)
(***************************************************************************)
Reap(o) ==
    /\ o \in cq
    /\ phase[o] = "cqd"
    /\ phase' = [phase EXCEPT ![o] = "reaped"]
    /\ cq'    = cq \ {o}
    /\ UNCHANGED <<sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj, cqe_posted, reg, torn, cqw, cqw_flag>>

(***************************************************************************)
(* Teardown — destroy the ring. CORRECT: QUIESCE every live-admitted op     *)
(* (move it to "abandoned" with no CQE) — the #811 death-interruptible      *)
(* unwind — so no late reply can post a stale completion. Already-posted    *)
(* CQEs (phase "cqd") remain reapable. BUGGY_STALE_AFTER_TEARDOWN skips the *)
(* quiesce, leaving admitted ops alive past tear-down.                      *)
(***************************************************************************)
Teardown ==
    /\ ALLOW_TEARDOWN
    /\ ~torn
    /\ torn' = TRUE
    /\ phase' = IF BUGGY_STALE_AFTER_TEARDOWN
                THEN phase                                  \* THE BUG: no quiesce
                ELSE [o \in Ops |->
                        IF phase[o] \in {"snap", "inflight", "completed"}
                        THEN "abandoned" ELSE phase[o]]
    \* Loom-4: tear-down WAKES the CQ-waiter (loom teardown / session-death wakes
    \* the ring's wait-list) so a waiter sleeping for a completion that will now
    \* never arrive returns instead of stranding (NoStrandedWaiter). The impl
    \* realizes this: session death posts an error CQE for every in-flight op
    \* (loom_async_complete -> wake), and loom teardown wakes the wait-list
    \* explicitly. BUGGY_CQWAIT_NO_WAKE skips it -> a sleeping waiter is stranded
    \* past torn (NoStrandedWaiter counterexample).
    /\ cqw_flag' = IF CqHookInstalled /\ ~BUGGY_CQWAIT_NO_WAKE THEN TRUE ELSE cqw_flag
    /\ cqw'      = IF cqw = "sleeping" /\ ~BUGGY_CQWAIT_NO_WAKE THEN "registered" ELSE cqw
    /\ UNCHANGED <<sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj, cqe_posted, reg, cq>>

(***************************************************************************)
(* ====================== Loom-4: the CQ-waiter ========================= *)
(*                                                                         *)
(* A thread in SYS_LOOM_ENTER (min_complete >= 1) that finds the CQ not yet *)
(* holding its target sleeps on the ring's CQ wait-list until a completion  *)
(* is posted (by the SQPOLL kthread or a peer ENTER) or the ring tears      *)
(* down. The wait/wake is the poll.tla register-then-observe discipline: a  *)
(* poll_waiter is installed AND the CQ sampled in one locked step, so no    *)
(* post between sample and sleep is lost; PostCqe / Teardown walk the list  *)
(* and wake. This is the new invariant surface Loom-4 adds (I-9 on the CQ   *)
(* wait-list); the SQPOLL kthread's submit + reader-drive reuse the         *)
(* Consume / Dispatch / ReplyArrives / PostCqe actions already modeled.     *)
(***************************************************************************)

(***************************************************************************)
(* CqWaitRegister — the CORRECT entry. The waiter installs its poll_waiter  *)
(* on the ring's wait-list AND samples the CQ in one locked step            *)
(* (register-then-observe): the readiness at register becomes the initial   *)
(* flag. No completion between the sample and the hook being live is lost.  *)
(***************************************************************************)
CqWaitRegister ==
    /\ ~BUGGY_CQWAIT_CHECK_EARLY
    /\ ~torn
    /\ cqw = "idle"
    /\ cqw'      = "registered"
    /\ cqw_flag' = CqAvailable                       \* sample at register
    /\ UNCHANGED <<phase, sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj,
                   cqe_posted, reg, cq, torn>>

(***************************************************************************)
(* BuggyCqWaitCheck — the BUGGY first half: sample the CQ into the flag but *)
(* install NO hook. A completion after this reaches no wait-list entry.     *)
(***************************************************************************)
BuggyCqWaitCheck ==
    /\ BUGGY_CQWAIT_CHECK_EARLY
    /\ ~torn
    /\ cqw = "idle"
    /\ cqw'      = "checking"
    /\ cqw_flag' = CqAvailable
    /\ UNCHANGED <<phase, sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj,
                   cqe_posted, reg, cq, torn>>

(***************************************************************************)
(* BuggyCqWaitRegisterLate — the BUGGY second half: the hook installs now,  *)
(* AFTER the sample, and the flag is NOT re-sampled. A completion posted    *)
(* between BuggyCqWaitCheck and here was missed (it set cq but reached no   *)
(* hook), so the waiter will sleep on an already-ready CQ.                  *)
(***************************************************************************)
BuggyCqWaitRegisterLate ==
    /\ BUGGY_CQWAIT_CHECK_EARLY
    /\ cqw = "checking"
    /\ cqw' = "registered"
    /\ UNCHANGED <<phase, sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj,
                   cqe_posted, reg, cq, torn, cqw_flag>>

(***************************************************************************)
(* CqWaitCommitOrSleep — the evaluate point (first entry + every resume     *)
(* from a wake). A tsleep: the flag scan and the sleep transition are       *)
(* atomic under the waiter's own Rendez lock.                               *)
(*                                                                         *)
(*   flag set -> return (the app reaps the completion that arrived);        *)
(*   else nothing more can complete -> return what is posted (the impl's    *)
(*        async_inflight == 0 early return);                                *)
(*   else                       -> sleep on the ring's CQ wait-list.        *)
(*                                                                         *)
(* The decision reads the waiter's own FLAG (set under its Rendez lock by   *)
(* the producer's wait-list walk), NOT the live CQ level (cq_tail lives     *)
(* behind l->lock) — the cross-lock hand-off poll.tla pins. A commit that   *)
(* re-read the live level would mask the missed-wakeup bug (it could never  *)
(* sleep while ready); the flag is precisely what a buggy register order /  *)
(* a skipped wake leaves stale.                                             *)
CqWaitCommitOrSleep ==
    /\ cqw = "registered"
    /\ cqw' = IF cqw_flag             THEN "returned"
              ELSE IF ~CanStillComplete THEN "returned"
              ELSE "sleeping"
    /\ UNCHANGED <<phase, sqe_arg, snap_arg, acted_arg, snap_obj, acted_obj,
                   cqe_posted, reg, cq, torn, cqw_flag>>

(***************************************************************************)
(* Done — terminal self-loop. Once the ring is torn down, the CQ drained,   *)
(* and no op remains in an advanceable phase, the model halts; the explicit *)
(* stutter keeps the legitimate terminal state from being read as a         *)
(* deadlock.                                                                *)
(***************************************************************************)
Done ==
    /\ torn
    /\ cq = {}
    /\ \A o \in Ops : phase[o] \notin {"snap", "inflight", "completed", "cqd"}
    /\ UNCHANGED vars

Next ==
    \/ \E o \in Ops, a \in Args : UserProduce(o, a)
    \/ \E o \in Ops, a \in Args : UserMutateSqe(o, a)
    \/ \E obj \in Objects : UserRegister(obj)
    \/ \E o \in Ops : Consume(o)
    \/ \E o \in Ops : Dispatch(o)
    \/ \E o \in Ops : ReplyArrives(o)
    \/ \E o \in Ops : PostCqe(o)
    \/ \E o \in Ops : BuggyDoublePost(o)
    \/ \E o \in Ops : Reap(o)
    \/ Teardown
    \/ CqWaitRegister
    \/ BuggyCqWaitCheck
    \/ BuggyCqWaitRegisterLate
    \/ CqWaitCommitOrSleep
    \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* ArgPinnedToSnapshot (ring TOCTOU, §6.1) — the value the kernel acted on
\* equals the kernel snapshot, never a later re-read of the shared ring.
\* Violated by BUGGY_LIVE_SQE_REREAD.
ArgPinnedToSnapshot ==
    \A o \in Ops : acted_arg[o] # NONE_ARG => acted_arg[o] = snap_arg[o]

\* ActedArgValidated — the kernel only ever acts on a value that passed the
\* submit-time validity gate. The security consequence of the TOCTOU fix:
\* an out-of-bounds buffer descriptor (arg_bad) never drives an op.
ActedArgValidated ==
    \A o \in Ops : acted_arg[o] # NONE_ARG => acted_arg[o] \in ValidArgs

\* ObjPinnedToSnapshot (I-30, §6.2) — the object the op acts against equals
\* the submit-time pin, never a re-resolution of the live handle slot.
\* Violated by BUGGY_RECHECK_AT_COMPLETION.
ObjPinnedToSnapshot ==
    \A o \in Ops : acted_obj[o] # NONE_OBJ => acted_obj[o] = snap_obj[o]

\* ActedUnderAdmittedRights — the op acts only against an object whose rights
\* were ALLOWED at submit. The privilege consequence of the pin: a clunk+reuse
\* repointing the slot to a denied object cannot escalate/confuse the op.
ActedUnderAdmittedRights ==
    \A o \in Ops : acted_obj[o] # NONE_OBJ => acted_obj[o] \in AllowedObjs

\* NoDoubleCompletion (I-29, no-double) — at most one CQE per op.
\* Violated by BUGGY_DOUBLE_POST.
NoDoubleCompletion ==
    \A o \in Ops : cqe_posted[o] <= 1

\* NoSpuriousCompletion (I-29, no stale user_data) — a CQE is posted only for
\* an op that was genuinely admitted (its object was pinned at submit). No
\* completion ever surfaces for an un-admitted / never-submitted op.
NoSpuriousCompletion ==
    \A o \in Ops : cqe_posted[o] >= 1 => snap_obj[o] # NONE_OBJ

\* CqNeverOverfull (back-pressure, §6.4/§6.5) — the kernel never posts into a
\* full CQ; the count of unreaped CQEs never exceeds the ring capacity.
\* Violated by BUGGY_LOST_ON_FULL_CQ.
CqNeverOverfull ==
    Cardinality(cq) <= CQ_CAP

\* NoStaleCompletion (I-29, no-stale) — after tear-down, no op remains in a
\* state from which it could still post a CQE; teardown quiesced them all.
\* This forbids the precondition of a stale completion into freed CQ memory.
\* Violated by BUGGY_STALE_AFTER_TEARDOWN.
NoStaleCompletion ==
    torn => \A o \in Ops : phase[o] \notin {"snap", "inflight", "completed"}

\* SnapImpliesAdmitted — whenever an object is pinned, both submit-time gates
\* held: the arg snapshot was valid AND the object's rights were allowed.
SnapImpliesAdmitted ==
    \A o \in Ops :
        snap_obj[o] # NONE_OBJ =>
            (snap_arg[o] \in ValidArgs /\ snap_obj[o] \in AllowedObjs)

\* CqMembersPosted — every member of the CQ ring is a posted-not-yet-reaped
\* completion (phase cqd, cqe_posted >= 1). The ring holds exactly live CQEs.
CqMembersPosted ==
    \A o \in Ops : o \in cq => (phase[o] = "cqd" /\ cqe_posted[o] >= 1)

\* BoundedInflight (I-10 tag pool) — admitted-and-holding-a-tag count is
\* bounded; submission back-pressures on the finite tag pool.
BoundedInflight ==
    Cardinality(InFlight) <= MAX_INFLIGHT

\* CqFlagTracksCq (Loom-4, register-then-observe) — once the waiter's hook is
\* installed, an available CQE has its wake flag set. This is the mechanism
\* behind NoMissedCqWake (poll.tla's FlagTracksReady analog). Violated by
\* BUGGY_CQWAIT_CHECK_EARLY (installs the hook without re-sampling the CQ) and
\* BUGGY_CQWAIT_NO_WAKE (a post sets no flag).
CqFlagTracksCq ==
    (CqHookInstalled /\ CqAvailable) => cqw_flag

\* NoMissedCqWake (Loom-4, I-9 on the CQ wait-list) — a CQ-waiter is never left
\* asleep while an unreaped CQE is available for it to reap. The headline new
\* property: the SQPOLL kthread / a peer ENTER posting a CQE always wakes a
\* sleeping waiter. Violated by BUGGY_CQWAIT_CHECK_EARLY (the waiter samples the
\* CQ before installing its hook, so a post in the gap reaches no waiter and it
\* sleeps on an already-ready CQ) and BUGGY_CQWAIT_NO_WAKE (the post sets no flag
\* / signals no wait-list, so the sleeper never re-evaluates).
NoMissedCqWake == ~(cqw = "sleeping" /\ CqAvailable)

\* NoStrandedWaiter (Loom-4) — after tear-down no CQ-waiter remains asleep:
\* teardown (loom destroy / session death) woke it, so a waiter blocked for a
\* completion that will now never arrive returns instead of stranding. Violated
\* by BUGGY_CQWAIT_NO_WAKE (teardown skips the wake).
NoStrandedWaiter == torn => cqw # "sleeping"

\* CqWaitFlagSound — the waiter's wake flag is never spurious: it is set only
\* when a completion was/is available, or the ring tore down. poll never returns
\* a phantom completion to an ENTER caller. (cqe_posted is monotonic, so the
\* disjunction, once true, stays true — the flag latches a genuine edge.)
CqWaitFlagSound ==
    cqw_flag => (CqAvailable \/ (\E o \in Ops : cqe_posted[o] >= 1) \/ torn)

Invariants ==
    /\ TypeOk
    /\ ArgPinnedToSnapshot
    /\ ActedArgValidated
    /\ ObjPinnedToSnapshot
    /\ ActedUnderAdmittedRights
    /\ NoDoubleCompletion
    /\ NoSpuriousCompletion
    /\ CqNeverOverfull
    /\ NoStaleCompletion
    /\ SnapImpliesAdmitted
    /\ CqMembersPosted
    /\ BoundedInflight
    /\ CqFlagTracksCq
    /\ NoMissedCqWake
    /\ NoStrandedWaiter
    /\ CqWaitFlagSound

(***************************************************************************)
(* ============================== LIVENESS ================================ *)
(*                                                                         *)
(* EventuallyCompletes — I-29 no-lost: once an op is ADMITTED (reaches      *)
(* "snap"), it always eventually posts a CQE (reaches "cqd" / "reaped").    *)
(* Checked with ALLOW_TEARDOWN FALSE (no teardown to legitimately abandon   *)
(* an admitted op). Holds under weak fairness of the kernel's forward       *)
(* actions: Dispatch -> ReplyArrives -> PostCqe, with Reap draining the CQ  *)
(* so a back-pressured completion always gets a slot. The CQ-full wait      *)
(* (PostCqe disabled when full) is released by WF on Reap.                  *)
(***************************************************************************)
Liveness ==
    /\ \A o \in Ops : WF_vars(Dispatch(o))
    /\ \A o \in Ops : WF_vars(ReplyArrives(o))
    /\ \A o \in Ops : WF_vars(PostCqe(o))
    /\ \A o \in Ops : WF_vars(Reap(o))
    /\ WF_vars(CqWaitRegister)
    /\ WF_vars(CqWaitCommitOrSleep)

Spec_Live == Init /\ [][Next]_vars /\ Liveness

EventuallyCompletes ==
    \A o \in Ops : (phase[o] = "snap") ~> (phase[o] \in {"cqd", "reaped"})

\* CqWaiterReturns (Loom-4 no-strand liveness) — a CQ-waiter that started always
\* eventually returns: a completion wakes it (PostCqe), or nothing more can
\* complete (the give-up return). Checked with ALLOW_TEARDOWN FALSE, where the
\* only wakes are real completions: an admitted op the waiter sleeps on always
\* completes (WF on Dispatch -> ReplyArrives -> PostCqe, with Reap draining the
\* CQ), and a waiter with nothing admitted in flight gives up at CqWaitCommitOrSleep.
CqWaiterReturns == (cqw # "idle") ~> (cqw = "returned")

====
