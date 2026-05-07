---- MODULE territory ----
(***************************************************************************)
(* Thylacine territory — P2-E spec.                                        *)
(*                                                                         *)
(* Models the Plan 9 territory primitives — `bind` and the corresponding  *)
(* cycle-freedom + isolation invariants per ARCHITECTURE.md §9.1 + §28    *)
(* I-1 (territory operations don't affect other procs' territories) +      *)
(* I-3 (mount points form a DAG, never a cycle).                          *)
(*                                                                         *)
(* Modeling decisions:                                                     *)
(*                                                                         *)
(*   Each Proc has its own territory function value (`bindings[p]`).      *)
(*   bindings[p][dst] is the SET of source paths bound at `dst` in proc   *)
(*   p's territory. (Plan 9's `bind(old, new, flags)` makes `old`'s      *)
(*   contents visible at `new`; we model `new`'s binding LIST since unions *)
(*   stack multiple bindings at one mount point.)                          *)
(*                                                                         *)
(*   Walking `dst` in proc p's territory yields each `src \in bindings[p] *)
(*   [dst]` (and `dst` itself for the union-with-original semantics — not *)
(*   modeled here; the spec focuses on the bind GRAPH, not the union     *)
(*   semantics).                                                           *)
(*                                                                         *)
(*   A bind-graph cycle is a non-trivial path from a path back to itself  *)
(*   following bindings — equivalently, `dst` is reachable from `src`     *)
(*   before adding the edge `dst -> src`. Cycle-freedom (I-3) is enforced *)
(*   at every Bind: the action's precondition is `~WouldCreateCycle`.    *)
(*                                                                         *)
(*   Isolation (I-1) is structural in this model: bindings[p] and        *)
(*   bindings[q] for p # q are independent function values. The Bind /    *)
(*   Unbind / ForkClone actions only ever update one proc's slot; no     *)
(*   action exists that updates two procs simultaneously. RFNAMEG-shared  *)
(*   territories (where two procs share one slot) are deliberately NOT    *)
(*   modeled — modeling them would require a separate `Territory` indirection *)
(*   layer; at v1.0 P2-E impl, RFNAMEG is unsupported (rfork extincts on  *)
(*   non-RFPROC flags), so the spec mirrors the impl's "private territory *)
(*   per Proc" semantics. Phase 5+ when RFNAMEG lands, the spec extends   *)
(*   with a Territory layer.                                                    *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation per CLAUDE.md spec-first *)
(* policy):                                                                *)
(*                                                                         *)
(*   territory.cfg              all flags FALSE — TLC proves NoCycle.     *)
(*   namespace_buggy.cfg        BUGGY_CYCLE=TRUE — counterexample where  *)
(*                              BuggyBind skips the cycle check and       *)
(*                              produces a cyclic graph.                   *)
(*                                                                         *)
(* Invariants enforced (TLC-checked):                                      *)
(*                                                                         *)
(*   NoCycle    — for every (proc, path) the path is not reachable from   *)
(*                its own bindings via the transitive closure. ARCH §28   *)
(*                I-3, §9.1 cycle-freedom guarantee.                       *)
(*                                                                         *)
(* Note on Walk determinism (mentioned in ARCH §9.1 alongside cycle-      *)
(* freedom + isolation): a path lookup from a fixed territory state       *)
(* always produces the same Spoor. In our spec, the binding graph is      *)
(* deterministic (a function of the current bindings state) — no random  *)
(* choice in lookup. Walk determinism is therefore structurally           *)
(* satisfied; no separate state invariant.                                *)
(*                                                                         *)
(* See ARCHITECTURE.md §9 (territory) + §28 invariants I-1, I-3.          *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Procs,         \* set of process identifiers
    Paths,         \* set of path identifiers (abstract — order is irrelevant)
    BUGGY_CYCLE    \* BOOLEAN — when TRUE, BuggyBind fires alongside Bind,
                   \*   skipping the cycle check. Set by namespace_buggy.cfg.

ASSUME Cardinality(Procs) >= 1
ASSUME Cardinality(Paths) >= 2
ASSUME BUGGY_CYCLE \in BOOLEAN

VARIABLES
    bindings       \* [Procs -> [Paths -> SUBSET Paths]]
                   \*   bindings[p][dst] = set of `src` paths bound at `dst`
                   \*   in proc p's territory.

vars == <<bindings>>

TypeOk == bindings \in [Procs -> [Paths -> SUBSET Paths]]

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
(*                                                                         *)
(* This is the CORRECT cycle check: it allows `bind(/x, /y)` where /x is  *)
(* a leaf (Reachable(p, {/x}) = {/x}, doesn't contain /y) but rejects     *)
(* `bind(/parent, /parent/child)` if /parent's existing bindings already  *)
(* reach /parent/child.                                                    *)
(***************************************************************************)
WouldCreateCycle(p, src, dst) ==
    \/ src = dst
    \/ dst \in Reachable(p, {src})

(***************************************************************************)
(* Init: every proc starts with an empty territory (no bindings).          *)
(***************************************************************************)
Init ==
    bindings = [p \in Procs |-> [path \in Paths |-> {}]]

(***************************************************************************)
(* Bind(p, src, dst) — CORRECT bind. Adds the edge `dst -> src` to proc p *)
(* iff (a) src # dst, (b) the edge doesn't already exist (idempotency at  *)
(* the spec level — re-binding is a no-op), and (c) the cycle check       *)
(* passes.                                                                 *)
(*                                                                         *)
(* Maps to `kernel/territory.c::bind` (P2-Eb impl).                       *)
(***************************************************************************)
Bind(p, src, dst) ==
    /\ ~WouldCreateCycle(p, src, dst)
    /\ src \notin bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \cup {src}]

(***************************************************************************)
(* BuggyBind(p, src, dst) — bug class: cycle check elided.                *)
(*                                                                         *)
(* Models a buggy `bind` that adds the edge without verifying that no     *)
(* cycle results. TLC will produce a counterexample where two BuggyBinds  *)
(* compose into a cycle. Caught by NoCycle invariant.                     *)
(*                                                                         *)
(* Real-world analogue: forgetting to call the cycle-check helper, OR     *)
(* a logic error in the cycle-check (e.g., walking the wrong direction). *)
(***************************************************************************)
BuggyBind(p, src, dst) ==
    /\ BUGGY_CYCLE
    /\ src # dst
    /\ src \notin bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \cup {src}]
    \* No cycle check.

(***************************************************************************)
(* Unbind(p, src, dst) — removes the edge `dst -> src` from proc p.       *)
(*                                                                         *)
(* Maps to `kernel/territory.c::unmount` (P2-Eb).                          *)
(***************************************************************************)
Unbind(p, src, dst) ==
    /\ src \in bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \ {src}]

(***************************************************************************)
(* ForkClone(parent, child) — copies parent's territory into child's.     *)
(*                                                                         *)
(* Models `rfork(RFPROC)` (no RFNAMEG): child gets a private copy of      *)
(* parent's territory. Subsequent modifications to either proc's          *)
(* bindings are independent (function values in TLA+ are immutable;       *)
(* EXCEPT creates a new function).                                         *)
(*                                                                         *)
(* Maps to `kernel/proc.c::rfork` (which at v1.0 P2-E supports only RFPROC*)
(* — territory cloning lands in P2-Eb when bind/mount syscalls go live).  *)
(*                                                                         *)
(* RFNAMEG (shared territory) is NOT modeled — see preamble.               *)
(***************************************************************************)
ForkClone(parent, child) ==
    /\ parent # child
    /\ bindings' = [bindings EXCEPT ![child] = bindings[parent]]

Next ==
    \/ \E p \in Procs, src \in Paths, dst \in Paths : Bind(p, src, dst)
    \/ \E p \in Procs, src \in Paths, dst \in Paths : BuggyBind(p, src, dst)
    \/ \E p \in Procs, src \in Paths, dst \in Paths : Unbind(p, src, dst)
    \/ \E parent, child \in Procs                   : ForkClone(parent, child)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(***************************************************************************)
(* NoCycle — the bind graph in every proc's territory is acyclic.          *)
(*                                                                         *)
(* For every (proc, path), the path is NOT reachable from its own         *)
(* binding set (i.e., starting from the paths bound at `path`, we cannot  *)
(* walk back to `path` via the transitive closure).                        *)
(*                                                                         *)
(* Equivalently: there is no non-empty walk x -> ... -> x in p's bind     *)
(* graph for any path x.                                                   *)
(*                                                                         *)
(* ARCH §28 I-3 (mount points form a DAG, never a cycle).                  *)
(***************************************************************************)
NoCycle ==
    \A p \in Procs, x \in Paths :
        x \notin Reachable(p, bindings[p][x])

(***************************************************************************)
(* Isolation (ARCH §28 I-1) — structural property of the spec's data       *)
(* model: bindings[p] and bindings[q] for p # q are independent function  *)
(* values. Bind / Unbind / ForkClone only modify ONE proc's slot per      *)
(* step. There is no shared-mutable-territory action; RFNAMEG (shared)    *)
(* is deliberately not modeled at this phase.                              *)
(*                                                                         *)
(* No state invariant is needed — isolation is encoded by the data       *)
(* model. A buggy variant that updated multiple procs in one step would   *)
(* require a temporal property to detect; we don't model it here. When    *)
(* RFNAMEG lands (Phase 5+ with the syscall surface), the spec extends    *)
(* with a Territory layer and Isolation becomes a state invariant relating     *)
(* shared procs.                                                           *)
(***************************************************************************)

Invariants ==
    /\ TypeOk
    /\ NoCycle

====
