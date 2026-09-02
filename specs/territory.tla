---- MODULE territory ----
(***************************************************************************)
(* Thylacine territory — P2-E + P5-attach-mount + UM (union mounts) spec. *)
(*                                                                         *)
(* Models the Plan 9 territory primitives — `bind` + `mount` + the         *)
(* corresponding cycle-freedom + isolation + mount-refcount invariants per *)
(* ARCHITECTURE.md §9.1 + §9.6 + §28 I-1 (territory operations don't       *)
(* affect other procs' territories) + I-3 (mount points form a DAG, never  *)
(* a cycle) + the mount-lifecycle invariants from §9.6.6 — AND, since the  *)
(* union-mounts arc (UM, 2026-09-02), the union WALK / READDIR / CREATE /   *)
(* ORDERING semantics that ARCH §9.6 (line 1785) declares but the v1.0     *)
(* walk never grew.                                                        *)
(*                                                                         *)
(* State layers:                                                           *)
(*                                                                         *)
(*   bindings: the bind graph. `bindings[p][dst]` is the SET of source     *)
(*   paths bound at `dst` in proc p's territory. Plan 9 `bind(old, new)`   *)
(*   makes `old`'s contents visible at `new`; we model `new`'s binding     *)
(*   LIST since unions stack multiple bindings at one mount point. Walking *)
(*   `dst` yields each `src \in bindings[p][dst]`. NoCycle guards it.      *)
(*                                                                         *)
(*   morder: the ORDERED mount table (UM). `morder[p][pt]` is a SEQUENCE   *)
(*   of member records `[s |-> Spoor, mb |-> BOOLEAN, mc |-> BOOLEAN]`     *)
(*   grafted at mount point `pt` in proc p's territory, IN DECLARED SEARCH *)
(*   ORDER (index 1 searched first). This REPLACES the prior unordered     *)
(*   `mounts` set: the union walk needs order, and one representation      *)
(*   avoids a set/seq consistency gap. Per ARCH §9.6, every filesystem     *)
(*   entity is a Spoor; mount grafts one at a point. `mb` records that the *)
(*   member was mounted MBEFORE (else MAFTER); `mc` records MCREATE.       *)
(*   MBEFORE prepends (searched earlier), MAFTER appends, MREPL replaces   *)
(*   the whole sequence with a single member. The mounted-ON directory's   *)
(*   own contents are NOT an implicit member (ARCH §9.6: "check the mount  *)
(*   table" — grafted sources only; this also sidesteps the self-mount     *)
(*   cycle check).                                                         *)
(*                                                                         *)
(*   holds: member CONTENTS. `holds[s]` is the set of component Names the  *)
(*   directory-Spoor s contains. Fixed at Init (a member's contents don't  *)
(*   change in this model) and explored over all assignments for coverage. *)
(*   Drives the union walk (first member holding the name) and readdir     *)
(*   (merge + dedup).                                                      *)
(*                                                                         *)
(*   root_spoor: name-resolution root per proc (SYS_WALK_OPEN spoor_fd==-1 *)
(*   sentinel). NONE before the first Chroot. Contributes one refcount.    *)
(*                                                                         *)
(*   refcount: per-Spoor kernel refcount = member occurrences across all   *)
(*   morder sequences + root_spoor contributions. MountRefcountConsistency *)
(*   relates it to the true count.                                         *)
(*                                                                         *)
(* Isolation (I-1) is structural: morder[p] / bindings[p] for p # q are   *)
(* independent function values; no action updates two procs' slots at once.*)
(* RFNAMEG-shared territories are NOT modeled (v1.0 impl: rfork extincts   *)
(* on non-RFPROC flags).                                                   *)
(*                                                                         *)
(* Union semantics (UM — the new load-bearing content of this arc):        *)
(*   - WalkSel(p, pt, nm)   — the member walk `pt/nm` resolves to: the     *)
(*                            FIRST member (declared order) whose holds     *)
(*                            contains nm, else NONE. WalkFirstHit proves   *)
(*                            it is the earliest holder. BUGGY_WALK_LAST_   *)
(*                            HIT selects the last holder -> counterexample.*)
(*   - ReaddirSel(p, pt)    — the union directory listing: one <<nm, mbr>> *)
(*                            per name held by ANY member, mbr = the FIRST  *)
(*                            holder (Plan 9 dedup, first-member-wins).     *)
(*                            ReaddirDedupFirstWins proves it. BUGGY_       *)
(*                            READDIR_LAST_WINS keeps the last holder.      *)
(*   - CreateSel(p, pt)     — where a create at the union lands: the FIRST  *)
(*                            member with mc=TRUE (MCREATE), else NONE      *)
(*                            (no writable member -> create fails, never    *)
(*                            silently in a read-only member). BUGGY_       *)
(*                            CREATE_ANY_MEMBER picks the first member      *)
(*                            regardless of mc.                             *)
(*   - OrderCorrect         — every MBEFORE member precedes every MAFTER    *)
(*                            member in the sequence. BuggyMountOrder       *)
(*                            appends an MBEFORE member -> violated.        *)
(*                                                                         *)
(* Mount lifecycle (§9.6.6):                                               *)
(*   - MountBefore/After/Repl(p, s, pt, mc) — graft s at pt; bump          *)
(*     refcount[s] (Repl first drops the replaced members' refs).          *)
(*   - Unmount(p, s, pt)     — remove member s at pt; drop refcount[s].     *)
(*   - ForkClone(parent,child)— deep-copy morder[parent]; bump refcount    *)
(*                              per cloned member + cloned root.            *)
(*   - BuggyDestroyLeak(p)    — clears morder[p] WITHOUT dropping refs.     *)
(*                                                                         *)
(* Buggy-config matrix (executable documentation per CLAUDE.md spec-first  *)
(* policy):                                                                *)
(*   territory.cfg                       all flags FALSE — invariants hold. *)
(*   territory_buggy.cfg                 BUGGY_CYCLE — cyclic bind graph.    *)
(*   territory_buggy_mount_no_refbump.cfg BUGGY_MOUNT_NO_REFBUMP.           *)
(*   territory_buggy_unmount_no_refdrop.cfg BUGGY_UNMOUNT_NO_REFDROP.       *)
(*   territory_buggy_destroy_leak.cfg    BUGGY_DESTROY_LEAK.                *)
(*   territory_buggy_chroot_no_refbump.cfg BUGGY_CHROOT_NO_REFBUMP.         *)
(*   territory_buggy_mount_order.cfg     BUGGY_MOUNT_ORDER — MBEFORE after  *)
(*                                       MAFTER; OrderCorrect fails.        *)
(*   territory_buggy_walk_last_hit.cfg   BUGGY_WALK_LAST_HIT — walk returns *)
(*                                       the last holder; WalkFirstHit fails.*)
(*   territory_buggy_readdir_last_wins.cfg BUGGY_READDIR_LAST_WINS — dedup  *)
(*                                       keeps the last holder; fails.      *)
(*   territory_buggy_create_any_member.cfg BUGGY_CREATE_ANY_MEMBER — create *)
(*                                       ignores MCREATE; CreateTargetCorrect*)
(*                                       fails.                             *)
(*                                                                         *)
(* See ARCHITECTURE.md §9 (territory) + §9.6 (mount + union) + §28 I-1,I-3.*)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
    Procs,                     \* set of process identifiers
    Paths,                     \* set of path identifiers (bind nodes + mount points)
    Spoors,                    \* set of Spoor (member / source) identifiers
    Names,                     \* set of component names (for union walk / readdir)
    BUGGY_CYCLE,               \* BuggyBind skips cycle check
    BUGGY_MOUNT_NO_REFBUMP,    \* BuggyMount skips refcount bump
    BUGGY_UNMOUNT_NO_REFDROP,  \* BuggyUnmount skips refcount drop
    BUGGY_DESTROY_LEAK,        \* BuggyDestroy clears morder[p] w/o ref drop
    BUGGY_CHROOT_NO_REFBUMP,   \* BuggyChroot stamps root w/o ref adjust
    BUGGY_MOUNT_ORDER,         \* BuggyMountOrder appends an MBEFORE member
    BUGGY_WALK_LAST_HIT,       \* WalkSel returns the LAST holder
    BUGGY_READDIR_LAST_WINS,   \* ReaddirSel dedups to the LAST holder
    BUGGY_CREATE_ANY_MEMBER,   \* CreateSel ignores MCREATE (first member)
    BUGGY_REMOVE_MCREATE_MEMBER \* RemoveSel picks the MCREATE member, not the holder

ASSUME Cardinality(Procs) >= 1
ASSUME Cardinality(Paths) >= 2
ASSUME Cardinality(Spoors) >= 1
ASSUME Cardinality(Names) >= 1
ASSUME BUGGY_CYCLE \in BOOLEAN
ASSUME BUGGY_MOUNT_NO_REFBUMP \in BOOLEAN
ASSUME BUGGY_UNMOUNT_NO_REFDROP \in BOOLEAN
ASSUME BUGGY_DESTROY_LEAK \in BOOLEAN
ASSUME BUGGY_CHROOT_NO_REFBUMP \in BOOLEAN
ASSUME BUGGY_MOUNT_ORDER \in BOOLEAN
ASSUME BUGGY_WALK_LAST_HIT \in BOOLEAN
ASSUME BUGGY_READDIR_LAST_WINS \in BOOLEAN
ASSUME BUGGY_CREATE_ANY_MEMBER \in BOOLEAN
ASSUME BUGGY_REMOVE_MCREATE_MEMBER \in BOOLEAN

(***************************************************************************)
(* NONE — sentinel for "no value" (a Proc's un-pivoted root_spoor, or a    *)
(* walk/readdir/create with no matching member). A string, guaranteed      *)
(* distinct from the symbolic Spoor model values (s1, s2, ...).            *)
(***************************************************************************)
NONE == "NONE"

(***************************************************************************)
(* A mount-table member: the grafted Spoor `s`, `mb` = mounted MBEFORE     *)
(* (else MAFTER), `mc` = MCREATE (creates may land here).                  *)
(***************************************************************************)
Member == [s : Spoors, mb : BOOLEAN, mc : BOOLEAN]

VARIABLES
    bindings,      \* [Procs -> [Paths -> SUBSET Paths]]
    morder,        \* [Procs -> [Paths -> Seq(Member)]]  — ordered mount table
    root_spoor,    \* [Procs -> Spoors \cup {NONE}]
    refcount,      \* [Spoors -> Nat]
    holds          \* [Spoors -> SUBSET Names]  — member contents (fixed at Init)

vars == <<bindings, morder, root_spoor, refcount, holds>>

TypeOk ==
    /\ bindings \in [Procs -> [Paths -> SUBSET Paths]]
    /\ morder \in [Procs -> [Paths -> Seq(Member)]]
    /\ root_spoor \in [Procs -> Spoors \cup {NONE}]
    /\ refcount \in [Spoors -> Nat]
    /\ holds \in [Spoors -> SUBSET Names]

(***************************************************************************)
(* Set min / max over naturals (TLA+ has no built-ins).                    *)
(***************************************************************************)
SetMin(S) == CHOOSE x \in S : \A y \in S : x <= y
SetMax(S) == CHOOSE x \in S : \A y \in S : x >= y

(***************************************************************************)
(* Member helpers over morder[p][pt].                                      *)
(***************************************************************************)
MemberSpoors(p, pt) == { morder[p][pt][i].s : i \in DOMAIN morder[p][pt] }
HasMember(p, pt, s) == \E i \in DOMAIN morder[p][pt] : morder[p][pt][i].s = s

(***************************************************************************)
(* Reachable(p, S) — transitive closure of S through proc p's bind graph.  *)
(***************************************************************************)
RECURSIVE ReachableImpl(_, _)
ReachableImpl(p, S) ==
    LET S2 == S \cup UNION { bindings[p][y] : y \in S }
    IN  IF S2 = S THEN S ELSE ReachableImpl(p, S2)

Reachable(p, S) == ReachableImpl(p, S)

WouldCreateCycle(p, src, dst) ==
    \/ src = dst
    \/ dst \in Reachable(p, {src})

(***************************************************************************)
(* MountEntriesForSpoor(s) — (p, pt) pairs where s is a member. Cardinality*)
(* gives the true mount-contribution count for refcount[s].                *)
(***************************************************************************)
MountEntriesForSpoor(s) ==
    { pair \in Procs \X Paths : HasMember(pair[1], pair[2], s) }

(***************************************************************************)
(* Init: empty territories; refcount 0; holds any fixed assignment.        *)
(***************************************************************************)
Init ==
    /\ bindings = [p \in Procs |-> [path \in Paths |-> {}]]
    /\ morder = [p \in Procs |-> [pt \in Paths |-> << >>]]
    /\ root_spoor = [p \in Procs |-> NONE]
    /\ refcount = [s \in Spoors |-> 0]
    /\ holds \in [Spoors -> SUBSET Names]

(***************************************************************************)
(* ================================= BIND ================================== *)
(***************************************************************************)

Bind(p, src, dst) ==
    /\ ~WouldCreateCycle(p, src, dst)
    /\ src \notin bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \cup {src}]
    /\ UNCHANGED <<morder, root_spoor, refcount, holds>>

BuggyBind(p, src, dst) ==
    /\ BUGGY_CYCLE
    /\ src # dst
    /\ src \notin bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \cup {src}]
    /\ UNCHANGED <<morder, root_spoor, refcount, holds>>

Unbind(p, src, dst) ==
    /\ src \in bindings[p][dst]
    /\ bindings' = [bindings EXCEPT ![p][dst] = @ \ {src}]
    /\ UNCHANGED <<morder, root_spoor, refcount, holds>>

(***************************************************************************)
(* ================================ MOUNT ================================== *)
(***************************************************************************)

(***************************************************************************)
(* MountBefore(p, s, pt, mc) — graft s at pt, MBEFORE: PREPEND (searched   *)
(* earliest). Idempotent: no-op if s is already a member (the impl         *)
(* converges flags without a new ref; the spec models the no-op as "does   *)
(* not fire" via the ~HasMember precondition). Bumps refcount[s].          *)
(* Maps to `kernel/territory.c::mount` with MBEFORE.                        *)
(***************************************************************************)
MountBefore(p, s, pt, mc) ==
    /\ ~HasMember(p, pt, s)
    /\ morder' = [morder EXCEPT ![p][pt] =
                    <<[s |-> s, mb |-> TRUE, mc |-> mc]>> \o @]
    /\ refcount' = [refcount EXCEPT ![s] = @ + 1]
    /\ UNCHANGED <<bindings, root_spoor, holds>>

(***************************************************************************)
(* MountAfter(p, s, pt, mc) — graft s at pt, MAFTER: APPEND (searched last).*)
(***************************************************************************)
MountAfter(p, s, pt, mc) ==
    /\ ~HasMember(p, pt, s)
    /\ morder' = [morder EXCEPT ![p][pt] =
                    Append(@, [s |-> s, mb |-> FALSE, mc |-> mc])]
    /\ refcount' = [refcount EXCEPT ![s] = @ + 1]
    /\ UNCHANGED <<bindings, root_spoor, holds>>

(***************************************************************************)
(* MountRepl(p, s, pt) — MREPL: replace the whole sequence at pt with the  *)
(* single member s. Drops one ref for each replaced member, then bumps s.  *)
(* The functional refcount update handles s possibly being a replaced      *)
(* member (net delta then +1). Precondition: not already the sole member   *)
(* (the impl's no-op re-mount), else the step is a non-event.              *)
(***************************************************************************)
MountRepl(p, s, pt) ==
    /\ morder[p][pt] # <<[s |-> s, mb |-> FALSE, mc |-> FALSE]>>
    /\ morder' = [morder EXCEPT ![p][pt] =
                    <<[s |-> s, mb |-> FALSE, mc |-> FALSE]>>]
    /\ refcount' = [x \in Spoors |->
                       refcount[x]
                       - (IF \E i \in DOMAIN morder[p][pt] :
                                morder[p][pt][i].s = x THEN 1 ELSE 0)
                       + (IF x = s THEN 1 ELSE 0)]
    /\ UNCHANGED <<bindings, root_spoor, holds>>

(***************************************************************************)
(* BuggyMountNoRefbump(p, s, pt) — MAFTER without the refcount bump.       *)
(***************************************************************************)
BuggyMountNoRefbump(p, s, pt) ==
    /\ BUGGY_MOUNT_NO_REFBUMP
    /\ ~HasMember(p, pt, s)
    /\ morder' = [morder EXCEPT ![p][pt] =
                    Append(@, [s |-> s, mb |-> FALSE, mc |-> FALSE])]
    /\ UNCHANGED <<bindings, root_spoor, refcount, holds>>

(***************************************************************************)
(* BuggyMountOrder(p, s, pt) — bug class: an MBEFORE member is APPENDED    *)
(* instead of prepended. If an MAFTER member already sits ahead of it, the *)
(* sequence then has an mb=TRUE member AFTER an mb=FALSE one -> OrderCorrect*)
(* violated (an MBEFORE source that should be searched first is searched   *)
(* last). Refcount is still bumped (only the ORDER is wrong).              *)
(***************************************************************************)
BuggyMountOrder(p, s, pt) ==
    /\ BUGGY_MOUNT_ORDER
    /\ ~HasMember(p, pt, s)
    /\ morder' = [morder EXCEPT ![p][pt] =
                    Append(@, [s |-> s, mb |-> TRUE, mc |-> FALSE])]
    /\ refcount' = [refcount EXCEPT ![s] = @ + 1]
    /\ UNCHANGED <<bindings, root_spoor, holds>>

(***************************************************************************)
(* Unmount(p, s, pt) — remove member s at pt; drop refcount[s]. SelectSeq  *)
(* filters the one matching member (>=1 by HasMember; exactly 1 by the     *)
(* mount idempotency preconditions).                                       *)
(***************************************************************************)
Unmount(p, s, pt) ==
    /\ HasMember(p, pt, s)
    /\ morder' = [morder EXCEPT ![p][pt] =
                    SelectSeq(@, LAMBDA m : m.s # s)]
    /\ refcount' = [refcount EXCEPT ![s] = @ - 1]
    /\ UNCHANGED <<bindings, root_spoor, holds>>

BuggyUnmountNoRefdrop(p, s, pt) ==
    /\ BUGGY_UNMOUNT_NO_REFDROP
    /\ HasMember(p, pt, s)
    /\ morder' = [morder EXCEPT ![p][pt] =
                    SelectSeq(@, LAMBDA m : m.s # s)]
    /\ UNCHANGED <<bindings, root_spoor, refcount, holds>>

(***************************************************************************)
(* ================================ CHROOT ================================= *)
(***************************************************************************)

Chroot(p, s) ==
    /\ root_spoor[p] # s
    /\ root_spoor' = [root_spoor EXCEPT ![p] = s]
    /\ refcount' = IF root_spoor[p] = NONE
                   THEN [refcount EXCEPT ![s] = @ + 1]
                   ELSE [refcount EXCEPT ![s] = @ + 1,
                                        ![root_spoor[p]] = @ - 1]
    /\ UNCHANGED <<bindings, morder, holds>>

BuggyChrootNoRefbump(p, s) ==
    /\ BUGGY_CHROOT_NO_REFBUMP
    /\ root_spoor[p] # s
    /\ root_spoor' = [root_spoor EXCEPT ![p] = s]
    /\ UNCHANGED <<bindings, morder, refcount, holds>>

(***************************************************************************)
(* ForkClone(parent, child) — deep-copy parent's territory into child's.   *)
(* Each cloned member contributes a new ref; the cloned root_spoor (if     *)
(* non-NONE) contributes one. Precondition: child in Init state.           *)
(***************************************************************************)
ChildMemberCount(parent, s) ==
    Cardinality({ pt \in Paths : HasMember(parent, pt, s) })

ForkClone(parent, child) ==
    /\ parent # child
    /\ bindings[child] = [path \in Paths |-> {}]
    /\ morder[child] = [pt \in Paths |-> << >>]
    /\ root_spoor[child] = NONE
    /\ bindings' = [bindings EXCEPT ![child] = bindings[parent]]
    /\ morder' = [morder EXCEPT ![child] = morder[parent]]
    /\ root_spoor' = [root_spoor EXCEPT ![child] = root_spoor[parent]]
    /\ refcount' = [s \in Spoors |->
                       refcount[s]
                       + ChildMemberCount(parent, s)
                       + (IF root_spoor[parent] = s THEN 1 ELSE 0)]
    /\ UNCHANGED holds

(***************************************************************************)
(* BuggyDestroyLeak(p) — clears morder[p] + root_spoor[p] WITHOUT dropping *)
(* refcounts. Catches the territory_unref final-release leak.              *)
(***************************************************************************)
BuggyDestroyLeak(p) ==
    /\ BUGGY_DESTROY_LEAK
    /\ (\E pt \in Paths : morder[p][pt] # << >>) \/ root_spoor[p] # NONE
    /\ morder' = [morder EXCEPT ![p] = [pt \in Paths |-> << >>]]
    /\ root_spoor' = [root_spoor EXCEPT ![p] = NONE]
    /\ UNCHANGED <<bindings, refcount, holds>>

(***************************************************************************)
(* ========================= UNION SEMANTICS (UM) ========================= *)
(***************************************************************************)

(***************************************************************************)
(* HolderIdxs(p, pt, nm) — sequence indices of members whose directory     *)
(* holds component `nm`.                                                   *)
(***************************************************************************)
HolderIdxs(p, pt, nm) ==
    { i \in DOMAIN morder[p][pt] : nm \in holds[morder[p][pt][i].s] }

FirstHolder(p, pt, nm) ==
    LET idxs == HolderIdxs(p, pt, nm)
    IN  IF idxs = {} THEN NONE ELSE morder[p][pt][SetMin(idxs)].s

LastHolder(p, pt, nm) ==
    LET idxs == HolderIdxs(p, pt, nm)
    IN  IF idxs = {} THEN NONE ELSE morder[p][pt][SetMax(idxs)].s

(***************************************************************************)
(* WalkSel — where walk `pt/nm` lands. Correct: the first holder. Buggy    *)
(* (BUGGY_WALK_LAST_HIT): the last holder.                                 *)
(***************************************************************************)
WalkSel(p, pt, nm) ==
    IF BUGGY_WALK_LAST_HIT THEN LastHolder(p, pt, nm)
                           ELSE FirstHolder(p, pt, nm)

(***************************************************************************)
(* Names held by ANY member at pt.                                         *)
(***************************************************************************)
NamesAt(p, pt) == UNION { holds[morder[p][pt][i].s] : i \in DOMAIN morder[p][pt] }

(***************************************************************************)
(* ReaddirSel — the union directory listing: one <<nm, member>> per name   *)
(* held by any member. Correct: member = first holder (dedup, first-wins). *)
(* Buggy (BUGGY_READDIR_LAST_WINS): member = last holder.                  *)
(***************************************************************************)
ReaddirSel(p, pt) ==
    { <<nm, IF BUGGY_READDIR_LAST_WINS THEN LastHolder(p, pt, nm)
                                       ELSE FirstHolder(p, pt, nm)>>
      : nm \in NamesAt(p, pt) }

ReaddirCorrect(p, pt) ==
    { <<nm, FirstHolder(p, pt, nm)>> : nm \in NamesAt(p, pt) }

(***************************************************************************)
(* CreateSel — where a create at the union lands. Correct: the first       *)
(* member with mc=TRUE, else NONE. Buggy (BUGGY_CREATE_ANY_MEMBER): the    *)
(* first member regardless of mc (NONE only if there are no members).      *)
(***************************************************************************)
McIdxs(p, pt) == { i \in DOMAIN morder[p][pt] : morder[p][pt][i].mc }

FirstCreateMember(p, pt) ==
    LET idxs == McIdxs(p, pt)
    IN  IF idxs = {} THEN NONE ELSE morder[p][pt][SetMin(idxs)].s

CreateSel(p, pt) ==
    IF BUGGY_CREATE_ANY_MEMBER
    THEN IF morder[p][pt] = << >> THEN NONE ELSE morder[p][pt][1].s
    ELSE FirstCreateMember(p, pt)

(***************************************************************************)
(* RemoveSel — where a REMOVE (unlink / rmdir / rename source) at a union  *)
(* lands. Correct: the FIRST HOLDER (the member whose directory holds nm)  *)
(* -- the entry is removed from the member that actually has it. This is   *)
(* identical to WalkSel: a remove first RESOLVES the leaf (first-hit), then *)
(* mutates that member. Buggy (BUGGY_REMOVE_MCREATE_MEMBER): the create    *)
(* target (FirstCreateMember) -- the UM-7 F3 bug, which routed remove       *)
(* through STALK_CREATE and so acted on the writable member instead of the *)
(* holder (`rm foo && test -e foo` could be TRUE, or a shadow was unlinked).*)
(***************************************************************************)
RemoveSel(p, pt, nm) ==
    IF BUGGY_REMOVE_MCREATE_MEMBER THEN FirstCreateMember(p, pt)
                                   ELSE FirstHolder(p, pt, nm)

(***************************************************************************)
(* ============================== ACTIONS ================================= *)
(***************************************************************************)

Next ==
    \/ \E p \in Procs, src \in Paths, dst \in Paths : Bind(p, src, dst)
    \/ \E p \in Procs, src \in Paths, dst \in Paths : BuggyBind(p, src, dst)
    \/ \E p \in Procs, src \in Paths, dst \in Paths : Unbind(p, src, dst)
    \/ \E p \in Procs, s \in Spoors, pt \in Paths, mc \in BOOLEAN : MountBefore(p, s, pt, mc)
    \/ \E p \in Procs, s \in Spoors, pt \in Paths, mc \in BOOLEAN : MountAfter(p, s, pt, mc)
    \/ \E p \in Procs, s \in Spoors, pt \in Paths                 : MountRepl(p, s, pt)
    \/ \E p \in Procs, s \in Spoors, pt \in Paths : BuggyMountNoRefbump(p, s, pt)
    \/ \E p \in Procs, s \in Spoors, pt \in Paths : BuggyMountOrder(p, s, pt)
    \/ \E p \in Procs, s \in Spoors, pt \in Paths : Unmount(p, s, pt)
    \/ \E p \in Procs, s \in Spoors, pt \in Paths : BuggyUnmountNoRefdrop(p, s, pt)
    \/ \E p \in Procs, s \in Spoors                 : Chroot(p, s)
    \/ \E p \in Procs, s \in Spoors                 : BuggyChrootNoRefbump(p, s)
    \/ \E parent, child \in Procs                   : ForkClone(parent, child)
    \/ \E p \in Procs                               : BuggyDestroyLeak(p)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(***************************************************************************)
(* NoCycle — the bind graph in every proc's territory is acyclic (I-3).    *)
(***************************************************************************)
NoCycle ==
    \A p \in Procs, x \in Paths :
        x \notin Reachable(p, bindings[p][x])

(***************************************************************************)
(* MountRefcountConsistency — refcount[s] equals member occurrences across *)
(* all morder sequences + root_spoor contributions.                        *)
(***************************************************************************)
MountRefcountConsistency ==
    \A s \in Spoors :
        refcount[s] = Cardinality(MountEntriesForSpoor(s))
                    + Cardinality({p \in Procs : root_spoor[p] = s})

MountRefcountNonNegative ==
    \A s \in Spoors : refcount[s] >= 0

(***************************************************************************)
(* WalkFirstHit (UM, I-28 union walk) — a union walk lands on the earliest *)
(* member holding the name. Stated as: WalkSel = FirstHolder always. The   *)
(* correct action makes them equal; BUGGY_WALK_LAST_HIT makes WalkSel the  *)
(* last holder, which differs whenever >=2 members hold the name.          *)
(***************************************************************************)
WalkFirstHit ==
    \A p \in Procs, pt \in Paths, nm \in Names :
        WalkSel(p, pt, nm) = FirstHolder(p, pt, nm)

(***************************************************************************)
(* ReaddirDedupFirstWins (UM) — the union listing is complete, deduplicated*)
(* by name, and each name resolves to its first holder.                    *)
(***************************************************************************)
ReaddirDedupFirstWins ==
    \A p \in Procs, pt \in Paths :
        ReaddirSel(p, pt) = ReaddirCorrect(p, pt)

(***************************************************************************)
(* CreateTargetCorrect (UM) — a create at a union lands in the first       *)
(* MCREATE member (or nowhere if none is MCREATE).                         *)
(***************************************************************************)
CreateTargetCorrect ==
    \A p \in Procs, pt \in Paths :
        CreateSel(p, pt) = FirstCreateMember(p, pt)

(***************************************************************************)
(* RemoveTargetCorrect (UM, UM-7 F3) — a remove of nm at a union acts on   *)
(* the FIRST member holding nm, never the MCREATE member by virtue of      *)
(* being writable. Stated as RemoveSel = FirstHolder always; the correct   *)
(* action makes them equal, BUGGY_REMOVE_MCREATE_MEMBER makes RemoveSel    *)
(* the create target, which differs whenever the first holder is not the   *)
(* first MCREATE member (the F3 mis-selection).                            *)
(***************************************************************************)
RemoveTargetCorrect ==
    \A p \in Procs, pt \in Paths, nm \in Names :
        RemoveSel(p, pt, nm) = FirstHolder(p, pt, nm)

(***************************************************************************)
(* OrderCorrect (UM) — in every mount sequence, every MBEFORE member       *)
(* precedes every MAFTER member (declared search order).                   *)
(***************************************************************************)
OrderCorrect ==
    \A p \in Procs, pt \in Paths :
        \A i, j \in DOMAIN morder[p][pt] :
            (i < j /\ ~morder[p][pt][i].mb) => ~morder[p][pt][j].mb

Invariants ==
    /\ TypeOk
    /\ NoCycle
    /\ MountRefcountConsistency
    /\ MountRefcountNonNegative
    /\ WalkFirstHit
    /\ ReaddirDedupFirstWins
    /\ CreateTargetCorrect
    /\ RemoveTargetCorrect
    /\ OrderCorrect

(***************************************************************************)
(* StateConstraint — a TLC exploration bound (NOT part of the spec's      *)
(* meaning). Caps the number of non-empty mount points so the ordered-    *)
(* member state space stays finite-and-small; every buggy counterexample  *)
(* needs at most two non-empty points (one point with two members for the *)
(* walk/readdir/order/create bugs, plus a fork target), so the bound does *)
(* not hide any modeled defect.                                            *)
(***************************************************************************)
StateConstraint ==
    Cardinality({ pp \in Procs \X Paths : Len(morder[pp[1]][pp[2]]) > 0 }) <= 2

====
