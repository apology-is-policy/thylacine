---- MODULE 9p_client ----
(***************************************************************************)
(* Thylacine 9P client — P5-spec (Phase 5 entry; spec-first).              *)
(*                                                                         *)
(* Models the kernel's 9P2000.L client session: per-session tag pool,      *)
(* fid table, outstanding-request table, send/receive protocol. Pins       *)
(* ARCH §28 invariants:                                                    *)
(*                                                                         *)
(*   I-10 — Per-9P-session tag uniqueness.                                 *)
(*   I-11 — Per-9P-session fid identity is stable for fid's open lifetime. *)
(*                                                                         *)
(* Also pins two composition-layer properties not enumerated in §28 but    *)
(* called out in ROADMAP §7 exit criteria + `stratum/v2/docs/OS-INTEGRATION.md` *)
(* §3 (the "sync one-op-at-a-time vs pipelined" distinction):              *)
(*                                                                         *)
(*   OutOfOrderCorrectness — Rmessages match Tmessages by TAG, not by      *)
(*                           arrival order. Tag T's reply applies the      *)
(*                           state mutation of outstanding[T], never of    *)
(*                           a different tag's outstanding op.             *)
(*                                                                         *)
(*   FlowControl           — Outstanding-request cardinality is bounded by *)
(*                           MaxWindow. Back-pressure manifests as a       *)
(*                           Send-side block (the spec's action            *)
(*                           precondition refuses Send when                *)
(*                           |Inflight| = MaxWindow), never as a silent    *)
(*                           drop of an in-flight op.                      *)
(*                                                                         *)
(* Scope:                                                                  *)
(*                                                                         *)
(*   We model ONE 9P session (one client ↔ one server, one fid namespace,  *)
(*   one outstanding-request table). Cross-session isolation is the        *)
(*   server's concern (Stratum's namespace.tla + fid.tla cover it);        *)
(*   Thylacine's client has nothing to enforce there.                      *)
(*                                                                         *)
(*   We abstract op kinds to three: "walk" (allocates a new fid bound      *)
(*   to a source fid's target), "clunk" (releases a fid), "io" (uses a     *)
(*   fid; Tread / Twrite / Tgetattr / Tsetattr / Tlock / Tstatfs / Tsync   *)
(*   / Treflink / Tfallocate / Tfadvise / xattr family all collapse here   *)
(*   — what matters for the invariants is the tag + fid + op-id lifecycle, *)
(*   not the I/O semantics). The full 9P2000.L op list is in               *)
(*   `stratum/v2/docs/reference/20-9p.md` + ARCH §10.2.                    *)
(*                                                                         *)
(*   We model the COMPLETED-INTENT semantics: when Send(Tclunk, fid) fires,*)
(*   `bound_fids` is updated AT SEND TIME (the client treats fid as gone   *)
(*   the moment it submits Tclunk). This matches the kernel's intent —     *)
(*   no further ops should target the clunked fid even while Tclunk's      *)
(*   Rmsg is in flight. Tclunk's Receive is a no-op on bound_fids.         *)
(*                                                                         *)
(*   We track per-op `op_id` (monotonic, distinct across all sends) so     *)
(*   the invariants can pair Send with the corresponding Receive. This     *)
(*   captures "the Rmsg arriving under tag T must apply outstanding[T]'s   *)
(*   state mutation, not some other tag's op."                             *)
(*                                                                         *)
(* Concurrency / lock discipline (R15-c F230 structural close):           *)
(*                                                                         *)
(*   The spec actions are ATOMIC. SendIO, SendWalk, SendClunk, and         *)
(*   ReceiveOp each fire as single state transitions — there is no         *)
(*   intermediate state where, say, a tag has been "reserved" but its      *)
(*   op_id hasn't yet been committed. This atomicity-at-spec models the    *)
(*   impl's per-session lock discipline: every p9_client_X call acquires   *)
(*   the session-level spin_lock, performs send-build + transport-exchange *)
(*   + dispatch + bookkeeping, and releases the lock — the entire critical*)
(*   section appears atomic to any other caller of the same client.        *)
(*                                                                         *)
(*   The lock guards (per `kernel/9p_client.c::client_lock` + the embedded *)
(*   session's bookkeeping arrays):                                        *)
(*                                                                         *)
(*     - next_fid (the monotonic fid allocator) — atomic via the lock      *)
(*       (alternatively atomic via __atomic_fetch_add; impl uses the lock  *)
(*       for uniformity).                                                  *)
(*     - outstanding[] (the tag-indexed in-flight table)                   *)
(*     - bound_fids[] (the fid table)                                      *)
(*     - out_buf (the inline Tmsg scratch buffer)                          *)
(*     - all session counters (next_op_id, total_sent, total_completed)   *)
(*                                                                         *)
(*   Cross-Proc sharing scenario (R15-c primary motivator): two Procs hold *)
(*   inherited dev9p Spoors that wrap the SAME p9_client (via              *)
(*   SYS_SPAWN_WITH_FDS / SYS_SPAWN_FULL). Both Procs concurrently call    *)
(*   into the client from different CPUs. Without the lock:                *)
(*                                                                         *)
(*     - Race on next_fid: both increment c->next_fid concurrently → two   *)
(*       callers receive the same fid value. Visible at the spec level as  *)
(*       a tag-collision-shaped invariant break (TagAndOpAccounting).      *)
(*                                                                         *)
(*     - Race on outstanding[]: both find outstanding[t] = NONE, both      *)
(*       write — second write wins; first op_id is dropped from            *)
(*       bookkeeping. Modeled by BuggyTagCollisionSend (below).             *)
(*                                                                         *)
(*     - Race on dispatch ↔ send: ReceiveOp clears outstanding[t] while    *)
(*       Send reads it as NONE; the dispatch's state mutation may end up   *)
(*       being applied to the wrong op_id. Modeled by BuggyOOOReceive.    *)
(*                                                                         *)
(*     - Race on out_buf: both Sends build into the same buffer; the wire  *)
(*       sees corrupted bytes. Not directly modeled (out_buf is below the  *)
(*       spec's abstraction level), but a corrupted Tmsg surfaces at the   *)
(*       transport layer as a malformed-frame error.                       *)
(*                                                                         *)
(*   The existing buggy variants (TagCollision, OOOMatch, FidAfterClunk,   *)
(*   Unbounded) ARE the spec's coverage of what unlocked races could       *)
(*   produce. Adding lock_held as an explicit state variable would         *)
(*   restate the structural property without adding TLC coverage — the     *)
(*   atomic-action style ALREADY pins the lock discipline (mirrors         *)
(*   pipe.tla §27-29 modeling decisions for the pipe's wait/wake critical *)
(*   section).                                                              *)
(*                                                                         *)
(*   Impl-side R15-c close: kernel/9p_session.c + kernel/9p_client.c gain  *)
(*   a per-session spin_lock_t; every Send and ReceiveOp path acquires the *)
(*   lock for the duration of the operation. See `kernel/9p_client.c`     *)
(*   p9_client_X functions and the spec-to-code mapping in                 *)
(*   `specs/SPEC-TO-CODE.md`.                                              *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   `outstanding[t]` is a record with kind, fid, new_fid, and op_id       *)
(*   fields, OR the sentinel NONE. Tag t is "free" iff outstanding[t] =    *)
(*   NONE. Cardinality({t : outstanding[t] # NONE}) is the in-flight       *)
(*   count, which the FlowControl invariant bounds.                        *)
(*                                                                         *)
(*   `bound_fids` is a SUBSET of FidIds (the fids currently bound on this  *)
(*   session). Init: empty. After OpenSession: {RootFid}. Walk Receive     *)
(*   adds the new_fid; Clunk Send removes the target fid.                  *)
(*                                                                         *)
(*   `sent_ops` and `completed_ops` are SUBSETs of 1..MaxOps. Each Send    *)
(*   bumps op_seq and adds the new op_id to sent_ops. Each Receive adds    *)
(*   the completing op_id (= outstanding[t].op_id) to completed_ops.       *)
(*   The TagAndOpAccounting invariant ties these together:                 *)
(*                                                                         *)
(*     {outstanding[t].op_id : t \in Inflight} = sent_ops \ completed_ops  *)
(*                                                                         *)
(*   Reading: "the set of op_ids currently in the outstanding table is     *)
(*   exactly the set of sent-but-not-completed op_ids." A tag-collision    *)
(*   bug (overwriting outstanding[t] before its Rmsg arrives) drops an     *)
(*   op_id from outstanding without ever placing it in completed_ops —     *)
(*   the invariant LHS shrinks but RHS doesn't. An out-of-order-match     *)
(*   bug (pairing Rmsg of tag T with outstanding[fake_t]'s op_id, then    *)
(*   clearing tag T) leaves tag fake_t's op still in outstanding even     *)
(*   though completed_ops now contains its op_id — LHS still contains it,*)
(*   RHS doesn't. Both bugs surface as the invariant breaking.            *)
(*                                                                         *)
(*   `RootFid` is a CONSTANT in FidIds, established by Tattach. Subsequent*)
(*   ops can use it as a Walk source or as an IO target. It is never      *)
(*   clunked (the spec's Send action refuses fid = RootFid).              *)
(*                                                                         *)
(*   MaxOps bounds op_seq for TLC. Real-world tag space is 16-bit         *)
(*   (NOTAG = 0xFFFF); op_seq tracks the cumulative count of sends across *)
(*   the session, used here only as a uniqueness identifier for the       *)
(*   spec's bookkeeping. MaxOps = 3 surfaces every buggy variant.         *)
(*                                                                         *)
(*   MaxWindow bounds Inflight. Real-world window depends on negotiated   *)
(*   msize / per-connection buffer sizing per OS-INTEGRATION.md §14;      *)
(*   the spec's bound is a model parameter. MaxWindow = 2 is the minimum *)
(*   that allows pipelined out-of-order completion + at least one free   *)
(*   tag slot under |TagIds| = 3.                                         *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation per CLAUDE.md spec-first*)
(* policy):                                                                *)
(*                                                                         *)
(*   9p_client.cfg                          all flags FALSE — TLC proves   *)
(*                                          all 4 invariants.              *)
(*   9p_client_buggy_tag_collision.cfg      BUGGY_TAG_COLLISION = TRUE —   *)
(*                                          counterexample where a tag is *)
(*                                          allocated while still in use; *)
(*                                          the prior op_id is lost.      *)
(*   9p_client_buggy_fid_after_clunk.cfg    BUGGY_FID_AFTER_CLUNK = TRUE —*)
(*                                          counterexample where an op    *)
(*                                          targets a fid not in          *)
(*                                          bound_fids (after Tclunk).    *)
(*   9p_client_buggy_ooo_match.cfg          BUGGY_OOO_MATCH = TRUE —      *)
(*                                          counterexample where a tag's *)
(*                                          Rmsg is paired with a        *)
(*                                          DIFFERENT tag's outstanding  *)
(*                                          op (out-of-order misorder).  *)
(*   9p_client_buggy_unbounded.cfg          BUGGY_UNBOUNDED = TRUE —      *)
(*                                          counterexample where Send    *)
(*                                          fires past MaxWindow.        *)
(*   9p_client_buggy_async_clunk_tag_leak.cfg                             *)
(*                                          BUGGY_ASYNC_CLUNK_TAG_LEAK = *)
(*                                          TRUE — counterexample where  *)
(*                                          an ownerless Rclunk is       *)
(*                                          consumed off the wire but    *)
(*                                          the tag is never freed (the  *)
(*                                          FID-LIFECYCLE async-clunk    *)
(*                                          hazard: the fire-and-forget  *)
(*                                          clunk's tag slot leaks).     *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   TypeOk                  — variable types.                             *)
(*   RootFidImmutable        — RootFid is bound iff session is open.       *)
(*   TagAndOpAccounting      — I-10 + OutOfOrderCorrectness (compound).    *)
(*   FidStability            — I-11.                                       *)
(*   BoundedOutstanding      — FlowControl.                                *)
(*                                                                         *)
(* See ARCHITECTURE.md §10.2 (9P dialect) + §14 (Stratum integration) +    *)
(* §28 (invariants I-10, I-11) + ROADMAP.md §7 (Phase 5 plan).             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    TagIds,                       \* set of tag values
    FidIds,                       \* set of fid values
    RootFid,                      \* the root fid (\in FidIds)
    MaxWindow,                    \* Nat — max in-flight ops
    MaxOps,                       \* Nat — bound on op_seq for TLC
    BUGGY_TAG_COLLISION,          \* BOOLEAN
    BUGGY_FID_AFTER_CLUNK,        \* BOOLEAN
    BUGGY_OOO_MATCH,              \* BOOLEAN
    BUGGY_UNBOUNDED,              \* BOOLEAN
    BUGGY_ASYNC_CLUNK_TAG_LEAK    \* BOOLEAN

ASSUME Cardinality(TagIds) >= 2
ASSUME Cardinality(FidIds) >= 2
ASSUME RootFid \in FidIds
ASSUME MaxWindow \in Nat /\ MaxWindow >= 1 /\ MaxWindow <= Cardinality(TagIds)
ASSUME MaxOps \in Nat /\ MaxOps >= 2
ASSUME BUGGY_TAG_COLLISION \in BOOLEAN
ASSUME BUGGY_FID_AFTER_CLUNK \in BOOLEAN
ASSUME BUGGY_OOO_MATCH \in BOOLEAN
ASSUME BUGGY_UNBOUNDED \in BOOLEAN
ASSUME BUGGY_ASYNC_CLUNK_TAG_LEAK \in BOOLEAN

\* Op kinds. "walk" allocates a new fid bound to src; "clunk" releases a fid;
\* "io" uses a fid for any of Tread/Twrite/Tgetattr/Tsetattr/Tlock/Tstatfs/
\* Tsync/Treflink/Tfallocate/Tfadvise/xattr-family. The spec collapses all
\* IO kinds because the invariants don't care about the I/O semantics.
Kinds == {"walk", "clunk", "io"}

\* Sentinel for "tag is free." Outside the Operations universe so its op_id=0
\* is distinguishable from any sent op_id (which start at 1).
NONE == [op_id |-> 0, kind |-> "none", fid |-> RootFid, new_fid |-> RootFid]

\* An in-flight op. op_id is the monotonic spec-side identifier; fid is the
\* primary target; new_fid is the destination for walk (== fid for io/clunk).
Operations == [op_id : 1..MaxOps, kind : Kinds, fid : FidIds, new_fid : FidIds]

VARIABLES
    session_open,    \* BOOLEAN — Tattach completed, Tdetach/close not yet
    bound_fids,      \* SUBSET FidIds — fids currently bound on this session
    outstanding,     \* [TagIds -> Operations \cup {NONE}]
    op_seq,          \* Nat — monotonic op-id counter (bounded by MaxOps)
    sent_ops,        \* SUBSET 1..MaxOps — every op_id ever sent
    completed_ops    \* SUBSET 1..MaxOps — every op_id whose Rmsg landed

vars == <<session_open, bound_fids, outstanding, op_seq, sent_ops, completed_ops>>

TypeOk ==
    /\ session_open \in BOOLEAN
    /\ bound_fids \subseteq FidIds
    /\ outstanding \in [TagIds -> Operations \cup {NONE}]
    /\ op_seq \in 0..MaxOps
    /\ sent_ops \subseteq 1..MaxOps
    /\ completed_ops \subseteq sent_ops

\* The set of tags currently holding an in-flight op.
Inflight == {t \in TagIds : outstanding[t] # NONE}

\* The set of op_ids currently in the outstanding table.
InflightOpIds == {outstanding[t].op_id : t \in Inflight}

(***************************************************************************)
(* Init: closed session; no fids bound; all tags free; counters zero.      *)
(***************************************************************************)
Init ==
    /\ session_open = FALSE
    /\ bound_fids = {}
    /\ outstanding = [t \in TagIds |-> NONE]
    /\ op_seq = 0
    /\ sent_ops = {}
    /\ completed_ops = {}

(***************************************************************************)
(* OpenSession — models the Tversion + Tattach handshake atomically.       *)
(* On Rattach, the root_fid is bound. Maps to                              *)
(* `kernel/9p_attach.c::session_attach` (Phase 5 P5-attach chunk).         *)
(***************************************************************************)
OpenSession ==
    /\ ~session_open
    /\ session_open' = TRUE
    /\ bound_fids' = {RootFid}
    /\ outstanding' = [t \in TagIds |-> NONE]
    /\ UNCHANGED <<op_seq, sent_ops, completed_ops>>

(***************************************************************************)
(* CloseSession — the client closes the connection. Requires every         *)
(* outstanding op to have completed (Inflight = {}); the client cannot     *)
(* abandon in-flight ops without abort semantics, which we don't model.    *)
(* Real-world counterpart: per-Proc connection cleanup in                  *)
(* `kernel/9p_attach.c` at proc_exit.                                      *)
(***************************************************************************)
CloseSession ==
    /\ session_open
    /\ Inflight = {}
    /\ session_open' = FALSE
    /\ bound_fids' = {}
    /\ UNCHANGED <<outstanding, op_seq, sent_ops, completed_ops>>

(***************************************************************************)
(* SendIO(t, fid) — allocate tag t, place an IO op targeting fid.          *)
(* Preconditions: session open; tag free; window not full; fid bound;      *)
(* op_seq budget remaining. Maps to the wire-level Tmsg-Send path for      *)
(* Tread/Twrite/Tgetattr/Tsetattr/Tlock/Tstatfs/Tsync/etc.                 *)
(***************************************************************************)
SendIO(t, fid) ==
    /\ session_open
    /\ outstanding[t] = NONE
    /\ Cardinality(Inflight) < MaxWindow
    /\ fid \in bound_fids
    /\ op_seq < MaxOps
    /\ outstanding' = [outstanding EXCEPT
                       ![t] = [op_id |-> op_seq + 1,
                               kind |-> "io",
                               fid |-> fid,
                               new_fid |-> fid]]
    /\ op_seq' = op_seq + 1
    /\ sent_ops' = sent_ops \cup {op_seq + 1}
    /\ UNCHANGED <<session_open, bound_fids, completed_ops>>

(***************************************************************************)
(* SendWalk(t, src_fid, new_fid) — Twalk that clones src_fid into new_fid. *)
(* Spec models the Twalk(n_names=0) shape (fid clone) — sufficient for the *)
(* invariants. Multi-component walks add structure but not new invariants. *)
(* Preconditions: src bound; new not bound; new not RootFid; window has    *)
(* room; tag free; session open.                                           *)
(***************************************************************************)
SendWalk(t, src_fid, new_fid) ==
    /\ session_open
    /\ outstanding[t] = NONE
    /\ Cardinality(Inflight) < MaxWindow
    /\ src_fid \in bound_fids
    /\ new_fid \in FidIds
    /\ new_fid \notin bound_fids
    /\ new_fid # RootFid                       \* root is server-managed
    /\ \A t2 \in TagIds :                      \* not pending in another walk
         outstanding[t2] # NONE =>
           ~(outstanding[t2].kind = "walk" /\ outstanding[t2].new_fid = new_fid)
    /\ op_seq < MaxOps
    /\ outstanding' = [outstanding EXCEPT
                       ![t] = [op_id |-> op_seq + 1,
                               kind |-> "walk",
                               fid |-> src_fid,
                               new_fid |-> new_fid]]
    /\ op_seq' = op_seq + 1
    /\ sent_ops' = sent_ops \cup {op_seq + 1}
    /\ UNCHANGED <<session_open, bound_fids, completed_ops>>

(***************************************************************************)
(* SendClunk(t, fid) — Tclunk that releases fid. Send-time unbinds         *)
(* bound_fids[fid] (modeling the client's intent: no further ops should    *)
(* target this fid even while Tclunk's Rmsg is in flight). Receive does    *)
(* not re-touch bound_fids for clunk kind.                                 *)
(* Preconditions: session open; tag free; window has room; fid bound; fid  *)
(* is not RootFid (root is released only at session close).                *)
(***************************************************************************)
SendClunk(t, fid) ==
    /\ session_open
    /\ outstanding[t] = NONE
    /\ Cardinality(Inflight) < MaxWindow
    /\ fid \in bound_fids
    /\ fid # RootFid
    \* Canonical client discipline: do not clunk a fid that has any
    \* in-flight op targeting it. The same constraint also blocks
    \* clunking a Walk's source fid that's still in flight (Walk's src is
    \* outstanding[t2].fid). Walk's new_fid is not yet in bound_fids, so
    \* Clunk against it would have failed the bound_fids check above
    \* regardless of this guard.
    /\ \A t2 \in TagIds :
         outstanding[t2] # NONE => outstanding[t2].fid # fid
    /\ op_seq < MaxOps
    /\ outstanding' = [outstanding EXCEPT
                       ![t] = [op_id |-> op_seq + 1,
                               kind |-> "clunk",
                               fid |-> fid,
                               new_fid |-> fid]]
    /\ bound_fids' = bound_fids \ {fid}
    /\ op_seq' = op_seq + 1
    /\ sent_ops' = sent_ops \cup {op_seq + 1}
    /\ UNCHANGED <<session_open, completed_ops>>

(***************************************************************************)
(* ReceiveOp(t) — Rmsg with tag t arrives; client matches by tag, applies  *)
(* state mutation. The KEY correctness step: the mutation reads            *)
(* outstanding[t] (NOT outstanding[any_other_tag]) — captures              *)
(* OutOfOrderCorrectness at the action level.                              *)
(*                                                                         *)
(* State mutation by kind:                                                 *)
(*   "walk"  — bind new_fid into bound_fids.                               *)
(*   "clunk" — no-op (Send already unbound).                               *)
(*   "io"    — no fid-table mutation.                                      *)
(*                                                                         *)
(* In all kinds, the op_id moves from sent-but-not-completed (LHS of       *)
(* TagAndOpAccounting) to completed (RHS). Tag t is freed.                 *)
(***************************************************************************)
ReceiveOp(t) ==
    /\ outstanding[t] # NONE
    /\ LET op == outstanding[t] IN
         /\ bound_fids' = IF op.kind = "walk"
                          THEN bound_fids \cup {op.new_fid}
                          ELSE bound_fids
         /\ completed_ops' = completed_ops \cup {op.op_id}
    /\ outstanding' = [outstanding EXCEPT ![t] = NONE]
    /\ UNCHANGED <<session_open, op_seq, sent_ops>>

(***************************************************************************)
(* ======================== BUGGY VARIANTS ================================ *)
(***************************************************************************)

(***************************************************************************)
(* BuggyTagCollisionSend(t, fid) — allocate tag t WITHOUT checking that    *)
(* it's free. The previously-stored op_id is overwritten and never reaches *)
(* completed_ops — the prior op is lost from the client's bookkeeping.     *)
(*                                                                         *)
(* Real-world analogue: a kernel/9p_session.c::alloc_tag bug that returns  *)
(* an already-in-use tag because the freelist isn't checked correctly, or  *)
(* a tag generation counter that wraps without a "is this tag in use" gate.*)
(*                                                                         *)
(* Caught by TagAndOpAccounting at the step that the buggy action fires.   *)
(***************************************************************************)
BuggyTagCollisionSend(t, fid) ==
    /\ BUGGY_TAG_COLLISION
    /\ session_open
    /\ outstanding[t] # NONE                   \* THE BUG: should be = NONE
    /\ fid \in bound_fids
    /\ op_seq < MaxOps
    /\ outstanding' = [outstanding EXCEPT
                       ![t] = [op_id |-> op_seq + 1,
                               kind |-> "io",
                               fid |-> fid,
                               new_fid |-> fid]]
    /\ op_seq' = op_seq + 1
    /\ sent_ops' = sent_ops \cup {op_seq + 1}
    /\ UNCHANGED <<session_open, bound_fids, completed_ops>>

(***************************************************************************)
(* BuggyFidAfterClunkSend(t, fid) — issue IO on a fid NOT in bound_fids.   *)
(* Captures "use fid after Tclunk" — the canonical I-11 violation.         *)
(*                                                                         *)
(* Real-world analogue: a kernel-side caller that retains a fid handle    *)
(* past its clunk (e.g., a stale pointer in a per-thread fid cache, or a  *)
(* reference held by a syscall-arg dispatcher that doesn't observe        *)
(* clunk's side-effect on the fid table).                                 *)
(*                                                                         *)
(* Caught by FidStability at the step that the buggy action fires.         *)
(***************************************************************************)
BuggyFidAfterClunkSend(t, fid) ==
    /\ BUGGY_FID_AFTER_CLUNK
    /\ session_open
    /\ outstanding[t] = NONE
    /\ Cardinality(Inflight) < MaxWindow
    /\ fid \in FidIds
    /\ fid \notin bound_fids                   \* THE BUG: should be \in
    /\ fid # RootFid                           \* not the root (always bound)
    /\ op_seq < MaxOps
    /\ outstanding' = [outstanding EXCEPT
                       ![t] = [op_id |-> op_seq + 1,
                               kind |-> "io",
                               fid |-> fid,
                               new_fid |-> fid]]
    /\ op_seq' = op_seq + 1
    /\ sent_ops' = sent_ops \cup {op_seq + 1}
    /\ UNCHANGED <<session_open, bound_fids, completed_ops>>

(***************************************************************************)
(* BuggyOOOReceive(t, fake_t) — Rmsg with tag t arrives, but the client    *)
(* mis-pairs it with outstanding[fake_t]'s op instead. The state mutation  *)
(* that gets applied is fake_t's op (wrong fid binding update); tag t's    *)
(* slot is cleared but fake_t's op_id ends up in completed_ops (NOT t's).  *)
(*                                                                         *)
(* Real-world analogue: a kernel/9p_session.c receive loop that reads the  *)
(* tag field of an Rmsg but indexes outstanding[] with a stale local       *)
(* variable, or a bug in tag-to-op lookup where the wrong table entry is   *)
(* returned. Linux v9fs has had analogous bugs historically — the          *)
(* "by-tag" match is load-bearing and easy to get wrong under pipelining.  *)
(*                                                                         *)
(* Caught by TagAndOpAccounting at the step that the buggy action fires    *)
(* (LHS contains fake_t's op_id, RHS doesn't since completed_ops grew by   *)
(* fake_t's op_id; the bijection breaks).                                  *)
(***************************************************************************)
BuggyOOOReceive(t, fake_t) ==
    /\ BUGGY_OOO_MATCH
    /\ outstanding[t] # NONE
    /\ outstanding[fake_t] # NONE
    /\ t # fake_t                              \* mis-pair two distinct ops
    /\ LET fake_op == outstanding[fake_t] IN
         /\ bound_fids' = IF fake_op.kind = "walk"
                          THEN bound_fids \cup {fake_op.new_fid}
                          ELSE bound_fids
         /\ completed_ops' = completed_ops \cup {fake_op.op_id}
    /\ outstanding' = [outstanding EXCEPT ![t] = NONE]
    /\ UNCHANGED <<session_open, op_seq, sent_ops>>

(***************************************************************************)
(* BuggyUnboundedSend(t, fid) — Send fires past MaxWindow. Captures the   *)
(* "no back-pressure" bug: the client doesn't bound outstanding, lets it  *)
(* grow without limit. In real systems this manifests as server overload, *)
(* memory blowup, or buffer-full deadlock that should have surfaced as    *)
(* Send-side blocking.                                                     *)
(*                                                                         *)
(* Caught by BoundedOutstanding at the step that the buggy action fires.   *)
(***************************************************************************)
BuggyUnboundedSend(t, fid) ==
    /\ BUGGY_UNBOUNDED
    /\ session_open
    /\ outstanding[t] = NONE
    /\ Cardinality(Inflight) >= MaxWindow      \* THE BUG: should be <
    /\ fid \in bound_fids
    /\ op_seq < MaxOps
    /\ outstanding' = [outstanding EXCEPT
                       ![t] = [op_id |-> op_seq + 1,
                               kind |-> "io",
                               fid |-> fid,
                               new_fid |-> fid]]
    /\ op_seq' = op_seq + 1
    /\ sent_ops' = sent_ops \cup {op_seq + 1}
    /\ UNCHANGED <<session_open, bound_fids, completed_ops>>

(***************************************************************************)
(* BuggyAsyncClunkLeakReceive(t) — the FID-LIFECYCLE async-clunk hazard    *)
(* (docs/FID-LIFECYCLE-DESIGN.md §3.1). The impl's p9_client_clunk_async   *)
(* sends Tclunk fire-and-forget: no submitter thread waits, and the        *)
(* Rclunk arrives OWNERLESS — it must be drained by a later op's elected   *)
(* reader via the #845 ownerless-dispatch arm, whose clear_outstanding     *)
(* frees the tag. The CLEAN model needs no new action: SendClunk already   *)
(* holds the tag until ReceiveOp(t) frees it, and the spec never modeled   *)
(* a blocking submitter — ReceiveOp(t) on a clunk-kind tag IS the          *)
(* ownerless drain (who drains is below the abstraction). The impl's      *)
(* monotonic never-reused fid allocator is strictly MORE conservative     *)
(* than the spec's finite-FidIds reuse-after-clunk traces, so the checked *)
(* state space is a superset of the impl's.                               *)
(*                                                                         *)
(* THE BUG this action injects: the ownerless Rclunk is consumed off the   *)
(* wire (the reply landed — completed_ops grows) but the dispatch fails    *)
(* to clear_outstanding — outstanding[t] retains the op FOREVER. One of    *)
(* the 64 outstanding[] slots is permanently burned; enough leaked clunks  *)
(* exhaust the tag pool and the client stalls. Caught by                   *)
(* TagAndOpAccounting at the leaking step (the op_id enters completed_ops  *)
(* while still present in InflightOpIds — the bijection breaks).           *)
(*                                                                         *)
(* Real-world analogue: demux_frame_locked consuming a reply whose tag has *)
(* no waiter and discarding it WITHOUT the dispatch_rmsg P9_TCLUNK →       *)
(* clear_outstanding arm — exactly what the pre-#845 code would have done  *)
(* with any ownerless reply.                                               *)
(***************************************************************************)
BuggyAsyncClunkLeakReceive(t) ==
    /\ BUGGY_ASYNC_CLUNK_TAG_LEAK
    /\ outstanding[t] # NONE
    /\ outstanding[t].kind = "clunk"
    /\ completed_ops' = completed_ops \cup {outstanding[t].op_id}
    /\ UNCHANGED <<session_open, bound_fids, outstanding, op_seq, sent_ops>>

Next ==
    \/ OpenSession
    \/ CloseSession
    \/ \E t \in TagIds, f \in FidIds : SendIO(t, f)
    \/ \E t \in TagIds, s \in FidIds, n \in FidIds : SendWalk(t, s, n)
    \/ \E t \in TagIds, f \in FidIds : SendClunk(t, f)
    \/ \E t \in TagIds : ReceiveOp(t)
    \/ \E t \in TagIds, f \in FidIds : BuggyTagCollisionSend(t, f)
    \/ \E t \in TagIds, f \in FidIds : BuggyFidAfterClunkSend(t, f)
    \/ \E t \in TagIds, ft \in TagIds : BuggyOOOReceive(t, ft)
    \/ \E t \in TagIds, f \in FidIds : BuggyUnboundedSend(t, f)
    \/ \E t \in TagIds : BuggyAsyncClunkLeakReceive(t)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(***************************************************************************)
(* RootFidImmutable — the root fid is bound iff the session is open. The   *)
(* root is established by Tattach (modeled by OpenSession's bound_fids =   *)
(* {RootFid}) and released by session close. No Send action can clunk it.  *)
(***************************************************************************)
RootFidImmutable ==
    (session_open <=> RootFid \in bound_fids)

(***************************************************************************)
(* TagAndOpAccounting — pairs I-10 (per-session tag uniqueness) with       *)
(* OutOfOrderCorrectness (Rmsg-by-tag matching). The set of op_ids         *)
(* currently in the outstanding table equals the set of sent-but-not-      *)
(* completed op_ids.                                                       *)
(*                                                                         *)
(* Violation shapes this catches:                                          *)
(*                                                                         *)
(*   - Tag collision (BuggyTagCollisionSend) — overwriting outstanding[t]  *)
(*     before its Rmsg arrived drops an op_id from LHS but not from RHS.   *)
(*                                                                         *)
(*   - Out-of-order mis-pair (BuggyOOOReceive) — completing fake_t's op    *)
(*     while clearing t's slot moves fake_t's op_id to RHS (completed)    *)
(*     while it's still in outstanding[fake_t], so LHS still contains it. *)
(*     The bijection breaks.                                               *)
(*                                                                         *)
(*   - Tag double-free (not explicitly modeled here, but would surface    *)
(*     similarly — if Receive cleared outstanding[t] without adding the   *)
(*     op_id to completed_ops, the bijection would break the same way).   *)
(***************************************************************************)
TagAndOpAccounting ==
    InflightOpIds = sent_ops \ completed_ops

(***************************************************************************)
(* FidStability — every outstanding op's primary fid is currently bound,   *)
(* except for clunk (which Send-time-unbinds its target). Captures I-11 in *)
(* the iff form:                                                           *)
(*                                                                         *)
(*   - In flight io / walk targeting an unbound fid (post-Tclunk on that  *)
(*     fid) violates: op_id is in outstanding[t], outstanding[t].fid is   *)
(*     not in bound_fids.                                                  *)
(***************************************************************************)
FidStability ==
    \A t \in TagIds :
        LET op == outstanding[t] IN
            (op # NONE /\ op.kind \in {"io", "walk"}) =>
                op.fid \in bound_fids

(***************************************************************************)
(* BoundedOutstanding — flow-control bound: outstanding-request count      *)
(* never exceeds MaxWindow. Violated by BuggyUnboundedSend.                *)
(***************************************************************************)
BoundedOutstanding ==
    Cardinality(Inflight) <= MaxWindow

Invariants ==
    /\ TypeOk
    /\ RootFidImmutable
    /\ TagAndOpAccounting
    /\ FidStability
    /\ BoundedOutstanding

====
