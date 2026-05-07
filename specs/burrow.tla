---- MODULE burrow ----
(***************************************************************************)
(* Thylacine BURROW refcount lifecycle — P2-Fb spec.                          *)
(*                                                                         *)
(* Models the refcount + mapping lifecycle for a Virtual Memory Object   *)
(* (BURROW) per ARCH §19. Pins ARCH §28 invariant I-7:                        *)
(*                                                                         *)
(*   I-7 — BURROW pages live until last handle closed AND last mapping       *)
(*         unmapped.                                                        *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   Each BURROW has two refcounts: handle_count (open handles) and         *)
(*   mapping_count (open mappings into address spaces). Pages exist iff    *)
(*   at least one count is > 0; the moment both reach 0, pages MUST be   *)
(*   freed.                                                                 *)
(*                                                                         *)
(*   We do NOT model individual handles or processes — handles.tla covers*)
(*   the per-handle policy (rights, transferability, provenance). burrow.tla*)
(*   focuses purely on the refcount mechanics. A handle dup or 9P transfer*)
(*   collapses to "open another handle"; a process exit collapses to     *)
(*   "close all handles in that process."                                 *)
(*                                                                         *)
(*   pages_alive[v] is a boolean (membership in a SUBSET of VmoIds): TRUE*)
(*   when v's pages are allocated, FALSE when freed (and not yet         *)
(*   re-allocated). The free transition occurs at the moment both counts *)
(*   reach 0 and is triggered by HandleClose or UnmapVmo (whichever      *)
(*   brings the last count to 0).                                         *)
(*                                                                         *)
(*   MaxRefs bounds each refcount to [0..MaxRefs] for TLC tractability.   *)
(*   Real-world refcounts are u32; this constraint is a model bound, not *)
(*   an impl bound. The bug classes (premature free / never free) are    *)
(*   exhibited at MaxRefs=2.                                              *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation per CLAUDE.md spec-first*)
(* policy):                                                                *)
(*                                                                         *)
(*   burrow.cfg                       all flags FALSE — TLC proves          *)
(*                                  NoUseAfterFree.                        *)
(*   burrow_buggy_free_on_close.cfg   BUGGY_FREE_ON_HANDLE_CLOSE=TRUE —     *)
(*                                  premature free on handle_count=0     *)
(*                                  while mapping_count > 0.              *)
(*   burrow_buggy_free_on_unmap.cfg   BUGGY_FREE_ON_UNMAP=TRUE —            *)
(*                                  premature free on mapping_count=0    *)
(*                                  while handle_count > 0.               *)
(*   burrow_buggy_never_free.cfg      BUGGY_NEVER_FREE=TRUE —               *)
(*                                  delayed/never free; pages alive at   *)
(*                                  both counts = 0 (resource leak).      *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   RefcountConsistent — non-allocated VMOs have all counts = 0 and     *)
(*                        pages not alive.                                 *)
(*                                                                         *)
(*   NoUseAfterFree     — pages are alive iff at least one refcount      *)
(*                        > 0. ARCH §28 I-7 in iff form: catches both    *)
(*                        premature free (counts > 0 AND pages dead) and *)
(*                        delayed free (counts = 0 AND pages alive).     *)
(*                                                                         *)
(* See ARCHITECTURE.md §19 (VMOs) + §28 invariant I-7.                    *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    VmoIds,                          \* set of BURROW identifiers (>= 1)
    MaxRefs,                         \* Nat — bound on refcounts for TLC
    BUGGY_FREE_ON_HANDLE_CLOSE,      \* BOOLEAN
    BUGGY_FREE_ON_UNMAP,             \* BOOLEAN
    BUGGY_NEVER_FREE                 \* BOOLEAN

ASSUME Cardinality(VmoIds) >= 1
ASSUME MaxRefs \in Nat /\ MaxRefs >= 2
ASSUME BUGGY_FREE_ON_HANDLE_CLOSE \in BOOLEAN
ASSUME BUGGY_FREE_ON_UNMAP \in BOOLEAN
ASSUME BUGGY_NEVER_FREE \in BOOLEAN

VARIABLES
    vmos,            \* SUBSET VmoIds — VMOs that have been created
    handle_count,    \* [VmoIds -> 0..MaxRefs]
    mapping_count,   \* [VmoIds -> 0..MaxRefs]
    pages_alive      \* SUBSET VmoIds — VMOs with allocated pages

vars == <<vmos, handle_count, mapping_count, pages_alive>>

TypeOk ==
    /\ vmos \subseteq VmoIds
    /\ handle_count \in [VmoIds -> 0..MaxRefs]
    /\ mapping_count \in [VmoIds -> 0..MaxRefs]
    /\ pages_alive \subseteq vmos

(***************************************************************************)
(* Init: no VMOs created; all counts = 0; no pages alive.                 *)
(***************************************************************************)
Init ==
    /\ vmos = {}
    /\ handle_count = [v \in VmoIds |-> 0]
    /\ mapping_count = [v \in VmoIds |-> 0]
    /\ pages_alive = {}

(***************************************************************************)
(* VmoCreate(v) — allocate a fresh BURROW of identity v. The creator gets    *)
(* the first handle: handle_count[v] = 1. Pages are allocated.            *)
(*                                                                         *)
(* Each v can be created at most once (v \notin vmos). Bounds state space.*)
(* In the impl, BURROW identities are unique to their lifetime; freed VMOs  *)
(* don't get re-created with the same id while modeling proceeds.         *)
(*                                                                         *)
(* Maps to `kernel/burrow.c::burrow_create_anon` (P2-Fd).                        *)
(***************************************************************************)
VmoCreate(v) ==
    /\ v \notin vmos
    /\ vmos' = vmos \cup {v}
    /\ handle_count' = [handle_count EXCEPT ![v] = 1]
    /\ mapping_count' = mapping_count
    /\ pages_alive' = pages_alive \cup {v}

(***************************************************************************)
(* HandleOpen(v) — increment handle_count. Models a handle dup (within   *)
(* a Proc) or a handle transfer (cross-Proc via 9P). Both produce a fresh*)
(* handle to an existing BURROW; both increment the count by 1.              *)
(*                                                                         *)
(* Precondition: v's pages are alive (you can't open a handle to a freed*)
(* BURROW).                                                                   *)
(*                                                                         *)
(* Maps to `kernel/handle.c::handle_dup` and the eventual                  *)
(* `kernel/handle.c::handle_transfer_via_9p` (Phase 4) for handles whose *)
(* kobj is a BURROW. Each call increments the BURROW's handle_count via        *)
(* `kernel/burrow.c::burrow_ref`.                                                *)
(***************************************************************************)
HandleOpen(v) ==
    /\ v \in pages_alive
    /\ handle_count[v] < MaxRefs
    /\ handle_count' = [handle_count EXCEPT ![v] = @ + 1]
    /\ UNCHANGED <<vmos, mapping_count, pages_alive>>

(***************************************************************************)
(* HandleClose(v) — decrement handle_count. If both counts reach 0, free *)
(* pages.                                                                  *)
(*                                                                         *)
(* Maps to `kernel/handle.c::handle_close` for BURROW-typed handles +       *)
(* `kernel/burrow.c::burrow_unref` (P2-Fd).                                      *)
(***************************************************************************)
HandleClose(v) ==
    /\ handle_count[v] > 0
    /\ handle_count' = [handle_count EXCEPT ![v] = @ - 1]
    /\ pages_alive' =
         IF /\ handle_count[v] = 1                  \* will reach 0 this step
            /\ mapping_count[v] = 0
         THEN pages_alive \ {v}
         ELSE pages_alive
    /\ UNCHANGED <<vmos, mapping_count>>

(***************************************************************************)
(* BuggyFreeOnHandleClose(v) — premature free: pages dropped when        *)
(* handle_count reaches 0 even if mapping_count > 0. UAF.                 *)
(*                                                                         *)
(* Real-world analogue: burrow_unref's free path forgets to check            *)
(* mapping_count. The VMA still references freed pages.                    *)
(*                                                                         *)
(* Caught by NoUseAfterFree.                                               *)
(***************************************************************************)
BuggyFreeOnHandleClose(v) ==
    /\ BUGGY_FREE_ON_HANDLE_CLOSE
    /\ handle_count[v] > 0
    /\ handle_count' = [handle_count EXCEPT ![v] = @ - 1]
    /\ pages_alive' =
         IF handle_count[v] = 1
         THEN pages_alive \ {v}                     \* free regardless of mapping_count
         ELSE pages_alive
    /\ UNCHANGED <<vmos, mapping_count>>

(***************************************************************************)
(* MapVmo(v) — create a new mapping for BURROW v in some address space.     *)
(* Increments mapping_count. Caller must already hold a handle (so v's   *)
(* pages are alive).                                                       *)
(*                                                                         *)
(* Maps to `kernel/burrow.c::burrow_map` (P2-Fd) called from mmap_handle.       *)
(***************************************************************************)
MapVmo(v) ==
    /\ v \in pages_alive
    /\ mapping_count[v] < MaxRefs
    /\ mapping_count' = [mapping_count EXCEPT ![v] = @ + 1]
    /\ UNCHANGED <<vmos, handle_count, pages_alive>>

(***************************************************************************)
(* UnmapVmo(v) — destroy one mapping. Decrements mapping_count. If both  *)
(* counts reach 0, free pages.                                             *)
(*                                                                         *)
(* Maps to `kernel/burrow.c::burrow_unmap` (P2-Fd) called from munmap.           *)
(***************************************************************************)
UnmapVmo(v) ==
    /\ mapping_count[v] > 0
    /\ mapping_count' = [mapping_count EXCEPT ![v] = @ - 1]
    /\ pages_alive' =
         IF /\ mapping_count[v] = 1                 \* will reach 0 this step
            /\ handle_count[v] = 0
         THEN pages_alive \ {v}
         ELSE pages_alive
    /\ UNCHANGED <<vmos, handle_count>>

(***************************************************************************)
(* BuggyFreeOnUnmap(v) — premature free: pages dropped when              *)
(* mapping_count reaches 0 even if handle_count > 0. UAF.                 *)
(*                                                                         *)
(* Real-world analogue: burrow_unmap's free path forgets to check            *)
(* handle_count. The handle still references freed pages.                  *)
(*                                                                         *)
(* Caught by NoUseAfterFree.                                               *)
(***************************************************************************)
BuggyFreeOnUnmap(v) ==
    /\ BUGGY_FREE_ON_UNMAP
    /\ mapping_count[v] > 0
    /\ mapping_count' = [mapping_count EXCEPT ![v] = @ - 1]
    /\ pages_alive' =
         IF mapping_count[v] = 1
         THEN pages_alive \ {v}                     \* free regardless of handle_count
         ELSE pages_alive
    /\ UNCHANGED <<vmos, handle_count>>

(***************************************************************************)
(* BuggyNoFreeHandleClose(v) / BuggyNoFreeUnmap(v) — delayed/never free: *)
(* the close/unmap action decrements the count but never triggers the    *)
(* free transition, even when both counts reach 0. Resource leak.         *)
(*                                                                         *)
(* Real-world analogue: burrow_unref's free path is structured behind a      *)
(* condition that's wrong (never fires) or a callsite that's missed.      *)
(*                                                                         *)
(* Caught by NoUseAfterFree (the iff form catches both premature AND     *)
(* delayed free).                                                          *)
(***************************************************************************)
BuggyNoFreeHandleClose(v) ==
    /\ BUGGY_NEVER_FREE
    /\ handle_count[v] > 0
    /\ handle_count' = [handle_count EXCEPT ![v] = @ - 1]
    /\ pages_alive' = pages_alive                   \* never modify
    /\ UNCHANGED <<vmos, mapping_count>>

BuggyNoFreeUnmap(v) ==
    /\ BUGGY_NEVER_FREE
    /\ mapping_count[v] > 0
    /\ mapping_count' = [mapping_count EXCEPT ![v] = @ - 1]
    /\ pages_alive' = pages_alive
    /\ UNCHANGED <<vmos, handle_count>>

Next ==
    \/ \E v \in VmoIds : VmoCreate(v)
    \/ \E v \in VmoIds : HandleOpen(v)
    \/ \E v \in VmoIds : HandleClose(v)
    \/ \E v \in VmoIds : BuggyFreeOnHandleClose(v)
    \/ \E v \in VmoIds : MapVmo(v)
    \/ \E v \in VmoIds : UnmapVmo(v)
    \/ \E v \in VmoIds : BuggyFreeOnUnmap(v)
    \/ \E v \in VmoIds : BuggyNoFreeHandleClose(v)
    \/ \E v \in VmoIds : BuggyNoFreeUnmap(v)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(***************************************************************************)
(* RefcountConsistent — VMOs that have not been created have all counts  *)
(* = 0 and pages not alive.                                                *)
(***************************************************************************)
RefcountConsistent ==
    \A v \in VmoIds :
        v \notin vmos => /\ handle_count[v] = 0
                         /\ mapping_count[v] = 0
                         /\ v \notin pages_alive

(***************************************************************************)
(* NoUseAfterFree — pages are alive IFF at least one refcount is > 0.    *)
(* ARCH §28 I-7 in iff form:                                              *)
(*                                                                         *)
(*   - LHS true, RHS false ⇒ premature free (pages dead while refs > 0).*)
(*   - LHS false, RHS true ⇒ delayed free (pages alive while refs = 0). *)
(*                                                                         *)
(* Both directions are violations of the "live until both 0" semantics.   *)
(***************************************************************************)
NoUseAfterFree ==
    \A v \in VmoIds :
        (handle_count[v] > 0 \/ mapping_count[v] > 0) <=> (v \in pages_alive)

Invariants ==
    /\ TypeOk
    /\ RefcountConsistent
    /\ NoUseAfterFree

====
