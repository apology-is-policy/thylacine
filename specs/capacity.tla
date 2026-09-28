---- MODULE capacity ----
(***************************************************************************)
(* Thylacine -- the I-32 page-accounting conservation law (B-1a', 2026-09-23) *)
(*                                                                         *)
(* One address space, one lazy reservation ("old") that a MAP_FIXED window  *)
(* ("new") may be overlaid into, and the page_count that charges it.        *)
(* ARCH 6.5 "Capacity" states the bar in prose: memory a program            *)
(* relinquishes RETURNS to the system, so its footprint shrinks. The kernel  *)
(* half of that bar is an accounting law, and this module is that law       *)
(* written down where a model checker can break it:                         *)
(*                                                                         *)
(*   ChargeConserved  page_count == the number of resident slots, always.   *)
(*   NoOrphan         a resident slot is always reachable through a live    *)
(*                    mapping -- because the ONLY refund paths (a detach, a  *)
(*                    decommit) walk MAPPINGS, and burrow_free_internal is   *)
(*                    Proc-agnostic: it frees a resident page but cannot     *)
(*                    uncharge it. A slot that loses its mapping while still *)
(*                    resident is therefore charged for the rest of the      *)
(*                    address space's life. That is exactly the B-1a audit's *)
(*                    F5, and exactly the pre-B-1a piece-detach bug before   *)
(*                    it.                                                    *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   A "slot" is one page of a reservation. Mappings are modeled per slot   *)
(*   (mapped[b][s]): a VMA piece covering s. The range detach, the protect  *)
(*   split and the D-3b replace all reduce to "which slots of which Burrow  *)
(*   are still covered by SOME piece of this address space", which is all   *)
(*   the accounting can see. Pieces, merges and prots are below this model. *)
(*                                                                         *)
(*   The Burrow frees when its LAST mapped slot goes (mapping_count -> 0,   *)
(*   handle_count already 0: the Tier-1 shape). The free drops every         *)
(*   resident page -- and leaves `charge` alone, which is the load-bearing  *)
(*   fact: burrow_free_internal has no Proc to refund. So every path that   *)
(*   unmaps a slot MUST release it first (free + uncharge), or the charge   *)
(*   outlives the page.                                                     *)
(*                                                                         *)
(*   Metadata (the pagemap's node pages) is charged and released by the     *)
(*   same paths and is not modeled separately: it rides the slot it indexes.*)
(*                                                                         *)
(* Buggy-config matrix (executable documentation):                         *)
(*                                                                         *)
(*   capacity.cfg                          all flags FALSE -- both hold.    *)
(*   capacity_buggy_replace_orphans.cfg    BUGGY_REPLACE_KEEPS_ORPHANS --   *)
(*       the window's slots of the OLD Burrow stay resident + charged after *)
(*       the swap (B-1a audit F5). NoOrphan violated at the Replace.        *)
(*   capacity_buggy_detach_no_refund.cfg   BUGGY_DETACH_NO_REFUND -- a      *)
(*       detach unmaps a resident slot without releasing it (the shape the  *)
(*       B-1a lazy-piece refund replaced). NoOrphan violated at the Detach. *)
(*                                                                         *)
(* Maps to the code (specs/SPEC-TO-CODE.md, "capacity.tla"):               *)
(*   Touch    -> arch/arm64/fault.c, the ANON_LAZY miss (charge, alloc,    *)
(*               pagemap_install)                                           *)
(*   Decommit -> kernel/burrow.c::burrow_decommit_in (SYS_BURROW_DECOMMIT) *)
(*   Detach   -> kernel/vma.c::vma_detach_range_in, phase 3: the ANON_LAZY  *)
(*               release (burrow_release_lazy_range_in) BEFORE the          *)
(*               geometry change; the free is vma_free_deferred's last drop *)
(*   Replace  -> kernel/vma.c::vma_replace_range_in = detach the window,    *)
(*               then insert the new mapping (F5 closed by construction)    *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Slots,                          \* the reservation's page indices (>= 1)
    BUGGY_REPLACE_KEEPS_ORPHANS,    \* BOOLEAN
    BUGGY_DETACH_NO_REFUND          \* BOOLEAN

ASSUME Cardinality(Slots) >= 1
ASSUME BUGGY_REPLACE_KEEPS_ORPHANS \in BOOLEAN
ASSUME BUGGY_DETACH_NO_REFUND \in BOOLEAN

Burrows == {"old", "new"}

VARIABLES
    live,       \* [Burrows -> BOOLEAN]: the Burrow exists (a mapping holds it)
    mapped,     \* [Burrows -> [Slots -> BOOLEAN]]: slot covered by a live piece
    resident,   \* [Burrows -> [Slots -> BOOLEAN]]: slot holds a page
    charge      \* Nat: the address space's page_count

vars == <<live, mapped, resident, charge>>

TypeOk ==
    /\ live \in [Burrows -> BOOLEAN]
    /\ mapped \in [Burrows -> [Slots -> BOOLEAN]]
    /\ resident \in [Burrows -> [Slots -> BOOLEAN]]
    /\ charge \in Nat

(***************************************************************************)
(* Init: the reservation is mapped whole and untouched; no window yet.     *)
(***************************************************************************)
Init ==
    /\ live = [b \in Burrows |-> b = "old"]
    /\ mapped = [b \in Burrows |-> [s \in Slots |-> b = "old"]]
    /\ resident = [b \in Burrows |-> [s \in Slots |-> FALSE]]
    /\ charge = 0

ResidentCount == Cardinality({<<b, s>> \in Burrows \X Slots : resident[b][s]})

(***************************************************************************)
(* Touch(b, s) -- the demand-zero fault: a page is allocated for a mapped, *)
(* non-resident slot and charged (page_count == true RSS).                 *)
(***************************************************************************)
Touch(b, s) ==
    /\ live[b] /\ mapped[b][s] /\ ~resident[b][s]
    /\ resident' = [resident EXCEPT ![b][s] = TRUE]
    /\ charge' = charge + 1
    /\ UNCHANGED <<live, mapped>>

(***************************************************************************)
(* Decommit(b, s) -- madvise(DONTNEED): a mapped resident slot's page is   *)
(* freed and uncharged; the mapping stays.                                 *)
(***************************************************************************)
Decommit(b, s) ==
    /\ live[b] /\ mapped[b][s] /\ resident[b][s]
    /\ resident' = [resident EXCEPT ![b][s] = FALSE]
    /\ charge' = charge - 1
    /\ UNCHANGED <<live, mapped>>

(***************************************************************************)
(* The two-step every unmapping path is built from.                        *)
(*                                                                         *)
(* Released(b, s, r): r is `resident` with slot (b, s) released -- and the  *)
(* charge that goes with it. Unmapped(b, s, m): m is `mapped` with the slot *)
(* uncovered. The Burrow frees when NO slot of it stays mapped: its         *)
(* remaining resident pages are dropped (burrow_free_internal walks the     *)
(* pagemap) but `charge` is NOT touched -- the free has no Proc.            *)
(***************************************************************************)
ReleaseCost(b, s) == IF resident[b][s] THEN 1 ELSE 0

LastMapped(b, s) == \A t \in Slots : t # s => ~mapped[b][t]

(* The Burrow-free that follows the last unmap: resident pages gone, charge *)
(* untouched. Applied to a `resident` function that may already have had    *)
(* the unmapped slot released.                                              *)
FreeIfLast(b, s, res) ==
    IF LastMapped(b, s) THEN [res EXCEPT ![b] = [t \in Slots |-> FALSE]] ELSE res

(***************************************************************************)
(* Detach(b, s) -- the range detach over one slot: release the slot's page *)
(* (free + uncharge) BEFORE the geometry changes, then unmap; if that was   *)
(* the Burrow's last mapped slot the Burrow frees.                         *)
(*                                                                         *)
(* BUGGY_DETACH_NO_REFUND skips the release: the slot is unmapped resident *)
(* and charged, and nothing can ever refund it.                            *)
(***************************************************************************)
Detach(b, s) ==
    /\ live[b] /\ mapped[b][s]
    /\ LET released == IF BUGGY_DETACH_NO_REFUND
                       THEN resident
                       ELSE [resident EXCEPT ![b][s] = FALSE]
           cost     == IF BUGGY_DETACH_NO_REFUND THEN 0 ELSE ReleaseCost(b, s)
       IN /\ resident' = FreeIfLast(b, s, released)
          /\ charge' = charge - cost
    /\ mapped' = [mapped EXCEPT ![b][s] = FALSE]
    /\ live' = [live EXCEPT ![b] = ~LastMapped(b, s)]

(***************************************************************************)
(* Replace(s) -- the D-3b MAP_FIXED window over one slot of the old         *)
(* reservation: the "new" Burrow takes the slot over. As built at B-1a',   *)
(* this is a detach of the window followed by an insert, so the old         *)
(* Burrow's slot is released before it stops being mapped.                 *)
(*                                                                         *)
(* BUGGY_REPLACE_KEEPS_ORPHANS is the D-3b surgery as it stood before       *)
(* B-1a': the old VMA is cut around the window and the old Burrow's slots   *)
(* under it stay resident and charged (only their PTEs went). The B-1a      *)
(* audit's F5.                                                              *)
(***************************************************************************)
Replace(s) ==
    /\ live["old"] /\ mapped["old"][s]
    /\ LET released == IF BUGGY_REPLACE_KEEPS_ORPHANS
                       THEN resident
                       ELSE [resident EXCEPT !["old"][s] = FALSE]
           cost     == IF BUGGY_REPLACE_KEEPS_ORPHANS THEN 0 ELSE ReleaseCost("old", s)
       IN /\ resident' = FreeIfLast("old", s, released)
          /\ charge' = charge - cost
    /\ mapped' = [mapped EXCEPT !["old"][s] = FALSE, !["new"][s] = TRUE]
    /\ live' = [live EXCEPT !["old"] = ~LastMapped("old", s), !["new"] = TRUE]

Next ==
    \/ \E b \in Burrows, s \in Slots : Touch(b, s)
    \/ \E b \in Burrows, s \in Slots : Decommit(b, s)
    \/ \E b \in Burrows, s \in Slots : Detach(b, s)
    \/ \E s \in Slots : Replace(s)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(* Every resident page is charged, and nothing else is: page_count == RSS. *)
ChargeConserved == charge = ResidentCount

(* A resident page is always reachable through a live mapping, so a detach *)
(* or a decommit can still refund it. The moment this fails the page is    *)
(* charged for the address space's life -- the F5 shape.                   *)
NoOrphan == \A b \in Burrows, s \in Slots : resident[b][s] => mapped[b][s]

(* A live Burrow has a mapping; a dead one has neither mappings nor pages. *)
LiveIsMapped == \A b \in Burrows :
    live[b] <=> (\E s \in Slots : mapped[b][s])

Invariants ==
    /\ TypeOk
    /\ ChargeConserved
    /\ NoOrphan
    /\ LiveIsMapped

====
