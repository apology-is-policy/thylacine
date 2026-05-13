---- MODULE territory ----
(***************************************************************************)
(* Thylacine territory — P2-E + P5-attach-mount spec.                      *)
(*                                                                         *)
(* Models the Plan 9 territory primitives — `bind` + `mount` + the         *)
(* corresponding cycle-freedom + isolation + mount-refcount invariants per *)
(* ARCHITECTURE.md §9.1 + §9.6 + §28 I-1 (territory operations don't       *)
(* affect other procs' territories) + I-3 (mount points form a DAG, never  *)
(* a cycle) + the mount-lifecycle invariants from §9.6.6.                  *)
(*                                                                         *)
(* Two state layers:                                                       *)
(*                                                                         *)
(*   bindings: the bind graph. `bindings[p][dst]` is the SET of source     *)
(*   paths bound at `dst` in proc p's territory. Plan 9 `bind(old, new)`   *)
(*   makes `old`'s contents visible at `new`; we model `new`'s binding     *)
(*   LIST since unions stack multiple bindings at one mount point. Walking *)
(*   `dst` yields each `src \in bindings[p][dst]`.                         *)
(*                                                                         *)
(*   mounts: the mount table. `mounts[p]` is a SET of <<path, Spoor>>      *)
(*   pairs — each pair grafts a Spoor's tree at that path in proc p's      *)
(*   territory. Per ARCH §9.6, every filesystem entity is a Spoor; mount   *)
(*   is the operation that places one in the namespace. Multiple Spoors    *)
(*   can be mounted at the same path (union mounts, MBEFORE/MAFTER).       *)
(*                                                                         *)
(*   refcount: per-Spoor contribution from the mount tables (across all    *)
(*   procs). MountRefcountConsistency relates refcount to the actual       *)
(*   cardinality of entries.                                               *)
(*                                                                         *)
(* Edge interpretation (bind graph): an edge `dst -> src` exists iff       *)
(* src \in bindings[p][dst]. Walking dst in p's territory produces src;    *)
(* the transitive walk produces Reachable(p, {dst}).                       *)
(*                                                                         *)
(* Cycle-freedom (I-3) is enforced at every Bind: the action's             *)
(* precondition is `~WouldCreateCycle`.                                    *)
(*                                                                         *)
(* Isolation (I-1) is structural: bindings[p] / mounts[p] for p # q are    *)
(* independent function values. No action updates two procs' slots         *)
(* simultaneously. RFNAMEG-shared territories are NOT modeled — at v1.0    *)
(* impl, RFNAMEG is unsupported (rfork extincts on non-RFPROC flags); the  *)
(* spec mirrors the impl's "private territory per Proc" semantics. Phase   *)
(* 5+ when RFNAMEG lands the spec extends with a Territory indirection     *)
(* layer.                                                                  *)
(*                                                                         *)
(* Mount lifecycle (§9.6.6):                                               *)
(*   - Mount(p, S, path)         — adds <<path, S>> to mounts[p];          *)
(*                                  bumps refcount[S].                     *)
(*   - Unmount(p, S, path)       — removes one entry; drops refcount[S].   *)
(*   - ForkClone(parent, child)  — deep-copies mounts[parent] into         *)
(*                                  mounts[child]; bumps refcount for each *)
(*                                  cloned entry (each clone contributes   *)
(*                                  one new reference per entry).          *)
(*   - DestroyTerritory(p)       — modeled as the contract that mounts[p]  *)
(*                                  must be {} before destroy; the impl is *)
(*                                  obliged to unmount every entry first.  *)
(*                                  Buggy destroy (`BUGGY_DESTROY_LEAK`)   *)
(*                                  clears entries WITHOUT decrementing    *)
(*                                  refcounts — produces a leak.           *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation per CLAUDE.md spec-first  *)
(* policy):                                                                *)
(*                                                                         *)
(*   territory.cfg                  all flags FALSE — TLC proves invariants*)
(*   territory_buggy.cfg            BUGGY_CYCLE=TRUE — counterexample      *)
(*                                  where BuggyBind skips the cycle check  *)
(*                                  and produces a cyclic graph.           *)
(*   territory_buggy_mount_no_refbump.cfg                                  *)
(*                                  BUGGY_MOUNT_NO_REFBUMP=TRUE —          *)
(*                                  counterexample where Mount adds the    *)
(*                                  entry but skips refcount bump.         *)
(*   territory_buggy_unmount_no_refdrop.cfg                                *)
(*                                  BUGGY_UNMOUNT_NO_REFDROP=TRUE —        *)
(*                                  counterexample where Unmount removes   *)
(*                                  the entry but doesn't decrement.       *)
(*   territory_buggy_destroy_leak.cfg                                      *)
(*                                  BUGGY_DESTROY_LEAK=TRUE — counter-     *)
(*                                  example where BuggyDestroy clears      *)
(*                                  mounts[p] without dropping refcounts.  *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   NoCycle    — for every (proc, path) the path is not reachable from   *)
(*                its own bindings via the transitive closure. ARCH §28   *)
(*                I-3, §9.1 cycle-freedom guarantee.                       *)
(*                                                                         *)
(*   MountRefcountConsistency                                              *)
(*              — for every Spoor s, refcount[s] equals the total          *)
(*                cardinality of (p, path) pairs across all procs such    *)
(*                that <<path, s>> \in mounts[p]. ARCH §9.6.6: "every      *)
(*                Spoor in the table has refcount ≥ 1 contributed by the   *)
(*                table"; the kernel's refcount counter must agree with    *)
(*                the actual entries.                                      *)
(*                                                                         *)
(*   MountRefcountNonNegative                                              *)
(*              — refcount never goes negative.                            *)
(*                                                                         *)
(* See ARCHITECTURE.md §9 (territory) + §9.6 (mount) + §28 invariants      *)
(* I-1, I-3.                                                               *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Procs,                     \* set of process identifiers
    Paths,                     \* set of path identifiers (abstract — order is irrelevant)
    Spoors,                    \* set of Spoor identifiers
    BUGGY_CYCLE,               \* BOOLEAN — BuggyBind skips cycle check
    BUGGY_MOUNT_NO_REFBUMP,    \* BOOLEAN — BuggyMount skips refcount bump
    BUGGY_UNMOUNT_NO_REFDROP,  \* BOOLEAN — BuggyUnmount skips refcount drop
    BUGGY_DESTROY_LEAK         \* BOOLEAN — BuggyDestroy clears mounts[p]
                               \*           without dropping refcounts.

ASSUME Cardinality(Procs) >= 1
ASSUME Cardinality(Paths) >= 2
ASSUME Cardinality(Spoors) >= 1
ASSUME BUGGY_CYCLE \in BOOLEAN
ASSUME BUGGY_MOUNT_NO_REFBUMP \in BOOLEAN
ASSUME BUGGY_UNMOUNT_NO_REFDROP \in BOOLEAN
ASSUME BUGGY_DESTROY_LEAK \in BOOLEAN

VARIABLES
    bindings,      \* [Procs -> [Paths -> SUBSET Paths]]
                   \*   bindings[p][dst] = set of `src` paths bound at `dst`
                   \*   in proc p's territory.
    mounts,        \* [Procs -> SUBSET (Paths \X Spoors)]
                   \*   mounts[p] = set of <<path, Spoor>> grafts in
                   \*   proc p's territory.
    refcount       \* [Spoors -> Nat]
                   \*   refcount[s] = kernel's mount-contribution refcount
                   \*   for Spoor s. Should equal the cardinality of (p,
                   \*   path) pairs across all procs with <<path, s>> \in
                   \*   mounts[p]; tracked separately so the impl bug
                   \*   "forgot to bump/drop refcount" is catchable.

vars == <<bindings, mounts, refcount>>

TypeOk ==
    /\ bindings \in [Procs -> [Paths -> SUBSET Paths]]
    /\ mounts \in [Procs -> SUBSET (Paths \X Spoors)]
    /\ refcount \in [Spoors -> Nat]

(***************************************************************************)
(* Reachable(p, S) — transitive closure of S through proc p's bind graph: *)
(* the set of paths reachable from any starting node in S by following    *)
(* zero-or-more bind edges.                                                *)
(*                                                                         *)
(* Edge interpretation: an edge `dst -> src` exists iff src \in           *)
(* bindings[p][dst]. Walking dst in p's territory produces src; following *)
(* the transitive walk produces Reachable(p, {dst}).                       *)
(*                                                                         *)
(* Implemented as a fixed-point iteration. Termination: Paths is finite,  *)
(* and S is monotonically growing; the loop stops when no new paths are   *)
(* added.                                                                  *)
(***************************************************************************)
RECURSIVE ReachableImpl(_, _)
ReachableImpl(p, S) ==
    LET S2 == S \cup UNION { bindings[p][y] : y \in S }
    IN  IF S2 = S THEN S ELSE ReachableImpl(p, S2)

Reachable(p, S) == ReachableImpl(p, S)

(***************************************************************************)
(* WouldCreateCycle(p, src, dst) — predicate: would adding the edge       *)
(* `dst -> src` to proc p's bind graph create a cycle?                     *)
(*                                                                         *)
(* Trivially yes if src = dst (self-loop = cycle of length 1).            *)
(* Otherwise, a cycle forms iff dst is already reachable from src via     *)
(* existing edges — then adding `dst -> src` closes the loop:             *)
(*   src -> ... -> dst -> (new) -> src.                                   *)
(***************************************************************************)
WouldCreateCycle(p, src, dst) ==
    \/ src = dst
    \/ dst \in Reachable(p, {src})

(***************************************************************************)
(* MountEntriesForSpoor(s) — set of (p, path) pairs across all procs      *)
(* with <<path, s>> \in mounts[p]. Cardinality gives the true count of    *)
(* mount entries referencing s; refcount[s] should equal this cardinality.*)
(*                                                                         *)
(* Cartesian-product filter form (TLA+ requires the bound-variable to be  *)
(* a simple identifier in a filter comprehension, so we destructure       *)
(* manually via pair[1] / pair[2]).                                       *)
(***************************************************************************)
MountEntriesForSpoor(s) ==
    { pair \in Procs \X Paths : <<pair[2], s>> \in mounts[pair[1]] }

(***************************************************************************)
(* Init: every proc starts with an empty territory (no bindings, no       *)
(* mounts). Refcount is 0 for every Spoor.                                 *)
(***************************************************************************)
Init ==
    /\ bindings = [p \in Procs |-> [path \in Paths |-> {}]]
    /\ mounts = [p \in Procs |-> {}]
    /\ refcount = [s \in Spoors |-> 0]

(***************************************************************************)
(* ================================= BIND ================================== *)
(***************************************************************************)

(***************************************************************************)
(* Bind(p, src, dst) — CORRECT bind. Adds the edge `dst -> src` to proc p *)
(* iff (a) src # dst, (b) the edge doesn't already exist, (c) cycle check  *)
(* passes. Maps to `kernel/territory.c::bind`.                             *)
(***************************************************************************)
Bind(p, src, dst) ==
    /\ ~WouldCreateCycle(p, src, dst)
    /\ src \notin bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \cup {src}]
    /\ UNCHANGED <<mounts, refcount>>

(***************************************************************************)
(* BuggyBind(p, src, dst) — bug class: cycle check elided.                *)
(***************************************************************************)
BuggyBind(p, src, dst) ==
    /\ BUGGY_CYCLE
    /\ src # dst
    /\ src \notin bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \cup {src}]
    /\ UNCHANGED <<mounts, refcount>>

(***************************************************************************)
(* Unbind(p, src, dst) — removes the edge `dst -> src` from proc p. Maps  *)
(* to `kernel/territory.c::unbind` (renamed from `unmount` at              *)
(* P5-attach-mount to free the verb `unmount` for the mount-table         *)
(* primitive; the existing call removes a bind edge, not a mount entry).  *)
(***************************************************************************)
Unbind(p, src, dst) ==
    /\ src \in bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \ {src}]
    /\ UNCHANGED <<mounts, refcount>>

(***************************************************************************)
(* ================================ MOUNT ================================== *)
(***************************************************************************)

(***************************************************************************)
(* Mount(p, s, path) — CORRECT mount. Adds <<path, s>> to mounts[p]; bumps*)
(* refcount[s]. Idempotent at the action level: re-mounting the same      *)
(* (path, s) is a no-op (precondition <<path, s>> \notin mounts[p]).      *)
(*                                                                         *)
(* Maps to `kernel/territory.c::mount` (lands at P5-attach-mount).         *)
(*                                                                         *)
(* The spec models a single Spoor per mount entry; union semantics        *)
(* (multiple Spoors at one path) are expressed by multiple Mount calls    *)
(* with different `s`. MBEFORE/MAFTER ordering is below the spec's        *)
(* granularity (set semantics in mounts[p]) — at the impl, the order is   *)
(* maintained in the mount array.                                          *)
(***************************************************************************)
Mount(p, s, path) ==
    /\ <<path, s>> \notin mounts[p]
    /\ mounts' = [mounts EXCEPT ![p] = @ \cup {<<path, s>>}]
    /\ refcount' = [refcount EXCEPT ![s] = @ + 1]
    /\ UNCHANGED bindings

(***************************************************************************)
(* BuggyMountNoRefbump(p, s, path) — bug class: mount adds the entry but  *)
(* skips the refcount bump. After this fires, refcount[s] is less than    *)
(* the cardinality of entries referencing s; subsequent unref drops the   *)
(* count below zero or frees the Spoor while entries still reference it.  *)
(*                                                                         *)
(* TLC catches this via MountRefcountConsistency.                          *)
(***************************************************************************)
BuggyMountNoRefbump(p, s, path) ==
    /\ BUGGY_MOUNT_NO_REFBUMP
    /\ <<path, s>> \notin mounts[p]
    /\ mounts' = [mounts EXCEPT ![p] = @ \cup {<<path, s>>}]
    /\ UNCHANGED <<bindings, refcount>>

(***************************************************************************)
(* Unmount(p, s, path) — CORRECT unmount. Removes <<path, s>> from        *)
(* mounts[p]; drops refcount[s]. The impl's `unmount(territory,           *)
(* target_path)` finds an entry by path; the spec's `path, s` are both    *)
(* arguments because at the spec level the entry is a (path, s) pair.     *)
(*                                                                         *)
(* Maps to `kernel/territory.c::unmount` (lands at P5-attach-mount).       *)
(***************************************************************************)
Unmount(p, s, path) ==
    /\ <<path, s>> \in mounts[p]
    /\ mounts' = [mounts EXCEPT ![p] = @ \ {<<path, s>>}]
    /\ refcount' = [refcount EXCEPT ![s] = @ - 1]
    /\ UNCHANGED bindings

(***************************************************************************)
(* BuggyUnmountNoRefdrop(p, s, path) — bug class: unmount removes the     *)
(* entry but doesn't decrement refcount[s]. After this fires, refcount[s] *)
(* exceeds the cardinality of remaining entries; the Spoor's storage is   *)
(* never freed (memory leak).                                              *)
(***************************************************************************)
BuggyUnmountNoRefdrop(p, s, path) ==
    /\ BUGGY_UNMOUNT_NO_REFDROP
    /\ <<path, s>> \in mounts[p]
    /\ mounts' = [mounts EXCEPT ![p] = @ \ {<<path, s>>}]
    /\ UNCHANGED <<bindings, refcount>>

(***************************************************************************)
(* ForkClone(parent, child) — copies parent's territory (bindings AND     *)
(* mounts) into child's. Each cloned mount entry contributes a new        *)
(* reference; refcount is bumped for every Spoor that appears in          *)
(* mounts[parent].                                                         *)
(*                                                                         *)
(* The refcount update bumps `refcount[s] += k` where k is the number of  *)
(* mount entries in parent referencing s. Models the impl's               *)
(* `territory_clone`: iterate over parent's mount entries, copy each,     *)
(* call spoor_ref for each.                                                *)
(*                                                                         *)
(* Precondition: child's territory must be in Init state (empty bindings, *)
(* empty mounts). This mirrors the impl: `territory_clone` is called on a *)
(* freshly-allocated Territory; cloning over a live one would overwrite   *)
(* its mount entries WITHOUT decrementing their refcounts (leak) AND lose *)
(* the bindings. The impl never does this — the kernel calls              *)
(* territory_alloc to get a fresh slot, then territory_clone to populate. *)
(*                                                                         *)
(* Maps to `kernel/territory.c::territory_clone`. RFNAMEG (shared         *)
(* territory) is NOT modeled — see preamble.                              *)
(***************************************************************************)
ForkClone(parent, child) ==
    /\ parent # child
    /\ bindings[child] = [path \in Paths |-> {}]
    /\ mounts[child] = {}
    /\ bindings' = [bindings EXCEPT ![child] = bindings[parent]]
    /\ mounts' = [mounts EXCEPT ![child] = mounts[parent]]
    /\ refcount' = [s \in Spoors |->
                       refcount[s] +
                       Cardinality({path \in Paths : <<path, s>> \in mounts[parent]})]

(***************************************************************************)
(* BuggyDestroyLeak(p) — bug class: Territory destruction clears mounts[p]*)
(* without dropping refcounts. After this fires, refcount[s] for every s  *)
(* that was referenced by mounts[p] exceeds the cardinality of remaining  *)
(* entries; the Spoor's storage is never freed.                            *)
(*                                                                         *)
(* This is the catch for the impl's `territory_unref` final-release path: *)
(* must iterate over mounts[] and call spoor_unref for each entry BEFORE  *)
(* kmem_cache_free.                                                        *)
(*                                                                         *)
(* Modeled as: clear mounts[p] without updating refcount. The clean       *)
(* equivalent ("DestroyTerritory") is not a distinct action — the impl    *)
(* model is "Unmount every entry, then free." TLC explores Unmount        *)
(* sequences directly; no clean Destroy action is needed.                  *)
(***************************************************************************)
BuggyDestroyLeak(p) ==
    /\ BUGGY_DESTROY_LEAK
    /\ mounts[p] # {}
    /\ mounts' = [mounts EXCEPT ![p] = {}]
    /\ UNCHANGED <<bindings, refcount>>

Next ==
    \/ \E p \in Procs, src \in Paths, dst \in Paths : Bind(p, src, dst)
    \/ \E p \in Procs, src \in Paths, dst \in Paths : BuggyBind(p, src, dst)
    \/ \E p \in Procs, src \in Paths, dst \in Paths : Unbind(p, src, dst)
    \/ \E p \in Procs, s \in Spoors, path \in Paths : Mount(p, s, path)
    \/ \E p \in Procs, s \in Spoors, path \in Paths : BuggyMountNoRefbump(p, s, path)
    \/ \E p \in Procs, s \in Spoors, path \in Paths : Unmount(p, s, path)
    \/ \E p \in Procs, s \in Spoors, path \in Paths : BuggyUnmountNoRefdrop(p, s, path)
    \/ \E parent, child \in Procs                   : ForkClone(parent, child)
    \/ \E p \in Procs                               : BuggyDestroyLeak(p)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(***************************************************************************)
(* NoCycle — the bind graph in every proc's territory is acyclic. ARCH §28*)
(* I-3 (mount points form a DAG, never a cycle).                          *)
(***************************************************************************)
NoCycle ==
    \A p \in Procs, x \in Paths :
        x \notin Reachable(p, bindings[p][x])

(***************************************************************************)
(* MountRefcountConsistency — for every Spoor s, the kernel's refcount[s] *)
(* equals the cardinality of mount entries referencing s across all       *)
(* procs.                                                                  *)
(*                                                                         *)
(* ARCH §9.6.6: "every Spoor in the table has refcount ≥ 1 contributed by *)
(* the table" — and the kernel's counter must agree with the actual       *)
(* entries. A bug that bumps without entry (refcount > entries) leaks     *)
(* storage; a bug that decrements without removing entry (refcount <      *)
(* entries) eventually frees a Spoor that's still referenced.             *)
(***************************************************************************)
MountRefcountConsistency ==
    \A s \in Spoors :
        refcount[s] = Cardinality(MountEntriesForSpoor(s))

(***************************************************************************)
(* MountRefcountNonNegative — refcount never underflows. Buggy variants   *)
(* that decrement without bumping in a previous step would surface here.  *)
(* (Type-check of `refcount \in [Spoors -> Nat]` enforces this at         *)
(* TypeOk, but having an explicit invariant gives a clearer counterexample*)
(* than a TypeOk violation.)                                              *)
(***************************************************************************)
MountRefcountNonNegative ==
    \A s \in Spoors : refcount[s] >= 0

(***************************************************************************)
(* Isolation (ARCH §28 I-1) — structural property of the spec's data      *)
(* model: bindings[p] / mounts[p] and bindings[q] / mounts[q] for p # q   *)
(* are independent function values. Every action only modifies ONE proc's *)
(* slot per step (Mount/Unmount/Bind/Unbind/BuggyDestroyLeak); ForkClone  *)
(* copies parent's bindings/mounts into child's slot but leaves parent's  *)
(* slot unchanged.                                                        *)
(*                                                                         *)
(* No state invariant is needed — isolation is encoded by the data       *)
(* model. A buggy variant that updated multiple procs in one step would   *)
(* require a temporal property to detect; we don't model it here. When    *)
(* RFNAMEG lands (Phase 5+ with the syscall surface), the spec extends    *)
(* with a Territory layer and Isolation becomes a state invariant.        *)
(***************************************************************************)

Invariants ==
    /\ TypeOk
    /\ NoCycle
    /\ MountRefcountConsistency
    /\ MountRefcountNonNegative

====
