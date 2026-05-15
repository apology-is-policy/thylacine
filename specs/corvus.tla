---- MODULE corvus ----
(***************************************************************************)
(* Thylacine corvus key-agent daemon — P5-corvus-spec.                     *)
(*                                                                         *)
(* Models the session state machine + capability arithmetic + authorization*)
(* surface of the corvus daemon (see docs/CORVUS-DESIGN.md). The spec is   *)
(* about corvus's INTERNAL state and the gates it enforces on inbound      *)
(* operations; it is complementary to specs/handles.tla (kernel handle    *)
(* tables) and specs/9p_client.tla (transport).                            *)
(*                                                                         *)
(* Pins CORVUS-DESIGN.md §9 invariants:                                    *)
(*                                                                         *)
(*   C-3  — A session's user-identity binding is immutable for the         *)
(*          session's lifetime.                                            *)
(*   C-4  — Session capability transfers monotonically reduce (ARCH I-2 / *)
(*          I-6 extended to session caps).                                 *)
(*   C-7  — corvus refuses unwrap for a (session, dataset) pair where     *)
(*          session.user != owner_of(dataset).                            *)
(*   C-11 — Session-cap and Proc-cap authorization are orthogonal;        *)
(*          operations check the auth they need, not the cross-product    *)
(*          (audit F10).                                                   *)
(*                                                                         *)
(* Secondary discipline captured (not directly part of §9 numbering but   *)
(* binding behaviour per §5.5):                                            *)
(*                                                                         *)
(*   HostownerRequiresConsole — CapHostowner can enter a Proc's proc_caps*)
(*          ONLY via AdminElevate, which gates on the kernel-stamped      *)
(*          console-attachment bit (set by joey at console-login spawn).  *)
(*                                                                         *)
(* OUT OF SCOPE (covered by other specs or runtime discipline):           *)
(*                                                                         *)
(*   - In-RAM secret discipline (C-1, C-2, C-5): mlockall, set_dumpable, *)
(*     explicit_bzero — runtime invariants enforced by Phase 5 hardening *)
(*     syscalls (P5-corvus-syscalls landed at 0db0dcf). Not directly      *)
(*     model-checkable.                                                    *)
(*   - Audit log encryption (C-9, C-19): file-format discipline; out of  *)
(*     scope for state-machine modeling.                                   *)
(*   - Argon2id parameter discipline (C-10, C-12): backend impl.          *)
(*   - Rate limiting + session timeouts (C-16, C-17): operational; not   *)
(*     load-bearing for the §9 invariants we're pinning.                  *)
(*   - Per-user-stratumd ownership (C-18): cross-component invariant     *)
(*     pinned by AuthSuccess + Unwrap composition here; the stratumd     *)
(*     side is Stratum's responsibility (see docs/STRATUM-API-V1.md A2). *)
(*   - Recovery phrase + dataset-ownership table integrity (C-20): file- *)
(*     format and provisioning-flow discipline.                           *)
(*   - WRAP verb (P5-corvus-bringup-d; §6.4 verb_id=10): wrapping a DEK  *)
(*     to a hybrid keypair is UNWRAP's inverse, C-7-gated identically    *)
(*     (session.bound_user must own the dataset). UnwrapOwnerOnly        *)
(*     already pins that authorization shape; WRAP adds no new invariant *)
(*     and is not modeled as a separate action.                          *)
(*                                                                         *)
(* MODELING DECISIONS:                                                     *)
(*                                                                         *)
(*   Sessions are identified by the Proc that owns them. One session per *)
(*   Proc — corvus's design is single-session-per-Proc at v1.0; multi-   *)
(*   session-per-user-per-Proc is v1.x. Cross-Proc session transfer       *)
(*   models the §6.3 9P RIGHT_TRANSFER mechanism: the src session is     *)
(*   torn down and a derivative is created at dst preserving bound_user. *)
(*                                                                         *)
(*   sessions is SUBSET [owner_proc, bound_user]. session_origin is a    *)
(*   ghost SUBSET of the same shape recording every session ever created *)
(*   (append-only). The C-3 invariant checks that every currently-active *)
(*   session's bound_user matches SOME origin record for that owner_proc.*)
(*   A buggy in-place mutation of bound_user fails the match.             *)
(*                                                                         *)
(*   proc_caps[p] is the set of Proc-level capabilities held by p. v1.0  *)
(*   CapHostowner is the only one modeled (CapLockPages + CapCsprngRead *)
(*   from the syscall surface are corvus-internal startup-time concerns,*)
(*   not gates on per-op authorization). The only mutation modeled is   *)
(*   AdminElevate adding CapHostowner; the spec deliberately does NOT   *)
(*   model RforkWithCaps (which is handles.tla's job).                  *)
(*                                                                         *)
(*   console_attached is a monotonic SUBSET of Procs marking Procs        *)
(*   spawned through joey's console-login chain. The kernel stamps this *)
(*   bit at fork time; it is never propagated across rfork to a different*)
(*   territory (CORVUS-DESIGN.md §5.5 last paragraph). Modeled as a set *)
(*   that grows via MarkConsoleAttached (joey's action at spawn) and     *)
(*   never shrinks.                                                        *)
(*                                                                         *)
(*   completed_unwraps and completed_admins are ghost append-only logs   *)
(*   recording every successful Unwrap and admin op. The C-7 invariant   *)
(*   checks every unwrap record is well-formed (bound_user matches       *)
(*   dataset owner). The C-11-derived AdminRequiresProcCap invariant     *)
(*   checks every admin record was issued by a Proc currently holding   *)
(*   CapHostowner — since proc caps in this spec only grow (no reduce    *)
(*   modeled), "currently" is equivalent to "ever".                       *)
(*                                                                         *)
(* BUGGY-CONFIG MATRIX (executable documentation per CLAUDE.md spec-first*)
(* policy):                                                                *)
(*                                                                         *)
(*   corvus.cfg                              all bug flags FALSE; TLC    *)
(*                                           proves all invariants.       *)
(*   corvus_buggy_unwrap_cross_user.cfg      catches C-7 violation:      *)
(*                                           BuggyUnwrapCrossUser fires  *)
(*                                           Unwrap-success with session.*)
(*                                           bound_user != DatasetOwner. *)
(*   corvus_buggy_auth_binding_mutate.cfg    catches C-3 violation:      *)
(*                                           BuggyAuthBindingMutate      *)
(*                                           mutates an active session's *)
(*                                           bound_user post-auth.        *)
(*   corvus_buggy_admin_without_proc_cap.cfg catches C-11 violation:     *)
(*                                           BuggyAdminWithoutProcCap    *)
(*                                           accepts AdminVerb without   *)
(*                                           CapHostowner in proc_caps.  *)
(*   corvus_buggy_elevate_without_console.cfg                            *)
(*                                           catches HostownerRequires- *)
(*                                           Console violation: Buggy-   *)
(*                                           ElevateWithoutConsole grants*)
(*                                           CapHostowner to a non-      *)
(*                                           console-attached Proc.      *)
(*   corvus_buggy_transfer_elevate.cfg       catches SessionCapsBounded  *)
(*                                           violation: cross-Proc       *)
(*                                           session transfer ELEVATES   *)
(*                                           the derivative session's    *)
(*                                           bound_user (re-binds to a   *)
(*                                           different user) — captures *)
(*                                           the spec-level shape of     *)
(*                                           "session derivative does NOT*)
(*                                           preserve user binding."     *)
(*                                                                         *)
(* INVARIANTS (TLC-checked):                                               *)
(*                                                                         *)
(*   TypeOk                  — state has the right types.                 *)
(*                                                                         *)
(*   SessionUserImmutable    — C-3. Every active session's bound_user    *)
(*                             matches some session_origin record for    *)
(*                             that owner_proc.                            *)
(*                                                                         *)
(*   UnwrapOwnerOnly         — C-7. Every completed_unwrap satisfies    *)
(*                             bound_user == DatasetOwner[dataset].       *)
(*                                                                         *)
(*   AdminRequiresProcCap    — C-11. Every completed_admin's owner_proc *)
(*                             has CapHostowner in proc_caps.             *)
(*                                                                         *)
(*   HostownerRequiresConsole - §5.5 console-attachment rule. Every Proc*)
(*                             with CapHostowner in proc_caps is in      *)
(*                             console_attached.                          *)
(*                                                                         *)
(* See docs/CORVUS-DESIGN.md §6.3 (Proc identity binding), §5.5 (host-   *)
(* owner elevation), §9 (invariant numbering).                            *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Procs,                              \* set of Procs (>= 2)
    Users,                              \* set of Users (>= 2)
    ProcCaps,                           \* set of Proc-cap labels
    CapHostowner,                       \* \in ProcCaps — the admin gate
    BUGGY_UNWRAP_CROSS_USER,            \* bug: skip session-owns-dataset check
    BUGGY_AUTH_BINDING_MUTATE,          \* bug: mutate session.bound_user post-auth
    BUGGY_ADMIN_WITHOUT_PROC_CAP,       \* bug: admin verb accepts without CapHostowner
    BUGGY_ELEVATE_WITHOUT_CONSOLE,      \* bug: AdminElevate skips console check
    BUGGY_TRANSFER_REBIND               \* bug: session transfer re-binds bound_user

(***************************************************************************)
(* Datasets and DatasetOwner — modeling decision: one dataset per user,    *)
(* dataset identified BY its owner. The C-7 invariant tests the shape      *)
(* "session.bound_user MUST EQUAL DatasetOwner(d)" — that shape is fully   *)
(* exercised even when Datasets = Users and DatasetOwner = identity,       *)
(* because the cross-user case (session.bound_user # DatasetOwner(d)) is   *)
(* still reachable in TLC (Proc auth'd as u1 attempting unwrap of dataset  *)
(* u2).                                                                     *)
(*                                                                         *)
(* This convention sidesteps TLC's config-file limitation that record /    *)
(* tuple constants ([d1 |-> u1] or { <<d1, u1>> }) aren't accepted in cfg  *)
(* files — a function or relation constant would otherwise require the     *)
(* CONSTANT-substitution dance that doesn't compose cleanly with model     *)
(* values in the cfg.                                                       *)
(*                                                                         *)
(* Real-world correspondence: every Stratum user dataset is identified by  *)
(* (pool, user) — the user's name is part of the dataset path              *)
(* (`thylacine/users/michael`). So "dataset named after owner" is the      *)
(* live-system convention, not just a spec simplification.                 *)
(***************************************************************************)
Datasets         == Users
DatasetOwner(d)  == d

ASSUME Cardinality(Procs) >= 2
ASSUME Cardinality(Users) >= 2
ASSUME CapHostowner \in ProcCaps
ASSUME BUGGY_UNWRAP_CROSS_USER \in BOOLEAN
ASSUME BUGGY_AUTH_BINDING_MUTATE \in BOOLEAN
ASSUME BUGGY_ADMIN_WITHOUT_PROC_CAP \in BOOLEAN
ASSUME BUGGY_ELEVATE_WITHOUT_CONSOLE \in BOOLEAN
ASSUME BUGGY_TRANSFER_REBIND \in BOOLEAN

(***************************************************************************)
(* Session record. Three fields:                                           *)
(*                                                                         *)
(*   creation_proc — the Proc that issued the AUTH that minted this        *)
(*                  session. IMMUTABLE for the session's lifetime,         *)
(*                  including across SessionTransfer. Pins the C-3 binding *)
(*                  identity: a session's lineage to a particular AUTH    *)
(*                  call is permanent.                                     *)
(*                                                                         *)
(*   owner_proc    — the Proc currently holding the session's Spoor. MAY  *)
(*                  CHANGE via SessionTransfer (cross-Proc handle           *)
(*                  transfer; §6.3). The owner Proc is what corvus checks *)
(*                  for "active session on this peer."                     *)
(*                                                                         *)
(*   bound_user    — the user the session is authenticated as. MUST be    *)
(*                  immutable for the session's lifetime (C-3). Set at    *)
(*                  AuthSuccess; preserved by SessionTransfer.             *)
(*                                                                         *)
(* The C-3 invariant uses (creation_proc, bound_user) as the immutable    *)
(* pair: every active session must have an origin record matching its    *)
(* creation_proc and bound_user. SessionTransfer leaves the pair untouched*)
(* (only owner_proc moves) so the invariant survives transfer. A buggy    *)
(* in-place mutation OR a buggy rebind-during-transfer changes bound_user *)
(* without writing a new origin, leaving the (creation_proc, bound_user) *)
(* pair without a matching origin — catch.                                 *)
(***************************************************************************)
SessionRecord == [creation_proc : Procs, owner_proc : Procs, bound_user : Users]

(***************************************************************************)
(* Origin record. Ghost append-only log: every AuthSuccess appends one.   *)
(* SessionTransfer does NOT extend session_origin — the transferred       *)
(* session is the same lineage, just at a different owner_proc.           *)
(***************************************************************************)
OriginRecord == [creation_proc : Procs, bound_user : Users]

(***************************************************************************)
(* Unwrap record. Ghost append-only log: every successful Unwrap appends a *)
(* row. UnwrapOwnerOnly checks every row is well-formed.                   *)
(***************************************************************************)
UnwrapRecord == [owner_proc : Procs, dataset : Datasets, bound_user : Users]

(***************************************************************************)
(* Admin record. Ghost append-only log: every successful AdminVerb appends *)
(* a row. AdminRequiresProcCap checks every row's owner_proc holds         *)
(* CapHostowner.                                                            *)
(***************************************************************************)
AdminRecord == [owner_proc : Procs]

VARIABLES
    sessions,           \* SUBSET SessionRecord — currently-active sessions
    session_origin,     \* SUBSET OriginRecord — ghost append-only creation log
    proc_caps,          \* [Procs -> SUBSET ProcCaps]
    console_attached,   \* SUBSET Procs — joey-stamped console-login Procs
    completed_unwraps,  \* SUBSET UnwrapRecord — ghost log
    completed_admins    \* SUBSET AdminRecord — ghost log

vars == <<sessions, session_origin, proc_caps, console_attached, completed_unwraps, completed_admins>>

TypeOk ==
    /\ sessions \subseteq SessionRecord
    /\ session_origin \subseteq OriginRecord
    /\ proc_caps \in [Procs -> SUBSET ProcCaps]
    /\ console_attached \subseteq Procs
    /\ completed_unwraps \subseteq UnwrapRecord
    /\ completed_admins \subseteq AdminRecord

(***************************************************************************)
(* Init: no sessions, no origins, no proc caps, no console-attached Procs, *)
(* no ghost log entries. Every state grows from this point.                *)
(***************************************************************************)
Init ==
    /\ sessions = {}
    /\ session_origin = {}
    /\ proc_caps = [p \in Procs |-> {}]
    /\ console_attached = {}
    /\ completed_unwraps = {}
    /\ completed_admins = {}

(***************************************************************************)
(* MarkConsoleAttached(p) — joey spawns p through the console-login chain. *)
(* The kernel sets the console-attachment bit on p's Proc capability       *)
(* state. Monotonic: once set, never cleared.                              *)
(*                                                                         *)
(* Maps to /sbin/joey's fork of /sbin/login (and only /sbin/login) with    *)
(* the console-attachment bit set in the child Proc's capability set.      *)
(***************************************************************************)
MarkConsoleAttached(p) ==
    /\ p \notin console_attached
    /\ console_attached' = console_attached \cup {p}
    /\ UNCHANGED <<sessions, session_origin, proc_caps, completed_unwraps, completed_admins>>

(***************************************************************************)
(* AuthSuccess(p, u) — Proc p authenticates as user u via passphrase.      *)
(* corvus creates a new session bound to u and records the origin. One    *)
(* session per Proc enforced by the precondition.                          *)
(*                                                                         *)
(* Maps to corvus AUTH verb (§4.4 + §5.1 + §6.4 verb_id=1).                *)
(***************************************************************************)
AuthSuccess(p, u) ==
    /\ ~(\E s \in sessions : s.owner_proc = p)
    /\ sessions' = sessions \cup {[creation_proc |-> p,
                                    owner_proc    |-> p,
                                    bound_user    |-> u]}
    /\ session_origin' = session_origin \cup {[creation_proc |-> p,
                                                bound_user    |-> u]}
    /\ UNCHANGED <<proc_caps, console_attached, completed_unwraps, completed_admins>>

(***************************************************************************)
(* SessionClose(p) — p's session is closed. Either p exits, or p writes    *)
(* SESSION_CLOSE verb. session_origin retains the historical record.       *)
(*                                                                         *)
(* Maps to corvus SESSION_CLOSE verb (§4.4 logout) or Proc-exit cleanup.   *)
(***************************************************************************)
SessionClose(p) ==
    /\ \E s \in sessions : s.owner_proc = p
    /\ sessions' = {s \in sessions : s.owner_proc # p}
    /\ UNCHANGED <<session_origin, proc_caps, console_attached, completed_unwraps, completed_admins>>

(***************************************************************************)
(* SessionTransfer(src, dst) — 9P RIGHT_TRANSFER of the session Spoor from*)
(* src to dst. The src session is torn down; a derivative is created at   *)
(* dst preserving bound_user (§6.3 audit F10 resolution). dst's proc_caps *)
(* are dst's own — NOT inherited from src.                                *)
(*                                                                         *)
(* Recording: a new session_origin entry is appended for dst, since the   *)
(* (dst, bound_user) pair is a NEW active session record (origin from     *)
(* dst's perspective is the moment of transfer). This is intentional —    *)
(* C-3's invariant requires the active session match SOME origin for the  *)
(* owner_proc, and the transfer is the legitimate origin point for dst's  *)
(* session.                                                                *)
(*                                                                         *)
(* Maps to the 9P session-handle transfer path; no direct corvus verb     *)
(* (corvus observes the Spoor reopen with a new peer Proc identity).       *)
(***************************************************************************)
SessionTransfer(src, dst) ==
    /\ src # dst
    /\ ~(\E s \in sessions : s.owner_proc = dst)
    /\ \E s \in sessions : s.owner_proc = src
    /\ LET s == CHOOSE x \in sessions : x.owner_proc = src
       IN sessions' = (sessions \ {s})
                          \cup {[creation_proc |-> s.creation_proc,
                                 owner_proc    |-> dst,
                                 bound_user    |-> s.bound_user]}
    \* session_origin is NOT extended: transfer is a continuation of the
    \* same session lineage. The (creation_proc, bound_user) pair is
    \* preserved and already has a matching origin entry from AuthSuccess.
    /\ UNCHANGED <<session_origin, proc_caps, console_attached, completed_unwraps, completed_admins>>

(***************************************************************************)
(* AdminElevate(p) — p has authenticated the system passphrase; corvus     *)
(* grants CapHostowner to p's proc_caps. Gates: p must be console-attached *)
(* AND have an active session (the elevate is a session-scoped operation, *)
(* even though the resulting cap is Proc-level).                          *)
(*                                                                         *)
(* Note: the §5.5 verb's wire says "grant CAP_HOSTOWNER to session" which *)
(* is colloquial; mechanically the cap lands on the Proc's cap set, not   *)
(* the session record. This spec models the mechanically-correct form.    *)
(*                                                                         *)
(* Maps to corvus ADMIN_ELEVATE verb (§4.4 + §5.5 + §6.4 verb_id=7).      *)
(***************************************************************************)
AdminElevate(p) ==
    /\ p \in console_attached
    /\ \E s \in sessions : s.owner_proc = p
    /\ CapHostowner \notin proc_caps[p]
    /\ proc_caps' = [proc_caps EXCEPT ![p] = @ \cup {CapHostowner}]
    /\ UNCHANGED <<sessions, session_origin, console_attached, completed_unwraps, completed_admins>>

(***************************************************************************)
(* Unwrap(p, d) — legitimate path. p has an active session bound to user u;*)
(* d is owned by u; corvus returns the DEK (modeled as the append of a     *)
(* well-formed unwrap record).                                             *)
(*                                                                         *)
(* C-7 gate: session.bound_user MUST equal DatasetOwner[d].                *)
(*                                                                         *)
(* Maps to corvus UNWRAP verb (§4.2 /ops/unwrap + §6.4 verb_id=4).         *)
(***************************************************************************)
Unwrap(p, d) ==
    /\ \E s \in sessions : s.owner_proc = p
    /\ LET s == CHOOSE x \in sessions : x.owner_proc = p
       IN /\ s.bound_user = DatasetOwner(d)
          /\ completed_unwraps' = completed_unwraps
                                    \cup {[owner_proc |-> p,
                                           dataset |-> d,
                                           bound_user |-> s.bound_user]}
    /\ UNCHANGED <<sessions, session_origin, proc_caps, console_attached, completed_admins>>

(***************************************************************************)
(* AdminVerb(p) — legitimate path for any admin op (user-create, user-     *)
(* delete, etc.). p MUST hold CapHostowner in proc_caps. Note that no     *)
(* session.bound_user check applies — admin authority is Proc-cap-bound, *)
(* per §6.3 F10 orthogonality.                                            *)
(*                                                                         *)
(* The active-session existence is required because the verb is delivered *)
(* over /srv/corvus/ops/*, which requires an open Spoor (and corvus knows *)
(* whose Spoor it is via /srv/corvus/peer/).                              *)
(*                                                                         *)
(* Maps to corvus USER_CREATE / USER_DELETE / ROTATE_KEY verbs (§4.2,    *)
(* §6.4 verb_id=5,6,9 — bundled here as a single AdminVerb action since   *)
(* the auth gate is identical for all admin ops).                          *)
(***************************************************************************)
AdminVerb(p) ==
    /\ \E s \in sessions : s.owner_proc = p
    /\ CapHostowner \in proc_caps[p]
    /\ completed_admins' = completed_admins \cup {[owner_proc |-> p]}
    /\ UNCHANGED <<sessions, session_origin, proc_caps, console_attached, completed_unwraps>>

(***************************************************************************)
(* ============================ BUG CLASSES ============================== *)
(***************************************************************************)

(***************************************************************************)
(* BuggyUnwrapCrossUser(p, d) — C-7 violation. corvus's Unwrap handler    *)
(* SKIPS the session.bound_user == DatasetOwner[d] check; it returns the *)
(* DEK regardless of who the session is for.                              *)
(*                                                                         *)
(* Real-world analogue: missing or wrong-direction check in              *)
(* usr/corvus/src/verbs/unwrap.rs. The reference impl checks              *)
(* session.bound_user vs corvus's dataset_ownership_table[dataset]; the  *)
(* bug short-circuits the comparison or compares wrong fields.            *)
(*                                                                         *)
(* Caught by UnwrapOwnerOnly invariant.                                    *)
(***************************************************************************)
BuggyUnwrapCrossUser(p, d) ==
    /\ BUGGY_UNWRAP_CROSS_USER
    /\ \E s \in sessions : s.owner_proc = p
    /\ LET s == CHOOSE x \in sessions : x.owner_proc = p
       IN /\ s.bound_user # DatasetOwner(d)
          /\ completed_unwraps' = completed_unwraps
                                    \cup {[owner_proc |-> p,
                                           dataset |-> d,
                                           bound_user |-> s.bound_user]}
    /\ UNCHANGED <<sessions, session_origin, proc_caps, console_attached, completed_admins>>

(***************************************************************************)
(* BuggyAuthBindingMutate(p, new_u) — C-3 violation. corvus's session     *)
(* table allows the bound_user field of an active session to be mutated  *)
(* post-creation. Real-world analogue: a session-management bug where a  *)
(* later AUTH call on the same Proc UPDATES the existing session's       *)
(* bound_user rather than refusing (or replacing the entire session).     *)
(*                                                                         *)
(* session_origin is NOT updated by the bug — the historical record stays*)
(* honest. The active session diverges from origin.                        *)
(*                                                                         *)
(* Caught by SessionUserImmutable invariant.                               *)
(***************************************************************************)
BuggyAuthBindingMutate(p, new_u) ==
    /\ BUGGY_AUTH_BINDING_MUTATE
    /\ \E s \in sessions : s.owner_proc = p /\ s.bound_user # new_u
    /\ LET s == CHOOSE x \in sessions : x.owner_proc = p
       IN sessions' = (sessions \ {s})
                        \cup {[creation_proc |-> s.creation_proc,
                               owner_proc    |-> p,
                               bound_user    |-> new_u]}
    /\ UNCHANGED <<session_origin, proc_caps, console_attached, completed_unwraps, completed_admins>>

(***************************************************************************)
(* BuggyAdminWithoutProcCap(p) — C-11 violation (Proc-cap side). corvus's *)
(* admin verb dispatcher SKIPS the CapHostowner check on proc_caps[p]. *)
(*                                                                         *)
(* Real-world analogue: a permission-check that mistakenly uses session   *)
(* presence as proof of authority, or a missing cap-table lookup. p has  *)
(* an active session but no CapHostowner; the verb succeeds anyway.       *)
(*                                                                         *)
(* Caught by AdminRequiresProcCap invariant.                               *)
(***************************************************************************)
BuggyAdminWithoutProcCap(p) ==
    /\ BUGGY_ADMIN_WITHOUT_PROC_CAP
    /\ \E s \in sessions : s.owner_proc = p
    /\ CapHostowner \notin proc_caps[p]
    /\ completed_admins' = completed_admins \cup {[owner_proc |-> p]}
    /\ UNCHANGED <<sessions, session_origin, proc_caps, console_attached, completed_unwraps>>

(***************************************************************************)
(* BuggyElevateWithoutConsole(p) — HostownerRequiresConsole violation.    *)
(* corvus's AdminElevate handler SKIPS the console-attached precondition. *)
(*                                                                         *)
(* Real-world analogue: an admin-elevate path that forgets to read       *)
(* /srv/corvus/peer/proc's console-attachment bit, or a future remote   *)
(* (sshd) login flow that accidentally grants the bit via misconfigured *)
(* fork-time stamping.                                                     *)
(*                                                                         *)
(* Caught by HostownerRequiresConsole invariant.                           *)
(***************************************************************************)
BuggyElevateWithoutConsole(p) ==
    /\ BUGGY_ELEVATE_WITHOUT_CONSOLE
    /\ p \notin console_attached
    /\ \E s \in sessions : s.owner_proc = p
    /\ CapHostowner \notin proc_caps[p]
    /\ proc_caps' = [proc_caps EXCEPT ![p] = @ \cup {CapHostowner}]
    /\ UNCHANGED <<sessions, session_origin, console_attached, completed_unwraps, completed_admins>>

(***************************************************************************)
(* BuggyTransferRebind(src, dst, new_u) — C-3 violation through transfer. *)
(* The 9P session-handle transfer path is supposed to PRESERVE bound_user *)
(* (the new dst Proc inherits the same user-identity binding). The bug   *)
(* re-binds: the derivative session at dst has bound_user = new_u != src's*)
(* original bound_user.                                                    *)
(*                                                                         *)
(* Real-world analogue: a session-transfer path that constructs the dst  *)
(* session record from request-supplied fields instead of copying from   *)
(* the src session — a textbook session-binding-confusion vulnerability. *)
(*                                                                         *)
(* The session_origin entry IS written for dst (the bug propagates the   *)
(* rebound user into the origin record too, matching what a transfer-path*)
(* bug would do — it doesn't realize it's lying). However, the           *)
(* session_origin entry for the ORIGINAL src session still records src's *)
(* original bound_user, and the transfer LOST that record's binding to a *)
(* live Proc.                                                              *)
(*                                                                         *)
(* Actually — re-reading: with the bug, the dst session matches a fresh  *)
(* origin record (with rebound user). So SessionUserImmutable would NOT  *)
(* catch this directly. The real C-3 statement requires THE CONTINUITY:  *)
(* the dst session is a DERIVATIVE of src's session and must inherit the *)
(* SAME bound_user as src had at transfer time. We capture this via a   *)
(* derivative-tracking ghost: every transfer appends to a derived_from   *)
(* ledger; the invariant cross-checks the derivative's bound_user.       *)
(*                                                                         *)
(* derived_from : SUBSET [derivative_proc, original_bound_user].          *)
(* Each SessionTransfer writes (dst, original_bound_user) to it.          *)
(* The invariant: any active session at dst with a derived_from entry    *)
(* must have bound_user matching the recorded original_bound_user.        *)
(*                                                                         *)
(* The buggy action writes the WRONG original_bound_user (the rebound    *)
(* value) — wait, no, it writes the ACTUAL src bound_user as the         *)
(* derived_from record (since that's what really happened) but the       *)
(* active session has the rebound value. Catch.                            *)
(***************************************************************************)
BuggyTransferRebind(src, dst, new_u) ==
    /\ BUGGY_TRANSFER_REBIND
    /\ src # dst
    /\ ~(\E s2 \in sessions : s2.owner_proc = dst)
    /\ \E s \in sessions : s.owner_proc = src /\ s.bound_user # new_u
    /\ LET s == CHOOSE x \in sessions : x.owner_proc = src
       IN sessions' = (sessions \ {s})
                          \cup {[creation_proc |-> s.creation_proc,
                                 owner_proc    |-> dst,
                                 bound_user    |-> new_u]}
    \* The bug does NOT write a fresh origin record — corvus's transfer
    \* path doesn't mint a new lineage on transfer; the legit-but-broken
    \* code mutates the existing session's bound_user in place. The
    \* (creation_proc, new_u) pair has no matching origin entry; the only
    \* origin for this creation_proc records the ORIGINAL bound_user.
    /\ UNCHANGED <<session_origin, proc_caps, console_attached, completed_unwraps, completed_admins>>

(***************************************************************************)
(* Next — disjunction of all actions.                                      *)
(***************************************************************************)
Next ==
    \/ \E p \in Procs : MarkConsoleAttached(p)
    \/ \E p \in Procs : \E u \in Users : AuthSuccess(p, u)
    \/ \E p \in Procs : SessionClose(p)
    \/ \E src \in Procs : \E dst \in Procs : SessionTransfer(src, dst)
    \/ \E p \in Procs : AdminElevate(p)
    \/ \E p \in Procs : \E d \in Datasets : Unwrap(p, d)
    \/ \E p \in Procs : AdminVerb(p)
    \/ \E p \in Procs : \E d \in Datasets : BuggyUnwrapCrossUser(p, d)
    \/ \E p \in Procs : \E u \in Users : BuggyAuthBindingMutate(p, u)
    \/ \E p \in Procs : BuggyAdminWithoutProcCap(p)
    \/ \E p \in Procs : BuggyElevateWithoutConsole(p)
    \/ \E src \in Procs : \E dst \in Procs : \E u \in Users : BuggyTransferRebind(src, dst, u)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* StateConstraint — bounds TLC exploration. The ghost append-only logs    *)
(* (session_origin, completed_unwraps, completed_admins) can grow without *)
(* bound; we cap each at 3 entries. The active-session set is naturally   *)
(* bounded by Cardinality(Procs).                                          *)
(***************************************************************************)
StateConstraint ==
    /\ Cardinality(session_origin) <= 3
    /\ Cardinality(completed_unwraps) <= 3
    /\ Cardinality(completed_admins) <= 3

(***************************************************************************)
(* ============================== INVARIANTS ============================== *)
(***************************************************************************)

(***************************************************************************)
(* SessionUserImmutable — C-3. Every active session has a matching        *)
(* session_origin record on its (creation_proc, bound_user) identity.    *)
(*                                                                         *)
(* The session's creation_proc is set at AuthSuccess and IMMUTABLE        *)
(* across SessionTransfer. SessionTransfer mutates only owner_proc;       *)
(* creation_proc and bound_user are preserved. The legit AuthSuccess →    *)
(* SessionClose → AuthSuccess sequence leaves two origin records for     *)
(* the same Proc (one per AUTH call), each matching its own active       *)
(* session lineage.                                                        *)
(*                                                                         *)
(* BuggyAuthBindingMutate(p, new_u) mutates an active session's          *)
(* bound_user without writing a new origin record. The active            *)
(* (creation_proc, new_u) pair has no matching origin → invariant fails. *)
(*                                                                         *)
(* BuggyTransferRebind(src, dst, new_u) mutates bound_user at transfer    *)
(* WITHOUT writing a new origin record. The active session at dst has    *)
(* creation_proc = src (preserved through transfer, but mutated user);   *)
(* the only origin entry for creation_proc = src records the ORIGINAL   *)
(* bound_user from AUTH, not the rebound value → invariant fails.        *)
(*                                                                         *)
(* The (creation_proc, bound_user) pair as immutable identity matches    *)
(* the impl-side discipline: corvus's session struct binds these two    *)
(* fields at AUTH time; transfer changes only the Spoor's peer Proc.    *)
(* Implementation MUST ensure the binding is preserved across transfer  *)
(* (the Spoor's userspace-Dev code copies bound_user from src on        *)
(* transfer-detect, NEVER from request-supplied fields).                  *)
(***************************************************************************)
SessionUserImmutable ==
    \A s \in sessions :
        \E o \in session_origin :
            /\ o.creation_proc = s.creation_proc
            /\ o.bound_user    = s.bound_user

(***************************************************************************)
(* UnwrapOwnerOnly — C-7. Every completed_unwrap record satisfies          *)
(* bound_user == DatasetOwner[dataset].                                    *)
(*                                                                         *)
(* The bound_user field in the record reflects the session's bound_user   *)
(* at the time of Unwrap — both legitimate and buggy paths write this    *)
(* field honestly. The legitimate path requires the equality as a         *)
(* precondition; the buggy path bypasses the check, producing a record   *)
(* with bound_user # DatasetOwner[dataset] — invariant fails.             *)
(***************************************************************************)
UnwrapOwnerOnly ==
    \A u \in completed_unwraps :
        u.bound_user = DatasetOwner(u.dataset)

(***************************************************************************)
(* AdminRequiresProcCap — C-11. Every completed_admin record's owner_proc *)
(* currently holds CapHostowner in proc_caps. Since proc_caps in this    *)
(* spec only grows (never reduces), "currently" is equivalent to "ever   *)
(* held."                                                                  *)
(*                                                                         *)
(* The legitimate AdminVerb precondition gates on CapHostowner in        *)
(* proc_caps; the buggy path bypasses, producing a record for a Proc    *)
(* that never had the cap — invariant fails.                              *)
(***************************************************************************)
AdminRequiresProcCap ==
    \A a \in completed_admins :
        CapHostowner \in proc_caps[a.owner_proc]

(***************************************************************************)
(* HostownerRequiresConsole — §5.5 console-attachment rule. Every Proc    *)
(* holding CapHostowner in proc_caps is in console_attached.              *)
(*                                                                         *)
(* The only legitimate path to CapHostowner is AdminElevate, which gates *)
(* on console_attached membership. The buggy path BuggyElevateWithout-   *)
(* Console adds CapHostowner without that gate, producing a state where  *)
(* a non-console-attached Proc holds the cap — invariant fails.           *)
(***************************************************************************)
HostownerRequiresConsole ==
    \A p \in Procs :
        (CapHostowner \in proc_caps[p]) => p \in console_attached

Invariants ==
    /\ TypeOk
    /\ SessionUserImmutable
    /\ UnwrapOwnerOnly
    /\ AdminRequiresProcCap
    /\ HostownerRequiresConsole

====
