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
(* P4-Ic5b1b additional impl coverage (no new spec actions/invariants     *)
(* needed): KObj_DMA reuses the HwKObjs partition introduced at P4-Ib.    *)
(* The five hw-kobj invariants enumerated below (HwHandlesAtOrigin,      *)
(* NoHwDup, HwResourceExclusive, HwHandleImpliesCap, RightsCeiling) apply *)
(* uniformly to all hw kobjs regardless of subtype (MMIO / IRQ / DMA),   *)
(* so the spec ALREADY pins what DMA needs. PA stability (the kernel-side*)
(* commitment that a DMA buffer's PA does not change for the handle's    *)
(* lifetime) is a structural impl property — no kernel code path exists  *)
(* that mutates a KObj_DMA's pa field post-create — and is therefore not *)
(* modeled as a state-level action. The kernel-allocates-PA-vs-user-     *)
(* specifies-PA distinction (DMA vs MMIO at the syscall surface) is also *)
(* an impl-level concern that doesn't surface in the abstract handle-    *)
(* table model. See specs/SPEC-TO-CODE.md handles.tla section for the    *)
(* DMA-specific impl mapping.                                              *)
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
(*   every other proc starts with {} (empty caps).                       *)
(*                                                                         *)
(*   P4-Ic3: proc_ceiling[p] \in SUBSET Caps is the dynamic capability  *)
(*   ceiling — the largest set of caps p is permitted to hold across its *)
(*   lifetime. Init: ProcRoot -> Caps; others -> {}. RforkWithCaps sets  *)
(*   the child's ceiling to proc_caps[parent] AT fork time (capturing    *)
(*   the parent's CURRENT caps as the child's permanent upper bound).   *)
(*   CapsCeiling requires proc_caps[p] \subseteq proc_ceiling[p] AT all  *)
(*   times. This preserves the old "non-root procs start with {} ceiling*)
(*   and BuggyCapsElevate is caught" semantics, while modeling P4-Ic3's *)
(*   rfork_with_caps kernel-internal primitive that allows kproc to     *)
(*   grant subset-of-parent caps to a child Proc.                        *)
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
(*   handles_buggy_rfork_elevate.cfg                                       *)
(*                                BUGGY_RFORK_ELEVATE=TRUE — counter-    *)
(*                                example where rfork grants child more  *)
(*                                caps than the parent currently holds.  *)
(*   handles_buggy_spawn_fds_elevate.cfg                                   *)
(*                                BUGGY_SPAWN_FDS_ELEVATE=TRUE — R15-c   *)
(*                                counterexample where SYS_SPAWN_WITH_FDS*)
(*                                installs a child handle with rights NOT*)
(*                                a subset of the parent slot's rights   *)
(*                                (the F231 bug class). Caught by the    *)
(*                                new SpawnFdsRightsMonotonic invariant. *)
(*   handles_buggy_rfork_hostowner.cfg                                     *)
(*                                BUGGY_RFORK_HOSTOWNER=TRUE — P5-        *)
(*                                hostowner counterexample where rfork    *)
(*                                fails to strip the elevation-only       *)
(*                                CapHostowner from a forked child, so    *)
(*                                the child's caps escape its (correctly  *)
(*                                stripped) ceiling. Caught by CapsCeiling*)
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
(*   CapsCeiling        — every proc's caps \subseteq proc_ceiling[p]   *)
(*                        (dynamic, set at Init for ProcRoot=Caps + at   *)
(*                        rfork time for child=proc_caps[parent]).      *)
(*                        ARCH §28 I-2 (capability monotonic reduction). *)
(*                                                                         *)
(*   SpawnFdsRightsMonotonic                                              *)
(*                      — every handle in a child Proc with via="spawn"  *)
(*                        carries rights \subseteq the parent's rights   *)
(*                        on the same kobj AT spawn time. R15-c F232:    *)
(*                        spec-level companion to F231's impl close. The *)
(*                        spawn_inherits ghost variable records the      *)
(*                        parent's rights snapshot at RforkWithFds time;  *)
(*                        the invariant cross-checks every child handle  *)
(*                        against that record. Bug class BuggySpawnFds-  *)
(*                        Elevate skips the rights-subset guard.         *)
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
    Caps,                   \* set of capability labels (must include CapHwCreate)
    CapHwCreate,            \* P4-Ib: capability required to create hw kobjs
    CapHostowner,           \* P5-hostowner: the elevation-only capability
    BUGGY_ELEVATE,          \* BOOLEAN — enables BuggyDupElevate
    BUGGY_HW_TRANSFER,      \* BOOLEAN — enables BuggyHwTransfer
    BUGGY_DIRECT_TRANSFER,  \* BOOLEAN — enables BuggyDirectTransfer
    BUGGY_CAPS_ELEVATE,     \* BOOLEAN — enables BuggyCapsElevate
    BUGGY_HW_DUP,           \* P4-Ib: enables BuggyHwDup (dup of hw kobj)
    BUGGY_HW_OVERLAP,       \* P4-Ib: enables BuggyHwOverlap (double-alloc of hw kobj)
    BUGGY_HW_CREATE_NO_CAP, \* P4-Ib: enables BuggyHwCreateNoCap (hw alloc without cap)
    BUGGY_RFORK_ELEVATE,    \* P4-Ic3: enables BuggyRforkElevate (rfork grants > parent)
    BUGGY_SPAWN_FDS_ELEVATE,\* R15-c F232: enables BuggySpawnFdsElevate (spawn-with-fds installs child rights > parent slot rights)
    BUGGY_RFORK_HOSTOWNER   \* P5-hostowner: enables BuggyRforkNoStrip (rfork omits the elevation-only-cap strip)

KObjs == TxKObjs \cup HwKObjs

ASSUME Cardinality(Procs) >= 2
ASSUME Cardinality(KObjs) >= 1
ASSUME Cardinality(Rights) >= 2
ASSUME RightTransfer \in Rights
ASSUME CapHwCreate \in Caps
ASSUME TxKObjs \cap HwKObjs = {}
ASSUME BUGGY_ELEVATE \in BOOLEAN
ASSUME BUGGY_HW_TRANSFER \in BOOLEAN
ASSUME BUGGY_DIRECT_TRANSFER \in BOOLEAN
ASSUME BUGGY_CAPS_ELEVATE \in BOOLEAN
ASSUME BUGGY_HW_DUP \in BOOLEAN
ASSUME BUGGY_HW_OVERLAP \in BOOLEAN
ASSUME BUGGY_HW_CREATE_NO_CAP \in BOOLEAN
ASSUME BUGGY_RFORK_ELEVATE \in BOOLEAN
ASSUME BUGGY_SPAWN_FDS_ELEVATE \in BOOLEAN
ASSUME BUGGY_RFORK_HOSTOWNER \in BOOLEAN
ASSUME CapHostowner \in Caps
ASSUME CapHostowner # CapHwCreate

(***************************************************************************)
(* ProcRoot is the proc that starts with the FORK-GRANTABLE capability     *)
(* universe (Caps \ ElevationOnly — the model's CAP_ALL). All other procs  *)
(* start with no capabilities ({}). CHOOSE is deterministic — TLC picks    *)
(* one root proc per spec invocation.                                       *)
(***************************************************************************)
ProcRoot == CHOOSE p \in Procs : TRUE

(***************************************************************************)
(* ElevationOnly — the elevation-only capability axis (P5-hostowner).      *)
(* These caps are NOT fork-grantable: no Proc holds one at creation        *)
(* (ProcRoot's initial set excludes them) and RforkWithCaps strips them    *)
(* from every child. The sole action admitting CapHostowner into a         *)
(* proc_caps is HostownerGrant. Models caps.h::CAP_ELEVATION_ONLY;         *)
(* CapHostowner is v1.0's only member.                                      *)
(***************************************************************************)
ElevationOnly == {CapHostowner}

(***************************************************************************)
(* InitialCapsOf — ProcRoot gets the fork-grantable universe (the model's  *)
(* CAP_ALL = Caps \ ElevationOnly); every other proc starts empty.         *)
(* ProcRoot deliberately does NOT start with CapHostowner — an             *)
(* elevation-only cap is never held at creation, only via HostownerGrant.  *)
(***************************************************************************)
InitialCapsOf(p) == IF p = ProcRoot THEN Caps \ ElevationOnly ELSE {}

(***************************************************************************)
(* Per-handle record. The `via` field encodes provenance — how this      *)
(* handle came to be in this proc's table:                                 *)
(*                                                                         *)
(*   "orig"   — kernel grant via HandleAlloc.                             *)
(*   "dup"    — duplicated within the proc via HandleDup.                 *)
(*   "9p"     — transferred via a 9P session (HandleTransferVia9P).       *)
(*   "spawn"  — installed at spawn-with-fds time (RforkWithFds; R15-c).   *)
(*   "direct" — BUGGY only: cross-proc transfer with no session.          *)
(***************************************************************************)
HandleRecord == [kobj : KObjs,
                 rights : SUBSET Rights,
                 origin_proc : Procs,
                 via : {"orig", "dup", "9p", "spawn", "direct"}]

(***************************************************************************)
(* SpawnInherit ghost record: persists the parent's rights AT THE TIME a   *)
(* spawn-fd handle is installed in a child. The SpawnFdsRightsMonotonic    *)
(* invariant cross-checks every "spawn"-via handle against the record.    *)
(***************************************************************************)
SpawnInherit == [child : Procs, kobj : KObjs, parent_rights : SUBSET Rights]

VARIABLES
    handles,        \* [Procs -> SUBSET HandleRecord]
    origin_rights,  \* [KObjs -> SUBSET Rights] — set at first alloc
    kobjs_alive,    \* SUBSET KObjs — kobjs that have been HandleAlloc'd
    proc_caps,      \* [Procs -> SUBSET Caps]
    proc_ceiling,   \* [Procs -> SUBSET Caps] — P4-Ic3 dynamic cap ceiling
    sessions,       \* SUBSET (Procs \X Procs) — directional 9P session pairs
    spawn_inherits  \* SUBSET SpawnInherit — R15-c F232 ghost ledger

vars == <<handles, origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

TypeOk ==
    /\ handles \in [Procs -> SUBSET HandleRecord]
    /\ origin_rights \in [KObjs -> SUBSET Rights]
    /\ kobjs_alive \subseteq KObjs
    /\ proc_caps \in [Procs -> SUBSET Caps]
    /\ proc_ceiling \in [Procs -> SUBSET Caps]
    /\ sessions \subseteq (Procs \X Procs)
    /\ spawn_inherits \subseteq SpawnInherit

(***************************************************************************)
(* Init: no kobjs alive; no handles; origin_rights all-empty placeholder; *)
(* proc_caps initialized per InitialCapsOf; proc_ceiling matches initial  *)
(* caps (ProcRoot=Caps universe, non-root=empty); no 9P sessions open;    *)
(* no spawn-fd inheritances recorded.                                      *)
(***************************************************************************)
Init ==
    /\ handles = [p \in Procs |-> {}]
    /\ origin_rights = [k \in KObjs |-> {}]
    /\ kobjs_alive = {}
    /\ proc_caps = [p \in Procs |-> InitialCapsOf(p)]
    /\ proc_ceiling = [p \in Procs |-> InitialCapsOf(p)]
    /\ sessions = {}
    /\ spawn_inherits = {}

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
    \* P4-Ib: hw kobj creation requires the CapHwCreate capability in the
    \* origin proc. Maps to the impl-side check in `kernel/syscall.c`'s
    \* SYS_MMIO_CREATE / SYS_IRQ_CREATE handlers: `if (!(p->caps &
    \* CAP_HW_CREATE)) return -EPERM`. The bug class
    \* `BuggyHwCreateNoCap` (below) bypasses this check; the
    \* `HwHandleImpliesCap` invariant catches the consequence.
    /\ (k \in HwKObjs) => CapHwCreate \in proc_caps[p]
    /\ kobjs_alive' = kobjs_alive \cup {k}
    /\ origin_rights' = [origin_rights EXCEPT ![k] = granted]
    /\ handles' = [handles EXCEPT ![p] = @ \cup {[kobj |-> k,
                                                  rights |-> granted,
                                                  origin_proc |-> p,
                                                  via |-> "orig"]}]
    /\ UNCHANGED <<proc_caps, proc_ceiling, sessions, spawn_inherits>>

(***************************************************************************)
(* HandleClose(p, h) — releases h from p's table.                         *)
(*                                                                         *)
(* Maps to `kernel/handle.c::handle_close` (P2-Fc).                        *)
(***************************************************************************)
HandleClose(p, h) ==
    /\ h \in handles[p]
    /\ handles' = [handles EXCEPT ![p] = @ \ {h}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

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
    \* P4-Ib: dup forbidden for hw kobjs. Extends I-5 (non-transferable
    \* across procs) to "non-duplicable at all". A driver holds a single
    \* handle to its MMIO range / INTID; there's no legitimate use case
    \* for in-proc duplication, and forbidding it pins
    \* HwResourceExclusive. The bug class `BuggyHwDup` (below) bypasses
    \* this; `NoHwDup` invariant catches the consequence.
    /\ h.kobj \in TxKObjs
    /\ new_rights \subseteq h.rights
    /\ new_rights # {}
    /\ handles' = [handles EXCEPT ![p] = @ \cup {[kobj |-> h.kobj,
                                                  rights |-> new_rights,
                                                  origin_proc |-> h.origin_proc,
                                                  via |-> "dup"]}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

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
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

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
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, proc_caps, proc_ceiling, spawn_inherits>>

CloseSession(src, dst) ==
    /\ <<src, dst>> \in sessions
    /\ sessions' = sessions \ {<<src, dst>>}
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, proc_caps, proc_ceiling, spawn_inherits>>

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
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

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
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

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
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

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
    \* P4-Ib: CapHwCreate cannot be dropped while p holds any hw handle.
    \* This pins `HwHandleImpliesCap` as a state invariant — without this
    \* restriction the cap could be dropped after alloc, leaving a state
    \* where the proc holds a hw handle but lacks the cap. At v1.0 the
    \* kernel doesn't revoke hw handles when the cap is dropped; the spec
    \* models this by forbidding the drop in the affected case.
    \* Equivalent kernel-side discipline: `rfork` with a mask that clears
    \* CAP_HW_CREATE returns -EBUSY if p still has any hw handle open.
    /\ (CapHwCreate \in lost) =>
           ~(\E h \in handles[p] : h.kobj \in HwKObjs)
    /\ proc_caps' = [proc_caps EXCEPT ![p] = @ \ lost]
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, proc_ceiling, sessions, spawn_inherits>>

(***************************************************************************)
(* BuggyCapsElevate(p, gained) — bug class: rfork raises a proc's caps  *)
(* above its initial set.                                                  *)
(*                                                                         *)
(* Caught by CapsCeiling invariant for non-root procs (whose ceiling is  *)
(* {} at Init; any gain violates).                                         *)
(*                                                                         *)
(* P4-Ic3 catch-surface note (R11 F162): the dynamic CapsCeiling          *)
(* introduced at P4-Ic3 narrows this bug class's catch surface — a proc  *)
(* whose `proc_ceiling` has been raised via RforkWithCaps (e.g.,         *)
(* `RforkWithCaps(ProcRoot, p, {})` initializes p with ceiling=Caps but  *)
(* caps={}) admits BuggyCapsElevate "within ceiling" without violating   *)
(* CapsCeiling. This is INTENTIONAL: the spec models post-fork as       *)
(* "ceiling captures the static authorization envelope; current caps    *)
(* are a subset that can shrink." A bug-class equivalent of "caps grew  *)
(* within ceiling" doesn't exist at v1.0 because there is no impl-side  *)
(* syscall that raises caps post-fork. If such a syscall lands at      *)
(* Phase 5+ (e.g., `GrantCaps`), the spec should add a state action     *)
(* modeling it; until then the narrowed coverage is the correct model. *)
(* TLC `handles_buggy_caps.cfg` continues to produce a counterexample  *)
(* at depth 4 via the direct-from-Init path (BuggyCapsElevate on a     *)
(* never-rfork'd proc whose ceiling is still {}).                        *)
(***************************************************************************)
BuggyCapsElevate(p, gained) ==
    /\ BUGGY_CAPS_ELEVATE
    /\ gained \subseteq Caps
    /\ gained \cap proc_caps[p] = {}
    /\ gained # {}
    /\ proc_caps' = [proc_caps EXCEPT ![p] = @ \cup gained]
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, proc_ceiling, sessions, spawn_inherits>>

(***************************************************************************)
(* BuggyHwDup(p, h, new_rights) — P4-Ib bug class: dup creates a second  *)
(* handle to a hw kobj. Mirrors HandleDup's effects but allows h.kobj    *)
(* to be in HwKObjs.                                                       *)
(*                                                                         *)
(* Real-world analogue: `handle_dup` switch on kind that accidentally     *)
(* allows KOBJ_MMIO / KOBJ_IRQ. The correct impl rejects with -EINVAL    *)
(* (see `kernel/handle.c::handle_dup`'s `h.kobj IS_NOT_TX → return -1`   *)
(* check, P4-Ib).                                                          *)
(*                                                                         *)
(* Caught by `NoHwDup` invariant: a handle to a hw kobj has via="dup".   *)
(***************************************************************************)
BuggyHwDup(p, h, new_rights) ==
    /\ BUGGY_HW_DUP
    /\ h \in handles[p]
    /\ h.kobj \in HwKObjs
    /\ new_rights \subseteq h.rights
    /\ new_rights # {}
    /\ handles' = [handles EXCEPT ![p] = @ \cup {[kobj |-> h.kobj,
                                                  rights |-> new_rights,
                                                  origin_proc |-> h.origin_proc,
                                                  via |-> "dup"]}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

(***************************************************************************)
(* BuggyHwOverlap(p, k, granted) — P4-Ib bug class: kernel allocates a   *)
(* second handle to an ALREADY-alive hw kobj (i.e., bypasses the         *)
(* `k \notin kobjs_alive` precondition specifically for hw kobjs).      *)
(*                                                                         *)
(* Real-world analogue: `kobj_mmio_create(pa)` for a PA that's already   *)
(* in g_mmio_claims — the impl missed the overlap check (off-by-one in   *)
(* the range comparison; race between two cores' creates; etc.). The     *)
(* result: two KObj_MMIO structs both claim the same hardware range,    *)
(* and two procs hold handles to "the same" resource.                    *)
(*                                                                         *)
(* The action does NOT update kobjs_alive (it's already in the set);    *)
(* origin_rights stays at the rights from the FIRST alloc. The new      *)
(* handle is in a DIFFERENT proc (otherwise it'd just be a second handle*)
(* to a kobj the same proc already owns, which is the dup hazard).      *)
(*                                                                         *)
(* Caught by `HwResourceExclusive` invariant.                              *)
(***************************************************************************)
BuggyHwOverlap(p, k, granted) ==
    /\ BUGGY_HW_OVERLAP
    /\ k \in HwKObjs
    /\ k \in kobjs_alive
    \* R9 F153 (P3) close: dropped the prior `~(\E h \in handles[p] :
    \* h.kobj = k)` precondition so the action can ALSO fire when p
    \* already holds a handle to k (same-proc double-alloc). The impl's
    \* g_mmio_claims is global and rejects same-proc overlap too;
    \* without this relaxation TLC didn't exercise that case. The
    \* `HwResourceExclusive` invariant catches both cross-proc and
    \* same-proc duplication because it counts handles across ALL
    \* procs for the given kobj.
    /\ granted \subseteq origin_rights[k]
    /\ granted # {}
    /\ handles' = [handles EXCEPT ![p] = @ \cup {[kobj |-> k,
                                                  rights |-> granted,
                                                  origin_proc |-> p,
                                                  via |-> "orig"]}]
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions, spawn_inherits>>

(***************************************************************************)
(* BuggyHwCreateNoCap(p, k, granted) — P4-Ib bug class: kernel grants a *)
(* hw handle to a proc that lacks CapHwCreate.                            *)
(*                                                                         *)
(* Real-world analogue: missing capability check in SYS_MMIO_CREATE /     *)
(* SYS_IRQ_CREATE — any userspace proc could call kobj_mmio_create(0x0,  *)
(* sizeof_RAM) and gain a handle to the entire physical memory.          *)
(*                                                                         *)
(* Differs from HandleAlloc only by skipping the CapHwCreate \in        *)
(* proc_caps[p] precondition.                                              *)
(*                                                                         *)
(* Caught by `HwHandleImpliesCap` invariant.                               *)
(***************************************************************************)
BuggyHwCreateNoCap(p, k, granted) ==
    /\ BUGGY_HW_CREATE_NO_CAP
    /\ k \in HwKObjs
    /\ k \notin kobjs_alive
    /\ CapHwCreate \notin proc_caps[p]
    /\ granted \subseteq Rights
    /\ granted # {}
    /\ kobjs_alive' = kobjs_alive \cup {k}
    /\ origin_rights' = [origin_rights EXCEPT ![k] = granted]
    /\ handles' = [handles EXCEPT ![p] = @ \cup {[kobj |-> k,
                                                  rights |-> granted,
                                                  origin_proc |-> p,
                                                  via |-> "orig"]}]
    /\ UNCHANGED <<proc_caps, proc_ceiling, sessions, spawn_inherits>>

(***************************************************************************)
(* RforkWithCaps(parent, child, granted) — P4-Ic3 kernel-internal       *)
(* primitive. The parent (typically kproc) creates a new Proc with caps  *)
(* set to `granted`, where granted is a subset of the parent's CURRENT  *)
(* caps. The child's ceiling is captured from the parent's current caps *)
(* (this becomes the child's permanent upper bound for all future        *)
(* operations).                                                            *)
(*                                                                         *)
(* Preconditions:                                                          *)
(*   - granted \subseteq proc_caps[parent]: subset-of-parent              *)
(*     (CapsCeiling for child is preserved).                              *)
(*   - The child slot is uninitialized: proc_caps = {}, proc_ceiling = {},*)
(*     handles = {}. This prevents re-grant on the same slot and matches  *)
(*     the impl-side semantics that proc_alloc returns a fresh-zeroed     *)
(*     Proc.                                                                *)
(*   - parent # child: rfork creates a distinct Proc.                      *)
(*                                                                         *)
(* Maps to `kernel/proc.c::rfork_with_caps(flags, entry, arg, mask)`.     *)
(***************************************************************************)
RforkWithCaps(parent, child, granted) ==
    /\ parent # child
    /\ granted \subseteq proc_caps[parent]
    /\ proc_caps[child] = {}
    /\ proc_ceiling[child] = {}
    /\ handles[child] = {}
    \* P5-hostowner: rfork strips ElevationOnly from BOTH the child's caps
    \* and its ceiling. Models rfork_internal's `& ~CAP_ELEVATION_ONLY`
    \* (P5-hostowner-b). A forked child is never authorized to hold an
    \* elevation-only cap, so its ceiling excludes ElevationOnly; only a
    \* later HostownerGrant on the child can confer one.
    /\ proc_ceiling' = [proc_ceiling EXCEPT ![child] = proc_caps[parent] \ ElevationOnly]
    /\ proc_caps' = [proc_caps EXCEPT ![child] = granted \ ElevationOnly]
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, sessions, spawn_inherits>>

(***************************************************************************)
(* HostownerGrant(p) — P5-hostowner. p is conferred the elevation-only     *)
(* capability CapHostowner. Models the kernel `cap` device's `use`-file    *)
(* redemption: a console-attached Proc redeems a pending grant and the     *)
(* kernel ORs CapHostowner into its capability set (CORVUS-DESIGN §5.5.1). *)
(*                                                                         *)
(* The console-attachment gate and corvus's passphrase check are NOT       *)
(* modeled here — they live in corvus.tla (AdminElevate). handles.tla's    *)
(* concern is the cap arithmetic: the grant raises BOTH proc_caps AND      *)
(* proc_ceiling for p, so CapsCeiling is preserved (the elevation          *)
(* legitimately raises p's authorization envelope). This is the SOLE       *)
(* action that admits CapHostowner into any proc_caps; RforkWithCaps       *)
(* strips it — the spec-level statement of CORVUS-DESIGN C-21.             *)
(*                                                                         *)
(* Precondition CapHostowner \notin proc_caps[p] models the device's       *)
(* one-shot consumption: a second redeem on an already-elevated Proc is    *)
(* a no-op.                                                                 *)
(***************************************************************************)
HostownerGrant(p) ==
    /\ CapHostowner \notin proc_caps[p]
    /\ proc_caps' = [proc_caps EXCEPT ![p] = @ \cup {CapHostowner}]
    /\ proc_ceiling' = [proc_ceiling EXCEPT ![p] = @ \cup {CapHostowner}]
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, sessions, spawn_inherits>>

(***************************************************************************)
(* BuggyRforkElevate(parent, child, gained) — P4-Ic3 bug class: kernel  *)
(* rfork_with_caps grants the child MORE caps than the parent holds.     *)
(*                                                                         *)
(* Real-world analogue: missing AND-with-parent-caps in rfork_with_caps  *)
(* — e.g., `child->caps = mask` instead of `child->caps = parent->caps  *)
(* & mask`. Or a kernel-side caller that constructs `granted` from a    *)
(* user-supplied bitmask without bounding it by the caller's own caps.   *)
(*                                                                         *)
(* The action mirrors RforkWithCaps's effects (correct ceiling = parent's*)
(* caps; child sees proc_caps = gained) but DROPS the                     *)
(* `granted \subseteq proc_caps[parent]` precondition. CapsCeiling catches*)
(* the consequence: proc_caps[child] = gained \notsubseteq proc_caps[parent]*)
(* = proc_ceiling[child].                                                  *)
(*                                                                         *)
(* Caught by `CapsCeiling` invariant.                                      *)
(***************************************************************************)
BuggyRforkElevate(parent, child, gained) ==
    /\ BUGGY_RFORK_ELEVATE
    /\ parent # child
    /\ proc_caps[child] = {}
    /\ proc_ceiling[child] = {}
    /\ handles[child] = {}
    /\ gained \subseteq Caps
    /\ ~(gained \subseteq proc_caps[parent])
    /\ proc_ceiling' = [proc_ceiling EXCEPT ![child] = proc_caps[parent]]
    /\ proc_caps' = [proc_caps EXCEPT ![child] = gained]
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, sessions, spawn_inherits>>

(***************************************************************************)
(* BuggyRforkNoStrip(parent, child, granted) — P5-hostowner bug class.     *)
(* rfork fails to strip the elevation-only cap from the child: the child   *)
(* receives `granted` verbatim, NOT `granted \ ElevationOnly`. Models an   *)
(* rfork_internal that ANDs parent caps with the mask but omits the        *)
(* `& ~CAP_ELEVATION_ONLY` strip.                                          *)
(*                                                                         *)
(* The child's CEILING is set CORRECTLY (proc_caps[parent] \ ElevationOnly *)
(* — a forked child is never authorized to hold an elevation-only cap).    *)
(* The bug is purely in the caps assignment. Once `granted` contains       *)
(* CapHostowner — reachable after a parent has been HostownerGrant'd —     *)
(* the child ends with CapHostowner in proc_caps but not proc_ceiling, and *)
(* the existing CapsCeiling invariant catches it. No dedicated invariant   *)
(* is needed: the ceiling discipline already pins "no elevation-only cap   *)
(* via fork."                                                               *)
(*                                                                         *)
(* Counterexample (handles_buggy_rfork_hostowner.cfg): HostownerGrant      *)
(* (ProcRoot) then BuggyRforkNoStrip(ProcRoot, child, {CapHostowner}) —    *)
(* depth 2.                                                                 *)
(***************************************************************************)
BuggyRforkNoStrip(parent, child, granted) ==
    /\ BUGGY_RFORK_HOSTOWNER
    /\ parent # child
    /\ granted \subseteq proc_caps[parent]
    /\ proc_caps[child] = {}
    /\ proc_ceiling[child] = {}
    /\ handles[child] = {}
    /\ proc_ceiling' = [proc_ceiling EXCEPT ![child] = proc_caps[parent] \ ElevationOnly]
    /\ proc_caps' = [proc_caps EXCEPT ![child] = granted]
    /\ UNCHANGED <<handles, origin_rights, kobjs_alive, sessions, spawn_inherits>>

(***************************************************************************)
(* RforkWithFds(parent, child, h, new_rights) — R15-c F232. Models the     *)
(* SYS_SPAWN_WITH_FDS / SYS_SPAWN_FULL fd-inheritance pattern: parent's    *)
(* handle h is replicated into child with rights == h.rights's subset.     *)
(*                                                                         *)
(* This is distinct from HandleTransferVia9P in three ways:                *)
(*                                                                         *)
(*   1. No session precondition. Spawn-with-fds is kernel-internal — the   *)
(*      kernel inherits fds during rfork's thunk, no 9P round-trip. The    *)
(*      I-4 invariant ("transfer only via 9P sessions") doesn't apply      *)
(*      here because no inter-process boundary is crossed at the syscall   *)
(*      surface — the kernel ITSELF acts as the trusted carrier (the same  *)
(*      idiom POSIX exec uses for FD inheritance from one Proc image to    *)
(*      the next).                                                          *)
(*                                                                         *)
(*   2. No RIGHT_TRANSFER precondition. Parent does not need to mark the   *)
(*      slot as transferable — every slot in parent is eligible. (The      *)
(*      orthogonal CAP_SPAWN gate at the syscall surface is not modeled    *)
(*      here; capability gates are a separate concern from rights flow.)   *)
(*                                                                         *)
(*   3. Provenance via="spawn" (not "9p"). The SpawnFdsRightsMonotonic      *)
(*      invariant treats spawn-via handles specifically, cross-checking    *)
(*      against the spawn_inherits ledger.                                  *)
(*                                                                         *)
(* Maps to `kernel/syscall.c::sys_bump_inherit_fds` + the spawn-with-fds   *)
(* thunk's per-fd `handle_alloc` loop. The R15-a F231 fix made the         *)
(* thunk use the PARENT'S CAPTURED slot rights (struct                     *)
(* spawn_with_fds_args::rights[]) rather than hardcoded                    *)
(* RIGHT_READ|RIGHT_WRITE|RIGHT_TRANSFER; this spec action pins the same   *)
(* discipline at the model level.                                          *)
(*                                                                         *)
(* Note: the action fires per-handle (matches the impl's per-fd loop). A   *)
(* single spawn-with-fds call can fire RforkWithFds N times if N fds are   *)
(* inherited; each call inserts one spawn_inherits ledger entry.           *)
(***************************************************************************)
RforkWithFds(parent, child, h, new_rights) ==
    /\ parent # child
    /\ h \in handles[parent]
    /\ h.kobj \in TxKObjs                       \* hw kobjs cannot inherit (I-5)
    /\ new_rights \subseteq h.rights            \* THE KEY GUARD (F231 closed)
    /\ new_rights # {}
    /\ handles' = [handles EXCEPT ![child] = @ \cup {[kobj |-> h.kobj,
                                                      rights |-> new_rights,
                                                      origin_proc |-> h.origin_proc,
                                                      via |-> "spawn"]}]
    /\ spawn_inherits' = spawn_inherits \cup {[child |-> child,
                                                kobj |-> h.kobj,
                                                parent_rights |-> h.rights]}
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions>>

(***************************************************************************)
(* BuggySpawnFdsElevate(parent, child, h, fabricated_rights) — R15-c F232  *)
(* bug class. The spawn-with-fds handler installs a child handle with      *)
(* rights NOT a subset of the parent slot's rights. Captures the pre-      *)
(* R15-a F231 bug shape (handle_alloc hardcoded RIGHT_READ|WRITE|TRANSFER  *)
(* regardless of what the parent had).                                      *)
(*                                                                         *)
(* Differs from RforkWithFds by skipping the `new_rights \subseteq         *)
(* h.rights` precondition. The spawn_inherits ledger still records the     *)
(* parent's ACTUAL rights at fork time (the bug fabricates only on the     *)
(* child side, not in the ledger), so SpawnFdsRightsMonotonic catches the  *)
(* divergence.                                                              *)
(*                                                                         *)
(* Caught by `SpawnFdsRightsMonotonic` invariant.                          *)
(***************************************************************************)
BuggySpawnFdsElevate(parent, child, h, fabricated_rights) ==
    /\ BUGGY_SPAWN_FDS_ELEVATE
    /\ parent # child
    /\ h \in handles[parent]
    /\ h.kobj \in TxKObjs
    /\ fabricated_rights \subseteq Rights
    /\ fabricated_rights # {}
    /\ ~(fabricated_rights \subseteq h.rights)  \* THE BUG: skips subset
    /\ handles' = [handles EXCEPT ![child] = @ \cup {[kobj |-> h.kobj,
                                                      rights |-> fabricated_rights,
                                                      origin_proc |-> h.origin_proc,
                                                      via |-> "spawn"]}]
    /\ spawn_inherits' = spawn_inherits \cup {[child |-> child,
                                                kobj |-> h.kobj,
                                                parent_rights |-> h.rights]}
    /\ UNCHANGED <<origin_rights, kobjs_alive, proc_caps, proc_ceiling, sessions>>

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
    \/ \E p \in Procs : \E h \in handles[p] : \E nr \in SUBSET Rights : BuggyHwDup(p, h, nr)
    \/ \E p \in Procs : \E k \in KObjs : \E granted \in SUBSET Rights : BuggyHwOverlap(p, k, granted)
    \/ \E p \in Procs : \E k \in KObjs : \E granted \in SUBSET Rights : BuggyHwCreateNoCap(p, k, granted)
    \/ \E parent \in Procs : \E child \in Procs : \E granted \in SUBSET Caps :
           RforkWithCaps(parent, child, granted)
    \/ \E parent \in Procs : \E child \in Procs : \E gained \in SUBSET Caps :
           BuggyRforkElevate(parent, child, gained)
    \/ \E parent \in Procs : \E child \in Procs : \E h \in handles[parent] : \E nr \in SUBSET Rights :
           RforkWithFds(parent, child, h, nr)
    \/ \E parent \in Procs : \E child \in Procs : \E h \in handles[parent] : \E fr \in SUBSET Rights :
           BuggySpawnFdsElevate(parent, child, h, fr)
    \/ \E p \in Procs : HostownerGrant(p)
    \/ \E parent \in Procs : \E child \in Procs : \E granted \in SUBSET Caps :
           BuggyRforkNoStrip(parent, child, granted)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* State constraint: bounds TLC's exploration. R15-c added the              *)
(* RforkWithFds + BuggySpawnFdsElevate actions and the spawn_inherits      *)
(* ghost ledger, which multiplied the reachable state space (every dup or  *)
(* spawn-fd inheritance can fire on every prior handle, accumulating       *)
(* per-Proc handle tables with combinatorially many rights subsets).      *)
(*                                                                         *)
(* The constraint bounds per-Proc handle count and ledger size. Bug-class  *)
(* cfgs find counterexamples at very small depths (BuggySpawnFdsElevate    *)
(* surfaces at depth 4), so the bound doesn't compromise buggy-cfg         *)
(* coverage. The clean cfg explores millions of states under the bound;   *)
(* tighter than that is unrealistic and looser is impractical at the      *)
(* current spec dimensions.                                                 *)
(***************************************************************************)
StateConstraint ==
    /\ \A p \in Procs : Cardinality(handles[p]) <= 2
    /\ Cardinality(spawn_inherits) <= 2

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
(* CapsCeiling — every proc's capabilities are a subset of its DYNAMIC   *)
(* ceiling (proc_ceiling[p]). At Init, the ceiling matches               *)
(* InitialCapsOf — so non-rfork'd procs keep the old "non-root starts    *)
(* empty" semantics. After RforkWithCaps fires on a child, the child's   *)
(* ceiling is the parent's caps AT fork time. ARCH §28 I-2 (capability   *)
(* monotonic reduction) + P4-Ic3 (rfork-time inheritance).                *)
(***************************************************************************)
CapsCeiling ==
    \A p \in Procs : proc_caps[p] \subseteq proc_ceiling[p]

(***************************************************************************)
(* NoHwDup — P4-Ib. No handle to a hw kobj was created via dup. Extends *)
(* I-5's "non-transferable across procs" to "non-duplicable at all":     *)
(* drivers hold exactly one origin handle per hw kobj.                    *)
(*                                                                         *)
(* Caught by `BuggyHwDup` (in the corresponding buggy config).             *)
(***************************************************************************)
NoHwDup ==
    \A p \in Procs : \A h \in handles[p] :
        (h.kobj \in HwKObjs) => h.via # "dup"

(***************************************************************************)
(* HwResourceExclusive — P4-Ib. For each hw kobj, at most one alive      *)
(* handle exists across all procs. Mathematically follows from           *)
(* `k \notin kobjs_alive` precondition on HandleAlloc + NoHwDup +        *)
(* HwHandlesAtOrigin; the explicit invariant catches `BuggyHwOverlap`    *)
(* which bypasses the `k \notin kobjs_alive` precondition.                *)
(*                                                                         *)
(* Models the kernel's "two drivers can't claim the same MMIO range /    *)
(* INTID" contract. The "kobj identity" here represents a hardware       *)
(* resource (PA range, INTID); two alive handles to the same identity   *)
(* would mean two procs each believing they exclusively own the resource.*)
(***************************************************************************)
HwResourceExclusive ==
    \A k \in HwKObjs :
        Cardinality(UNION {{h \in handles[p] : h.kobj = k} : p \in Procs}) <= 1

(***************************************************************************)
(* HwHandleImpliesCap — P4-Ib. Every alive handle to a hw kobj is held  *)
(* by a proc that has CapHwCreate. Models the impl-side requirement:    *)
(* SYS_MMIO_CREATE / SYS_IRQ_CREATE check `p->caps & CAP_HW_CREATE`     *)
(* before allocating the handle.                                           *)
(*                                                                         *)
(* The state-invariant phrasing relies on ReduceCaps's restriction        *)
(* (above) that forbids dropping CapHwCreate while holding hw handles —  *)
(* without that, a proc could alloc-with-cap then drop-cap, producing a *)
(* state that satisfies "had cap at alloc" but violates "has cap now".  *)
(*                                                                         *)
(* Caught by `BuggyHwCreateNoCap` (in the corresponding buggy config).    *)
(***************************************************************************)
HwHandleImpliesCap ==
    \A p \in Procs : \A h \in handles[p] :
        (h.kobj \in HwKObjs) => CapHwCreate \in proc_caps[p]

(***************************************************************************)
(* SpawnFdsRightsMonotonic — R15-c F232. For every "spawn"-via handle in   *)
(* a child Proc, the child's rights must be a subset of the parent's      *)
(* rights AT THE TIME OF INHERITANCE (recorded in spawn_inherits).         *)
(*                                                                         *)
(* The ghost ledger spawn_inherits records (child, kobj, parent_rights)   *)
(* triples written atomically by RforkWithFds. The invariant requires the *)
(* child handle's rights to be a subset of the recorded parent_rights.    *)
(*                                                                         *)
(* Captures the R15-a F231 closure at the spec level. Pre-fix, the         *)
(* kernel's spawn-with-fds handler hardcoded                                *)
(* RIGHT_READ|RIGHT_WRITE|RIGHT_TRANSFER for every child slot regardless  *)
(* of the parent slot's actual rights — a child whose parent had only     *)
(* RIGHT_READ would emerge with RIGHT_READ|WRITE|TRANSFER, violating       *)
(* this invariant under BuggySpawnFdsElevate.                              *)
(*                                                                         *)
(* Why distinct from RightsCeiling: RightsCeiling bounds rights by         *)
(* origin_rights[k] — adequate for the BuggyDupElevate / BuggyHwTransfer   *)
(* shapes that fabricate beyond ORIGINAL grant. F231's shape fabricates   *)
(* WITHIN origin (parent had {R}, child got {R,W} where origin_rights had *)
(* {R,W,T}) so RightsCeiling does NOT catch it. SpawnFdsRightsMonotonic    *)
(* bounds by parent's CURRENT rights (at fork) instead — which is the     *)
(* discipline the impl enforces.                                            *)
(***************************************************************************)
SpawnFdsRightsMonotonic ==
    \A p \in Procs : \A h \in handles[p] :
        h.via = "spawn" =>
            \E sr \in spawn_inherits :
                /\ sr.child = p
                /\ sr.kobj = h.kobj
                /\ h.rights \subseteq sr.parent_rights

Invariants ==
    /\ TypeOk
    /\ RightsCeiling
    /\ HwHandlesAtOrigin
    /\ OnlyTransferVia9P
    /\ CapsCeiling
    /\ NoHwDup
    /\ HwResourceExclusive
    /\ HwHandleImpliesCap
    /\ SpawnFdsRightsMonotonic

====
