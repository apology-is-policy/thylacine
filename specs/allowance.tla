---- MODULE allowance ----
(***************************************************************************)
(* Thylacine hardware allowance -- the Menagerie driver-authority lift     *)
(* (docs/MENAGERIE.md section 4; ARCH section 28 I-34). The one new kernel  *)
(* mechanism the driver framework needs: scope the coarse CAP_HW_CREATE     *)
(* per-Proc to a bounded ALLOWANCE -- a set of permitted hardware           *)
(* resources -- checked at the three create gates (SYS_MMIO/IRQ/DMA_CREATE, *)
(* kernel/syscall.c:216/368/458). The I-25 (legate scope) analog for        *)
(* hardware: a driver's authority is exactly its warden-granted allowance,  *)
(* a subset of its bound node's resources, never widened, fully revoked on  *)
(* unbind / removal / crash.                                                *)
(*                                                                         *)
(* Spec-first RE-ENABLED for this surface (user-voted 2026-06-15; CLAUDE.md *)
(* the 4th "RE-ENABLED for ..." entry). The central hazard is an SMP race   *)
(* -- the famous capability-revocation-vs-in-flight-create class -- exactly *)
(* the kind machine-checked exploration is for (the asid-rollover /         *)
(* death-wake precedent). The broader spec-to-code suspension stands        *)
(* elsewhere.                                                               *)
(*                                                                         *)
(* WHAT THIS MODELS -- the KERNEL mechanism, not the warden's policy.       *)
(*   The warden (the TCB device manager) is the privileged ACTOR that       *)
(*   drives Confer (on DeviceAdded) and Revoke (on DeviceRemoved); it is    *)
(*   not a state variable. Resources is the warden's broad universe         *)
(*   (everything not kernel-I-5-reserved is simply IN Resources; the        *)
(*   reserved set is OUT). Each driver Proc carries, kernel-side:           *)
(*     node[d]      -- its bound node's resources (DeviceAdded carries it). *)
(*     conferred[d] -- the grant the warden made = node INTERSECT manifest  *)
(*                     (the ceiling; manifest <= node, so conferred <= node).*)
(*     allowance[d] -- the LIVE set the create gate checks. = conferred     *)
(*                     while RUNNING; emptied on Revoke.                     *)
(*     handles[d]   -- the live KObj_MMIO/IRQ/DMA the driver has minted.    *)
(*   A driver lifecycle: idle --Confer--> running --Revoke--> revoked.      *)
(*                                                                         *)
(* THE CENTRAL GUARD -- the create is a TWO-STEP under the protecting lock, *)
(* and Revoke takes the SAME lock. A SYS_*_CREATE that passes the gate      *)
(* (CreateBegin: resource in allowance) does NOT yet hold the handle; the   *)
(* handle-table install (CreateCommit) RE-CHECKS the live allowance under   *)
(* the lock. Revoke (DeviceRemoved) empties the allowance + group-          *)
(* terminates the driver (drops every handle) atomically under that lock.   *)
(* So an in-flight create concurrent with a revoke serializes one of two    *)
(* ways: (a) commit-before-revoke -- the handle lands, then revoke's        *)
(* handle-sweep drops it; or (b) revoke-before-commit -- the commit         *)
(* re-checks, sees the emptied allowance, and aborts. Either way no live    *)
(* handle survives over a revoked allowance. This is the I-25-teardown x    *)
(* I-30-submit-pin discipline (resolve+act under one lock, never re-trust a *)
(* pre-check across a yield).                                               *)
(*                                                                         *)
(* THE BUG CLASSES (executable counterexamples; CLAUDE.md spec-first):      *)
(*   BUGGY_COMMIT_NO_RECHECK     -- the create commit installs the handle   *)
(*     UNCONDITIONALLY (no re-check of the live allowance). A revoke that    *)
(*     interleaves between CreateBegin and the commit leaves a live handle   *)
(*     over the emptied allowance -- the revoke-vs-create SMP race the       *)
(*     scripture names. Caught by HandlesWithinAllowance.                    *)
(*   BUGGY_REVOKE_LEAVES_HANDLES -- Revoke empties the allowance but FAILS   *)
(*     to drop the driver's handles (an incomplete I-25 teardown -- the      *)
(*     group-terminate didn't sweep the hw handles). The allowance          *)
(*     "outlives" nothing, but the AUTHORITY does. Caught by                 *)
(*     RevokedFullyCleared + HandlesWithinAllowance.                         *)
(*   BUGGY_CONFER_WIDEN          -- the warden's intersection (node INTER    *)
(*     manifest) is buggy and confers an allowance NOT a subset of the       *)
(*     node's resources (a grant past the device). Caught by                 *)
(*     ConferredWithinNode.                                                  *)
(*   BUGGY_SELF_WIDEN            -- the live allowance grows past the        *)
(*     conferred grant (a kernel bug where the allowance set is mutable      *)
(*     after confer, or a stale/widened copy). Caught by                     *)
(*     AllowanceWithinConferred.                                             *)
(*                                                                         *)
(* INVARIANTS (TLC-checked) -- the four legs of I-34:                       *)
(*   HandlesWithinAllowance   -- a driver only ever holds handles within    *)
(*                               its LIVE allowance (the gate is sound +     *)
(*                               the revoke race loses nothing). THE crux.   *)
(*   AllowanceWithinConferred -- the live allowance is never widened past   *)
(*                               the warden's grant ("never widened").       *)
(*   ConferredWithinNode      -- the grant never exceeds the bound node's   *)
(*                               resources ("a subset of its node").         *)
(*   RevokedFullyCleared      -- a revoked driver holds no handle + no       *)
(*                               allowance ("fully revoked on teardown").    *)
(*                                                                         *)
(* LIVENESS (clean cfg) -- EventuallyResolves: every in-flight create       *)
(*   eventually resolves (commits or aborts); the gate protocol cannot      *)
(*   wedge a SYS_*_CREATE. The temporal witness that the re-check discipline *)
(*   does not deadlock against a concurrent revoke.                         *)
(*                                                                         *)
(* SPEC-TO-CODE (the impl this gates, build-arc step 3):                    *)
(*   Confer        <-> the warden's spawn-with-narrowed-allowance path +    *)
(*                     struct Proc.allowance set at driver spawn.            *)
(*   CreateBegin   <-> the in-allowance check at sys_{mmio,irq,dma}_create. *)
(*   CreateCommit  <-> the handle_alloc install, re-validating the          *)
(*                     allowance under the same lock Revoke takes.           *)
(*   Revoke        <-> DeviceRemoved -> allowance clear + proc_group_        *)
(*                     terminate (the #809/#811 cascade drops the handles).  *)
(* See specs/SPEC-TO-CODE.md (allowance.tla) once the impl lands.           *)
(*                                                                         *)
(* AUDIT NOTES (the focused I-34 round, post-impl):                         *)
(*   - Revoke is modeled ATOMIC (empties allowance[d] AND handles[d] in one *)
(*     step). The impl defers the handle-sweep to the death-wake cascade    *)
(*     (proc_revoke_allowance sets revoked; the warden then proc_group_     *)
(*     terminate's -> reap drops the handles), so RevokedFullyCleared       *)
(*     captures the END state, not the transient window. That window        *)
(*     (revoked, handles not yet swept) is bounded-safe: the flagged-to-die *)
(*     threads die at the EL0 checkpoint before using a handle, and a       *)
(*     removed-device MMIO access faults. The warden MUST pair              *)
(*     proc_revoke_allowance with proc_group_terminate (audit F2).          *)
(*   - The model abstracts the resource universe to opaque tokens; the      *)
(*     per-kind gate predicate (MMIO full-window containment + overflow,    *)
(*     IRQ membership, DMA scalar ceiling) is runtime-tested, NOT spec-      *)
(*     verified -- protocol-faithful, predicate-abstracted (audit F3).      *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Drivers,                       \* driver Proc ids (>= 1; the warden is the implicit actor)
    Resources,                     \* the hardware resource universe (PA windows | IRQ | DMA pools), opaque tokens
    BUGGY_COMMIT_NO_RECHECK,       \* TRUE = the revoke-vs-create race (commit skips the re-check)
    BUGGY_REVOKE_LEAVES_HANDLES,   \* TRUE = revoke empties the allowance but leaks the handles
    BUGGY_CONFER_WIDEN,            \* TRUE = the warden confers an allowance not a subset of the node
    BUGGY_SELF_WIDEN               \* TRUE = the live allowance grows past the conferred grant

ASSUME Cardinality(Drivers) >= 1
ASSUME Cardinality(Resources) >= 1
ASSUME BUGGY_COMMIT_NO_RECHECK \in BOOLEAN
ASSUME BUGGY_REVOKE_LEAVES_HANDLES \in BOOLEAN
ASSUME BUGGY_CONFER_WIDEN \in BOOLEAN
ASSUME BUGGY_SELF_WIDEN \in BOOLEAN

VARIABLES
    state,       \* [Drivers -> {"idle", "running", "revoked"}]
    node,        \* [Drivers -> SUBSET Resources]  the bound node's resources (set at Confer)
    conferred,   \* [Drivers -> SUBSET Resources]  the warden's grant ceiling (set at Confer)
    allowance,   \* [Drivers -> SUBSET Resources]  the LIVE gate set (= conferred while running; {} when revoked)
    handles,     \* [Drivers -> SUBSET Resources]  the live hw handles the driver has minted
    pending      \* [Drivers -> SUBSET Resources]  the in-flight create (|.| <= 1: {} = none, {r} = r in flight)

vars == <<state, node, conferred, allowance, handles, pending>>

TypeOk ==
    /\ state     \in [Drivers -> {"idle", "running", "revoked"}]
    /\ node      \in [Drivers -> SUBSET Resources]
    /\ conferred \in [Drivers -> SUBSET Resources]
    /\ allowance \in [Drivers -> SUBSET Resources]
    /\ handles   \in [Drivers -> SUBSET Resources]
    /\ pending   \in [Drivers -> SUBSET Resources]
    /\ \A d \in Drivers : Cardinality(pending[d]) <= 1   \* at most one in-flight create per Proc-thread modeled

Init ==
    /\ state     = [d \in Drivers |-> "idle"]
    /\ node      = [d \in Drivers |-> {}]
    /\ conferred = [d \in Drivers |-> {}]
    /\ allowance = [d \in Drivers |-> {}]
    /\ handles   = [d \in Drivers |-> {}]
    /\ pending   = [d \in Drivers |-> {}]

(***************************************************************************)
(* Confer(d, N, A) -- the warden processes DeviceAdded(d, node=N) and       *)
(* confers allowance A = N INTERSECT manifest. CORRECT: A is a subset of N  *)
(* (the intersection with the manifest can only narrow the node, never      *)
(* widen it). Binds the driver: idle -> running, with node/conferred/       *)
(* allowance all set. The warden's own broad allowance is the whole         *)
(* Resources universe, so N (a real device's resources) is always grantable.*)
(***************************************************************************)
Confer(d, N, A) ==
    /\ state[d] = "idle"
    /\ N \subseteq Resources
    /\ A \subseteq N                            \* the node-bound discipline (A = N INTERSECT manifest)
    /\ state'     = [state EXCEPT ![d] = "running"]
    /\ node'      = [node EXCEPT ![d] = N]
    /\ conferred' = [conferred EXCEPT ![d] = A]
    /\ allowance' = [allowance EXCEPT ![d] = A]
    /\ UNCHANGED <<handles, pending>>

(***************************************************************************)
(* BuggyConferWiden(d, N, A) -- bug class: the warden's node-INTERSECT-     *)
(* manifest computation is wrong and grants A that is NOT a subset of N (a  *)
(* superset of the device's actual resources). Real-world analogue: an      *)
(* off-by-one / inverted-mask in the resource-intersection, or a manifest   *)
(* `needs` honored verbatim without intersecting the node.                  *)
(*                                                                         *)
(* Caught by ConferredWithinNode.                                           *)
(***************************************************************************)
BuggyConferWiden(d, N, A) ==
    /\ BUGGY_CONFER_WIDEN
    /\ state[d] = "idle"
    /\ N \subseteq Resources
    /\ A \subseteq Resources
    /\ ~(A \subseteq N)                         \* THE BUG: the grant exceeds the bound node
    /\ state'     = [state EXCEPT ![d] = "running"]
    /\ node'      = [node EXCEPT ![d] = N]
    /\ conferred' = [conferred EXCEPT ![d] = A]
    /\ allowance' = [allowance EXCEPT ![d] = A]
    /\ UNCHANGED <<handles, pending>>

(***************************************************************************)
(* CreateBegin(d, r) -- the SYS_*_CREATE gate. The driver requests a handle *)
(* over resource r; the kernel checks r is within the driver's LIVE         *)
(* allowance. On pass, the create is "in flight" (the gate passed; the      *)
(* handle-table install is the next step). pending[d] = {r} records it.     *)
(*                                                                         *)
(* Models the new check inserted at kernel/syscall.c:216/368/458:           *)
(*   `if (!allowance_permits(p, resource)) return -1;`                      *)
(***************************************************************************)
CreateBegin(d, r) ==
    /\ state[d] = "running"
    /\ pending[d] = {}
    /\ r \in allowance[d]                        \* THE GATE CHECK (passes: r in the live allowance)
    /\ pending' = [pending EXCEPT ![d] = {r}]
    /\ UNCHANGED <<state, node, conferred, allowance, handles>>

(***************************************************************************)
(* CreateCommit(d) -- the SAFE handle-table install. RE-CHECKS the live     *)
(* allowance under the lock that Revoke also takes: install the handle ONLY *)
(* if the driver is still running AND r is still in the allowance. A revoke *)
(* that interleaved between CreateBegin and here emptied the allowance ->   *)
(* the re-check fails -> the create aborts (installs nothing). pending is    *)
(* cleared either way (the syscall returns).                                *)
(*                                                                         *)
(* Models handle_alloc guarded by an under-lock re-validation of the        *)
(* allowance -- the create gate and the revoke serialize on the same lock.  *)
(***************************************************************************)
CreateCommit(d) ==
    /\ pending[d] # {}
    /\ LET r == CHOOSE x \in pending[d] : TRUE IN
         /\ handles' = IF (state[d] = "running" /\ r \in allowance[d])
                         THEN [handles EXCEPT ![d] = @ \cup {r}]
                         ELSE handles                     \* revoke won the race -> abort
    /\ pending' = [pending EXCEPT ![d] = {}]
    /\ UNCHANGED <<state, node, conferred, allowance>>

(***************************************************************************)
(* BuggyCreateCommit(d) -- THE central bug class: the handle-table install  *)
(* commits the in-flight create UNCONDITIONALLY, never re-checking the live *)
(* allowance. If a Revoke interleaved between CreateBegin and here, this     *)
(* installs a live handle over the now-emptied allowance -- a handle slips   *)
(* through a being-revoked allowance. The revoke-vs-create SMP race.        *)
(*                                                                         *)
(* Real-world analogue: the create handler checks the allowance at entry    *)
(* but the handle_alloc + the allowance-membership are NOT under the same   *)
(* lock the revoke path takes, so a concurrent DeviceRemoved on another CPU *)
(* tears down the allowance in the window, yet the create still lands.      *)
(*                                                                         *)
(* Caught by HandlesWithinAllowance (depth 4: Confer -> CreateBegin ->      *)
(* Revoke -> BuggyCreateCommit).                                            *)
(***************************************************************************)
BuggyCreateCommit(d) ==
    /\ BUGGY_COMMIT_NO_RECHECK
    /\ pending[d] # {}
    /\ LET r == CHOOSE x \in pending[d] : TRUE IN
         handles' = [handles EXCEPT ![d] = @ \cup {r}]    \* THE BUG: no re-check, commit regardless
    /\ pending' = [pending EXCEPT ![d] = {}]
    /\ UNCHANGED <<state, node, conferred, allowance>>

(***************************************************************************)
(* Revoke(d) -- the warden processes DeviceRemoved(d): revoke the allowance *)
(* + group-terminate the driver. Atomically (under the lock) empties the    *)
(* live allowance AND drops every minted handle (the #809/#811 cascade      *)
(* sweeps the driver's KObj_MMIO/IRQ/DMA). running -> revoked.              *)
(*                                                                         *)
(* Does NOT clear pending: an in-flight create on another CPU is mid-       *)
(* syscall, unreachable from the revoke path. Its OWN CreateCommit clears   *)
(* pending and -- in the safe spec -- aborts via the re-check.              *)
(***************************************************************************)
Revoke(d) ==
    /\ state[d] = "running"
    /\ state'     = [state EXCEPT ![d] = "revoked"]
    /\ allowance' = [allowance EXCEPT ![d] = {}]
    /\ handles'   = [handles EXCEPT ![d] = {}]            \* group-terminate sweeps the handles
    /\ UNCHANGED <<node, conferred, pending>>

(***************************************************************************)
(* BuggyRevokeLeak(d) -- bug class: DeviceRemoved empties the allowance but *)
(* FAILS to drop the driver's handles. An incomplete teardown -- the        *)
(* group-terminate cascade didn't reach the hw handles (e.g. revoke clears  *)
(* the allowance set but forgets the handle-sweep, or the sweep races the    *)
(* handle table). The driver is "revoked" yet still holds live authority    *)
(* over the gone device.                                                    *)
(*                                                                         *)
(* Caught by RevokedFullyCleared (and HandlesWithinAllowance: handles non-  *)
(* empty while allowance is {}).                                            *)
(***************************************************************************)
BuggyRevokeLeak(d) ==
    /\ BUGGY_REVOKE_LEAVES_HANDLES
    /\ state[d] = "running"
    /\ state'     = [state EXCEPT ![d] = "revoked"]
    /\ allowance' = [allowance EXCEPT ![d] = {}]
    /\ UNCHANGED <<node, conferred, handles, pending>>    \* THE BUG: handles not cleared

(***************************************************************************)
(* BuggySelfWiden(d, r) -- bug class: the LIVE allowance grows past the      *)
(* conferred grant (a kernel bug where the per-Proc allowance set is        *)
(* mutable after the warden's confer, or a stale widened copy is consulted).*)
(* r is added to the allowance though it was never conferred.               *)
(*                                                                         *)
(* Caught by AllowanceWithinConferred.                                      *)
(***************************************************************************)
BuggySelfWiden(d, r) ==
    /\ BUGGY_SELF_WIDEN
    /\ state[d] = "running"
    /\ r \in Resources
    /\ r \notin conferred[d]                     \* THE BUG: widening beyond the grant
    /\ allowance' = [allowance EXCEPT ![d] = @ \cup {r}]
    /\ UNCHANGED <<state, node, conferred, handles, pending>>

(***************************************************************************)
(* Next: disjunction of all actions. The buggy actions are guarded by their *)
(* flags (disabled in the clean cfg). r ranges over Resources; N, A range   *)
(* over SUBSET Resources (the warden's confer choices).                     *)
(***************************************************************************)
Next ==
    \/ \E d \in Drivers, N \in SUBSET Resources, A \in SUBSET Resources : Confer(d, N, A)
    \/ \E d \in Drivers, N \in SUBSET Resources, A \in SUBSET Resources : BuggyConferWiden(d, N, A)
    \/ \E d \in Drivers, r \in Resources : CreateBegin(d, r)
    \/ \E d \in Drivers : CreateCommit(d)
    \/ \E d \in Drivers : BuggyCreateCommit(d)
    \/ \E d \in Drivers : Revoke(d)
    \/ \E d \in Drivers : BuggyRevokeLeak(d)
    \/ \E d \in Drivers, r \in Resources : BuggySelfWiden(d, r)

(***************************************************************************)
(* Weak fairness on the clean-path progress actions -> the protocol makes   *)
(* progress, so the liveness witness (EventuallyResolves) is meaningful.    *)
(* No fairness on the Buggy* actions (they are disabled in the clean cfg    *)
(* anyway, and safety checking is fairness-independent).                    *)
(***************************************************************************)
Fairness ==
    /\ \A d \in Drivers : WF_vars(CreateCommit(d))
    /\ \A d \in Drivers : WF_vars(Revoke(d))

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(***************************************************************************)
(* HandlesWithinAllowance -- THE crux of I-34. Every live handle a driver   *)
(* holds is within its LIVE allowance. This single statement covers:        *)
(*   - the gate is sound (no create over a resource not in the allowance);  *)
(*   - the revoke-vs-create race loses nothing (a handle minted after the   *)
(*     allowance was emptied violates this);                                 *)
(*   - "fully revoked" on the handle axis (revoked => allowance {} =>        *)
(*     handles must be {} to satisfy the subset).                            *)
(***************************************************************************)
HandlesWithinAllowance ==
    \A d \in Drivers : handles[d] \subseteq allowance[d]

(***************************************************************************)
(* AllowanceWithinConferred -- the live allowance is never widened past the *)
(* warden's grant. The "never widened" leg of I-34. (Revoke narrows the     *)
(* allowance to {}, which trivially satisfies the subset.)                   *)
(***************************************************************************)
AllowanceWithinConferred ==
    \A d \in Drivers : allowance[d] \subseteq conferred[d]

(***************************************************************************)
(* ConferredWithinNode -- the warden's grant never exceeds the bound        *)
(* node's resources. The "a subset of its bound node's resources" leg --    *)
(* a manifest cannot widen a driver past its device.                        *)
(***************************************************************************)
ConferredWithinNode ==
    \A d \in Drivers : conferred[d] \subseteq node[d]

(***************************************************************************)
(* RevokedFullyCleared -- "fully revoked on unbind/removal/crash": a        *)
(* revoked driver holds no handle AND no allowance. The teardown is total.  *)
(***************************************************************************)
RevokedFullyCleared ==
    \A d \in Drivers :
        (state[d] = "revoked") => (handles[d] = {} /\ allowance[d] = {})

Safety ==
    /\ TypeOk
    /\ HandlesWithinAllowance
    /\ AllowanceWithinConferred
    /\ ConferredWithinNode
    /\ RevokedFullyCleared

(***************************************************************************)
(* Liveness witness (clean cfg): every in-flight create eventually          *)
(* resolves -- the re-check-under-lock gate cannot wedge a SYS_*_CREATE      *)
(* against a concurrent revoke. Under WF on CreateCommit, a pending create   *)
(* always commits or aborts.                                                 *)
(***************************************************************************)
EventuallyResolves ==
    \A d \in Drivers : (pending[d] # {}) ~> (pending[d] = {})

====
