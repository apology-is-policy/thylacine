---- MODULE capacity ----
(***************************************************************************)
(* Thylacine -- the I-32 page-accounting conservation law (B-1a', 2026-09-23) *)
(* and the anonymous-mapping merge that reuses offsets (B-3e, 2026-10-19)    *)
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
(* B-3e (ARCH 6.5 "Adjacent anonymous mappings merge") adds the one         *)
(* operation that maps an offset of a LIVE Burrow again after it was        *)
(* unmapped: a fresh reservation that extends a neighbour's Burrow instead  *)
(* of minting one. Two more laws say when that is sound:                    *)
(*                                                                         *)
(*   NoAlias          no offset of a Burrow is mapped at two addresses. An  *)
(*                    alias is a page in two places: the detach of one       *)
(*                    releases the page while the other still maps it.       *)
(*                    The extension keeps it by the ONE-BIAS rule -- every   *)
(*                    mapping of an anonymous lazy Burrow places offset 0 at *)
(*                    one address, so a free address names an offset no     *)
(*                    mapping covers.                                       *)
(*   ZeroRead         at a free address, no live Burrow holds a page at the *)
(*                    offset that address names -- so whatever Burrow an    *)
(*                    extension reuses, the new mapping reads zero. It      *)
(*                    follows from NoOrphan plus the bias, and it is what    *)
(*                    the kernel's own pre-extension check asserts.          *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   The window's pages are the set Slots, which is ALSO each Burrow's     *)
(*   offset set: both Burrows sit at bias 0, so address a names offset a.  *)
(*   `at[a]` is what address a maps -- <<b, s>>, or Free. A VMA piece is a  *)
(*   run of addresses; the range detach, the protect split and the D-3b    *)
(*   replace all reduce to "which addresses map which offsets", which is   *)
(*   all the accounting can see. Pieces, merges of pieces and prots are    *)
(*   below this model, and so is ADJACENCY: an Extend may reuse ANY live   *)
(*   Burrow, which over-approximates the kernel (a neighbour only) and is  *)
(*   sound for these safety properties.                                    *)
(*                                                                         *)
(*   The Burrow frees when its LAST mapped address goes (mapping_count ->  *)
(*   0, handle_count already 0: the Tier-1 shape). The free drops every     *)
(*   resident page -- and leaves `charge` alone, which is the load-bearing  *)
(*   fact: burrow_free_internal has no Proc to refund. So every path that   *)
(*   unmaps a slot MUST release it first (free + uncharge), or the charge   *)
(*   outlives the page.                                                     *)
(*                                                                         *)
(*   Metadata (the pagemap's node pages, and the nodes an extension's       *)
(*   growth adds) is charged and released by the same paths and is not     *)
(*   modeled separately: it rides the slot it indexes.                      *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation):                         *)
(*                                                                         *)
(*   capacity.cfg                          all flags FALSE -- all hold.     *)
(*   capacity_buggy_replace_orphans.cfg    BUGGY_REPLACE_KEEPS_ORPHANS --   *)
(*       the window's slots of the OLD Burrow stay resident + charged after *)
(*       the swap (B-1a audit F5). NoOrphan violated at the Replace.        *)
(*   capacity_buggy_detach_no_refund.cfg   BUGGY_DETACH_NO_REFUND -- a      *)
(*       detach unmaps a resident slot without releasing it (the shape the  *)
(*       B-1a lazy-piece refund replaced). NoOrphan violated at the Detach. *)
(*   capacity_buggy_extend_alias.cfg       BUGGY_EXTEND_ALIAS -- the        *)
(*       extension maps WHATEVER offset it is handed, not the one the       *)
(*       Burrow's bias names (a second bias: an mremap, or a map of the     *)
(*       same Burrow at another address). NoAlias violated at the Extend,   *)
(*       with ChargeConserved, NoOrphan and ZeroRead all HOLDING -- the     *)
(*       accounting cannot see an alias.                                    *)
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
(*   Extend   -> kernel/vma.c::vma_extend_anon_in, reached from the lazy    *)
(*               reservation core (SYS_BURROW_RESERVE, _ATTACH_LAZY); the  *)
(*               bias is the Burrow's lazy_va0, stamped and checked in      *)
(*               vma_alloc; ZeroRead is its pre-extension empty check       *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Slots,                          \* the window's pages = each Burrow's offsets (>= 1)
    BUGGY_REPLACE_KEEPS_ORPHANS,    \* BOOLEAN
    BUGGY_DETACH_NO_REFUND,         \* BOOLEAN
    BUGGY_EXTEND_ALIAS              \* BOOLEAN

ASSUME Cardinality(Slots) >= 1
ASSUME BUGGY_REPLACE_KEEPS_ORPHANS \in BOOLEAN
ASSUME BUGGY_DETACH_NO_REFUND \in BOOLEAN
ASSUME BUGGY_EXTEND_ALIAS \in BOOLEAN

Burrows == {"old", "new"}

Free == <<"free", "free">>          \* an address no mapping covers

VARIABLES
    live,       \* [Burrows -> BOOLEAN]: the Burrow exists (a mapping holds it)
    at,         \* [Slots -> (Burrows \X Slots) \cup {Free}]: what each address maps
    resident,   \* [Burrows -> [Slots -> BOOLEAN]]: offset holds a page
    charge      \* Nat: the address space's page_count

vars == <<live, at, resident, charge>>

TypeOk ==
    /\ live \in [Burrows -> BOOLEAN]
    /\ at \in [Slots -> (Burrows \X Slots) \cup {Free}]
    /\ resident \in [Burrows -> [Slots -> BOOLEAN]]
    /\ charge \in Nat

(* The addresses that map offset s of Burrow b, and whether b is mapped at  *)
(* all in a given `at`.                                                     *)
Mappers(m, b, s) == {a \in Slots : m[a] = <<b, s>>}
MapsAny(m, b) == \E a \in Slots : m[a] # Free /\ m[a][1] = b

(***************************************************************************)
(* Init: the reservation is mapped whole, at bias 0, untouched; no window. *)
(***************************************************************************)
Init ==
    /\ live = [b \in Burrows |-> b = "old"]
    /\ at = [a \in Slots |-> <<"old", a>>]
    /\ resident = [b \in Burrows |-> [s \in Slots |-> FALSE]]
    /\ charge = 0

ResidentCount == Cardinality({<<b, s>> \in Burrows \X Slots : resident[b][s]})

(***************************************************************************)
(* Touch(a) -- the demand-zero fault at address a: a page is allocated for *)
(* the offset a maps, and charged (page_count == true RSS).                *)
(***************************************************************************)
Touch(a) ==
    LET m == at[a] IN
    /\ m # Free
    /\ live[m[1]] /\ ~resident[m[1]][m[2]]
    /\ resident' = [resident EXCEPT ![m[1]][m[2]] = TRUE]
    /\ charge' = charge + 1
    /\ UNCHANGED <<live, at>>

(***************************************************************************)
(* Decommit(a) -- madvise(DONTNEED): the page under a mapped address is    *)
(* freed and uncharged; the mapping stays.                                 *)
(***************************************************************************)
Decommit(a) ==
    LET m == at[a] IN
    /\ m # Free
    /\ live[m[1]] /\ resident[m[1]][m[2]]
    /\ resident' = [resident EXCEPT ![m[1]][m[2]] = FALSE]
    /\ charge' = charge - 1
    /\ UNCHANGED <<live, at>>

(***************************************************************************)
(* The two-step every unmapping path is built from: release the offset's   *)
(* page and its charge, then unmap the address. The Burrow frees when no   *)
(* address maps it any more: its remaining resident pages are dropped      *)
(* (burrow_free_internal walks the pagemap) but `charge` is NOT touched -- *)
(* the free has no Proc.                                                   *)
(***************************************************************************)
ReleaseCost(b, s) == IF resident[b][s] THEN 1 ELSE 0

(* The free that follows a Burrow's last unmap, applied to a `resident`     *)
(* function that may already have had the unmapped offset released.        *)
FreeIfLast(b, m2, res) ==
    IF MapsAny(m2, b) THEN res ELSE [res EXCEPT ![b] = [t \in Slots |-> FALSE]]

(***************************************************************************)
(* Detach(a) -- the range detach over one address: release the page of     *)
(* the offset it maps (free + uncharge) BEFORE the geometry changes, then  *)
(* unmap; if that was the Burrow's last mapped address the Burrow frees.   *)
(*                                                                         *)
(* BUGGY_DETACH_NO_REFUND skips the release: the offset is unmapped        *)
(* resident and charged, and nothing can ever refund it.                   *)
(***************************************************************************)
Detach(a) ==
    LET m == at[a] IN
    /\ m # Free
    /\ live[m[1]]
    /\ LET b        == m[1]
           s        == m[2]
           m2       == [at EXCEPT ![a] = Free]
           released == IF BUGGY_DETACH_NO_REFUND
                       THEN resident
                       ELSE [resident EXCEPT ![b][s] = FALSE]
           cost     == IF BUGGY_DETACH_NO_REFUND THEN 0 ELSE ReleaseCost(b, s)
       IN /\ at' = m2
          /\ resident' = FreeIfLast(b, m2, released)
          /\ charge' = charge - cost
          /\ live' = [live EXCEPT ![b] = MapsAny(m2, b)]

(***************************************************************************)
(* Replace(a) -- the D-3b MAP_FIXED window over one address of the old     *)
(* reservation: the "new" Burrow takes the address over, at bias 0. As     *)
(* built at B-1a', this is a detach of the window followed by an insert,   *)
(* so the old Burrow's offset is released before it stops being mapped.   *)
(*                                                                         *)
(* BUGGY_REPLACE_KEEPS_ORPHANS is the D-3b surgery as it stood before       *)
(* B-1a': the old VMA is cut around the window and the old Burrow's slots   *)
(* under it stay resident and charged (only their PTEs went). The B-1a      *)
(* audit's F5.                                                              *)
(***************************************************************************)
Replace(a) ==
    LET m == at[a] IN
    /\ m # Free
    /\ m[1] = "old"
    /\ live["old"]
    /\ LET s        == m[2]
           m2       == [at EXCEPT ![a] = <<"new", a>>]
           released == IF BUGGY_REPLACE_KEEPS_ORPHANS
                       THEN resident
                       ELSE [resident EXCEPT !["old"][s] = FALSE]
           cost     == IF BUGGY_REPLACE_KEEPS_ORPHANS THEN 0 ELSE ReleaseCost("old", s)
       IN /\ at' = m2
          /\ resident' = FreeIfLast("old", m2, released)
          /\ charge' = charge - cost
          /\ live' = [live EXCEPT !["old"] = MapsAny(m2, "old"), !["new"] = TRUE]

(***************************************************************************)
(* Extend(a, b) -- a fresh reservation lands at the free address a and is  *)
(* served by extending the LIVE Burrow b instead of minting one (the       *)
(* neighbour's Burrow, in the kernel). Nothing is charged: a reservation   *)
(* is free, and the page comes at the first Touch.                         *)
(*                                                                         *)
(* The offset is the one b's bias names for a -- offset a, at bias 0 --    *)
(* and the kernel checks it holds no page before mapping it (the guard is  *)
(* a precondition here; in the kernel a page there is an extinction).      *)
(* BUGGY_EXTEND_ALIAS takes WHATEVER offset it is handed: a second bias.   *)
(***************************************************************************)
Extend(a, b) ==
    /\ at[a] = Free
    /\ live[b]
    /\ \E s \in Slots :
        /\ IF BUGGY_EXTEND_ALIAS THEN TRUE ELSE s = a
        /\ ~resident[b][s]
        /\ at' = [at EXCEPT ![a] = <<b, s>>]
    /\ UNCHANGED <<live, resident, charge>>

Next ==
    \/ \E a \in Slots : Touch(a)
    \/ \E a \in Slots : Decommit(a)
    \/ \E a \in Slots : Detach(a)
    \/ \E a \in Slots : Replace(a)
    \/ \E a \in Slots, b \in Burrows : Extend(a, b)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(* Every resident page is charged, and nothing else is: page_count == RSS. *)
ChargeConserved == charge = ResidentCount

(* A resident page is always reachable through a mapped address, so a      *)
(* detach or a decommit can still refund it. The moment this fails the     *)
(* page is charged for the address space's life -- the F5 shape.           *)
NoOrphan == \A b \in Burrows, s \in Slots :
    resident[b][s] => Mappers(at, b, s) # {}

(* A live Burrow is mapped; a dead one has neither mappings nor pages.     *)
LiveIsMapped == \A b \in Burrows : live[b] <=> MapsAny(at, b)

(* No offset is mapped at two addresses.                                   *)
NoAlias == \A b \in Burrows, s \in Slots : Cardinality(Mappers(at, b, s)) <= 1

(* A free address names, in every live Burrow, an offset that holds no     *)
(* page: an extension at that address reads zero whichever Burrow it       *)
(* reuses.                                                                  *)
ZeroRead == \A a \in Slots, b \in Burrows :
    (at[a] = Free /\ live[b]) => ~resident[b][a]

Invariants ==
    /\ TypeOk
    /\ ChargeConserved
    /\ NoOrphan
    /\ LiveIsMapped
    /\ NoAlias
    /\ ZeroRead

====
