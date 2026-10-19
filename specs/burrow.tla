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
(* and, since AS-R9, the I-32 charge record's half of the same lifecycle: *)
(* the drop that ends a Burrow settles its payer's charge exactly once,  *)
(* and no record outlives the Burrow it lives in.                         *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   Each BURROW has two refcounts: handle_count (open handles) and         *)
(*   mapping_count (open mappings into address spaces). Pages exist iff    *)
(*   at least one count is > 0; the moment both reach 0, pages MUST be   *)
(*   freed.                                                                 *)
(*                                                                         *)
(*   BURROW types are an impl detail not modeled here:                     *)
(*     BURROW_TYPE_ANON — backing is alloc_pages; free path is free_pages.*)
(*     BURROW_TYPE_MMIO — backing is a fixed PA owned by a KObj_MMIO; free*)
(*                        path is kobj_mmio_unref + struct kfree (no      *)
(*                        page-allocator interaction). Added at P4-Ic1.   *)
(*     BURROW_TYPE_DMA  — backing is a buddy-allocated contiguous page    *)
(*                        chunk owned by a KObj_DMA; free path is        *)
(*                        kobj_dma_unref + struct kfree. The KObj_DMA's  *)
(*                        own unref then triggers free_pages on the      *)
(*                        underlying chunk if the user-handle's ref      *)
(*                        already dropped. Added at P4-Ic5b1b.            *)
(*   All three share the same dual-count NoUseAfterFree semantics, which *)
(*   is the invariant this spec proves. The hw types' additional         *)
(*   relationship (holding a ref on the underlying KObj_* so the         *)
(*   PA/chunk claim survives the user-handle's close) is modeled in      *)
(*   handles.tla's HwResourceExclusive — the cross-spec link is          *)
(*   impl-level, not spec-level. See                                     *)
(*   `kernel/burrow.c::burrow_free_internal`'s type switch.              *)
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
(* The charge record (AS-R9):                                              *)
(*                                                                         *)
(*   Every Burrow here is an eager region whose creator paid: the record *)
(*   (`record`, the struct's charge_as_id/charge_pages) is stamped at    *)
(*   creation and `billed[v]` is the payer's ledger entry for it, 1 until*)
(*   the refund. Every drop is one of the payer's own claims -- the race *)
(*   AS-R9 closed is between two of them (a munmap and a Loom ring close,*)
(*   say), so a non-payer's drop and `shared_out` (monotonic; it only    *)
(*   adds reasons to settle) are left out. Uncharged Burrow types add no *)
(*   behaviour.                                                          *)
(*                                                                         *)
(*   The settled drop (HandleClose / UnmapVmo) takes the record in the   *)
(*   same step -- the same v->lock hold -- that decides finality. The    *)
(*   refund itself lands later, outside the leaf lock, but the amount is *)
(*   the dropping thread's private scalar by then, so folding it into the*)
(*   step loses no interleaving the ledger can observe.                   *)
(*                                                                         *)
(*   BUGGY_CLAIM_RESTORE replaces every drop with the pre-AS-R9 shape:   *)
(*   claim, drop, restore-if-not-final, each its own v->lock hold. A     *)
(*   Thread owns one ref of one Burrow from its claim to its drop, so    *)
(*   `Held` keeps two threads from dropping the same ref.                *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation per CLAUDE.md spec-first*)
(* policy):                                                                *)
(*                                                                         *)
(*   burrow.cfg                       all flags FALSE — TLC proves          *)
(*                                  every invariant below.                *)
(*   burrow_buggy_free_on_close.cfg   BUGGY_FREE_ON_HANDLE_CLOSE=TRUE —     *)
(*                                  premature free on handle_count=0     *)
(*                                  while mapping_count > 0.              *)
(*   burrow_buggy_free_on_unmap.cfg   BUGGY_FREE_ON_UNMAP=TRUE —            *)
(*                                  premature free on mapping_count=0    *)
(*                                  while handle_count > 0.               *)
(*   burrow_buggy_never_free.cfg      BUGGY_NEVER_FREE=TRUE —               *)
(*                                  delayed/never free; pages alive at   *)
(*                                  both counts = 0 (resource leak).      *)
(*   burrow_buggy_claim_restore.cfg   BUGGY_CLAIM_RESTORE=TRUE — a        *)
(*                                  restore writes the record into a     *)
(*                                  Burrow a sibling claim's drop freed, *)
(*                                  or the freeing drop finds the record *)
(*                                  claimed away and frees it with the   *)
(*                                  restore landing behind it. Violates  *)
(*                                  NoRecordAfterFree.                   *)
(*   burrow_buggy_claim_restore_ledger.cfg  the same flag, judged on the *)
(*                                  ledger alone: the charge outlives the*)
(*                                  Burrow, never refunded. Violates     *)
(*                                  RefundExactlyOnce.                   *)
(*                                                                         *)
(*   specs/check-burrow.sh runs all six: the clean count is pinned, and   *)
(*   each buggy cfg lists its invariants one by one and must be refuted   *)
(*   on the one named above.                                              *)
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
(*   NoRecordAfterFree  — a charge record is only ever on a live Burrow. *)
(*                                                                         *)
(*   RefundExactlyOnce  — the ledger entry never goes negative, and once *)
(*                        a Burrow is freed and no claim is in flight on *)
(*                        it, its charge has been refunded.              *)
(*                                                                         *)
(* See ARCHITECTURE.md §19 (VMOs) + §28 invariants I-7 and I-32.          *)
(***************************************************************************)
EXTENDS Naturals, Integers, FiniteSets

CONSTANTS
    VmoIds,                          \* set of BURROW identifiers (>= 1)
    MaxRefs,                         \* Nat — bound on refcounts for TLC
    Threads,                         \* model values; only the buggy shape uses them
    BUGGY_FREE_ON_HANDLE_CLOSE,      \* BOOLEAN
    BUGGY_FREE_ON_UNMAP,             \* BOOLEAN
    BUGGY_NEVER_FREE,                \* BOOLEAN
    BUGGY_CLAIM_RESTORE              \* BOOLEAN

ASSUME Cardinality(VmoIds) >= 1
ASSUME MaxRefs \in Nat /\ MaxRefs >= 2
ASSUME BUGGY_FREE_ON_HANDLE_CLOSE \in BOOLEAN
ASSUME BUGGY_FREE_ON_UNMAP \in BOOLEAN
ASSUME BUGGY_NEVER_FREE \in BOOLEAN
ASSUME BUGGY_CLAIM_RESTORE \in BOOLEAN

VARIABLES
    vmos,            \* SUBSET VmoIds — VMOs that have been created
    handle_count,    \* [VmoIds -> 0..MaxRefs]
    mapping_count,   \* [VmoIds -> 0..MaxRefs]
    pages_alive,     \* SUBSET VmoIds — VMOs with allocated pages
    record,          \* SUBSET VmoIds — VMOs whose charge record is set
    billed,          \* [VmoIds -> Int] — the payer's ledger entry per VMO
    pc,              \* [Threads -> {"idle", "claimed", "dropped"}]
    tv,              \* [Threads -> VmoIds] — the VMO a thread is dropping
    tk,              \* [Threads -> RefKinds] — which of its refs
    took,            \* [Threads -> BOOLEAN] — its claim took the record
    freed            \* [Threads -> BOOLEAN] — its drop freed the VMO

vars        == <<vmos, handle_count, mapping_count, pages_alive, record, billed,
                 pc, tv, tk, took, freed>>
chargeVars  == <<record, billed>>
threadVars  == <<pc, tv, tk, took, freed>>

RefKinds == {"handle", "mapping"}
V0 == CHOOSE v \in VmoIds : TRUE

TypeOk ==
    /\ vmos \subseteq VmoIds
    /\ handle_count \in [VmoIds -> 0..MaxRefs]
    /\ mapping_count \in [VmoIds -> 0..MaxRefs]
    /\ pages_alive \subseteq vmos
    /\ record \subseteq vmos
    /\ billed \in [VmoIds -> -1..1]
    /\ pc \in [Threads -> {"idle", "claimed", "dropped"}]
    /\ tv \in [Threads -> VmoIds]
    /\ tk \in [Threads -> RefKinds]
    /\ took \in [Threads -> BOOLEAN]
    /\ freed \in [Threads -> BOOLEAN]

(***************************************************************************)
(* Init: no VMOs created; all counts = 0; no pages alive; no charges.     *)
(***************************************************************************)
Init ==
    /\ vmos = {}
    /\ handle_count = [v \in VmoIds |-> 0]
    /\ mapping_count = [v \in VmoIds |-> 0]
    /\ pages_alive = {}
    /\ record = {}
    /\ billed = [v \in VmoIds |-> 0]
    /\ pc = [t \in Threads |-> "idle"]
    /\ tv = [t \in Threads |-> V0]
    /\ tk = [t \in Threads |-> "handle"]
    /\ took = [t \in Threads |-> FALSE]
    /\ freed = [t \in Threads |-> FALSE]

(***************************************************************************)
(* Settle(v, final) — the charge half of a settled drop, taken in the same*)
(* step as the decrement. A final drop claims the record if it is still  *)
(* set and refunds it; a non-final one leaves it for the drop that does  *)
(* end the region.                                                         *)
(*                                                                         *)
(* Maps to `kernel/burrow.c::burrow_charge_claim_locked`, called inside   *)
(* the v->lock hold of `burrow_unref_settled_in` and                      *)
(* `burrow_release_mapping_settled_deferred`.                             *)
(***************************************************************************)
Settle(v, final) ==
    IF final /\ v \in record
    THEN /\ record' = record \ {v}
         /\ billed' = [billed EXCEPT ![v] = @ - 1]
    ELSE UNCHANGED chargeVars

(***************************************************************************)
(* VmoCreate(v) — allocate a fresh BURROW of identity v. The creator gets    *)
(* the first handle: handle_count[v] = 1. Pages are allocated, and the   *)
(* creator's charge is recorded on the Burrow and billed to its ledger.  *)
(*                                                                         *)
(* Each v can be created at most once (v \notin vmos). Bounds state space.*)
(* In the impl, BURROW identities are unique to their lifetime; freed VMOs  *)
(* don't get re-created with the same id while modeling proceeds.         *)
(*                                                                         *)
(* Maps to `kernel/burrow.c::burrow_create_anon` (P2-Fd) + the eager      *)
(* charge's `burrow_charge_record` (#131).                                 *)
(***************************************************************************)
VmoCreate(v) ==
    /\ v \notin vmos
    /\ vmos' = vmos \cup {v}
    /\ handle_count' = [handle_count EXCEPT ![v] = 1]
    /\ mapping_count' = mapping_count
    /\ pages_alive' = pages_alive \cup {v}
    /\ record' = record \cup {v}
    /\ billed' = [billed EXCEPT ![v] = 1]
    /\ UNCHANGED threadVars

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
    /\ UNCHANGED chargeVars
    /\ UNCHANGED threadVars

(***************************************************************************)
(* HandleClose(v) — decrement handle_count. If both counts reach 0, free *)
(* pages and settle the charge, in the same step.                          *)
(*                                                                         *)
(* Maps to `kernel/handle.c::handle_close` for BURROW-typed handles +       *)
(* `kernel/burrow.c::burrow_unref` (P2-Fd); since AS-R9 the decision is   *)
(* `burrow_unref_settled_in`'s.                                            *)
(***************************************************************************)
HandleClose(v) ==
    /\ ~BUGGY_CLAIM_RESTORE
    /\ handle_count[v] > 0
    /\ handle_count' = [handle_count EXCEPT ![v] = @ - 1]
    /\ LET final == handle_count[v] = 1 /\ mapping_count[v] = 0
       IN /\ pages_alive' = IF final THEN pages_alive \ {v} ELSE pages_alive
          /\ Settle(v, final)
    /\ UNCHANGED <<vmos, mapping_count>>
    /\ UNCHANGED threadVars

(***************************************************************************)
(* BuggyFreeOnHandleClose(v) — premature free: pages dropped when        *)
(* handle_count reaches 0 even if mapping_count > 0. UAF.                 *)
(*                                                                         *)
(* Real-world analogue: burrow_unref's free path forgets to check            *)
(* mapping_count. The VMA still references freed pages.                    *)
(*                                                                         *)
(* Settles exactly as the correct drop does when it frees, so the only   *)
(* invariant it breaks is the one it is named for.                         *)
(*                                                                         *)
(* Caught by NoUseAfterFree.                                               *)
(***************************************************************************)
BuggyFreeOnHandleClose(v) ==
    /\ BUGGY_FREE_ON_HANDLE_CLOSE
    /\ handle_count[v] > 0
    /\ handle_count' = [handle_count EXCEPT ![v] = @ - 1]
    /\ LET final == handle_count[v] = 1            \* free regardless of mapping_count
       IN /\ pages_alive' = IF final THEN pages_alive \ {v} ELSE pages_alive
          /\ Settle(v, final)
    /\ UNCHANGED <<vmos, mapping_count>>
    /\ UNCHANGED threadVars

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
    /\ UNCHANGED chargeVars
    /\ UNCHANGED threadVars

(***************************************************************************)
(* UnmapVmo(v) — destroy one mapping. Decrements mapping_count. If both  *)
(* counts reach 0, free pages and settle the charge, in the same step.     *)
(*                                                                         *)
(* Maps to `kernel/burrow.c::burrow_unmap` (P2-Fd) called from munmap;    *)
(* since AS-R9 the decision is `burrow_release_mapping_settled_deferred`'s.*)
(***************************************************************************)
UnmapVmo(v) ==
    /\ ~BUGGY_CLAIM_RESTORE
    /\ mapping_count[v] > 0
    /\ mapping_count' = [mapping_count EXCEPT ![v] = @ - 1]
    /\ LET final == mapping_count[v] = 1 /\ handle_count[v] = 0
       IN /\ pages_alive' = IF final THEN pages_alive \ {v} ELSE pages_alive
          /\ Settle(v, final)
    /\ UNCHANGED <<vmos, handle_count>>
    /\ UNCHANGED threadVars

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
    /\ LET final == mapping_count[v] = 1           \* free regardless of handle_count
       IN /\ pages_alive' = IF final THEN pages_alive \ {v} ELSE pages_alive
          /\ Settle(v, final)
    /\ UNCHANGED <<vmos, handle_count>>
    /\ UNCHANGED threadVars

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
    /\ UNCHANGED chargeVars
    /\ UNCHANGED threadVars

BuggyNoFreeUnmap(v) ==
    /\ BUGGY_NEVER_FREE
    /\ mapping_count[v] > 0
    /\ mapping_count' = [mapping_count EXCEPT ![v] = @ - 1]
    /\ pages_alive' = pages_alive
    /\ UNCHANGED <<vmos, handle_count>>
    /\ UNCHANGED chargeVars
    /\ UNCHANGED threadVars

(***************************************************************************)
(* The pre-AS-R9 drop, three v->lock holds with gaps between them:       *)
(*                                                                         *)
(*   ClaimBeforeDrop  — take the record (burrow_charge_claim), keeping   *)
(*                      the pages privately.                              *)
(*   DropAfterClaim   — drop the ref (burrow_unref_freed /                *)
(*                      burrow_release_mapping_freed), learning whether   *)
(*                      it freed.                                         *)
(*   SettleAfterDrop  — refund if it freed; otherwise write the record   *)
(*                      back (burrow_charge_restore) for the drop that    *)
(*                      does end the region. By then a sibling claim's    *)
(*                      drop may have been that drop: the write lands in  *)
(*                      a freed Burrow -- an extinction on the magic      *)
(*                      check, or a charge planted on whatever reused the *)
(*                      slot -- and the payer is never refunded.          *)
(*                                                                         *)
(* Real-world analogue: every asynchronous settle site on this tree        *)
(* before AS-R9 (Loom, Weft, vma_detach_range_in's eager-ANON arm). The    *)
(* pair survives only in SYS_JIT_DESTROY, whose surviving alias under      *)
(* as->lock is the independent reference this race lacks.                  *)
(*                                                                         *)
(* Caught by NoRecordAfterFree and RefundExactlyOnce.                      *)
(***************************************************************************)
Held(v, k) == Cardinality({t \in Threads : pc[t] = "claimed" /\ tv[t] = v /\ tk[t] = k})

Count(v, k) == IF k = "handle" THEN handle_count[v] ELSE mapping_count[v]

ClaimBeforeDrop(t, v, k) ==
    /\ BUGGY_CLAIM_RESTORE
    /\ pc[t] = "idle"
    /\ Count(v, k) > Held(v, k)                     \* a ref nobody else is dropping
    /\ pc' = [pc EXCEPT ![t] = "claimed"]
    /\ tv' = [tv EXCEPT ![t] = v]
    /\ tk' = [tk EXCEPT ![t] = k]
    /\ took' = [took EXCEPT ![t] = (v \in record)]
    /\ record' = record \ {v}
    /\ UNCHANGED <<vmos, handle_count, mapping_count, pages_alive, billed, freed>>

DropAfterClaim(t) ==
    /\ pc[t] = "claimed"
    /\ LET v == tv[t]
           final == IF tk[t] = "handle"
                    THEN handle_count[v] = 1 /\ mapping_count[v] = 0
                    ELSE mapping_count[v] = 1 /\ handle_count[v] = 0
       IN /\ handle_count' = IF tk[t] = "handle"
                             THEN [handle_count EXCEPT ![v] = @ - 1]
                             ELSE handle_count
          /\ mapping_count' = IF tk[t] = "mapping"
                              THEN [mapping_count EXCEPT ![v] = @ - 1]
                              ELSE mapping_count
          /\ pages_alive' = IF final THEN pages_alive \ {v} ELSE pages_alive
          /\ freed' = [freed EXCEPT ![t] = final]
    /\ pc' = [pc EXCEPT ![t] = "dropped"]
    /\ UNCHANGED <<vmos, record, billed, tv, tk, took>>

SettleAfterDrop(t) ==
    /\ pc[t] = "dropped"
    /\ LET v == tv[t]
       IN IF took[t] /\ freed[t]
          THEN /\ billed' = [billed EXCEPT ![v] = @ - 1]
               /\ record' = record
          ELSE IF took[t]
          THEN /\ record' = record \cup {v}         \* the restore: a write into v
               /\ billed' = billed
          ELSE UNCHANGED chargeVars
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ tv' = [tv EXCEPT ![t] = V0]
    /\ tk' = [tk EXCEPT ![t] = "handle"]
    /\ took' = [took EXCEPT ![t] = FALSE]
    /\ freed' = [freed EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<vmos, handle_count, mapping_count, pages_alive>>

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
    \/ \E t \in Threads, v \in VmoIds, k \in RefKinds : ClaimBeforeDrop(t, v, k)
    \/ \E t \in Threads : DropAfterClaim(t)
    \/ \E t \in Threads : SettleAfterDrop(t)

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

(***************************************************************************)
(* NoRecordAfterFree — the record lives in the Burrow, so it is only ever *)
(* on a live one: never written into a freed Burrow (the restore's use-  *)
(* after-free write), and never left on one by the drop that freed it    *)
(* (a settlement lost with the struct).                                    *)
(***************************************************************************)
NoRecordAfterFree == record \subseteq pages_alive

(***************************************************************************)
(* RefundExactlyOnce — the ledger entry never goes below zero (no double *)
(* refund), and a freed Burrow with no claim in flight on it has been    *)
(* refunded (no charge outlives the region it paid for). The in-flight   *)
(* guard exists for the buggy shape, whose own freeing drop refunds one  *)
(* step after it frees.                                                    *)
(***************************************************************************)
InFlight(v) == \E t \in Threads : pc[t] # "idle" /\ tv[t] = v

RefundExactlyOnce ==
    \A v \in VmoIds :
        /\ billed[v] >= 0
        /\ (v \in vmos /\ v \notin pages_alive /\ ~InFlight(v)) => billed[v] = 0

Invariants ==
    /\ TypeOk
    /\ RefcountConsistent
    /\ NoUseAfterFree
    /\ NoRecordAfterFree
    /\ RefundExactlyOnce

====
