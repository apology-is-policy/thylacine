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
EXTENDS Naturals, FiniteSets

CONSTANTS
    Sharers,                          \* address spaces sharing the page (>= 2)
    BUGGY_BREAK_UNLOCKED,             \* 1: drop/decide not atomic
    BUGGY_TEARDOWN_NO_PIN,            \* 2: share dropped before the copy, no pin
    BUGGY_VFORK_OBSERVE_BEFORE_PARK   \* 3: check-then-park, unlocked

ASSUME Cardinality(Sharers) >= 2

(* At most one bug is enabled at a time, so a counterexample is unambiguous
   about which mechanism produced it. Counted arithmetically, NOT as
   Cardinality of a set of the three flags -- a set collapses duplicates, so
   {TRUE, TRUE, FALSE} has cardinality 2 and would pass a <= 1 test with two
   bugs enabled. The flags are values, not identities. *)
ASSUME (IF BUGGY_BREAK_UNLOCKED           THEN 1 ELSE 0)
     + (IF BUGGY_TEARDOWN_NO_PIN          THEN 1 ELSE 0)
     + (IF BUGGY_VFORK_OBSERVE_BEFORE_PARK THEN 1 ELSE 0) <= 1

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

VARIABLES
    pc,         \* [Sharers -> PCs]
    share,      \* Nat: the pristine page's share count
    pin,        \* Nat: breakers holding the page across a copy
    nfree,      \* Nat: times the pristine page was returned to the buddy
    vpc,        \* VPCs: the vfork parent
    vreleased   \* BOOLEAN: the vfork child stopped sharing (exec or exit)

vars == <<pc, share, pin, nfree, vpc, vreleased>>

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

Init ==
    /\ pc        = [s \in Sharers |-> "shared"]
    /\ share     = Cardinality(Sharers)
    /\ pin       = 0
    /\ nfree     = 0
    /\ vpc       = "forked"
    /\ vreleased = FALSE

vunchanged  == UNCHANGED <<vpc, vreleased>>
pgunchanged == UNCHANGED <<pc, share, pin, nfree>>

(***************************************************************************)
(* A sharer write-faults on its read-only mapping.                          *)
(***************************************************************************)
Fault(s) ==
    /\ pc[s] = "shared"
    /\ pc' = [pc EXCEPT ![s] = "acq"]
    /\ UNCHANGED <<share, pin, nfree>>
    /\ vunchanged

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
         THEN /\ pc' = [pc EXCEPT ![s] = "inplace"]
              /\ UNCHANGED <<share, pin, nfree>>
         ELSE /\ pc'    = [pc EXCEPT ![s] = "copying"]
              /\ pin'   = IF TakesPin THEN pin + 1 ELSE pin
              /\ share' = IF BUGGY_TEARDOWN_NO_PIN THEN share - 1 ELSE share
              /\ UNCHANGED nfree
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

LookUnlocked(s) ==
    /\ BUGGY_BREAK_UNLOCKED
    /\ pc[s] = "dropped"
    /\ IF share = 0
         THEN \* "nobody else is left" -- take the page in place
              /\ pc' = [pc EXCEPT ![s] = "inplace"]
              /\ UNCHANGED <<share, pin, nfree>>
         ELSE /\ pc'  = [pc EXCEPT ![s] = "copying"]
              /\ pin' = pin + 1
              /\ UNCHANGED <<share, nfree>>
    /\ vunchanged

(***************************************************************************)
(* The copy completes: install the private page, drop the pin, and -- on    *)
(* the correct path -- drop the share now that we are off the pristine one. *)
(***************************************************************************)
BreakFinish(s) ==
    /\ pc[s] = "copying"
    /\ pc'    = [pc EXCEPT ![s] = "private"]
    /\ pin'   = IF TakesPin THEN pin - 1 ELSE pin
    /\ share' = IF EarlyDrop THEN share ELSE share - 1
    /\ UNCHANGED nfree
    /\ vunchanged

(***************************************************************************)
(* A sharer exits without ever writing: drop its share.                     *)
(***************************************************************************)
Exit(s) ==
    /\ pc[s] = "shared"
    /\ share > 0
    /\ share' = share - 1
    /\ pc'    = [pc EXCEPT ![s] = "gone"]
    /\ UNCHANGED <<pin, nfree>>
    /\ vunchanged

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
    \/ FreePristine
    \/ VChildRelease \/ VParentCheck \/ VParentParkLate

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

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
(* LIVENESS -- the vfork parent is always released (L-3c-2's NoStrand).     *)
(***************************************************************************)
EventuallyReleased == <>(vpc = "resumed")

====
