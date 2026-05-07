---- MODULE handles ----
(***************************************************************************)
(* Thylacine handles — P2-Fa spec.                                         *)
(*                                                                         *)
(* Models the kernel handle table + handle rights + transfer semantics +  *)
(* per-Proc coarse capabilities.                                           *)
(*                                                                         *)
(* Pins ARCH §28 invariants:                                                *)
(*                                                                         *)
(*   I-2 — Capability set monotonically reduces (rfork only reduces).      *)
(*   I-4 — Handles transfer between processes ONLY via 9P sessions.        *)
(*   I-5 — KObj_MMIO, KObj_IRQ, KObj_DMA cannot be transferred.            *)
(*   I-6 — Handle rights monotonically reduce on transfer.                 *)
(*                                                                         *)
(* I-7 (BURROW refcount + mapping lifecycle) is OUT OF SCOPE — covered by    *)
(* `burrow.tla` because it concerns memory-object lifetime, not handle       *)
(* policy.                                                                 *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   Kobj universe is partitioned into TxKObjs (transferable: Process,   *)
(*   Thread, BURROW, Spoor) and HwKObjs (non-transferable: MMIO, IRQ, DMA,   *)
(*   Interrupt). The partition is a CONSTANT — no function-valued        *)
(*   constants, simpler for TLC.                                          *)
(*                                                                         *)
(*   handles[p] is the SET of HandleRecords held by Proc p. Each record   *)
(*   is [kobj, rights, origin_proc, via]. We model as a set rather than a *)
(*   table because the index slot is an impl detail; the invariants       *)
(*   reason over "what handles a proc holds," not over indices.           *)
(*                                                                         *)
(*   origin_rights[k] is the SET of rights granted to the FIRST holder   *)
(*   of kobj k, fixed at HandleAlloc time and never changed. The state   *)
(*   invariant `RightsCeiling` requires every handle's rights to be a    *)
(*   subset of origin_rights[its kobj]. A buggy elevation that fabricates*)
(*   bits never granted to anyone violates this.                          *)
(*                                                                         *)
(*   Provenance per handle (`via` field): "orig" (kernel grant), "dup"   *)
(*   (within proc), "9p" (transferred via 9P session), "direct" (BUGGY    *)
(*   only — direct cross-proc transfer with no session). The state       *)
(*   invariant `OnlyTransferVia9P` requires every handle to have via #   *)
(*   "direct".                                                            *)
(*                                                                         *)
(*   For non-transferable types: `origin_proc` is set at HandleAlloc and *)
(*   preserved by all transfer paths. The state invariant                 *)
(*   `HwHandlesAtOrigin` requires every hw-typed handle to be held by    *)
(*   its origin_proc. A buggy hw-transfer puts a hw handle in a non-origin*)
(*   proc.                                                                 *)
(*                                                                         *)
(*   Sessions are SUBSET (Procs \X Procs) — directional 9P session pairs.*)
(*   Open and close as separate actions. Transfer-via-9P requires the    *)
(*   session to be open. Buggy direct transfer skips the session.         *)
(*                                                                         *)
(*   Process capabilities are modeled as proc_caps[p] \in SUBSET Caps.   *)
(*   ReduceCaps lowers; BuggyCapsElevate raises. Each proc has an       *)
(*   InitialCapsOf(p) ceiling: ProcRoot starts with Caps (the universe), *)
(*   every other proc starts with {} (empty caps). CapsCeiling requires  *)
(*   proc_caps[p] \subseteq InitialCapsOf(p).                            *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation per CLAUDE.md spec-first*)
(* policy):                                                                *)
(*                                                                         *)
(*   handles.cfg                  all flags FALSE — TLC proves all       *)
(*                                invariants.                              *)
(*   handles_buggy_elevate.cfg    BUGGY_ELEVATE=TRUE — counterexample   *)
(*                                where dup fabricates rights bits.        *)
(*   handles_buggy_hw.cfg         BUGGY_HW_TRANSFER=TRUE — counter-     *)
(*                                example where a hw handle ends up at   *)
(*                                a non-origin proc.                       *)
(*   handles_buggy_direct.cfg     BUGGY_DIRECT_TRANSFER=TRUE — counter- *)
(*                                example where a handle has via="direct".*)
(*   handles_buggy_caps.cfg       BUGGY_CAPS_ELEVATE=TRUE — counter-    *)
(*                                example where a non-root proc gains   *)
(*                                caps it never started with.             *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   RightsCeiling      — every handle's rights \subseteq origin_rights  *)
(*                        of the kobj it points at. ARCH §28 I-6 (rights *)
(*                        monotonic on transfer) + the bound on dup.     *)
(*                                                                         *)
(*   HwHandlesAtOrigin  — every hw-typed handle is held by its origin    *)
(*                        proc. ARCH §28 I-5 (KObj_MMIO/IRQ/DMA non-     *)
(*                        transferable).                                   *)
(*                                                                         *)
(*   OnlyTransferVia9P  — every handle's via field is in {orig, dup, 9p}*)
(*                        — never "direct". ARCH §28 I-4 (transfer only *)
(*                        via 9P).                                          *)
(*                                                                         *)
(*   CapsCeiling        — every proc's caps \subseteq its InitialCapsOf *)
(*                        ceiling. ARCH §28 I-2 (capability monotonic    *)
(*                        reduction).                                      *)
(*                                                                         *)
(* See ARCHITECTURE.md §13 (capabilities), §18 (handles), §28 invariants. *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Procs,                  \* set of process identifiers (>= 2)
    TxKObjs,                \* SUBSET KObjs — transferable kobjs (Tx universe)
    HwKObjs,                \* SUBSET KObjs — hardware/non-transferable kobjs
    Rights,                 \* set of right labels (must include RightTransfer)
    RightTransfer,          \* the "RIGHT_TRANSFER" element of Rights
    Caps,                   \* set of capability labels
    BUGGY_ELEVATE,          \* BOOLEAN — enables BuggyDupElevate
    BUGGY_HW_TRANSFER,      \* BOOLEAN — enables BuggyHwTransfer
    BUGGY_DIRECT_TRANSFER,  \* BOOLEAN — enables BuggyDirectTransfer
    BUGGY_CAPS_ELEVATE      \* BOOLEAN — enables BuggyCapsElevate

KObjs == TxKObjs \cup HwKObjs

ASSUME Cardinality(Procs) >= 2
ASSUME Cardinality(KObjs) >= 1
ASSUME Cardinality(Rights) >= 2
ASSUME RightTransfer \in Rights
ASSUME TxKObjs \cap HwKObjs = {}
ASSUME BUGGY_ELEVATE \in BOOLEAN
ASSUME BUGGY_HW_TRANSFER \in BOOLEAN
ASSUME BUGGY_DIRECT_TRANSFER \in BOOLEAN
ASSUME BUGGY_CAPS_ELEVATE \in BOOLEAN

(***************************************************************************)
(* ProcRoot is the proc that starts with full capabilities (Caps). All   *)
(* other procs start with no capabilities ({}). CHOOSE is deterministic — *)
(* TLC picks one root proc per spec invocation.                            *)
(***************************************************************************)
ProcRoot == CHOOSE p \in Procs : TRUE

InitialCapsOf(p) == IF p = ProcRoot THEN Caps ELSE {}

(***************************************************************************)
(* Per-handle record. The `via` field encodes provenance — how this      *)
(* handle came to be in this proc's table:                                 *)
(*                                                                         *)
(*   "orig"   — kernel grant via HandleAlloc.                             *)
(*   "dup"    — duplicated within the proc via HandleDup.                 *)
(*   "9p"     — transferred via a 9P session (HandleTransferVia9P).       *)
(*   "direct" — BUGGY only: cross-proc transfer with no session.          *)
(***************************************************************************)
HandleRecord == [kobj : KObjs,
                 rights : SUBSET Rights,
                 origin_proc : Procs,
                 via : {"orig", "dup", "9p", "direct"}]

VARIABLES
    handles,        \* [Procs -> SUBSET HandleRecord]
    origin_rights,  \* [KObjs -> SUBSET Rights] — set at first alloc
    kobjs_alive,    \* SUBSET KObjs — kobjs that have been HandleAlloc'd
    proc_caps,      \* [Procs -> SUBSET Caps]
    sessions        \* SUBSET (Procs \X Procs) — directional 9P session pairs

vars == <<handles, origin_rights, kobjs_alive, proc_caps, sessions>>

TypeOk ==
    /\ handles \in [Procs -> SUBSET HandleRecord]
    /\ origin_rights \in [KObjs -> SUBSET Rights]
    /\ kobjs_alive \subseteq KObjs
    /\ proc_caps \in [Procs -> SUBSET Caps]
    /\ sessions \subseteq (Procs \X Procs)

(***************************************************************************)
(* Init: no kobjs alive; no handles; origin_rights all-empty placeholder; *)
(* proc_caps initialized per InitialCapsOf; no 9P sessions open.           *)
(***************************************************************************)
Init ==
    /\ handles = [p \in Procs |-> {}]
    /\ origin_rights = [k \in KObjs |-> {}]
    /\ kobjs_alive = {}
    /\ proc_caps = [p \in Procs |-> InitialCapsOf(p)]
    /\ sessions = {}

(***************************************************************************)
(* HandleAlloc(p, k, granted) — kernel creates kobj k (of its statically *)
(* assigned type) and grants rights `granted` to proc p. The kobj's      *)
(* origin_rights is set to `granted` permanently. The handle in p has    *)
(* via="orig" and origin_proc=p.                                          *)
(*                                                                         *)
(* Once allocated, a kobj's origin_rights never changes — it is the      *)
(* CEILING for the RightsCeiling invariant.                                *)
(*                                                                         *)
(* Each kobj is allocated at most once (k \notin kobjs_alive). Bounds the*)
(* state space; reflects the impl: a kobj identity is unique to its      *)
(* lifetime, never recycled while live.                                    *)
(*                                                                         *)
(* Maps to `kernel/handle.c::handle_alloc` (P2-Fc).                        *)
(***************************************************************************)
HandleAlloc(p, k, granted) ==
    /\ k \notin kobjs_alive
    /\ granted \subseteq Rights
    /\ granted # {}
    /\ kobjs_alive' = kobjs_alive \cup {k}
    /\ origin_rights' = [origin_rights EXCEPT ![k] = granted]
    /\ handles' = [handles EXCEPT ![p] = @ \cup {[kobj |-> k,
                                                  rights |-> granted,
                                                  origin_proc |-> p,
                                                  via |-> "orig"]}]
    /\ UNCHANGED <<proc_caps, sessions>>

(***************************************************************************)
(* HandleClose(p, h) — releases h from p's table.                         *)
(*                                                                         *)
(* Maps to `kernel/handle.c::handle_close` (P2-Fc).                        *)
(***************************************************************************)
HandleClose(p, h) ==
    /\ h \in handles[p]
    /\ handles' = [handles EXCEPT ![p] = @ \ {h}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, sessions>>

(***************************************************************************)
(* HandleDup(p, h, new_rights) — creates a fresh handle in p's table     *)
(* sharing h's kobj, with possibly reduced rights. via="dup",            *)
(* origin_proc preserved from parent.                                      *)
(*                                                                         *)
(* new_rights MUST be a non-empty subset of h.rights (rights monotonic   *)
(* reduce). The resulting handle satisfies new_rights \subseteq h.rights *)
(* \subseteq origin_rights[h.kobj], so RightsCeiling is preserved.        *)
(*                                                                         *)
(* Maps to `kernel/handle.c::handle_dup` (P2-Fc).                          *)
(***************************************************************************)
HandleDup(p, h, new_rights) ==
    /\ h \in handles[p]
    /\ new_rights \subseteq h.rights
    /\ new_rights # {}
    /\ handles' = [handles EXCEPT ![p] = @ \cup {[kobj |-> h.kobj,
                                                  rights |-> new_rights,
                                                  origin_proc |-> h.origin_proc,
                                                  via |-> "dup"]}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, sessions>>

(***************************************************************************)
(* BuggyDupElevate(p, h, new_rights) — bug class: dup creates a handle  *)
(* with rights NOT a subset of the parent's rights, fabricating bits.     *)
(*                                                                         *)
(* Real-world analogue: missing bounds check in a dup path; arithmetic   *)
(* error on rights mask.                                                   *)
(*                                                                         *)
(* Caught by RightsCeiling invariant when the elevation includes bits     *)
(* NOT in origin_rights[h.kobj] (TLC explores HandleAlloc with strict    *)
(* subset rights, leaving room for elevation to be visible).              *)
(***************************************************************************)
BuggyDupElevate(p, h, new_rights) ==
    /\ BUGGY_ELEVATE
    /\ h \in handles[p]
    /\ new_rights \subseteq Rights
    /\ ~(new_rights \subseteq h.rights)
    /\ handles' = [handles EXCEPT ![p] = @ \cup {[kobj |-> h.kobj,
                                                  rights |-> new_rights,
                                                  origin_proc |-> h.origin_proc,
                                                  via |-> "dup"]}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, sessions>>

(***************************************************************************)
(* OpenSession / CloseSession — 9P session lifecycle modeled abstractly. *)
(* v1.0 P2-F has no 9P client (Phase 4); the spec models the abstract    *)
(* precondition that transfer requires session connectivity.              *)
(*                                                                         *)
(* Sessions are directional (src -> dst); the receiver's policy is        *)
(* enforced at the destination's side per ARCH §18.6.                      *)
(***************************************************************************)
OpenSession(src, dst) ==
    /\ src # dst
    /\ <<src, dst>> \notin sessions
    /\ sessions' = sessions \cup {<<src, dst>>}
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, proc_caps>>

CloseSession(src, dst) ==
    /\ <<src, dst>> \in sessions
    /\ sessions' = sessions \ {<<src, dst>>}
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, proc_caps>>

(***************************************************************************)
(* HandleTransferVia9P(src, dst, h, new_rights) — copies h from src to   *)
(* dst over an open 9P session, with possibly reduced rights.             *)
(*                                                                         *)
(* Preconditions (the policy):                                             *)
(*   1. h's kobj IS transferable (h.kobj \in TxKObjs).                    *)
(*   2. RightTransfer \in h.rights (the holder explicitly granted        *)
(*      transfer permission).                                               *)
(*   3. The directional session <<src, dst>> is open.                     *)
(*   4. new_rights \subseteq h.rights (monotonic reduce).                 *)
(*   5. new_rights # {}.                                                   *)
(*                                                                         *)
(* The destination's handle has via="9p"; origin_proc preserved.          *)
(*                                                                         *)
(* Maps to `kernel/handle.c::handle_transfer_via_9p` (P2-Fc impl;        *)
(* Phase 4 wires the actual 9P payload).                                   *)
(***************************************************************************)
HandleTransferVia9P(src, dst, h, new_rights) ==
    /\ src # dst
    /\ h \in handles[src]
    /\ h.kobj \in TxKObjs
    /\ RightTransfer \in h.rights
    /\ <<src, dst>> \in sessions
    /\ new_rights \subseteq h.rights
    /\ new_rights # {}
    /\ handles' = [handles EXCEPT ![dst] = @ \cup {[kobj |-> h.kobj,
                                                    rights |-> new_rights,
                                                    origin_proc |-> h.origin_proc,
                                                    via |-> "9p"]}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, sessions>>

(***************************************************************************)
(* BuggyHwTransfer(src, dst, h, new_rights) — bug class: a hardware-typed*)
(* handle is transferred over 9P.                                          *)
(*                                                                         *)
(* The handle_transfer_via_9p switch in §18.3 has NO code path for       *)
(* KObj_MMIO / KObj_IRQ / KObj_DMA / KObj_Interrupt. A buggy variant     *)
(* would add a case for one of those. The spec models this as a separate*)
(* action that fires when h.kobj IS in HwKObjs.                           *)
(*                                                                         *)
(* Caught by HwHandlesAtOrigin invariant: a hw handle ends up at a       *)
(* non-origin proc.                                                        *)
(***************************************************************************)
BuggyHwTransfer(src, dst, h, new_rights) ==
    /\ BUGGY_HW_TRANSFER
    /\ src # dst
    /\ h \in handles[src]
    /\ h.kobj \in HwKObjs
    /\ <<src, dst>> \in sessions
    /\ new_rights \subseteq h.rights
    /\ new_rights # {}
    /\ handles' = [handles EXCEPT ![dst] = @ \cup {[kobj |-> h.kobj,
                                                    rights |-> new_rights,
                                                    origin_proc |-> h.origin_proc,
                                                    via |-> "9p"]}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, sessions>>

(***************************************************************************)
(* BuggyDirectTransfer(src, dst, h, new_rights) — bug class: a syscall  *)
(* exists that transfers a handle from src to dst without going through  *)
(* a 9P session.                                                          *)
(*                                                                         *)
(* §18.1 subordination invariant: handles transfer between procs ONLY   *)
(* via 9P. A direct-transfer syscall would break this. Buggy variant     *)
(* marks the receiving handle's via="direct"; correct path uses "9p".    *)
(*                                                                         *)
(* No session precondition. No transfer-rights precondition. Just shovels*)
(* the handle across.                                                      *)
(*                                                                         *)
(* Caught by OnlyTransferVia9P invariant.                                  *)
(***************************************************************************)
BuggyDirectTransfer(src, dst, h, new_rights) ==
    /\ BUGGY_DIRECT_TRANSFER
    /\ src # dst
    /\ h \in handles[src]
    /\ new_rights \subseteq h.rights
    /\ new_rights # {}
    /\ handles' = [handles EXCEPT ![dst] = @ \cup {[kobj |-> h.kobj,
                                                    rights |-> new_rights,
                                                    origin_proc |-> h.origin_proc,
                                                    via |-> "direct"]}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, sessions>>

(***************************************************************************)
(* ReduceCaps(p, lost) — proc voluntarily drops capability bits.          *)
(*                                                                         *)
(* Maps to `kernel/proc.c::rfork` capability mask (Phase 5+ syscall      *)
(* surface). At v1.0 P2-F there is no kernel-internal caller of          *)
(* capability reduction; the spec models the future behavior so I-2 is   *)
(* pinned at design time.                                                  *)
(***************************************************************************)
ReduceCaps(p, lost) ==
    /\ lost \subseteq proc_caps[p]
    /\ lost # {}
    /\ proc_caps' = [proc_caps EXCEPT ![p] = @ \ lost]
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, sessions>>

(***************************************************************************)
(* BuggyCapsElevate(p, gained) — bug class: rfork raises a proc's caps  *)
(* above its initial set.                                                  *)
(*                                                                         *)
(* Caught by CapsCeiling invariant for non-root procs (whose ceiling is  *)
(* {}; any gain violates).                                                 *)
(***************************************************************************)
BuggyCapsElevate(p, gained) ==
    /\ BUGGY_CAPS_ELEVATE
    /\ gained \subseteq Caps
    /\ gained \cap proc_caps[p] = {}
    /\ gained # {}
    /\ proc_caps' = [proc_caps EXCEPT ![p] = @ \cup gained]
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, sessions>>

(***************************************************************************)
(* Next: disjunction of all actions. \E ranges over actual handles in a  *)
(* proc (h \in handles[p]) rather than over the full HandleRecord type, *)
(* keeping TLC's exploration tight.                                        *)
(***************************************************************************)
Next ==
    \/ \E p \in Procs : \E k \in KObjs : \E granted \in SUBSET Rights : HandleAlloc(p, k, granted)
    \/ \E p \in Procs : \E h \in handles[p] : HandleClose(p, h)
    \/ \E p \in Procs : \E h \in handles[p] : \E nr \in SUBSET Rights : HandleDup(p, h, nr)
    \/ \E p \in Procs : \E h \in handles[p] : \E nr \in SUBSET Rights : BuggyDupElevate(p, h, nr)
    \/ \E src \in Procs : \E dst \in Procs : OpenSession(src, dst)
    \/ \E src \in Procs : \E dst \in Procs : CloseSession(src, dst)
    \/ \E src \in Procs : \E dst \in Procs : \E h \in handles[src] : \E nr \in SUBSET Rights :
           HandleTransferVia9P(src, dst, h, nr)
    \/ \E src \in Procs : \E dst \in Procs : \E h \in handles[src] : \E nr \in SUBSET Rights :
           BuggyHwTransfer(src, dst, h, nr)
    \/ \E src \in Procs : \E dst \in Procs : \E h \in handles[src] : \E nr \in SUBSET Rights :
           BuggyDirectTransfer(src, dst, h, nr)
    \/ \E p \in Procs, lost \in SUBSET Caps                   : ReduceCaps(p, lost)
    \/ \E p \in Procs, gained \in SUBSET Caps                 : BuggyCapsElevate(p, gained)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(***************************************************************************)
(* RightsCeiling — no handle ever holds rights bits NOT granted at the   *)
(* original kobj allocation. Subsumes ARCH §28 I-6 (rights monotonic on  *)
(* transfer) plus the analogous bound on dup.                              *)
(*                                                                         *)
(* Note: a kobj that has not yet been allocated has origin_rights[k]={}, *)
(* and there are no handles to it; the invariant is vacuously true.       *)
(***************************************************************************)
RightsCeiling ==
    \A p \in Procs : \A h \in handles[p] :
        h.rights \subseteq origin_rights[h.kobj]

(***************************************************************************)
(* HwHandlesAtOrigin — every handle of a hardware type is held by its    *)
(* origin proc (the one that originally received it via HandleAlloc).    *)
(* ARCH §28 I-5 (KObj_MMIO/IRQ/DMA non-transferable).                     *)
(***************************************************************************)
HwHandlesAtOrigin ==
    \A p \in Procs : \A h \in handles[p] :
        (h.kobj \in HwKObjs) => h.origin_proc = p

(***************************************************************************)
(* OnlyTransferVia9P — no handle has via="direct". ARCH §28 I-4.         *)
(***************************************************************************)
OnlyTransferVia9P ==
    \A p \in Procs : \A h \in handles[p] : h.via # "direct"

(***************************************************************************)
(* CapsCeiling — every proc's capabilities are a subset of its initial   *)
(* ceiling. ARCH §28 I-2 (capability monotonic reduction).                *)
(***************************************************************************)
CapsCeiling ==
    \A p \in Procs : proc_caps[p] \subseteq InitialCapsOf(p)

Invariants ==
    /\ TypeOk
    /\ RightsCeiling
    /\ HwHandlesAtOrigin
    /\ OnlyTransferVia9P
    /\ CapsCeiling

====
