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
(* THE MODEL. The resolver is DOWNWARD-ONLY ('..' pops an in-call trail    *)
(* and is a no-op at the base -- kernel/stalk.c), so from a root the       *)
(* reachable device trees are the least set R with:                        *)
(*      root \in R                                                         *)
(*      <<pt, s>> \in mounts /\ Home[pt] \in R  =>  s \in R                *)
(* Home[pt] is the tree (device instance) the mount point pt lives in.     *)
(* The kernel cannot see INSIDE a 9P tree, so it cannot tell which         *)
(* directories of a reachable tree are themselves reachable: the rule is   *)
(* per TREE and therefore conservative -- it may keep an entry no walk can *)
(* fire, and must never drop one a walk can.                               *)
(*                                                                         *)
(* PROPERTIES.                                                             *)
(*   ShedLosesNothing  -- no entry whose mount point lived in a tree       *)
(*                        reachable from the NEW root is ever dropped.     *)
(*                        (History variable `lost`.) This is the soundness *)
(*                        half: every resolution from the new root crosses *)
(*                        exactly the mounts it crossed before the shed.   *)
(*   NoResidueAfterPivot -- right after a pivot every surviving entry is   *)
(*                        reachable. The completeness half: the slots come *)
(*                        back. (Unmount may strand entries later; those   *)
(*                        are revived by re-mounting the tree and are not  *)
(*                        this mechanism's business.)                      *)
(*                                                                         *)
(* BUGGY_SHED_NONTRANSITIVE is the tempting wrong implementation: keep     *)
(* only entries whose mount point lives in the NEW ROOT's own tree. It     *)
(* drops a mount nested inside a mounted tree (/dev/pts inside the devdev  *)
(* tree mounted at /dev) at the next pivot, and ShedLosesNothing fails.    *)
(* BUGGY_SHED_KEEPS_ALL is the pre-fix kernel: NoResidueAfterPivot fails.  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Trees,                      \* device instances (dc, devno)
    Points,                     \* mount-point identities
    BUGGY_SHED_NONTRANSITIVE,
    BUGGY_SHED_KEEPS_ALL

ASSUME BUGGY_SHED_NONTRANSITIVE \in BOOLEAN
ASSUME BUGGY_SHED_KEEPS_ALL \in BOOLEAN

VARIABLES
    Home,        \* [Points -> Trees]: the tree each mount point lives in.
                 \* Chosen at Init and never changed (the territory.tla `holds`
                 \* idiom), so TLC explores EVERY layout of points over trees.
    root,        \* the Territory's root tree
    mounts,      \* SUBSET (Points \X Trees): <<pt, s>> = tree s grafted at pt
    lost,        \* history: entries dropped while reachable from the new root
    fresh        \* TRUE in the state right after a pivot

vars == <<Home, root, mounts, lost, fresh>>

RECURSIVE ReachImpl(_, _)
ReachImpl(R, M) ==
    LET R2 == R \cup { e[2] : e \in { m \in M : Home[m[1]] \in R } }
    IN  IF R2 = R THEN R ELSE ReachImpl(R2, M)

Reach(r, M) == ReachImpl({r}, M)

Init ==
    /\ Home \in [Points -> Trees]
    /\ root \in Trees
    /\ mounts = {}
    /\ lost = {}
    /\ fresh = FALSE

\* I-3 is territory.tla's business; this module only forbids the self-mount
\* so the closure stays meaningful.
Mount(pt, s) ==
    /\ Home[pt] # s
    /\ mounts' = mounts \cup { <<pt, s>> }
    /\ fresh' = FALSE
    /\ UNCHANGED <<Home, root, lost>>

Unmount(pt, s) ==
    /\ <<pt, s>> \in mounts
    /\ mounts' = mounts \ { <<pt, s>> }
    /\ fresh' = FALSE
    /\ UNCHANGED <<Home, root, lost>>

Keep(new, M) ==
    IF BUGGY_SHED_KEEPS_ALL THEN M
    ELSE IF BUGGY_SHED_NONTRANSITIVE
         THEN { m \in M : Home[m[1]] = new }
         ELSE { m \in M : Home[m[1]] \in Reach(new, M) }

Pivot(new) ==
    /\ new # root
    /\ root' = new
    /\ mounts' = Keep(new, mounts)
    /\ lost' = lost \cup { m \in mounts \ Keep(new, mounts) :
                              Home[m[1]] \in Reach(new, mounts) }
    /\ fresh' = TRUE
    /\ UNCHANGED Home

Next ==
    \/ \E pt \in Points, s \in Trees : Mount(pt, s)
    \/ \E pt \in Points, s \in Trees : Unmount(pt, s)
    \/ \E new \in Trees : Pivot(new)

Spec == Init /\ [][Next]_vars

TypeOk ==
    /\ Home \in [Points -> Trees]
    /\ root \in Trees
    /\ mounts \subseteq (Points \X Trees)
    /\ lost \subseteq (Points \X Trees)
    /\ fresh \in BOOLEAN

ShedLosesNothing == lost = {}

NoResidueAfterPivot ==
    fresh => \A m \in mounts : Home[m[1]] \in Reach(root, mounts)

\* The shed never changes what is reachable from the new root: the closure
\* over the kept entries equals the closure over all of them. Stated on the
\* post-state using the history set: nothing reachable was lost, so the two
\* closures coincide.
Invariants == TypeOk /\ ShedLosesNothing /\ NoResidueAfterPivot
=============================================================================
