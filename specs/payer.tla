---- MODULE payer ----
(***************************************************************************)
(* Thylacine -- the payer record: pages that outlive the mapping that      *)
(* faulted them (vote 118, 2026-11-13: a private FILE image; vote 119,     *)
(* 2026-11-14: shared anonymous memory)                                    *)
(*                                                                         *)
(* capacity.tla is the I-32 law for memory ONE address space owns: a page  *)
(* is charged to the space that maps it, and a slot is released before it  *)
(* is unmapped, because burrow_free_internal has no space to refund. A     *)
(* record-paid Burrow breaks both premises on purpose. Several address     *)
(* spaces map it (fork shares it, it is never cloned), its pages stay      *)
(* resident when a mapping goes (the next sharer still reads them), and    *)
(* its free CAN refund -- the record names who pays. This module is that   *)
(* second law, written where a model checker can break it:                 *)
(*                                                                         *)
(*   RecordIsResidency  the record's count is the resident pages, plus the *)
(*                      pages a faulter has charged and not yet installed. *)
(*   ChargeExact        every live space's page_count share is exactly     *)
(*                      what it owes: the payer, the record; every other   *)
(*                      sharer, one page per leaf it has installed. The    *)
(*                      payer's own leaves are free (its mint paid).       *)
(*   NoFreedLeaf        no leaf names a page that is gone (I-7, I-13): a   *)
(*                      page leaves the Burrow only when the Burrow does,  *)
(*                      and the Burrow goes only when nothing maps it.     *)
(*   PayerIsLive        the record never names a dead space.               *)
(*   DeadUntouched      nothing charges or refunds a space after it died.  *)
(*   LapseOnlyAtDeath   the record is unpaid only after every space that   *)
(*                      paid it has died (vote 119: the payer pays until   *)
(*                      its death, then the next minting faulter takes the *)
(*                      residency). The gap between is the vote's own.     *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   ONE Burrow, Slots its pages, Spaces the address spaces. The creator   *)
(*   maps it whole and pays from the start; every other space comes from   *)
(*   a Fork, which maps the slots its parent maps -- the same Burrow, no   *)
(*   copy. Nothing ever maps a slot again once it was unmapped: such a     *)
(*   Burrow has no handle and is never extended (anon) or re-attached      *)
(*   (an image's next exec takes a fresh private image).                   *)
(*                                                                         *)
(*   A space's own steps are serialized by its as->lock, so a space has    *)
(*   at most one fault in flight, and its other steps wait for it. The     *)
(*   mint is two steps because the record and the slot are under two       *)
(*   different locks taken in turn (g_payer_lock, then v->lock), and       *)
(*   another SPACE may act between them: Charge (the mint charge and, if   *)
(*   it made this space the payer, the adopt), then Install (win the slot, *)
(*   or lose it to a peer and refund the mint). The leaf goes in at the    *)
(*   Install either way, on the winner's page.                             *)
(*                                                                         *)
(*   flagged[sp] is the payer flag on sp's mappings of the Burrow (every   *)
(*   one of them: a split carries it, the insert while paying sets it, the *)
(*   adopt sets it on all). It decides whether a leaf is charged, and the  *)
(*   invariant checks that decision against the truth (payer = sp).        *)
(*                                                                         *)
(*   Death is one step: the drain (which may free the Burrow, refunding    *)
(*   the dying space itself while it still exists), then the lapse. The    *)
(*   order against the descriptor's free is a lifetime property, below     *)
(*   this model (the host fixture tools/host-tests/addrspace-lifetime.c    *)
(*   checks it). The cap, exemption, node pages and the pool are below it  *)
(*   too: they bound HOW MUCH is charged, never TO WHOM.                   *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation):                         *)
(*                                                                         *)
(*   payer.cfg                          all flags FALSE -- all hold.       *)
(*   payer_buggy_handover_no_owed.cfg   BUGGY_HANDOVER_NO_OWED -- the      *)
(*       faulter that takes over a lapsed record pays only its own page,   *)
(*       not the residency its dead predecessor paid. ChargeExact.         *)
(*   payer_buggy_no_adopt.cfg           BUGGY_NO_ADOPT -- the new payer    *)
(*       keeps its leaves charged: each such page is paid twice.           *)
(*       ChargeExact.                                                      *)
(*   payer_buggy_fork_copies_payer.cfg  BUGGY_FORK_COPIES_PAYER -- the     *)
(*       child's mapping carries the parent's payer flag, so its leaves    *)
(*       are free though nobody pays them. ChargeExact.                    *)
(*   payer_buggy_loser_keeps_charge.cfg BUGGY_LOSER_KEEPS_CHARGE -- the    *)
(*       faulter that loses the install race keeps its mint: the payer     *)
(*       pays for a page that does not exist. RecordIsResidency, with      *)
(*       ChargeExact HOLDING (the payer pays the record exactly; the       *)
(*       record is what is wrong).                                         *)
(*   payer_buggy_unmap_releases.cfg     BUGGY_UNMAP_RELEASES -- an unmap   *)
(*       releases the slot as a private lazy mapping's detach does, with   *)
(*       the accounting kept exact (the record and the payer refunded).    *)
(*       NoFreedLeaf, with every accounting law HOLDING: a page another    *)
(*       sharer still maps is freed, and no counter can see it.            *)
(*   payer_buggy_no_lapse.cfg           BUGGY_NO_LAPSE -- death leaves the *)
(*       record naming the dead space. PayerIsLive.                        *)
(*   payer_buggy_unmap_lapses.cfg       BUGGY_UNMAP_LAPSES -- the payer's  *)
(*       last unmap settles the record as an eager region's does when it   *)
(*       survives only in another Proc (#131): refunded and lapsed while   *)
(*       the payer lives. LapseOnlyAtDeath, with ChargeExact HOLDING.      *)
(*                                                                         *)
(* Maps to the code (specs/SPEC-TO-CODE.md, "payer.tla").                  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Spaces,                     \* the address spaces (>= 2)
    Creator,                    \* the space that mints the Burrow and pays first
    Slots,                      \* the Burrow's pages (>= 1; never 0, the no-fault mark)
    BUGGY_HANDOVER_NO_OWED,     \* BOOLEAN
    BUGGY_NO_ADOPT,             \* BOOLEAN
    BUGGY_FORK_COPIES_PAYER,    \* BOOLEAN
    BUGGY_LOSER_KEEPS_CHARGE,   \* BOOLEAN
    BUGGY_UNMAP_RELEASES,       \* BOOLEAN
    BUGGY_NO_LAPSE,             \* BOOLEAN
    BUGGY_UNMAP_LAPSES          \* BOOLEAN

ASSUME Cardinality(Spaces) >= 2
ASSUME Creator \in Spaces
ASSUME Cardinality(Slots) >= 1 /\ 0 \notin Slots
ASSUME BUGGY_HANDOVER_NO_OWED \in BOOLEAN
ASSUME BUGGY_NO_ADOPT \in BOOLEAN
ASSUME BUGGY_FORK_COPIES_PAYER \in BOOLEAN
ASSUME BUGGY_LOSER_KEEPS_CHARGE \in BOOLEAN
ASSUME BUGGY_UNMAP_RELEASES \in BOOLEAN
ASSUME BUGGY_NO_LAPSE \in BOOLEAN
ASSUME BUGGY_UNMAP_LAPSES \in BOOLEAN

None == "none"                  \* the lapsed record's payer

VARIABLES
    st,         \* [Spaces -> {"unborn", "alive", "dead"}]
    maps,       \* [Spaces -> SUBSET Slots]: the slots a mapping of sp covers
    leaf,       \* [Spaces -> SUBSET Slots]: the slots sp has a leaf for
    flagged,    \* [Spaces -> BOOLEAN]: sp's mappings carry the payer flag
    inflight,   \* [Spaces -> Slots \cup {0}]: the slot sp has charged and not installed
    resident,   \* SUBSET Slots: the slots that hold a page
    live,       \* BOOLEAN: the Burrow exists
    payer,      \* Spaces \cup {None}: the record's payer
    record,     \* Nat: the record's count
    paidonce,   \* SUBSET Spaces (ghost): every space that has been the payer
    pc,         \* [Spaces -> Nat]: sp's page_count share for this Burrow
    frozen      \* [Spaces -> Nat] (ghost): pc at death

vars == <<st, maps, leaf, flagged, inflight, resident, live, payer, record,
          paidonce, pc, frozen>>

TypeOk ==
    /\ st \in [Spaces -> {"unborn", "alive", "dead"}]
    /\ maps \in [Spaces -> SUBSET Slots]
    /\ leaf \in [Spaces -> SUBSET Slots]
    /\ flagged \in [Spaces -> BOOLEAN]
    /\ inflight \in [Spaces -> Slots \cup {0}]
    /\ resident \in SUBSET Slots
    /\ live \in BOOLEAN
    /\ payer \in Spaces \cup {None}
    /\ record \in Nat
    /\ paidonce \in SUBSET Spaces
    /\ pc \in [Spaces -> Nat]
    /\ frozen \in [Spaces -> Nat]

Init ==
    /\ st = [sp \in Spaces |-> IF sp = Creator THEN "alive" ELSE "unborn"]
    /\ maps = [sp \in Spaces |-> IF sp = Creator THEN Slots ELSE {}]
    /\ leaf = [sp \in Spaces |-> {}]
    /\ flagged = [sp \in Spaces |-> sp = Creator]
    /\ inflight = [sp \in Spaces |-> 0]
    /\ resident = {}
    /\ live = TRUE
    /\ payer = Creator
    /\ record = 0
    /\ paidonce = {Creator}
    /\ pc = [sp \in Spaces |-> 0]
    /\ frozen = [sp \in Spaces |-> 0]

Idle(sp) == st[sp] = "alive" /\ inflight[sp] = 0

(* What a leaf costs the space that installs or clears it.                 *)
LeafCost(f) == IF f THEN 0 ELSE 1

(***************************************************************************)
(* Fork(p, c) -- the child maps the slots its parent maps, the same        *)
(* Burrow, with no leaf yet (clone_one_vma's shared arm). The payer flag  *)
(* is NOT inherited: the child pays a leaf for every page it touches.      *)
(***************************************************************************)
Fork(p, c) ==
    /\ Idle(p) /\ st[c] = "unborn" /\ maps[p] # {}
    /\ st' = [st EXCEPT ![c] = "alive"]
    /\ maps' = [maps EXCEPT ![c] = maps[p]]
    /\ flagged' = [flagged EXCEPT ![c] = BUGGY_FORK_COPIES_PAYER /\ flagged[p]]
    /\ UNCHANGED <<leaf, inflight, resident, live, payer, record, paidonce, pc, frozen>>

(* The adopt (vma_payer_adopt_in): a space that has just become the payer   *)
(* turns its mappings into payer mappings -- each charged leaf uninstalled  *)
(* and refunded -- before any leaf of its own is decided. Returns the new   *)
(* <<leaf, flagged, pc>> for sp.                                            *)
Adopt(sp, newpayer, l, f, c) ==
    IF newpayer = sp /\ ~f[sp] /\ ~BUGGY_NO_ADOPT
    THEN <<[l EXCEPT ![sp] = {}],
           [f EXCEPT ![sp] = TRUE],
           [c EXCEPT ![sp] = @ - Cardinality(l[sp])]>>
    ELSE <<l, f, c>>

(***************************************************************************)
(* Charge(sp, s) -- the mint charge: a fault of sp on a slot that holds no *)
(* page. The payer is charged one page. A lapsed record makes sp the payer *)
(* and it pays the whole residency first (owed = the record). Then the     *)
(* adopt, if this charge made sp the payer.                                *)
(***************************************************************************)
Charge(sp, s) ==
    /\ Idle(sp) /\ live /\ s \in maps[sp] /\ s \notin resident
    /\ LET handover == payer = None
           newpayer == IF handover THEN sp ELSE payer
           owed     == IF handover /\ ~BUGGY_HANDOVER_NO_OWED THEN record ELSE 0
           c1       == [pc EXCEPT ![newpayer] = @ + owed + 1]
           a        == Adopt(sp, newpayer, leaf, flagged, c1)
       IN /\ payer' = newpayer
          /\ paidonce' = paidonce \cup {newpayer}
          /\ record' = record + 1
          /\ leaf' = a[1]
          /\ flagged' = a[2]
          /\ pc' = a[3]
          /\ inflight' = [inflight EXCEPT ![sp] = s]
    /\ UNCHANGED <<st, maps, resident, live, frozen>>

(***************************************************************************)
(* Install(sp) -- the slot install-once (pagemap_install). The winner's    *)
(* page becomes resident; a loser frees its own page and refunds the mint  *)
(* to whoever pays the record NOW (a death or a hand-over may have moved   *)
(* it since the charge). Either way the leaf goes in, on the resident page,*)
(* charged to sp unless its mappings are payer mappings.                   *)
(***************************************************************************)
Install(sp) ==
    /\ st[sp] = "alive" /\ inflight[sp] # 0
    /\ LET s      == inflight[sp]
           lost   == s \in resident
           refund == lost /\ ~BUGGY_LOSER_KEEPS_CHARGE
           c1     == IF refund /\ payer # None
                     THEN [pc EXCEPT ![payer] = @ - 1] ELSE pc
       IN /\ resident' = resident \cup {s}
          /\ record' = IF refund THEN record - 1 ELSE record
          /\ leaf' = [leaf EXCEPT ![sp] = @ \cup {s}]
          /\ pc' = [c1 EXCEPT ![sp] = @ + LeafCost(flagged[sp])]
          /\ inflight' = [inflight EXCEPT ![sp] = 0]
    /\ UNCHANGED <<st, maps, flagged, live, payer, paidonce, frozen>>

(***************************************************************************)
(* Hit(sp, s) -- a fault on a resident page: the leaf alone. A payer whose *)
(* mappings are not yet payer mappings adopts first (file_payer_adopt_if_due).*)
(***************************************************************************)
Hit(sp, s) ==
    /\ Idle(sp) /\ s \in maps[sp] /\ s \in resident /\ s \notin leaf[sp]
    /\ LET a == Adopt(sp, payer, leaf, flagged, pc)
       IN /\ leaf' = [a[1] EXCEPT ![sp] = @ \cup {s}]
          /\ flagged' = a[2]
          /\ pc' = [a[3] EXCEPT ![sp] = @ + LeafCost(a[2][sp])]
    /\ UNCHANGED <<st, maps, inflight, resident, live, payer, record, paidonce, frozen>>

(***************************************************************************)
(* Uninstall(sp, s) -- the leaf goes, the page stays: MADV_DONTNEED on a   *)
(* shared mapping, a protect down to none. Refunded unless a payer leaf.   *)
(***************************************************************************)
Uninstall(sp, s) ==
    /\ Idle(sp) /\ s \in leaf[sp]
    /\ leaf' = [leaf EXCEPT ![sp] = @ \ {s}]
    /\ pc' = [pc EXCEPT ![sp] = @ - LeafCost(flagged[sp])]
    /\ UNCHANGED <<st, maps, flagged, inflight, resident, live, payer, record,
                   paidonce, frozen>>

(* The Burrow's free, once nothing maps it: every page goes, and the record *)
(* is refunded whole to its payer, if it has one (burrow_payer_release).    *)
(* Applied after the unmap or the drain that removed the last mapping.      *)
FreeOrKeep(m, res, rec, pay, c) ==
    IF \E t \in Spaces : m[t] # {}
    THEN <<TRUE, res, rec, pay, c>>
    ELSE <<FALSE, {}, 0, None,
           IF pay # None THEN [c EXCEPT ![pay] = @ - rec] ELSE c>>

(***************************************************************************)
(* Unmap(sp, s) -- munmap / a range detach over one slot of sp's mapping:  *)
(* the leaf is cleared (refunded unless a payer leaf) and the slot is no   *)
(* longer mapped by sp. The PAGE STAYS: another sharer may read it, and    *)
(* the record goes on paying for it. The last mapping anywhere frees the   *)
(* Burrow.                                                                 *)
(*                                                                         *)
(* BUGGY_UNMAP_RELEASES releases the slot as a private detach does, and    *)
(* keeps the books exact while doing it (the record and the payer          *)
(* refunded). BUGGY_UNMAP_LAPSES settles the record when the payer's last  *)
(* mapping goes and the Burrow lives on in another space.                  *)
(***************************************************************************)
Unmap(sp, s) ==
    /\ Idle(sp) /\ s \in maps[sp]
    /\ LET c0   == [pc EXCEPT ![sp] = @ - (IF s \in leaf[sp]
                                           THEN LeafCost(flagged[sp]) ELSE 0)]
           m1   == [maps EXCEPT ![sp] = @ \ {s}]
           rel  == BUGGY_UNMAP_RELEASES /\ s \in resident
           res1 == IF rel THEN resident \ {s} ELSE resident
           rec1 == IF rel THEN record - 1 ELSE record
           c1   == IF rel /\ payer # None THEN [c0 EXCEPT ![payer] = @ - 1] ELSE c0
           lapse == BUGGY_UNMAP_LAPSES /\ payer = sp /\ m1[sp] = {}
           pay2 == IF lapse THEN None ELSE payer
           c2   == IF lapse THEN [c1 EXCEPT ![sp] = @ - rec1] ELSE c1
           fr   == FreeOrKeep(m1, res1, rec1, pay2, c2)
       IN /\ maps' = m1
          /\ leaf' = [leaf EXCEPT ![sp] = @ \ {s}]
          /\ live' = fr[1]
          /\ resident' = fr[2]
          /\ record' = fr[3]
          /\ payer' = fr[4]
          /\ pc' = fr[5]
    /\ UNCHANGED <<st, flagged, inflight, paidonce, frozen>>

(***************************************************************************)
(* Die(sp) -- the address space's teardown: vma_drain_in drops every       *)
(* mapping (the last one anywhere frees the Burrow, refunding its payer --  *)
(* possibly sp itself, which still exists), then burrow_payers_lapse: the  *)
(* record stops naming sp and keeps its count for the next minting         *)
(* faulter. pc[sp] dies with the space.                                    *)
(***************************************************************************)
Die(sp) ==
    /\ Idle(sp)
    /\ LET m1  == [maps EXCEPT ![sp] = {}]
           fr  == FreeOrKeep(m1, resident, record, payer, pc)
           pay == IF fr[4] = sp /\ ~BUGGY_NO_LAPSE THEN None ELSE fr[4]
       IN /\ st' = [st EXCEPT ![sp] = "dead"]
          /\ maps' = m1
          /\ leaf' = [leaf EXCEPT ![sp] = {}]
          /\ live' = fr[1]
          /\ resident' = fr[2]
          /\ record' = fr[3]
          /\ payer' = pay
          /\ pc' = fr[5]
          /\ frozen' = [frozen EXCEPT ![sp] = fr[5][sp]]
    /\ UNCHANGED <<flagged, inflight, paidonce>>

Next ==
    \/ \E p, c \in Spaces : Fork(p, c)
    \/ \E sp \in Spaces, s \in Slots : Charge(sp, s)
    \/ \E sp \in Spaces : Install(sp)
    \/ \E sp \in Spaces, s \in Slots : Hit(sp, s)
    \/ \E sp \in Spaces, s \in Slots : Uninstall(sp, s)
    \/ \E sp \in Spaces, s \in Slots : Unmap(sp, s)
    \/ \E sp \in Spaces : Die(sp)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

InFlight == Cardinality({sp \in Spaces : inflight[sp] # 0})

RecordIsResidency ==
    IF live THEN record = Cardinality(resident) + InFlight
            ELSE record = 0 /\ resident = {}

Owed(sp) == IF payer = sp THEN record ELSE Cardinality(leaf[sp])

ChargeExact == \A sp \in Spaces : st[sp] = "alive" => pc[sp] = Owed(sp)

NoFreedLeaf == \A sp \in Spaces : leaf[sp] \subseteq resident

PayerIsLive == payer # None => st[payer] = "alive"

DeadUntouched == \A sp \in Spaces : st[sp] = "dead" => pc[sp] = frozen[sp]

LapseOnlyAtDeath == (live /\ payer = None) => \A sp \in paidonce : st[sp] = "dead"

(* A leaf needs a mapping, a mapping needs a live space and a live Burrow. *)
Shape ==
    /\ \A sp \in Spaces : leaf[sp] \subseteq maps[sp]
    /\ \A sp \in Spaces : st[sp] # "alive" => maps[sp] = {} /\ inflight[sp] = 0
    /\ \A sp \in Spaces : inflight[sp] # 0 => inflight[sp] \in maps[sp]
    /\ live <=> \E sp \in Spaces : maps[sp] # {}

Invariants ==
    /\ TypeOk
    /\ Shape
    /\ RecordIsResidency
    /\ ChargeExact
    /\ NoFreedLeaf
    /\ PayerIsLive
    /\ DeadUntouched
    /\ LapseOnlyAtDeath

====
