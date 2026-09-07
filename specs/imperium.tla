---- MODULE imperium ----
(***************************************************************************)
(* Thylacine imperium -- the fork-PROPAGATING legate scope                  *)
(* (docs/IMPERIUM-DESIGN.md section 11.4; ARCH section 28 I-25 STRENGTHENED *)
(* + I-2). The IM-2 kernel mechanism: a clearance grant redeemed with the   *)
(* PROPAGATING flag creates a legate scope whose elevated caps FLOW to the   *)
(* root's rfork descendants (members), so an elevated sub-shell's children  *)
(* are elevated too -- and every one of them dies with the scope.           *)
(*                                                                         *)
(* Spec-first RE-ENABLED for this surface (user-directed 2026-09-07; the    *)
(* eighth "RE-ENABLED for ..." instance, specs/SPEC-TO-CODE.md). The two    *)
(* hazards are interleavings a model checker finds and prose talks itself   *)
(* past: the fork-vs-teardown race (a child linked into the table AFTER the *)
(* teardown sweep walked past its parent -- benign at v1.0 because members  *)
(* were never elevated, LOAD-BEARING now that they are) and the nested-     *)
(* redeem re-tag (a member's further redeem allocating a FRESH scope,        *)
(* walking its elevated caps out of the imperium teardown -- the A-4a F2     *)
(* shape).                                                                  *)
(*                                                                         *)
(* WHAT THIS MODELS -- the KERNEL mechanism, not corvus's policy.           *)
(*   Procs carry, kernel-side:                                              *)
(*     state[p]       -- "unborn" | "alive" | "terminating" | "dead"       *)
(*                       (terminating = group_exit_msg set; the Proc dies   *)
(*                       at its EL0-return die-check, which is on the       *)
(*                       RETURN tail only, so an in-flight syscall          *)
(*                       completes first: the pre-existing I-24 semantics,  *)
(*                       one syscall per thread, inherited here unchanged). *)
(*     scope[p]       -- legate_scope_id (0 = none).                        *)
(*     root[p]        -- PROC_FLAG_LEGATE_ROOT (never inherited; persists   *)
(*                       on the dead root, as the flag does in proc_flags). *)
(*     propagating[p] -- the scope property as carried by this Proc (root:  *)
(*                       from the grant's flag; member: inherited).         *)
(*     lcaps[p]       -- legate_caps: the set that FLOWS at rfork.          *)
(*     caps[p]        -- the ELEVATION-ONLY caps the Proc holds (the model  *)
(*                       tracks only these; fork-grantable caps are         *)
(*                       orthogonal and unchanged).                         *)
(*     flowed[p]      -- history: the caps that flowed IN at p's fork.      *)
(*     first[p]       -- history: what p's scope looked like when p JOINED  *)
(*                       it (scope / propagating / lcaps / via redeem or    *)
(*                       fork / the ANCHOR root whose life bounds p's caps) *)
(*                       -- the set-once witnesses + the I-25 anchor.       *)
(*   Caps is the elevation-only universe (opaque tokens). A grant's cap set *)
(*   G is any nonempty subset (corvus's self-restricted level).             *)
(*                                                                         *)
(* THE RULES (11.4):                                                        *)
(*   RedeemFresh(p, G, prop): scope[p] = 0 -> a FRESH scope; p is its ROOT; *)
(*     propagating := prop; lcaps := G; caps := caps U G.                    *)
(*   RedeemFurther(p, G): scope[p] # 0 -> OR G into caps, keep the tag, the *)
(*     root status, the propagating property and lcaps (CAP_JIT under        *)
(*     imperium: the member keeps the imperium tag and the JIT cap does NOT  *)
(*     flow to its children). A PROPAGATING redeem on a scoped Proc is       *)
(*     REFUSED (not enabled) -- propagating never nests; abdicate first.     *)
(*   Fork(parent, child): child->caps = (parent & mask) & ~(ELEVATION_ONLY *)
(*     & ~flow), flow = lcaps[parent] iff propagating[parent]; the child   *)
(*     JOINS the scope as a MEMBER (root FALSE), propagating inherited as a *)
(*     member property, lcaps[child] = flow. REFUSED while the parent is    *)
(*     terminating: the child's tag + caps are copied BEFORE the child is   *)
(*     linked into the table, so the only straggler shape is a sweep that   *)
(*     runs between the copy and the link -- and that sweep marks the       *)
(*     PARENT (which is in the table). A re-check of the parent's           *)
(*     group_exit_msg under the SAME g_proc_table_lock hold as the link     *)
(*     therefore sees it. Fork is one atomic step here BECAUSE of that       *)
(*     critical section; BUGGY_STRAGGLER is the check being absent, or      *)
(*     taken outside it (stale).                                            *)
(*   Exit(p): a clean exit -> dead at once (exits -> proc_become_zombie_-   *)
(*     locked). If p is a root, the SWEEP fires in the same lock hold:      *)
(*     every other alive member of the scope is marked terminating.        *)
(*   Terminate(p): a kill / exit_group / the sweep's own marking -> p is    *)
(*     terminating; NO sweep yet (the sweep rides the ZOMBIE transition).   *)
(*     So a KILLED root leaves its members alive and elevated until it      *)
(*     reaches its die-check -- the root is doomed but the scope is not     *)
(*     ended, and the members are caught by the sweep when it is.          *)
(*   Die(p): a terminating Proc reaches its die-check -> dead; if p is a    *)
(*     root the sweep fires now (proc_become_zombie_locked again).         *)
(*   Expire(p): ANY alive member's EL0 tail observes valid_until passed ->  *)
(*     every alive Proc of the scope, root included, is marked terminating  *)
(*     (proc_for_each(legate_teardown_cb, except = NULL)).                  *)
(*                                                                         *)
(* THE BUG CLASSES (executable counterexamples; CLAUDE.md spec-first):      *)
(*   BUGGY_STRAGGLER          -- Fork admits a TERMINATING parent: a child   *)
(*     born after the sweep walked past its parent is ALIVE + ELEVATED with  *)
(*     no live root. Caught by NoElevatedOutlivesScope. (Note what the model *)
(*     ALSO says: a child forked from a parent that is terminating for any   *)
(*     OTHER reason -- its own kill, or the root merely flagged but not yet  *)
(*     dead -- is caught by the root's later zombie sweep, because the tag   *)
(*     walk covers the whole table. Only the post-sweep parent is dangerous; *)
(*     the under-lock re-check closes every shape uniformly.)               *)
(*   BUGGY_RETAG              -- a further redeem on a scoped Proc allocates  *)
(*     a FRESH scope + root (the pre-IM-2 proc_become_legate shape): the      *)
(*     member walks its flowed caps out of the imperium teardown. Caught by  *)
(*     OneScopePerProc (depth 2) and MembersNeverRoot (depth 3); the         *)
(*     PRIVILEGE consequence is NoElevatedOutlivesScope at depth 4, which is *)
(*     what imperium_buggy_retag.cfg checks so the trace shows the escape.   *)
(*   BUGGY_FLOW_WITHOUT_FLAG  -- caps flow at Fork under a NON-propagating    *)
(*     scope (the v1.0 `& ~CAP_ELEVATION_ONLY` strip removed wholesale        *)
(*     instead of carved by the flag). Caught by FlowOnlyUnderPropagating.   *)
(*   BUGGY_NEST_ADMITTED      -- a PROPAGATING redeem on a scoped Proc is     *)
(*     admitted as a further redeem that also flips the flag and widens the  *)
(*     flowing set. Caught by ScopeTraitsSetOnce (the G6 companion: what     *)
(*     flows, and whether it flows, are fixed when the scope is joined).     *)
(*                                                                         *)
(* INVARIANTS (TLC-checked) -- the legs of I-25 STRENGTHENED:               *)
(*   NoElevatedOutlivesScope -- an ALIVE Proc holding elevated caps has a   *)
(*                              LIVE (alive or terminating) ANCHOR: the     *)
(*                              root Proc of the scope it first joined,     *)
(*                              by identity (a re-tag moves the scope       *)
(*                              number; it cannot move the anchor).         *)
(*   FlowOnlyUnderPropagating -- caps that FLOWED into a Proc did so under  *)
(*                              a propagating scope, bounded by the flowing *)
(*                              set the Proc joined with.                   *)
(*   OneScopePerProc         -- a Proc's scope, once set, never changes.    *)
(*   ScopeTraitsSetOnce      -- its propagating property and flowing set    *)
(*                              never change either.                        *)
(*   MembersNeverRoot        -- a Proc that joined a scope by FORK is never *)
(*                              a root (the ROOT flag is not inherited and  *)
(*                              a member cannot become one).                *)
(*   PropagatingIsScopeWide  -- every Proc of a scope agrees on whether it  *)
(*                              propagates (what /proc/<pid>/imperium       *)
(*                              reports is a scope fact, not a Proc fact).  *)
(*   FlowNeverWidens         -- a member's flowing set is within its root's.*)
(*                                                                         *)
(* LIVENESS (clean cfg) -- ScopeEventuallyEmpty: once a scope's root is     *)
(*   dead, every member is eventually dead (the sweep + the die-check make  *)
(*   progress; weak fairness on Die).                                       *)
(*                                                                         *)
(* SPEC-TO-CODE (the impl this gates, IM-2; rows in specs/SPEC-TO-CODE.md): *)
(*   RedeemFresh(p, G, TRUE)  <-> SYS_CAP_GRANT_IMPERIUM (111, flags         *)
(*                          PROPAGATING) registered + SYS_CAP_USE redeemed: *)
(*                          proc_become_legate's fresh arm sets             *)
(*                          LEGATE_FLAG_PROPAGATING + legate_caps.           *)
(*   RedeemFresh(p, G, FALSE) <-> the A-4a clearance redeem, fresh arm.     *)
(*   RedeemFurther(p, G) <-> proc_become_legate's further arm (scope # 0):   *)
(*                          OR caps, keep the tag/root/lcaps/propagating,    *)
(*                          valid_until = the earlier nonzero deadline; a    *)
(*                          PROPAGATING grant is refused there.              *)
(*   Fork                <-> rfork_internal: the cap derivation + the scope  *)
(*                          inherit, then the parent's group_exit_msg        *)
(*                          re-checked under g_proc_table_lock in the SAME   *)
(*                          hold as proc_link_child; set -> the rfork fails. *)
(*   Exit / Die          <-> proc_become_zombie_locked ->                    *)
(*                          proc_legate_teardown_if_root (the sweep).       *)
(*   Terminate           <-> proc_group_terminate (the flag; no sweep).      *)
(*   Expire              <-> el0_return_die_check's valid_until arm.         *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Procs,                    \* Proc ids (opaque; an id is born once, never reused)
    Caps,                     \* the elevation-only capability universe (opaque tokens)
    MaxScopes,                \* the scope allocator's bound (a state-space bound, not a mechanism)
    BUGGY_STRAGGLER,          \* TRUE = Fork admits a terminating parent
    BUGGY_RETAG,              \* TRUE = a further redeem allocates a fresh scope (re-tags)
    BUGGY_FLOW_WITHOUT_FLAG,  \* TRUE = caps flow at Fork under a non-propagating scope
    BUGGY_NEST_ADMITTED       \* TRUE = a PROPAGATING redeem on a scoped Proc is admitted

ASSUME Cardinality(Procs) >= 2
ASSUME Cardinality(Caps) >= 1
ASSUME MaxScopes \in Nat /\ MaxScopes >= 1
ASSUME BUGGY_STRAGGLER \in BOOLEAN
ASSUME BUGGY_RETAG \in BOOLEAN
ASSUME BUGGY_FLOW_WITHOUT_FLAG \in BOOLEAN
ASSUME BUGGY_NEST_ADMITTED \in BOOLEAN

VARIABLES
    state,        \* [Procs -> {"unborn", "alive", "terminating", "dead"}]
    scope,        \* [Procs -> 0..MaxScopes]   legate_scope_id (0 = none)
    root,         \* [Procs -> BOOLEAN]        PROC_FLAG_LEGATE_ROOT
    propagating,  \* [Procs -> BOOLEAN]        the scope property, as carried by this Proc
    lcaps,        \* [Procs -> SUBSET Caps]    legate_caps: what FLOWS at fork
    caps,         \* [Procs -> SUBSET Caps]    the elevation-only caps held
    flowed,       \* [Procs -> SUBSET Caps]    history: the caps that flowed IN at fork
    first,        \* [Procs -> Join]           history: the scope as joined (see Join)
    nextScope     \* the allocator (1..MaxScopes+1)

vars == <<state, scope, root, propagating, lcaps, caps, flowed, first, nextScope>>

Scopes == 0..MaxScopes

\* What a Proc's scope looked like at the moment the Proc joined it. `via` says
\* how: "redeem" (it is the root), "fork" (it is a member), "none" (unscoped).
\* `anchor` is the ROOT PROC whose life bounds the caps the Proc holds: itself
\* for a root, and for a member the anchor its parent carried -- so a member's
\* anchor is the original root however deep the fork tree goes, and whatever a
\* re-tag later does to the scope NUMBER.
Join == [scope: Scopes, prop: BOOLEAN, lcaps: SUBSET Caps,
         via: {"none", "redeem", "fork"}, anchor: Procs \cup {"none"}]

Unjoined == [scope |-> 0, prop |-> FALSE, lcaps |-> {}, via |-> "none",
             anchor |-> "none"]

TypeOk ==
    /\ state       \in [Procs -> {"unborn", "alive", "terminating", "dead"}]
    /\ scope       \in [Procs -> Scopes]
    /\ root        \in [Procs -> BOOLEAN]
    /\ propagating \in [Procs -> BOOLEAN]
    /\ lcaps       \in [Procs -> SUBSET Caps]
    /\ caps        \in [Procs -> SUBSET Caps]
    /\ flowed      \in [Procs -> SUBSET Caps]
    /\ first       \in [Procs -> Join]
    /\ nextScope   \in 1..(MaxScopes + 1)

(***************************************************************************)
(* Init: exactly one Proc is alive (the session shell, unscoped, no         *)
(* elevated caps -- the imperium tool's parent); every other id is unborn.  *)
(* CHOOSE picks a fixed one so the initial state is a single state.         *)
(***************************************************************************)
Origin == CHOOSE p \in Procs : TRUE

Init ==
    /\ state       = [p \in Procs |-> IF p = Origin THEN "alive" ELSE "unborn"]
    /\ scope       = [p \in Procs |-> 0]
    /\ root        = [p \in Procs |-> FALSE]
    /\ propagating = [p \in Procs |-> FALSE]
    /\ lcaps       = [p \in Procs |-> {}]
    /\ caps        = [p \in Procs |-> {}]
    /\ flowed      = [p \in Procs |-> {}]
    /\ first       = [p \in Procs |-> Unjoined]
    /\ nextScope   = 1

Alive(p)       == state[p] = "alive"
Terminating(p) == state[p] = "terminating"
Live(p)        == Alive(p) \/ Terminating(p)   \* not yet ZOMBIE: no sweep has fired for it

(***************************************************************************)
(* The SWEEP: what proc_legate_teardown_if_root does at a root's ZOMBIE     *)
(* transition -- every OTHER alive Proc carrying the root's tag is marked   *)
(* terminating (proc_group_terminate), in the same g_proc_table_lock hold.  *)
(* A Proc already terminating stays so (the CAS is set-once); dead/unborn  *)
(* Procs are untouched.                                                     *)
(***************************************************************************)
Swept(st, r) ==
    [p \in Procs |->
        IF p = r THEN "dead"
        ELSE IF st[p] = "alive" /\ scope[p] = scope[r] /\ scope[r] # 0
             THEN "terminating"
             ELSE st[p]]

(***************************************************************************)
(* RedeemFresh(p, G, prop) -- SYS_CAP_USE on a registered clearance grant of *)
(* cap set G (nonempty; corvus's self-restricted level), flag prop, by an   *)
(* UNSCOPED Proc: a fresh scope, p is its ROOT.                             *)
(***************************************************************************)
RedeemFresh(p, G, prop) ==
    /\ Alive(p)
    /\ scope[p] = 0
    /\ nextScope <= MaxScopes
    /\ scope'       = [scope EXCEPT ![p] = nextScope]
    /\ first'       = [first EXCEPT ![p] = [scope |-> nextScope, prop |-> prop,
                                            lcaps |-> G, via |-> "redeem",
                                            anchor |-> p]]
    /\ root'        = [root EXCEPT ![p] = TRUE]
    /\ propagating' = [propagating EXCEPT ![p] = prop]
    /\ lcaps'       = [lcaps EXCEPT ![p] = G]
    /\ caps'        = [caps EXCEPT ![p] = @ \cup G]
    /\ nextScope'   = nextScope + 1
    /\ UNCHANGED <<state, flowed>>

(***************************************************************************)
(* RedeemFurther(p, G) -- a NON-propagating redeem by a Proc already in a   *)
(* scope (root or member): OR the caps, keep everything else (the JIT case).*)
(* The PROPAGATING further redeem is REFUSED -- there is no action for it   *)
(* in the clean model; BuggyRedeemNest below is what admitting it means.    *)
(***************************************************************************)
RedeemFurther(p, G) ==
    /\ Alive(p)
    /\ scope[p] # 0
    /\ ~BUGGY_RETAG
    /\ caps' = [caps EXCEPT ![p] = @ \cup G]
    /\ UNCHANGED <<state, scope, root, propagating, lcaps, flowed, first, nextScope>>

(***************************************************************************)
(* BuggyRedeemRetag(p, G) -- bug class: the further redeem allocates a      *)
(* FRESH scope and makes p its root (the pre-IM-2 proc_become_legate, which *)
(* always allocated). A member of a propagating scope that redeems, say,    *)
(* CAP_JIT thereby LEAVES the imperium scope: the teardown walk no longer   *)
(* matches it, and the caps that flowed in from the imperium root outlive   *)
(* that root. The A-4a F2 shape, now privilege-bearing.                     *)
(***************************************************************************)
BuggyRedeemRetag(p, G) ==
    /\ BUGGY_RETAG
    /\ Alive(p)
    /\ scope[p] # 0
    /\ nextScope <= MaxScopes
    /\ scope'       = [scope EXCEPT ![p] = nextScope]     \* THE BUG: a fresh tag
    /\ root'        = [root EXCEPT ![p] = TRUE]
    /\ propagating' = [propagating EXCEPT ![p] = FALSE]
    /\ lcaps'       = [lcaps EXCEPT ![p] = G]
    /\ caps'        = [caps EXCEPT ![p] = @ \cup G]
    /\ nextScope'   = nextScope + 1
    /\ UNCHANGED <<state, flowed, first>>

(***************************************************************************)
(* BuggyRedeemNest(p, G) -- bug class: a PROPAGATING redeem by a scoped     *)
(* Proc is admitted as a further redeem that ALSO turns propagation on and  *)
(* widens the flowing set. A member of a plain scope would thereby start    *)
(* elevating its children; a member of an imperium scope would widen what   *)
(* its children receive beyond what the root was granted.                   *)
(***************************************************************************)
BuggyRedeemNest(p, G) ==
    /\ BUGGY_NEST_ADMITTED
    /\ Alive(p)
    /\ scope[p] # 0
    /\ propagating' = [propagating EXCEPT ![p] = TRUE]    \* THE BUG: the trait flips ...
    /\ lcaps'       = [lcaps EXCEPT ![p] = @ \cup G]      \* ... and the flowing set widens
    /\ caps'        = [caps EXCEPT ![p] = @ \cup G]
    /\ UNCHANGED <<state, scope, root, flowed, first, nextScope>>

(***************************************************************************)
(* Fork(parent, child) -- rfork. The child JOINS the parent's scope as a    *)
(* MEMBER; the elevated caps FLOW iff the scope is propagating (bounded by  *)
(* lcaps -- never the parent's further-redeemed extras such as CAP_JIT).    *)
(* The straggler close: a terminating parent cannot fork (the parent's      *)
(* group_exit_msg is re-checked under the lock hold that links the child).  *)
(***************************************************************************)
Flow(parent) ==
    IF propagating[parent] \/ (BUGGY_FLOW_WITHOUT_FLAG /\ scope[parent] # 0)
    THEN lcaps[parent]
    ELSE {}

ForkOK(parent) ==
    \/ Alive(parent)
    \/ (BUGGY_STRAGGLER /\ Terminating(parent))   \* THE BUG: the sweep walked past, the child lands anyway

Fork(parent, child) ==
    /\ ForkOK(parent)
    /\ state[child] = "unborn"
    /\ state'       = [state EXCEPT ![child] = "alive"]
    /\ scope'       = [scope EXCEPT ![child] = scope[parent]]
    /\ first'       = [first EXCEPT ![child] =
                         IF scope[parent] = 0 THEN Unjoined
                         ELSE [scope |-> scope[parent], prop |-> propagating[parent],
                               lcaps |-> Flow(parent), via |-> "fork",
                               anchor |-> first[parent].anchor]]
    /\ root'        = [root EXCEPT ![child] = FALSE]           \* the ROOT flag never inherits
    /\ propagating' = [propagating EXCEPT ![child] = propagating[parent]]
    /\ lcaps'       = [lcaps EXCEPT ![child] = Flow(parent)]
    /\ caps'        = [caps EXCEPT ![child] = Flow(parent)]    \* (parent & mask) & ~(ELEVATION_ONLY & ~flow), elevation-only part
    /\ flowed'      = [flowed EXCEPT ![child] = Flow(parent)]
    /\ UNCHANGED nextScope

(***************************************************************************)
(* Exit(p) -- a clean exit (exits): dead at once. A root's exit sweeps its  *)
(* scope in the same lock hold (proc_become_zombie_locked). Caps drop with  *)
(* death.                                                                   *)
(***************************************************************************)
Exit(p) ==
    /\ Alive(p)
    /\ state' = IF root[p] THEN Swept(state, p) ELSE [state EXCEPT ![p] = "dead"]
    /\ caps'  = [caps EXCEPT ![p] = {}]
    /\ UNCHANGED <<scope, root, propagating, lcaps, flowed, first, nextScope>>

(***************************************************************************)
(* Terminate(p) -- group_exit_msg set from outside (a kill, an exit_group,  *)
(* the sweep's own marking of a member). No sweep: a root so flagged is     *)
(* doomed, not dead, and its members stay alive and elevated until Die(p)   *)
(* reaches the ZOMBIE chokepoint.                                           *)
(***************************************************************************)
Terminate(p) ==
    /\ Alive(p)
    /\ state' = [state EXCEPT ![p] = "terminating"]
    /\ UNCHANGED <<scope, root, propagating, lcaps, caps, flowed, first, nextScope>>

\* A terminating Proc reaches its EL0-return die-check and dies; a root's
\* death sweeps its scope (proc_become_zombie_locked -> the teardown walk).
Die(p) ==
    /\ Terminating(p)
    /\ state' = IF root[p] THEN Swept(state, p) ELSE [state EXCEPT ![p] = "dead"]
    /\ caps'  = [caps EXCEPT ![p] = {}]
    /\ UNCHANGED <<scope, root, propagating, lcaps, flowed, first, nextScope>>

(***************************************************************************)
(* Expire(p) -- valid_until passed, observed at ANY alive member's EL0 tail:*)
(* every alive Proc of the scope, the root included, is marked terminating  *)
(* (proc_for_each(legate_teardown_cb, except = NULL)); each then dies at    *)
(* its own die-check, and the root's death sweeps once more (a no-op).      *)
(***************************************************************************)
Expire(p) ==
    /\ Alive(p)
    /\ scope[p] # 0
    /\ state' = [q \in Procs |->
                   IF state[q] = "alive" /\ scope[q] = scope[p] THEN "terminating"
                   ELSE state[q]]
    /\ UNCHANGED <<scope, root, propagating, lcaps, caps, flowed, first, nextScope>>

Grants == (SUBSET Caps) \ {{}}

Next ==
    \/ \E p \in Procs, G \in Grants, prop \in BOOLEAN : RedeemFresh(p, G, prop)
    \/ \E p \in Procs, G \in Grants : RedeemFurther(p, G)
    \/ \E p \in Procs, G \in Grants : BuggyRedeemRetag(p, G)
    \/ \E p \in Procs, G \in Grants : BuggyRedeemNest(p, G)
    \/ \E parent \in Procs, child \in Procs : Fork(parent, child)
    \/ \E p \in Procs : Exit(p)
    \/ \E p \in Procs : Terminate(p)
    \/ \E p \in Procs : Die(p)
    \/ \E p \in Procs : Expire(p)

\* Weak fairness on the die-check: a flagged Proc eventually dies (the EL0 tail
\* is reached; a kernel-parked thread is death-woken per #811).
Fairness == \A p \in Procs : WF_vars(Die(p))

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* INVARIANTS                                                              *)
(***************************************************************************)

\* I-25 STRENGTHENED, the crux: an alive Proc holding elevated caps has a LIVE
\* anchor -- the root Proc that granted the scope it FIRST joined has not
\* reached its ZOMBIE transition. (The root is its own anchor; a member marked
\* terminating is not "alive"; a straggler or a re-tagged member is -- and its
\* anchor is dead. Keyed on the root's IDENTITY, not the scope number, so a
\* re-tag cannot satisfy it by moving to a scope whose root is itself.)
NoElevatedOutlivesScope ==
    \A p \in Procs :
        (Alive(p) /\ caps[p] # {}) =>
            (first[p].anchor \in Procs /\ Live(first[p].anchor))

\* Caps that FLOWED in did so under a propagating scope, and never more than
\* the flowing set the Proc joined with.
FlowOnlyUnderPropagating ==
    \A p \in Procs :
        flowed[p] # {} => (first[p].prop /\ flowed[p] \subseteq first[p].lcaps)

\* A Proc's scope, once set, never changes (G6: one scope per Proc, set once).
OneScopePerProc ==
    \A p \in Procs : scope[p] = 0 \/ scope[p] = first[p].scope

\* The G6 companion: a scope's propagating property and its flowing set are
\* fixed when the Proc joins it -- neither a further redeem nor anything else
\* moves them (so "propagating never nests" and "what flows never widens").
ScopeTraitsSetOnce ==
    \A p \in Procs :
        scope[p] # 0 => (propagating[p] = first[p].prop /\ lcaps[p] = first[p].lcaps)

\* A Proc that joined its scope by FORK is never a root: the ROOT flag is not
\* inherited, and a member cannot become one (a re-tag is the only way).
MembersNeverRoot ==
    \A p \in Procs : first[p].via = "fork" => ~root[p]

\* Whether a scope propagates is a fact about the SCOPE: every Proc carrying
\* the tag agrees (what /proc/<pid>/imperium reports is consistent).
PropagatingIsScopeWide ==
    \A p, q \in Procs :
        (scope[p] # 0 /\ scope[p] = scope[q]) => propagating[p] = propagating[q]

\* A member's flowing set is within its root's: nothing a member does widens
\* what its own children can receive beyond what the root was granted.
FlowNeverWidens ==
    \A p \in Procs :
        (scope[p] # 0 /\ ~root[p]) =>
            \E r \in Procs : root[r] /\ scope[r] = scope[p] /\ lcaps[p] \subseteq lcaps[r]

Safety ==
    /\ TypeOk
    /\ NoElevatedOutlivesScope
    /\ FlowOnlyUnderPropagating
    /\ OneScopePerProc
    /\ ScopeTraitsSetOnce
    /\ MembersNeverRoot
    /\ PropagatingIsScopeWide
    /\ FlowNeverWidens

(***************************************************************************)
(* LIVENESS: once a scope's root is dead, every Proc of that scope is       *)
(* eventually dead -- the sweep reaches every member and the die-check      *)
(* makes progress. (Only meaningful in the clean model: the straggler bug   *)
(* violates safety first.)                                                  *)
(***************************************************************************)
ScopeEventuallyEmpty ==
    \A s \in 1..MaxScopes :
        ((\E r \in Procs : root[r] /\ scope[r] = s /\ state[r] = "dead")
         ~> (\A p \in Procs : scope[p] = s => state[p] = "dead"))

====
