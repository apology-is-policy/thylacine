---- MODULE cow ----
(***************************************************************************)
(* Thylacine copy-on-write address-space sharing (LINEAGE L-4; ARCH 28      *)
(* I-44). Spec-first is RE-ENABLED for this surface (LINEAGE 6): the COW    *)
(* break racing a concurrent break, a sharer's exit, and the vfork release  *)
(* is exactly the subtle SMP class machine-checked exploration catches and  *)
(* tests do not -- the same argument that re-enabled asid.tla and           *)
(* death_wake.tla. This model lands BEFORE the L-4 implementation.          *)
(*                                                                         *)
(* WHAT THIS MODELS. One shared anonymous page reached by N sharers (the    *)
(* address spaces a fork produced), each mapping it READ-ONLY. A write      *)
(* faults and the fault arm BREAKS the share:                              *)
(*                                                                         *)
(*   atomically, under the COW lock:                                        *)
(*     if (share == 1)  -> take the page IN PLACE, re-install writable      *)
(*                         (no copy: the common case after one side execs)  *)
(*     else             -> pin, copy out, then drop the share and install   *)
(*                                                                          *)
(*   That lock is GLOBAL, not per-Burrow: two sharers of a page hold        *)
(*   DIFFERENT Burrow locks, so no per-Burrow lock can serialise the        *)
(*   decide. What this model requires is only that drop-decide-act be ONE   *)
(*   step -- exactly what BUGGY_BREAK_UNLOCKED splits -- and a global leaf  *)
(*   lock gives it. Plan 9 serialises Page.ref under palloc.lock likewise.  *)
(*                                                                         *)
(* The count is PER-PAGE: a new struct page.cow_share field (taking the     *)
(* free _pad, so sizeof stays 48), NOT the double-booked refcount --        *)
(* LINEAGE 2.8 measures why THAT field is unusable (written per-block-head, *)
(* so tails are stale; already SLUB's inuse count), and 5.4's L-4b          *)
(* correction why a per-slot array cannot carry the count AT ALL: after a   *)
(* break the slot and the page the count describes diverge, and the free    *)
(* decision corrupts even though take-in-place survives. Freeing a shared   *)
(* page needs to know how many holders remain, and that is a fact about the *)
(* PAGE. A fork clones the Burrow per address space (Plan 9 dupseg: same    *)
(* pages, its own filepages[]), so the Sharers below are those cloned       *)
(* Burrows and `share` is their common page's cow_share.                    *)
(*                                                                         *)
(* ATOMICITY IS MODELED BY STEP GRANULARITY, the standard TLA+ idiom: a     *)
(* sequence performed under one lock hold is ONE action, and the buggy      *)
(* variants split it into two so a peer can interleave. An explicit mutex   *)
(* variable would add state without adding reachable interleavings here,    *)
(* because every critical section in this protocol is straight-line.        *)
(*                                                                         *)
(* THE THREE BUG CLASSES, one flag each so every counterexample names its   *)
(* own mechanism (at most one flag per cfg -- ASSUMEd below):               *)
(*                                                                         *)
(*  1. BUGGY_BREAK_UNLOCKED -- the drop/decide/act sequence is NOT atomic.  *)
(*     Two sharers each drop their share and THEN look, so both can read    *)
(*     zero and both conclude "I am the last sharer" -> both take the SAME  *)
(*     page in place, both writable. One address space's writes then land   *)
(*     in another's: the aliasing I-44 forbids. -> NoAliasedWritable.       *)
(*                                                                         *)
(*  2. BUGGY_TEARDOWN_NO_PIN -- the breaker drops its share BEFORE copying  *)
(*     instead of after, and takes no pin across the copy. A concurrent     *)
(*     exit then drives the count to zero and frees the pristine page while *)
(*     the breaker is still reading it. -> NoUseAfterFree. This is the      *)
(*     break-vs-teardown race, and it is why the correct path pins across   *)
(*     the copy rather than trusting the count to stay put.                 *)
(*                                                                         *)
(*  3. BUGGY_VFORK_OBSERVE_BEFORE_PARK -- the L-3c-2 suspend, modeled       *)
(*     retroactively (the death_wake precedent: a shipped mechanism on the  *)
(*     death lineage earns a model). The parent checks "has the child       *)
(*     stopped sharing my address space?" OUTSIDE the lock and then parks,  *)
(*     so a release landing in the window is lost and the parent parks      *)
(*     forever. -> the EventuallyReleased liveness witness. The correct     *)
(*     path checks and parks in one atomic step -- the same                 *)
(*     register-then-observe death_wake.tla proves for the death wake.      *)
(*                                                                         *)
(* WHAT THIS DELIBERATELY DOES NOT MODEL. The intra-address-space           *)
(* install-once race (two THREADS of ONE address space faulting one page)   *)
(* is the already-audited REVENANT / lazy-arm shape -- loser frees, winner  *)
(* installs -- and sits below this abstraction, which treats a sharer as    *)
(* one agent per address space. L-4b reuses that arm rather than inventing  *)
(* a second one.                                                            *)
(***************************************************************************)

(***************************************************************************)
(* B-1a EXTENSION (2026-09-23; ARCH 6.5 "The permission ceiling"; I-12 +    *)
(* I-44). `burrow_protect` moves a VMA's prot among rw / ro / none under    *)
(* its mint-time ceiling, and that reaches this model in three places, each *)
(* with its own bug flag (4-6, the same one-flag-per-cfg rule):             *)
(*                                                                         *)
(*  4. BUGGY_PROTECT_KEEPS_PTE -- a protect lowers the VMA's prot but       *)
(*     leaves the writable PTE a break installed, so the hardware keeps     *)
(*     granting writes the VMA no longer permits. The correct ProtectDown   *)
(*     is the D-3b rule as ONE step under the address-space lock: uninstall *)
(*     the range, then change the prot. -> NoWritablePteBeyondProt.         *)
(*                                                                         *)
(*  5. BUGGY_FAULT_IGNORES_PROT -- the write-fault arm enters the break     *)
(*     without first enforcing the VMA's prot (fault.c's step 2 skipped). A *)
(*     write to an ro/none COW mapping then allocates and copies for an     *)
(*     access that had to be refused. -> BreakOnlyWhenWritable.             *)
(*                                                                         *)
(*  6. BUGGY_CLONE_PER_PIECE -- a fork mints one clone Burrow PER VMA, and  *)
(*     a clone takes a share on EVERY resident page of its source. Once a   *)
(*     protect (or a MAP_FIXED window, D-3b) has split one Burrow into k    *)
(*     VMA pieces, the child holds k shares of every page while being ONE   *)
(*     holder: the count lies from the fork onwards -- the child is charged *)
(*     k x resident, and the parent can never take a page in place while    *)
(*     the phantom clones live. -> ShareIsHolderCount, violated by the      *)
(*     INITIAL state, which is exactly where this bug is.                   *)
(*                                                                         *)
(* prot[s] is the VMA's declared prot. ptew[s] says whether s has a         *)
(* WRITABLE PTE installed: a fork uninstalls every writable PTE, so all     *)
(* sides start FALSE and only a break or a re-fault installs one. A write   *)
(* under a lowered prot is refused at the fault boundary BEFORE the Burrow  *)
(* is resolved -- it is a snare death, i.e. Exit, never a break -- so Fault *)
(* is guarded on prot = rw. Protect and the break serialise on the          *)
(* address-space lock the fault holds across the whole demand_page, so      *)
(* ProtectDown never interleaves an in-flight break (its pc guard). The     *)
(* CEILING itself is a pure per-call comparison with no interleaving and is *)
(* deliberately not modeled; the kernel tests are its witness. The FILE     *)
(* arm is the one fault whose admission and install span an UNLOCK (the    *)
(* page-in sleeps); it is beneath this model (above) and the kernel re-runs *)
(* the admission after the sleep (fault.c::file_fault_still_admitted; the   *)
(* B-1a audit's F1), witnessed by the two file_pagein_racing_protect tests. *)
(*                                                                         *)
(* ADDITIVE BY MEASUREMENT: every new action is gated on ALLOW_PROTECT, and *)
(* with it FALSE the four pre-existing cfgs reproduce their counts exactly  *)
(* (cow 580 / break 211 / teardown 124 / vfork 231; specs/check-cow.sh pins *)
(* them). There ptew is a function of pc and prot is constant, so the       *)
(* count is preserved by construction as well as by measurement.            *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Sharers,                          \* address spaces sharing the page (>= 2)
    BUGGY_BREAK_UNLOCKED,             \* 1: drop/decide not atomic
    BUGGY_TEARDOWN_NO_PIN,            \* 2: share dropped before the copy, no pin
    BUGGY_VFORK_OBSERVE_BEFORE_PARK,  \* 3: check-then-park, unlocked
    ALLOW_PROTECT,                    \* B-1a: the protect actions exist at all
    Pieces,                           \* B-1a: VMA pieces one Burrow is split into
    BUGGY_PROTECT_KEEPS_PTE,          \* 4: prot lowered, writable PTE kept
    BUGGY_FAULT_IGNORES_PROT,         \* 5: the break entered before the prot check
    BUGGY_CLONE_PER_PIECE             \* 6: one clone Burrow per VMA piece

ASSUME Cardinality(Sharers) >= 2
ASSUME Pieces \in Nat /\ Pieces >= 1

(* At most one bug is enabled at a time, so a counterexample is unambiguous
   about which mechanism produced it. Counted arithmetically, NOT as
   Cardinality of a set of the three flags -- a set collapses duplicates, so
   {TRUE, TRUE, FALSE} has cardinality 2 and would pass a <= 1 test with two
   bugs enabled. The flags are values, not identities. *)
ASSUME (IF BUGGY_BREAK_UNLOCKED           THEN 1 ELSE 0)
     + (IF BUGGY_TEARDOWN_NO_PIN          THEN 1 ELSE 0)
     + (IF BUGGY_VFORK_OBSERVE_BEFORE_PARK THEN 1 ELSE 0)
     + (IF BUGGY_PROTECT_KEEPS_PTE         THEN 1 ELSE 0)
     + (IF BUGGY_FAULT_IGNORES_PROT        THEN 1 ELSE 0)
     + (IF BUGGY_CLONE_PER_PIECE           THEN 1 ELSE 0) <= 1

(* Sharer program counters:
     "shared"  -- mapped READ-ONLY on the pristine page (the post-fork state)
     "acq"     -- write-faulted; about to break
     "dropped" -- BUGGY_BREAK_UNLOCKED only: share already dropped, count not
                  yet inspected (the window in which two can both read zero)
     "copying" -- allocated a private page, copying OUT of the pristine one
     "private" -- broken: owns a private writable page (no longer a sharer)
     "inplace" -- took the pristine page itself, writable (last-sharer path)
     "gone"    -- exited without ever breaking                               *)
PCs == {"shared", "acq", "dropped", "copying", "private", "inplace", "gone"}

(* The vfork parent's sub-machine (L-3c-2). *)
VPCs == {"forked", "checked", "parked", "resumed"}

(* A VMA's declared prot (B-1a). X is never a protect target (ARCH 6.5), so an
   anonymous mapping's whole ladder is these three; "none" keeps the Burrow and
   its pages (a guard is a range sealed at none, not an unmapping). *)
Prots == {"rw", "ro", "none"}
Rank(q) == CASE q = "rw" -> 2 [] q = "ro" -> 1 [] q = "none" -> 0

VARIABLES
    pc,         \* [Sharers -> PCs]
    share,      \* Nat: the pristine page's share count
    pin,        \* Nat: breakers holding the page across a copy
    nfree,      \* Nat: times the pristine page was returned to the buddy
    vpc,        \* VPCs: the vfork parent
    vreleased,  \* BOOLEAN: the vfork child stopped sharing (exec or exit)
    prot,       \* [Sharers -> Prots]: each sharer's VMA prot (B-1a)
    ptew        \* [Sharers -> BOOLEAN]: a WRITABLE PTE is installed (B-1a)

vars == <<pc, share, pin, nfree, vpc, vreleased, prot, ptew>>

(* How many shares of the page a sharer's fork left it holding. The correct
   fork holds exactly one per address space. Under bug 6 every non-parent
   sharer (a clone) holds one per VMA piece: the clone is minted per piece and
   each mint walks the whole source Burrow. Which sharer is the parent is
   immaterial to a symmetric model, so CHOOSE. *)
Parent    == CHOOSE s \in Sharers : TRUE
Held(s)   == IF BUGGY_CLONE_PER_PIECE /\ s # Parent THEN Pieces ELSE 1
InitShare == IF BUGGY_CLONE_PER_PIECE
               THEN 1 + Pieces * (Cardinality(Sharers) - 1)
               ELSE Cardinality(Sharers)

(* TRUE when the protocol drops the share BEFORE the copy rather than after. *)
EarlyDrop == BUGGY_TEARDOWN_NO_PIN \/ BUGGY_BREAK_UNLOCKED

(* TRUE when a pin is held across the copy. Only bug 2 removes it. *)
TakesPin == ~BUGGY_TEARDOWN_NO_PIN

(* Sharers that still REFERENCE the pristine page: mapped on it, mid-break and
   still reading it, or holding it in place. *)
Referencing == {s \in Sharers :
                  pc[s] \in {"shared", "acq", "dropped", "copying", "inplace"}}

TypeOk ==
    /\ pc        \in [Sharers -> PCs]
    /\ share     \in Nat
    /\ pin       \in Nat
    /\ nfree     \in Nat
    /\ vpc       \in VPCs
    /\ vreleased \in BOOLEAN
    /\ prot      \in [Sharers -> Prots]
    /\ ptew      \in [Sharers -> BOOLEAN]

Init ==
    /\ pc        = [s \in Sharers |-> "shared"]
    /\ share     = InitShare
    /\ pin       = 0
    /\ nfree     = 0
    /\ vpc       = "forked"
    /\ vreleased = FALSE
    /\ prot      = [s \in Sharers |-> "rw"]
    /\ ptew      = [s \in Sharers |-> FALSE]   \* the fork uninstalled them all

vunchanged    == UNCHANGED <<vpc, vreleased>>
protunchanged == UNCHANGED <<prot, ptew>>
pgunchanged   == UNCHANGED <<pc, share, pin, nfree, prot, ptew>>
pgunchanged_but_prot == UNCHANGED <<pc, share, pin, nfree>>

(***************************************************************************)
(* A sharer write-faults on its read-only mapping. The VMA's prot is         *)
(* enforced BEFORE the Burrow is resolved (fault.c step 2 before step 3): a *)
(* write under ro/none never gets here -- it is refused, and the refusal is  *)
(* a snare death (Exit). Bug 5 drops that guard.                             *)
(***************************************************************************)
Fault(s) ==
    /\ pc[s] = "shared"
    /\ (prot[s] = "rw" \/ BUGGY_FAULT_IGNORES_PROT)
    /\ pc' = [pc EXCEPT ![s] = "acq"]
    /\ UNCHANGED <<share, pin, nfree>>
    /\ vunchanged
    /\ protunchanged

(***************************************************************************)
(* CORRECT (and bug 2): the whole decide runs under the Burrow lock, so it  *)
(* is ONE step. Last sharer -> take in place, leaving the count at 1 so     *)
(* nothing frees the page underneath us. Otherwise -> pin and copy; the     *)
(* share is dropped only when the copy is DONE (BreakFinish).               *)
(*                                                                         *)
(* Bug 2 inverts exactly that ordering: drop first, take no pin.            *)
(***************************************************************************)
DecideLocked(s) ==
    /\ ~BUGGY_BREAK_UNLOCKED
    /\ pc[s] = "acq"
    /\ IF share = 1
         THEN /\ pc'   = [pc EXCEPT ![s] = "inplace"]
              /\ ptew' = [ptew EXCEPT ![s] = TRUE]   \* re-installed writable
              /\ UNCHANGED <<share, pin, nfree>>
         ELSE /\ pc'    = [pc EXCEPT ![s] = "copying"]
              /\ pin'   = IF TakesPin THEN pin + 1 ELSE pin
              /\ share' = IF BUGGY_TEARDOWN_NO_PIN THEN share - 1 ELSE share
              /\ UNCHANGED <<nfree, ptew>>
    /\ UNCHANGED prot
    /\ vunchanged

(***************************************************************************)
(* Bug 1: no lock. Drop the share first, THEN (as a separate step) look at  *)
(* the count -- so two sharers can both reach zero before either looks.     *)
(***************************************************************************)
DropUnlocked(s) ==
    /\ BUGGY_BREAK_UNLOCKED
    /\ pc[s] = "acq"
    /\ share > 0
    /\ share' = share - 1
    /\ pc'    = [pc EXCEPT ![s] = "dropped"]
    /\ UNCHANGED <<pin, nfree>>
    /\ vunchanged
    /\ protunchanged

LookUnlocked(s) ==
    /\ BUGGY_BREAK_UNLOCKED
    /\ pc[s] = "dropped"
    /\ IF share = 0
         THEN \* "nobody else is left" -- take the page in place
              /\ pc'   = [pc EXCEPT ![s] = "inplace"]
              /\ ptew' = [ptew EXCEPT ![s] = TRUE]
              /\ UNCHANGED <<share, pin, nfree>>
         ELSE /\ pc'  = [pc EXCEPT ![s] = "copying"]
              /\ pin' = pin + 1
              /\ UNCHANGED <<share, nfree, ptew>>
    /\ UNCHANGED prot
    /\ vunchanged

(***************************************************************************)
(* The copy completes: install the private page, drop the pin, and -- on    *)
(* the correct path -- drop the share now that we are off the pristine one. *)
(***************************************************************************)
BreakFinish(s) ==
    /\ pc[s] = "copying"
    /\ pc'    = [pc EXCEPT ![s] = "private"]
    /\ ptew'  = [ptew EXCEPT ![s] = TRUE]     \* the private page, writable
    /\ pin'   = IF TakesPin THEN pin - 1 ELSE pin
    /\ share' = IF EarlyDrop THEN share ELSE share - 1
    /\ UNCHANGED <<nfree, prot>>
    /\ vunchanged

(***************************************************************************)
(* A sharer exits without ever writing: drop what it holds. This is also    *)
(* where a write refused under a lowered prot ends up -- the snare death    *)
(* tears the address space down, and every clone the fork minted for it     *)
(* (one, or Pieces under bug 6) drops its share with it.                    *)
(***************************************************************************)
Exit(s) ==
    /\ pc[s] = "shared"
    /\ share >= Held(s)
    /\ share' = share - Held(s)
    /\ pc'    = [pc EXCEPT ![s] = "gone"]
    /\ UNCHANGED <<pin, nfree>>
    /\ vunchanged
    /\ protunchanged

(***************************************************************************)
(* The pristine page returns to the buddy when the count says nothing maps  *)
(* it and no breaker is pinned on it. This is deliberately the REAL free    *)
(* decision -- it trusts `share` and `pin`, exactly as the implementation   *)
(* will -- so a protocol that lets the count lie gets caught here rather    *)
(* than being papered over by a guard the kernel would not have.            *)
(***************************************************************************)
FreePristine ==
    /\ share = 0
    /\ pin = 0
    /\ nfree = 0
    /\ nfree' = 1
    /\ UNCHANGED <<pc, share, pin>>
    /\ vunchanged
    /\ protunchanged

(***************************************************************************)
(* B-1a: the protect actions (all gated on ALLOW_PROTECT).                  *)
(*                                                                         *)
(* A sharer with an address space (not mid-break: the break holds the same  *)
(* lock, so the two serialise; not gone: no VMA) lowers or raises the VMA's *)
(* prot. Lowering is the D-3b shape in ONE step: uninstall the range, then  *)
(* change the prot -- a writable PTE cannot outlive the permission that     *)
(* justified it. Bug 4 keeps the PTE. Raising installs nothing: the next    *)
(* access faults and the fault arm installs under the NEW prot (Reinstall   *)
(* for a page the sharer already owns; Fault + the break for a shared one). *)
(***************************************************************************)
Protectable(s) == pc[s] \in {"shared", "private", "inplace"}

ProtectDown(s) ==
    /\ ALLOW_PROTECT
    /\ Protectable(s)
    /\ \E q \in Prots :
         /\ Rank(q) < Rank(prot[s])
         /\ prot' = [prot EXCEPT ![s] = q]
    /\ ptew' = [ptew EXCEPT ![s] = IF BUGGY_PROTECT_KEEPS_PTE THEN ptew[s] ELSE FALSE]
    /\ pgunchanged_but_prot
    /\ vunchanged

ProtectUp(s) ==
    /\ ALLOW_PROTECT
    /\ Protectable(s)
    /\ \E q \in Prots :
         /\ Rank(q) > Rank(prot[s])
         /\ prot' = [prot EXCEPT ![s] = q]
    /\ UNCHANGED ptew
    /\ pgunchanged_but_prot
    /\ vunchanged

(* A sharer that already owns its page (private copy, or the pristine page
   taken in place) write-faults after a protect uninstalled its PTE: there is
   nothing to break, the fault arm just re-installs -- writable only because
   the prot says rw. *)
Reinstall(s) ==
    /\ ALLOW_PROTECT
    /\ pc[s] \in {"private", "inplace"}
    /\ ~ptew[s]
    /\ prot[s] = "rw"
    /\ ptew' = [ptew EXCEPT ![s] = TRUE]
    /\ UNCHANGED prot
    /\ pgunchanged_but_prot
    /\ vunchanged

(***************************************************************************)
(* The vfork sub-machine (L-3c-2). The release condition is not a RECORD of *)
(* the release -- it IS the release: "the child no longer maps my space".   *)
(***************************************************************************)
VChildRelease ==
    /\ ~vreleased
    /\ vreleased' = TRUE
    /\ vpc' = IF vpc = "parked" THEN "resumed" ELSE vpc
    /\ pgunchanged

VParentCheck ==
    /\ vpc = "forked"
    /\ IF BUGGY_VFORK_OBSERVE_BEFORE_PARK
         THEN \* observe with no lock held -- the window opens right here
              vpc' = IF vreleased THEN "resumed" ELSE "checked"
         ELSE \* correct: check and park in ONE step, under the lock
              vpc' = IF vreleased THEN "resumed" ELSE "parked"
    /\ UNCHANGED vreleased
    /\ pgunchanged

(* Bug 3 only: park AFTER the unlocked observation. A release that landed in
   between is lost -- the parent parks on an already-released child. *)
VParentParkLate ==
    /\ BUGGY_VFORK_OBSERVE_BEFORE_PARK
    /\ vpc = "checked"
    /\ vpc' = "parked"
    /\ UNCHANGED vreleased
    /\ pgunchanged

Next ==
    \/ \E s \in Sharers :
         \/ Fault(s) \/ DecideLocked(s)
         \/ DropUnlocked(s) \/ LookUnlocked(s)
         \/ BreakFinish(s) \/ Exit(s)
         \/ ProtectDown(s) \/ ProtectUp(s) \/ Reinstall(s)
    \/ FreePristine
    \/ VChildRelease \/ VParentCheck \/ VParentParkLate

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

(* With protect enabled the state graph has cycles (rw -> ro -> rw ...), so
   weak fairness on Next alone admits a run that protects forever and never
   lets the vfork child release -- a spurious liveness counterexample about a
   sub-machine protect does not touch. The vfork release depends on nothing a
   protect does, so it gets its own fairness. The old Spec is left as it was:
   the four pre-existing cfgs are fingerprinted against it. *)
SpecProtect == Spec /\ WF_vars(VChildRelease) /\ WF_vars(VParentCheck)

(***************************************************************************)
(* SAFETY -- I-44.                                                          *)
(***************************************************************************)

(* No page is writable through two mappings at once. "inplace" means writable
   through the PRISTINE page; a "private" sharer is writable only through its
   own copy, which no one else can reach. So at most one in-place taker. *)
NoAliasedWritable ==
    Cardinality({s \in Sharers : pc[s] = "inplace"}) <= 1

(* Nothing reads or holds the pristine page after it has been freed. *)
NoUseAfterFree ==
    (nfree > 0) => (Referencing = {})

(* The page is returned to the buddy at most once. *)
NoDoubleFree == nfree <= 1

Safety ==
    /\ TypeOk
    /\ NoAliasedWritable
    /\ NoUseAfterFree
    /\ NoDoubleFree

(***************************************************************************)
(* SAFETY -- B-1a. Kept OUT of Safety so the pre-existing cfgs check exactly *)
(* what they always did; cow_protect checks all of it.                      *)
(***************************************************************************)

(* The share count is the number of address spaces that hold the page -- not
   the number of clone Burrows a fork happened to mint. Bug 6 breaks this in
   the initial state. *)
ShareIsHolderCount == share = Cardinality(Referencing)

(* No writable PTE beyond the VMA's prot: a protect that lowers the prot took
   the PTE with it (bug 4 keeps it). *)
NoWritablePteBeyondProt == \A s \in Sharers : ptew[s] => prot[s] = "rw"

(* The break is entered only for a write the VMA permits: prot is enforced at
   the fault boundary before the Burrow is resolved (bug 5 skips it). *)
BreakOnlyWhenWritable ==
    \A s \in Sharers : pc[s] \in {"acq", "dropped", "copying"} => prot[s] = "rw"

ProtectSafety ==
    /\ ShareIsHolderCount
    /\ NoWritablePteBeyondProt
    /\ BreakOnlyWhenWritable

(***************************************************************************)
(* LIVENESS -- the vfork parent is always released (L-3c-2's NoStrand).     *)
(***************************************************************************)
EventuallyReleased == <>(vpc = "resumed")

====
