---- MODULE asid ----
(***************************************************************************)
(* Thylacine ASID generation-rollover (RW-1 B-F1; ARCH section 6.2.1).      *)
(*                                                                         *)
(* WHAT THIS MODELS: the Linux arm64 rolling-ASID allocator                 *)
(* (arch/arm64/mm/context.c) as Thylacine adopts it. User ASIDs are a       *)
(* recycled CACHE keyed by a global generation counter -- NOT a per-Proc    *)
(* permanent allocation. The prior per-Proc-permanent design extincted the  *)
(* kernel on the 256th concurrent Proc (an unprivileged whole-system DoS,   *)
(* RW-1 B-F1, P1); this design removes exhaustion entirely.                 *)
(*                                                                         *)
(* THE MECHANISM:                                                           *)
(*   - A global `gen` (asid_generation) + the hardware ASID form a per-Proc *)
(*     context_id. A Proc at generation 0 ("never assigned") always misses. *)
(*   - FAST PATH (lockless): if the Proc's stored generation == `gen`, its   *)
(*     ASID is still valid -> publish it into this CPU's active slot and run.*)
(*   - SLOW PATH (new_context, conceptually under g_asid_lock): generation  *)
(*     stale -> claim a free ASID from the bitmap (`amap`). If none is free, *)
(*     ROLL OVER: bump `gen`, reset `amap`, preserve every CPU's active ASID *)
(*     into `reserved` (so a running CPU is never yanked), and set           *)
(*     `flush_pending` for every CPU. The Proc then claims a fresh ASID.     *)
(*   - flush_pending: a CPU does a LOCAL TLB flush at its next slow switch    *)
(*     after a rollover -- the broadcast flush only covers entries present    *)
(*     at flush time; the per-CPU local flush clears what the CPU cached in   *)
(*     the rollover window.                                                  *)
(*                                                                         *)
(* THE OBLIGATION (I-31, ARCH section 28):                                  *)
(*   No two CPUs concurrently run distinct user address spaces that share an *)
(*   ASID -- else the TLB returns a wrong translation and one Proc reads or  *)
(*   writes another's memory. The rollover race (a generation rollover       *)
(*   concurrent with another CPU's context switch reassigning a live ASID)   *)
(*   is the classic, subtle rolling-ASID hazard. This is WHY the surface is  *)
(*   model-first (spec-first re-enabled per the SMP precedent in ARCH 8.4).  *)
(*                                                                         *)
(* INVARIANTS:                                                              *)
(*   NoActiveAlias    -- I-31 core: same active ASID on two CPUs => same     *)
(*                       Proc (address space). [the headline]                *)
(*   NoStaleTLB       -- a running Proc's TLB tag for its ASID is empty or    *)
(*                       itself (the flush_pending obligation).              *)
(*   ActiveClaimed    -- every active ASID is claimed in `amap` (the rollover *)
(*                       reservation obligation; the steal bug breaks this).  *)
(*   CurrentGenClaimed-- a current-generation Proc's ASID is claimed (the    *)
(*                       fast-path soundness premise).                       *)
(*                                                                         *)
(* BUGGY-CONFIG MATRIX (executable documentation per CLAUDE.md spec-first):  *)
(*   asid.cfg                            all BUGGY=FALSE -- TLC GREEN.        *)
(*   asid_buggy_rollover_steals_active   BUGGY_ROLLOVER_STEALS_ACTIVE=TRUE -- *)
(*       rollover resets the bitmap WITHOUT reserving the active ASIDs, so a  *)
(*       later new_context hands out an ASID still live on another CPU ->     *)
(*       ActiveClaimed + NoActiveAlias VIOLATED. [the scripture-required cfg] *)
(*   asid_buggy_fast_no_regen            BUGGY_FAST_NO_REGEN=TRUE -- the fast *)
(*       path reuses a Proc's ASID without re-checking the global generation, *)
(*       so it reuses an ASID a rollover already reassigned -> NoActiveAlias  *)
(*       VIOLATED. (Proves the gen-recheck is load-bearing -- the lockless    *)
(*       fast-path-vs-rollover correctness obligation.)                      *)
(*   asid_buggy_no_flush_pending         BUGGY_NO_FLUSH_PENDING=TRUE -- a     *)
(*       rollover does not set flush_pending for peer CPUs, so a peer reuses  *)
(*       a recycled ASID over stale TLB entries -> NoStaleTLB VIOLATED.       *)
(*   asid_buggy_fast_no_flush_check      BUGGY_FAST_NO_FLUSH_CHECK=TRUE -- the *)
(*       fast path runs without honoring flush_pending (equivalently, without *)
(*       the Linux `old_active_asid != 0` guard), so a peer CPU fast-switches  *)
(*       to a Proc holding a recycled ASID over un-flushed stale TLB entries  *)
(*       -> NoStaleTLB VIOLATED. (This bug was SURFACED BY THE MODEL: the     *)
(*       fast path's correctness rests on TWO guards, generation-match AND     *)
(*       no-pending-flush; omitting the second silently corrupts across a      *)
(*       rollover. The impl MUST carry the `old_active_asid != 0` fast-path    *)
(*       guard -- that is this second guard.)                                  *)
(*                                                                         *)
(* THE TWO FAST-PATH GUARDS (both load-bearing; each has a buggy cfg):        *)
(*   (1) generation-match  -- do not reuse a stale-generation ASID (a rollover *)
(*       may have reassigned it). Buggy omission: asid_buggy_fast_no_regen.    *)
(*   (2) no-pending-flush   -- do not fast-path while flush_pending is set     *)
(*       (the CPU has un-flushed stale TLB entries from before a rollover).    *)
(*       In Linux this is the `old_active_asid != 0` guard: flush_context      *)
(*       zeroes active_asids[all] on rollover, so a peer's next switch finds    *)
(*       active==0, fails the fast-path test, takes the slow path, and there   *)
(*       honors flush_pending. The model abstracts "active_asids[c] was zeroed *)
(*       by the rollover" as "fpend[c] is set" (the two are set/cleared in     *)
(*       lockstep), so ~fpend[c] is the faithful fast-path guard. Buggy        *)
(*       omission: asid_buggy_fast_no_flush_check.                            *)
(*                                                                         *)
(* ATOMIC-GRAIN FIDELITY: the real fast path is a cmpxchg into active_asids  *)
(* and the rollover's flush_context is a per-CPU xchg of the same slot; their *)
(* race is serialized by that single memory location. At TLA+ action grain   *)
(* this collapses faithfully: a fast publish that races a rollover is either  *)
(* (a) observed by the rollover (active != NULL) and RESERVED, or (b) ordered *)
(* after the generation bump, where guard (1) (stale generation) or guard (2) *)
(* (flush_pending set / active zeroed) DISABLES FastSwitch and forces the     *)
(* slow path. BUGGY_FAST_NO_REGEN and BUGGY_FAST_NO_FLUSH_CHECK remove guards  *)
(* (1) and (2) respectively and demonstrate the resulting corruption -- the   *)
(* executable proof that BOTH guards make the lockless fast path safe.        *)
(*                                                                         *)
(* OUT OF MODEL SCOPE (handled by the focused audit, not here): the          *)
(* g_asid_lock vs runqueue-lock ordering (no rq-lock -> g_asid_lock -> rq    *)
(* cycle), the 8-vs-16-bit ASID width (TCR_EL1.AS), and kproc's ASID-0       *)
(* bypass (the hook is gated on pgtable_root != 0 -- kproc never enters the   *)
(* allocator, so it is not a participant in this model).                    *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    CPUs,                            \* cpu ids (>= 2 -- the race needs two)
    Procs,                           \* user address spaces (> |Asids| forces rollover)
    Asids,                           \* hardware user ASIDs (> |CPUs| guarantees a free slot post-rollover)
    MaxGen,                          \* generation bound (finite-model cap; >= 2)
    NULL,                            \* "none" sentinel
    BUGGY_ROLLOVER_STEALS_ACTIVE,    \* BOOLEAN
    BUGGY_FAST_NO_REGEN,             \* BOOLEAN
    BUGGY_NO_FLUSH_PENDING,          \* BOOLEAN
    BUGGY_FAST_NO_FLUSH_CHECK        \* BOOLEAN

ASSUME Cardinality(CPUs) >= 2
ASSUME Cardinality(Asids) >= 1
ASSUME MaxGen \in Nat /\ MaxGen >= 2
ASSUME NULL \notin Procs /\ NULL \notin Asids
ASSUME BUGGY_ROLLOVER_STEALS_ACTIVE \in BOOLEAN
ASSUME BUGGY_FAST_NO_REGEN \in BOOLEAN
ASSUME BUGGY_NO_FLUSH_PENDING \in BOOLEAN
ASSUME BUGGY_FAST_NO_FLUSH_CHECK \in BOOLEAN

Gens == 1..MaxGen

VARIABLES
    gen,         \* global asid_generation, in Gens (starts at 1; Proc gen 0 = "never assigned")
    pgen,        \* [Procs -> 0..MaxGen]      -- the generation half of each Proc's context_id
    pasid,       \* [Procs -> Asids \cup {NULL}] -- the ASID half
    active,      \* [CPUs -> Asids \cup {NULL}]  -- active_asids[cpu] (published, lockless)
    reserved,    \* [CPUs -> Asids \cup {NULL}]  -- reserved_asids[cpu] (preserved across rollover)
    curproc,     \* [CPUs -> Procs \cup {NULL}]  -- the Proc each CPU is currently running
    amap,        \* SUBSET Asids                 -- claimed ASIDs in the current generation
    fpend,       \* [CPUs -> BOOLEAN]            -- flush_pending[cpu]
    tlb          \* [CPUs -> [Asids -> Procs \cup {NULL}]] -- the (ASID -> address space) the CPU's TLB caches

vars == <<gen, pgen, pasid, active, reserved, curproc, amap, fpend, tlb>>

ReservedSet == { reserved[k] : k \in CPUs } \ {NULL}

TypeOk ==
    /\ gen      \in Gens
    /\ pgen     \in [Procs -> 0..MaxGen]
    /\ pasid    \in [Procs -> Asids \cup {NULL}]
    /\ active   \in [CPUs -> Asids \cup {NULL}]
    /\ reserved \in [CPUs -> Asids \cup {NULL}]
    /\ curproc  \in [CPUs -> Procs \cup {NULL}]
    /\ amap     \in SUBSET Asids
    /\ fpend    \in [CPUs -> BOOLEAN]
    /\ tlb      \in [CPUs -> [Asids -> Procs \cup {NULL}]]

Init ==
    /\ gen      = 1
    /\ pgen     = [p \in Procs |-> 0]
    /\ pasid    = [p \in Procs |-> NULL]
    /\ active   = [c \in CPUs |-> NULL]
    /\ reserved = [c \in CPUs |-> NULL]
    /\ curproc  = [c \in CPUs |-> NULL]
    /\ amap     = {}
    /\ fpend    = [c \in CPUs |-> FALSE]
    /\ tlb      = [c \in CPUs |-> [a \in Asids |-> NULL]]

(***************************************************************************)
(* FastSwitch(c, p): the lockless fast path. Two guards, both load-bearing:   *)
(*   (1) generation-match (pgen[p] = gen): the Proc's ASID is still valid this *)
(*       generation. After a rollover bumps `gen`, a stale-generation Proc     *)
(*       fails this and is forced to the slow path (re-stamp + flush).         *)
(*   (2) no-pending-flush (~fpend[c]): the CPU has no un-flushed stale TLB      *)
(*       from before a rollover. This is Linux's `old_active_asid != 0` guard  *)
(*       (flush_context zeroes active_asids[all], so a peer's next switch      *)
(*       takes the slow path and honors flush_pending). Omitting it lets a CPU *)
(*       run a recycled ASID over stale entries -> NoStaleTLB.                 *)
(* The fast path takes no lock and does no flush -- a Proc that passes BOTH     *)
(* guards has not crossed an un-reconciled rollover.                          *)
(* BUGGY_FAST_NO_REGEN drops guard (1); BUGGY_FAST_NO_FLUSH_CHECK drops (2).    *)
(***************************************************************************)
FastSwitch(c, p) ==
    /\ pasid[p] \in Asids
    /\ IF BUGGY_FAST_NO_REGEN THEN pgen[p] # 0 ELSE pgen[p] = gen
    /\ \/ BUGGY_FAST_NO_FLUSH_CHECK
       \/ ~fpend[c]
    /\ active'  = [active EXCEPT ![c] = pasid[p]]
    /\ curproc' = [curproc EXCEPT ![c] = p]
    /\ UNCHANGED <<gen, pgen, pasid, reserved, amap, fpend, tlb>>

(***************************************************************************)
(* SlowSwitch(c, p): a MISS (stale generation or never-assigned). Resolve a  *)
(* current-generation ASID, honor this CPU's pending local flush, publish.   *)
(*                                                                         *)
(*   canKeep  -- the Proc's old ASID is reclaimable (free this generation OR  *)
(*               reserved for it across a rollover -- the                     *)
(*               check_update_reserved_asid path that lets an active Proc     *)
(*               keep its ASID through a rollover).                          *)
(*   no rollover -- keep the old ASID, or claim any free one.                *)
(*   rollover    -- no free ASID: bump the generation, reset the bitmap,      *)
(*                  RESERVE every CPU's active ASID (clean) so a running CPU   *)
(*                  is never yanked, set flush_pending for peers, then claim  *)
(*                  a fresh ASID. The triggering CPU flushes its own TLB now.  *)
(***************************************************************************)
SlowSwitch(c, p) ==
    /\ pgen[p] # gen \/ pasid[p] = NULL
    /\ LET freeNow     == Asids \ amap
           canKeep     == /\ pasid[p] \in Asids
                          /\ (pasid[p] \notin amap \/ pasid[p] \in ReservedSet)
           needRollover == ~canKeep /\ freeNow = {}
       IN
       \/ /\ ~needRollover
          /\ \E na \in (IF canKeep THEN {pasid[p]} ELSE freeNow) :
                /\ gen'      = gen
                /\ amap'     = amap \cup {na}
                /\ reserved' = reserved
                /\ fpend'    = [fpend EXCEPT ![c] = FALSE]
                /\ tlb'      = IF fpend[c]
                                 THEN [tlb EXCEPT ![c] = [a \in Asids |-> NULL]]
                                 ELSE tlb
                /\ active'   = [active EXCEPT ![c] = na]
                /\ curproc'  = [curproc EXCEPT ![c] = p]
                /\ pgen'     = [pgen EXCEPT ![p] = gen]
                /\ pasid'    = [pasid EXCEPT ![p] = na]
       \/ /\ needRollover
          /\ gen < MaxGen
          /\ LET rr        == [k \in CPUs |-> IF active[k] \in Asids
                                                THEN active[k] ELSE reserved[k]]
                 rrSet     == { rr[k] : k \in CPUs } \ {NULL}
                 amapFlush == IF BUGGY_ROLLOVER_STEALS_ACTIVE THEN {} ELSE rrSet
                 postFree  == Asids \ amapFlush
             IN \E na \in postFree :
                  /\ gen'      = gen + 1
                  /\ reserved' = rr
                  /\ amap'     = amapFlush \cup {na}
                  /\ fpend'    = IF BUGGY_NO_FLUSH_PENDING
                                   THEN [fpend EXCEPT ![c] = FALSE]
                                   ELSE [k \in CPUs |-> IF k = c THEN FALSE ELSE TRUE]
                  /\ tlb'      = [tlb EXCEPT ![c] = [a \in Asids |-> NULL]]
                  /\ active'   = [active EXCEPT ![c] = na]
                  /\ curproc'  = [curproc EXCEPT ![c] = p]
                  /\ pgen'     = [pgen EXCEPT ![p] = gen + 1]
                  /\ pasid'    = [pasid EXCEPT ![p] = na]

(***************************************************************************)
(* Deschedule(c): the CPU switches to the kernel/idle (kproc, ASID 0 -- not  *)
(* an allocator participant). The outgoing ASID stays claimed in `amap`       *)
(* (Linux frees ASIDs only at rollover, never at deschedule); this is how the *)
(* bitmap fills and forces a rollover with few Procs.                         *)
(***************************************************************************)
Deschedule(c) ==
    /\ curproc[c] # NULL
    /\ active'  = [active EXCEPT ![c] = NULL]
    /\ curproc' = [curproc EXCEPT ![c] = NULL]
    /\ UNCHANGED <<gen, pgen, pasid, reserved, amap, fpend, tlb>>

(***************************************************************************)
(* CacheTranslation(c): the running CPU caches a translation under its        *)
(* current ASID (TLB fill). Models the TLB content NoStaleTLB reasons about.  *)
(***************************************************************************)
CacheTranslation(c) ==
    /\ curproc[c] # NULL
    /\ active[c] \in Asids
    /\ tlb' = [tlb EXCEPT ![c][active[c]] = curproc[c]]
    /\ UNCHANGED <<gen, pgen, pasid, active, reserved, curproc, amap, fpend>>

Next ==
    \/ \E c \in CPUs, p \in Procs : FastSwitch(c, p)
    \/ \E c \in CPUs, p \in Procs : SlowSwitch(c, p)
    \/ \E c \in CPUs : Deschedule(c)
    \/ \E c \in CPUs : CacheTranslation(c)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

\* I-31 core: the same active hardware ASID on two CPUs implies the same
\* address space (Proc). Distinct Procs never share a live ASID; the TLB can
\* always disambiguate. (Threads of one multi-thread Proc share an ASID --
\* that is correct and is the curproc[c1] = curproc[c2] case.)
NoActiveAlias ==
    \A c1, c2 \in CPUs :
        (c1 # c2 /\ active[c1] \in Asids /\ active[c1] = active[c2])
            => curproc[c1] = curproc[c2]

\* The flush_pending obligation: a running Proc's TLB tag for its own ASID is
\* either empty or itself -- never a stale other address space. A rollover that
\* recycles an ASID onto a CPU with stale entries (no local flush) breaks this.
NoStaleTLB ==
    \A c \in CPUs :
        (curproc[c] # NULL /\ active[c] \in Asids)
            => tlb[c][active[c]] \in {NULL, curproc[c]}

\* The rollover reservation obligation: every active ASID is claimed in the
\* current generation's bitmap, so new_context cannot hand it out again.
ActiveClaimed ==
    \A c \in CPUs : active[c] \in Asids => active[c] \in amap

\* The fast-path soundness premise: a current-generation Proc's ASID is claimed
\* (so reusing it on the fast path can never collide with a free-list claim).
CurrentGenClaimed ==
    \A p \in Procs :
        (pgen[p] = gen /\ pasid[p] \in Asids) => pasid[p] \in amap

====
