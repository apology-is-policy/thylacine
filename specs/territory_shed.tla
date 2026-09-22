--------------------------- MODULE territory_shed ---------------------------
(***************************************************************************)
(* The mount-table SHED at pivot / chroot (ARCH 9.6.10; the #80 seam).     *)
(*                                                                         *)
(* A Territory's mount table is a bounded array keyed on the IDENTITY of   *)
(* the directory mounted onto. When the root is swapped, entries whose     *)
(* mount point can no longer be reached from the new root stay in the      *)
(* table, cannot be named (unmount needs a resolved mount point), and are  *)
(* copied into every child: slots lost for the life of the namespace.      *)
(* The shed drops exactly those entries, atomically with the swap.         *)
(*                                                                         *)
(* TWO DEFINITIONS, KEPT APART ON PURPOSE. Round 1 of the audit showed the *)
(* first version of this module could not fail: its soundness invariant    *)
(* compared Keep with the SAME closure Keep was built from, so any         *)
(* reachability rule at all -- {root}, or every tree -- passed. The ground *)
(* truth is therefore OPERATIONAL here and shares nothing with the rule:   *)
(*                                                                         *)
(*   THE WALKER is a resolution from the new root, run against the table   *)
(*   as it stood BEFORE the shed (`ghost`). It starts where the resolver   *)
(*   starts and moves only the ways the resolver moves:                    *)
(*     - it starts in the root's own tree;                                 *)
(*     - if the root is a UNION directory handle it also consults the      *)
(*       union's mount POINT, which lives in the tree the union was        *)
(*       mounted in, not in member[0]'s (kernel/stalk.c: union_base =      *)
(*       base->union_snap->point), so it may start there too;              *)
(*     - it CROSSES a mount whose point lives in the tree it stands in;    *)
(*     - in a tree of a Dev that stamps the walker's own devno on every    *)
(*       walk (devenv; Dev.devno_per_walker) it may land in any tree of    *)
(*       that Dev.                                                         *)
(*   It never goes UP: '..' pops the resolver's in-call trail and is a     *)
(*   no-op at the base. BUGGY_RESOLVER_DOTDOT_ESCAPES is a resolver that   *)
(*   does go up; the rule is unsound against it, which is what makes the   *)
(*   downward-only premise a recorded dependency instead of a belief.      *)
(*                                                                         *)
(*   THE RULE (ImplReach) is what kernel/territory.c computes: the least   *)
(*   set R of trees with the root's tree in R, the union point's tree in   *)
(*   R, every per-walker tree in R once one is, and                        *)
(*        <<pt, s>> \in mounts /\ Home[pt] \in R  =>  s \in R.             *)
(*   The kernel cannot see INSIDE a 9P tree, so the rule is per TREE and   *)
(*   conservative: it may keep an entry no walk fires, never drop one a    *)
(*   walk can.                                                             *)
(*                                                                         *)
(* PROPERTIES.                                                             *)
(*   ShedLosesNothing   -- wherever the walker stands, every entry of the  *)
(*                         pre-shed table that fires there SURVIVED. By    *)
(*                         induction along the walk, a resolution from the *)
(*                         new root crosses exactly the mounts it crossed  *)
(*                         before the shed.                                *)
(*   WalkerWithinClosure -- the walker never leaves TrueReach: the two     *)
(*                         truth-side definitions (the operational walker  *)
(*                         and the closed form NoResidue uses) AGREE. It   *)
(*                         is a consistency check, not an independent pin  *)
(*                         (audit r2 F6): every too-small TrueReach that   *)
(*                         fails here has a witness NoResidueAfterPivot    *)
(*                         fails on first. Kept because it names WHICH of  *)
(*                         the two definitions moved when one does.        *)
(*   NoResidueAfterPivot -- right after a pivot every surviving entry is   *)
(*                         keyed in TrueReach: the slots come back. (A     *)
(*                         TrueReach that is too LARGE only weakens this   *)
(*                         half; nothing here can pin it from above, and   *)
(*                         the soundness half does not use it.)            *)
(*                                                                         *)
(* BUGGY CONFIGS, each failing its OWN invariant:                          *)
(*   NONTRANSITIVE  keep only entries keyed in the seeds      -> ShedLoses *)
(*   NO_UNION_SEED  forget the union root's point (audit F1)  -> ShedLoses *)
(*   UNDECLARED_PER_WALKER  match a per-walker Dev on devno   -> ShedLoses *)
(*   DOTDOT_ESCAPES the resolver goes up (premise violation)  -> ShedLoses *)
(*   KEEPS_ALL      the pre-fix kernel                        -> NoResidue *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Trees,                      \* device instances (dc, devno)
    Points,                     \* mount-point identities
    PerWalker,                  \* trees of a Dev that restamps devno per walk
    NoPoint,                    \* "the root is not a union handle"
    BUGGY_SHED_NONTRANSITIVE,
    BUGGY_SHED_NO_UNION_SEED,
    BUGGY_SHED_UNDECLARED_PER_WALKER,
    BUGGY_SHED_KEEPS_ALL,
    BUGGY_RESOLVER_DOTDOT_ESCAPES

ASSUME PerWalker \subseteq Trees
ASSUME NoPoint \notin Points
ASSUME BUGGY_SHED_NONTRANSITIVE \in BOOLEAN
ASSUME BUGGY_SHED_NO_UNION_SEED \in BOOLEAN
ASSUME BUGGY_SHED_UNDECLARED_PER_WALKER \in BOOLEAN
ASSUME BUGGY_SHED_KEEPS_ALL \in BOOLEAN
ASSUME BUGGY_RESOLVER_DOTDOT_ESCAPES \in BOOLEAN

Nowhere == "nowhere"

VARIABLES
    Home,        \* [Points -> Trees]: the tree each mount point lives in.
                 \* Chosen at Init and never changed (the territory.tla `holds`
                 \* idiom), so TLC explores EVERY layout of points over trees.
    root,        \* the root Spoor's own tree
    rootpt,      \* the union mount point it carries, or NoPoint
    mounts,      \* SUBSET (Points \X Trees): <<pt, s>> = tree s grafted at pt
    ghost,       \* the table as it stood BEFORE the last shed (while fresh)
    pos,         \* the tree the walker stands in, or Nowhere
    fresh        \* TRUE from a pivot until the next namespace edit

vars == <<Home, root, rootpt, mounts, ghost, pos, fresh>>

\* ---- THE TRUTH. Shares no operator with the rule below, so a fault in the
\* rule's closure cannot also move the yardstick. A productive step adds a
\* tree, so Cardinality(Trees) steps reach the fixpoint.

TrueStart(r, upt) == IF upt = NoPoint THEN {r} ELSE {r, Home[upt]}

TrueStep(R, M) ==
    LET W == IF R \cap PerWalker # {} THEN R \cup PerWalker ELSE R
    IN  W \cup { m[2] : m \in { e \in M : Home[e[1]] \in W } }

RECURSIVE TrueIter(_, _, _)
TrueIter(R, M, n) == IF n = 0 THEN R ELSE TrueIter(TrueStep(R, M), M, n - 1)

TrueReach(r, upt, M) == TrueIter(TrueStart(r, upt), M, Cardinality(Trees) + 1)

\* ---- THE RULE: what kernel/territory.c computes, with its buggy variants.

Seeds(r, upt) ==
    {r} \cup (IF ~BUGGY_SHED_NO_UNION_SEED /\ upt # NoPoint
              THEN {Home[upt]} ELSE {})

Widen(R) ==
    IF ~BUGGY_SHED_UNDECLARED_PER_WALKER /\ R \cap PerWalker # {}
    THEN R \cup PerWalker ELSE R

RECURSIVE Close(_, _)
Close(R, M) ==
    LET R1 == Widen(R)
        R2 == R1 \cup { e[2] : e \in { m \in M : Home[m[1]] \in R1 } }
    IN  IF R2 = R THEN R ELSE Close(R2, M)

ImplReach(r, upt, M) ==
    IF BUGGY_SHED_NONTRANSITIVE THEN Widen(Seeds(r, upt))
    ELSE Close(Seeds(r, upt), M)

Keep(r, upt, M) ==
    IF BUGGY_SHED_KEEPS_ALL THEN M
    ELSE { m \in M : Home[m[1]] \in ImplReach(r, upt, M) }

\* ---- namespace edits

Init ==
    /\ Home \in [Points -> Trees]
    /\ root \in Trees
    /\ rootpt = NoPoint
    /\ mounts = {}
    /\ ghost = {}
    /\ pos = Nowhere
    /\ fresh = FALSE

\* Same-tree binds are allowed (they are the commonest real mount); I-3 is
\* territory.tla's business, not this module's.
Mount(pt, s) ==
    /\ mounts' = mounts \cup { <<pt, s>> }
    /\ ghost' = {} /\ pos' = Nowhere /\ fresh' = FALSE
    /\ UNCHANGED <<Home, root, rootpt>>

Unmount(pt, s) ==
    /\ <<pt, s>> \in mounts
    /\ mounts' = mounts \ { <<pt, s>> }
    /\ ghost' = {} /\ pos' = Nowhere /\ fresh' = FALSE
    /\ UNCHANGED <<Home, root, rootpt>>

\* The kernel skips only a swap to the SAME Spoor; a chroot into another
\* directory of the same tree is a real swap, so `new = root` is explored.
\* Any point may ride on the new root: a union handle can outlive the mounts
\* that made it a union, and over-approximating is the safe direction.
Pivot(new, upt) ==
    /\ root' = new
    /\ rootpt' = upt
    /\ ghost' = mounts
    /\ mounts' = Keep(new, upt, mounts)
    /\ pos' = Nowhere
    /\ fresh' = TRUE
    /\ UNCHANGED Home

\* ---- the walker (see the header): a resolution from the new root over `ghost`

WalkStart ==
    /\ fresh /\ pos = Nowhere
    /\ pos' \in TrueStart(root, rootpt)
    /\ UNCHANGED <<Home, root, rootpt, mounts, ghost, fresh>>

WalkCross(m) ==
    /\ fresh /\ m \in ghost /\ Home[m[1]] = pos
    /\ pos' = m[2]
    /\ UNCHANGED <<Home, root, rootpt, mounts, ghost, fresh>>

WalkRestamp(t) ==
    /\ fresh /\ pos \in PerWalker /\ t \in PerWalker
    /\ pos' = t
    /\ UNCHANGED <<Home, root, rootpt, mounts, ghost, fresh>>

WalkDotDot(m) ==
    /\ BUGGY_RESOLVER_DOTDOT_ESCAPES
    /\ fresh /\ m \in ghost /\ m[2] = pos
    /\ pos' = Home[m[1]]
    /\ UNCHANGED <<Home, root, rootpt, mounts, ghost, fresh>>

Next ==
    \/ \E pt \in Points, s \in Trees : Mount(pt, s)
    \/ \E pt \in Points, s \in Trees : Unmount(pt, s)
    \/ \E new \in Trees, upt \in Points \cup {NoPoint} : Pivot(new, upt)
    \/ WalkStart
    \/ \E m \in ghost : WalkCross(m)
    \/ \E t \in Trees : WalkRestamp(t)
    \/ \E m \in ghost : WalkDotDot(m)

Spec == Init /\ [][Next]_vars

TypeOk ==
    /\ Home \in [Points -> Trees]
    /\ root \in Trees
    /\ rootpt \in Points \cup {NoPoint}
    /\ mounts \subseteq (Points \X Trees)
    /\ ghost \subseteq (Points \X Trees)
    /\ pos \in Trees \cup {Nowhere}
    /\ fresh \in BOOLEAN

ShedLosesNothing ==
    (fresh /\ pos # Nowhere) =>
        \A m \in ghost : Home[m[1]] = pos => m \in mounts

WalkerWithinClosure ==
    (fresh /\ pos # Nowhere) => pos \in TrueReach(root, rootpt, ghost)

NoResidueAfterPivot ==
    fresh => \A m \in mounts : Home[m[1]] \in TrueReach(root, rootpt, ghost)

Invariants ==
    TypeOk /\ ShedLosesNothing /\ WalkerWithinClosure /\ NoResidueAfterPivot
=============================================================================
