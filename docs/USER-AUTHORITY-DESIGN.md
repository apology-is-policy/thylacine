# User authority: mandates, administrative legates and Imperium

Status: implementation specification for review, 2026-09-24. The operator
endorsed separating use, identity administration and delegation, and requested
this specification with `imperium` as its userspace driver. The detailed
contracts below are proposed implementation requirements, not an assertion that
they are implemented or that every new ABI has been ratified. No numeric
capability bits, syscall IDs or Corvus verb IDs are allocated by this document.

Owner: Astra, coordinated with Main and Aux through Yip 0114/0115. Read against
main `5ed51ff5`; existing UI changes are in Astra's branch. Main's queued Aux
integration `d819d8f1` supplies identity-cape semantics; later seal changes must
be reconciled before implementation. This is a design-only change.

## 1. Outcome and boundaries

A production installation can create, rename, suspend and retire users; manage
membership and eligibility; grant bounded standing access; and appoint scoped
administrators without giving those administrators a general hostowner shell.
Every change is authorized by Corvus against the live actor and exact target.
The terminal program does not receive an administrative master key.

Three grants are distinct:

1. **Use:** a bounded resource operation, either standing or activated temporarily.
2. **Activate:** eligibility to request a bounded legate scope after the required
   trusted authentication. Eligibility is not active authority.
3. **Administer:** eligibility to activate operations on identities or authority
   records. Only a live administrative legate can exercise it.

Standing baseline access is installed at login from valid mandates. Standing
administrative records never place management power in every application.
Existing coarse elevation-only bits do not become ambient login bits. Permanent
means **until explicitly revoked**, not immune to suspension or revocation.

`imperium` drives all three faces. *Censor* remains the administrative role;
*mandatum* is the record; *album* is Corvus's protected store. A separate
privileged `censor` daemon or executable is unnecessary. Existing `imperium dac`,
`chown`, `kill`, `post`, `--list` and `abdicate` remain compatible.

This specification does not add authentication caching, authorize kernel or
firmware replacement, or grant remote servers authority they have not delegated.
The remembered-proof question has a separate lifetime and threat model. Existing
Imperium distinct-key authentication remains required for each new activation.

## 2. Relation to current scripture and code

On ratification, this document refines MANDATE-DESIGN §§3, 4, 7–10 and 12:

| Earlier formulation | Replacement contract |
|---|---|
| issuer's held operational caps bound a mandate | the issuer's live administrative activation and explicit durable delegation envelope bound it |
| `actor.clearance >= issuer_level` | domain/action/resource/subject coverage plus recorded grant provenance; no scalar administrator rank |
| self-mandates always forbidden | direct or transitive self-escalation forbidden; harmless self-targeting reductions explicitly allowed |
| separate `censor` tool | administrative commands in `imperium`; Corvus remains the authority |
| namespace tier requires no kernel work | login-only installation can reuse namespaces, but race-free live suspension/revocation needs kernel admission and teardown support |
| resource-scoped capability grants deferred indefinitely | a named delivery stage with its own enforcement gate and no flat-bit approximation |

I-2/I-6 attenuation, I-22 no ambient superuser, I-25 execution-scope teardown,
I-27 trusted attention and I-35 mandate revocation remain obligations. The I-35
prose and its model must be amended at ratification to express these refinements.
Do not silently reinterpret the existing invariant while implementing.

Verified existing mechanisms: Corvus's live `SYS_SRV_PEER` checks; clearance
eligibility/key storage; deferred Imperium requests; `/cap` grant/redeem;
principal identity distinct from process `stripes`; legate subtree teardown;
namespace composition; physical SAK; Lictor visibility/restoration commits.

Missing mechanisms: album/delegation graph, scoped administrative policy,
administrative scope kind, account lifecycle transaction, live policy revocation,
resource-scoped use table, supported user-management client. USER_CREATE after
bootstrap, GROUP_CREATE and CLEARANCE_GRANT/REVOKE currently require
CAP_HOSTOWNER. The new flow replaces ordinary use of those broad gates; it
must not expose them unchanged through a more attractive UI.

Do not relabel a live Proc's principal on rename, membership edit or migration.
Numeric principals are stable; names are attributes. I-39 debug capability-cover
must include new authority (§8), and the seal may hide image data but never the
public elevation ledger. Adopt Aux's corrected seal changes, not an obsolete SHA.

## 3. Trust and authorization ownership

| Component | Responsibility | Must not acquire |
|---|---|---|
| `imperium` / optional Halcyon management view | discovery, intent construction, ordinary result display | key bytes, authority by PID/name supplied in a request, direct album writes |
| Corvus | identity/policy resolution, authentication, exact authorization, durable transactions and audit | trust in a client-drawn approval or cached peer credentials |
| Lictor + kernel seat | exclusive trusted display/input; acknowledge the exact current semantic frame | authority to change the proposed transaction |
| kernel `/cap`, scope and admission machinery | bind activations to process incarnations, lifetime and policy generation; close publication races; teardown | interpretation of usernames, group policy or Roman role labels |
| login / service owners | materialize approved resource views and enforce revocation barriers | choice of policy wider than Corvus's committed snapshot |

The installer is the founding authority ceremony, not a permanent root account.
Recovery retains a separately protected system authority source. Corvus, the
kernel, trusted seat, installer and relevant resource owners are still trusted
components; capability vocabulary does not remove that trust.

## 4. Missing administrative authorities

The following are **typed Corvus authority operations**, not globally effective
new `CAP_*` flags. Each requires a scoped administrative activation with an
explicit subject set, operation set and limits. A generic CAP_USER_ADMIN bit
would discard exactly the distinctions this design requires.

| Operation | Permits | Additional bound / excluded effect |
|---|---|---|
| `user.enroll` | reserve a principal, create a disabled account, instantiate a named floor template | enrollment realm, count/storage quota, template revision; does not mint arbitrary grants |
| `user.profile` | rename and edit display metadata | stable target principal; no identity substitution or credential reset |
| `user.suspend` | block login/activation and terminate sessions | authorized subject set; cannot silently revoke founding policy |
| `user.resume` | reopen a suspended account after policy reconciliation | separate permission; no revival of revoked grants or old sessions |
| `user.retire` | permanently disable an account and tombstone its name/ID relationships | no automatic destruction or recovery of encrypted data; never reuse the principal ID |
| `group.create` | create a group in a permitted realm | creation quota; empty membership by default |
| `group.membership` | add/remove members | every resulting authority increase requires grant coverage too; version group selectors |
| `authority.grant` | issue bounded use or activation mandates | the selected grant envelope must cover the complete proposed grant |
| `authority.revoke` | revoke specified records and dependents | lineage/revocation domain; cannot revoke a founding root without founding authority |
| `authority.delegate` | issue administrative eligibility to another subject | nested envelope may only shrink; explicit remaining delegation depth |
| `clearance.enroll` | enroll a subject in a permitted activation policy | grant envelope, authentication floor and eligible-cap ceiling apply |
| `clearance.key-reset` | invalidate a distinct-key verifier and start target re-enrollment | separate high-assurance permission; does not expose the old key or log in as target |
| `authority.rotate-domain` | invalidate all grants from a domain key generation | explicit domain-root authority; include the full revocation closure in trusted confirmation |
| `floor.define` | create a new revision of a floor template | installation/founding domain; affects existing users only through an explicit migration |
| `audit.read` | inspect permitted administrative history and policy explanations | subjects/domains bounded; never credential material |

`user.resume`, `authority.delegate`, `authority.rotate-domain`, `floor.define`
and `clearance.key-reset` are
not implied by similarly named operations. No general `user.edit` escape hatch.
Whole-device recovery is a separate founding ceremony, not a delegable operation
in a routine administrator bundle. Home destruction requires an explicit storage
policy and later dedicated operation; `user.retire` must not perform it.

A named role is a displayable bundle of these operations and envelopes, not a
new source of power. The initial workstation owner may be eligible for all
administrative operations, but each use still activates a bounded scope. A
help-desk role might permit enroll/profile/suspend for ordinary users, while
lacking delegation, floor edits, resume of protected users, or key resets.

## 5. Mandatum and policy representation

Corvus stores canonical, versioned records, with all referenced IDs local to this
installation. IDs and generations never wrap or get reused; exhaustion fails
closed. Names are resolved to IDs before authorization and display.

```
Mandatum {
  id, revision, schema_version,
  subject_principal, issuer_principal,
  kind: Use | Activate | Admin,
  domain, operations,
  resource_selector, subject_selector,
  activation_policy, grant_envelope?,
  term, redelegation_depth,
  supports: [(mandate_id, revision)],
  domain_key_generation,
  state: Live | Revoking | Revoked,
  transaction_id
}
```

`supports` is the conjunction of the authorizing records needed for this grant,
not a list of interchangeable excuses. The graph is acyclic and every record
has a path to a founding root. Domain-key rotation invalidates old-generation
records and their dependents through the same restrictive transaction/barrier
as explicit revocation; a login-only generation check is insufficient. The issuer principal is audit identity, not an
implicit support: leaving an administrative shell does not revoke grants it
properly issued. Removing a supporting durable delegation record does.

For the first implementation, choose one complete supporting envelope per issued
record; do not combine fragments from unrelated administrators to manufacture a
larger delegation envelope. Multiple separately justified records may contribute
to ordinary access, with provenance retained for each. A fresh source can replace
a dependency only through a new, explicitly approved transaction.

An Admin grant envelope includes allowed target principals/realms, grant kinds,
operations, resource bounds, maximum term, allowed activation requirements,
maximum delegation depth, quotas and whether until-revoked grants are permitted.
An issuer may delegate a subset of its durable envelope while holding a short
administrative activation. This is not an extension of its temporary operational
Imperium: the durable delegation permission is the source of the persistent grant.
Child validity cannot exceed its durable supports. Perpetual children require
supports explicitly allowing perpetual delegation.

Subject selectors are exact principals or versioned enrollment realms, not
mutable username globs. Resource selectors use stable object/service identity,
not an unchecked path string. Group-based eligibility uses a versioned dependency;
membership changes invalidate affected evaluations and are authorized against
the total effective delta. Nested groups are not supported initially.

A user cannot grant themselves new authority through an account alias, a group,
an alternate support path or a delegation cycle. Authorized self-reduction is
allowed; self-resume or self-reset does not bypass its distinct authorization.
A beneficial self-change requires a different authorized administrator or the
founding ceremony. This does not prevent two willing principals from sharing
information or using resources on one another's behalf; no information-flow
noninterference claim is made.

## 6. Terms, authentication and defaults

Three clocks are separate: durable mandate validity, activated-scope lifetime,
and the authentication episode deadline. Cached authentication is not introduced.

- Existing operational Imperium keeps its four-hour policy default and ceiling
  during migration. `--for` may request a shorter duration. A policy editor may
  change the ceiling/default only within its delegated envelope.
- Administrative changes use a single-transaction scope: at most five minutes
  after trusted conferral, destroyed after success, denial, cancellation or
  requester exit. No open-ended administrative shell in the first delivery.
- Baseline mandates default to until-revoked. Optional durable `until` deadlines
  require trusted UTC with rollback detection; if unavailable, refuse that term.
  Do not reinterpret a reboot-relative duration as a persistent deadline.
- Active durations use kernel monotonic deadlines and never survive reboot.
  Changes to wall time cannot extend an active scope.
- Policy controls authentication strength. The initial Admin policy requires a
  distinct per-administrator key and physical SAK for every transaction;
  founding/recovery requires the separate system credential and trusted ceremony.
  A delegated policy cannot weaken its inherited authentication requirement.

No key appears in arguments, environment, ordinary PTY input, Beacon messages,
transcripts or the audit log. Enrollment creates a disabled account and activates
it only after the target establishes its own credential through a trusted
onboarding ceremony. No administrator-selected reusable password is returned to
the terminal. Initial delivery uses local target presence; remote invitations
need their own authenticated protocol. Account administrators cannot recover
existing encrypted home data merely by resetting authentication eligibility.

## 7. Userspace driver and operator workflows

These are the specified future CLI forms, not commands claimed to exist today.
Read-only commands do not activate authority, though disclosure is scoped.

```
imperium --list
imperium explain post
imperium dac chown --for 30m
imperium users list
imperium users show alice
imperium users enroll alice --template workstation
imperium users rename alice alicia
imperium users suspend alice
imperium users resume alice
imperium users retire alice
imperium groups add developers alice
imperium groups remove developers alice
imperium grants list --user alice
imperium grants issue --user alice --policy ./alice-access.toml
imperium grants revoke <mandate-id>
imperium grants rotate --domain <domain-id>
imperium eligibility grant --user alice --level fs-admin
imperium eligibility revoke --user alice --level fs-admin
imperium admins grant --user alice --policy ./helpdesk.toml
imperium keys reset --user alice --level imperium
imperium policy show <policy-id>
imperium policy set <policy-id> --file ./policy.toml
imperium audit --user alice
imperium transaction <transaction-id>
```

Policy files contain public request data only; they neither confer authority nor
carry authentication. Versioned schema with unknown/duplicate fields rejected;
explicit grant kind, operations, selectors, term, activation and delegation
fields. `--dry-run` on a mutation asks Corvus for validation/explanation but cannot
mint a reusable approval. Missing administrative authority produces an actionable
denial, never an automatic hostowner fallback. No `--yes` bypasses trusted SAK.

`--list` prints standing access, activation eligibility, current active scopes
and administrative eligibility as distinct sections. Every capability includes
its human explanation, exact resource bounds, source mandate, default/maximum
term and authentication requirement. Raw hex may supplement names but never
replace them. Unsupported operations appear explicitly unavailable, not as if
implemented. `users show` includes account state and policy revision; another
user's private eligibility details require disclosure authority.

Example: enrollment requests `user.enroll` for the `ordinary-users` realm and a
fixed workstation template. Corvus resolves the template, checks quotas and
shows the baseline to be issued. The operator authenticates in the trusted scene.
The account is committed disabled, target onboarding provisions credentials,
then a separately recorded transition enables login. An enrollment right is
permission to instantiate the template; it does not give the enrollment process
access to the new private home or arbitrary capability-grant authority.

Example: giving Alice network administration requires `authority.delegate`
covering that realm and nested envelope. It grants eligibility, not an ambient
admin bit. Alice's later operation gets its own administrative legate and trusted
confirmation. She cannot grant filesystem authority from that network envelope.

## 8. Kernel contract: scope, admission and enforcement

Administrative policy stays in Corvus. The kernel does need three additions;
none may be approximated with an ordinary client token or a naked PID.

### 8.0 Elevation precursor integrity (UA-P0, blocking)

Aux's Yip 0115 report identifies a current missing check: the grant redemption
path does not record or reject a previously debugged requester. An equal-cap
peer can modify an unelevated process, detach, and wait for a legitimate later
elevation. Source inspection confirms no prior-debug gate in
`cap_redeem_grant_for_writer` / `proc_become_legate`; no exploit was executed in
this design review. The existing kernel-side repair is owned by Aux; this arc
tracks it as a blocking dependency, not a solved premise.

Before new elevation ships, the kernel must remember debugger control capable
of modifying an execution context, reject tainted contexts at every authority
increase, and serialize debug attachment/control with grant redemption. Taint
must survive detachment, forked address-space derivation and any transition
that preserves attacker-modified execution; a client cannot clear it by sealing
itself or changing identity labels. A clean executable-launch reset, if offered,
needs an explicit proof that no controlled execution state or authority-bearing
continuation survives; ordinary exec must not be assumed to provide that proof.
The repair's exact flags and reset rules land in its own design/implementation.
Tests attach/write/detach before SAK, race attach against redeem, and attempt
taint laundering through native fork/exec. Refusal leaves no pending usable grant.

### 8.1 Typed administrative legate

Extend the existing grant/redeem scope contract with explicit `Execution` and
`Administrative` kinds. An Administrative scope may contain zero operational cap
bits; it confers no DAC, kill, identity-minting or hostowner authority. It carries
kernel-generated scope identity, root incarnation, Corvus issuer incarnation,
policy revision, approved transaction binding and monotonic deadline. Only the
existing trusted clearance issuer can install it. Unknown kinds are rejected.
Execution scopes keep their nonempty-cap and allowed-propagation checks.

The initial administrative scope is non-propagating and single-transaction.
Corvus matches live `srv_peer` scope metadata to its immutable transaction;
ordinary clients cannot assert that they hold the corresponding record. A plain
or native-forked child acquires no administrative authority. The tool remains
untrusted even after activation: it can request only the approved transaction.
Scope-root death, expiry or policy invalidation defeats further commit admission.

No second administrative scope may be nested into an active execution scope.
The user abdicates first. This preserves one live legate scope per Proc.

### 8.2 Principal admission and revocation

Add a Corvus-owned, generation-bearing authorization admission record for a
principal. It is kernel state, not an ordinary inherited handle. Only the boot-
appointed policy authority may change it, using the existing issuer capability
plus an incarnation-bound appointment; holding an administrative user role does
not expose this control to the client.

All identity-bearing spawn/login publication, grant redemption and relevant
scope creation consult the principal admission generation under the lifecycle
synchronization. A restrictive transition closes admission before its process
walk. Fork/spawn cannot publish a child carrying a stale generation or escape
the closing principal. Cross-principal spawn by the trusted login service checks
the destination record; normal processes still cannot set identities.

A revocation barrier enumerates by stable principal/scope membership, never PID
alone, marks and wakes affected processes using the existing group-termination
machinery, and waits for teardown acknowledgement. Success means no affected
thread remains able to execute and no old session may be published. A timeout
leaves admission closed and the transaction visibly revoking; it never reports
complete or reinstates old access. No syscall spins under a global lock waiting
for userspace acknowledgements; use the established fd/poll completion idiom.

Initial restrictive policy changes terminate all sessions for affected principals
rather than attempting an in-place downgrade of arbitrary applications. The UI
must state this before approval. Additions apply to new sessions or new explicit
activations; they do not silently widen already-running processes. Suspension
preserves mandates but closes admission; resume starts fresh sessions only.

The debug authority-cover relation must include administrative scope authority
and the future scoped-use grants, not merely the legacy bitmask. An operational
CAP_DEBUG holder with no corresponding administrative authority must not be able
to commandeer an administrative scope. The current owner-axis bitmask cover does not establish this property, and
current CAP_DEBUG/CAP_HOSTOWNER exception paths must not bypass the new scope
boundary. Same-owner signal/kill authorization remains separate and unchanged:
termination does not allow executing with the target's authority. Sealing is
defense in depth, not the replacement for that comparison. A bare admin marker
bit would also be insufficient: it cannot order two different admin envelopes. `/proc/<pid>/imperium` exposes scope kind and
bounds subject to existing disclosure policy; it never exposes key material.

### 8.3 Resource-scoped use authority

Keep the existing broad operational bit grants for explicit temporary Imperium.
Standing restricted grants use a bounded per-Proc scoped-use table whose entries
retain mandate provenance and session generation. They are never ORed into
CAP_ALL or approximated by a broad CAP_DAC_OVERRIDE/CAP_KILL bit. Services obtain
kernel-authenticated scope metadata and recheck their owned policy state.

| Resource operation | Required selector and enforcement |
|---|---|
| filesystem DAC bypass | stable server/dataset and directory-object root, allowed actions; enforce across walk/open/create and derived handles; reject unsupported FS |
| ownership change | same filesystem bounds plus permitted destination principals/groups; cannot transfer to an arbitrary identity |
| signal/terminate | exact principal/session or versioned managed cohort and signal set; preserve same-user baseline rules |
| service posting | explicit service-name domain plus owner/session limits; reserved TCB names remain ungrantable |
| debug | target coverage and seal semantics at every existing gate; no standing debug entitlement in the first resource-scoped release |
| JIT | current explicit process-bound clearance; no new standing/inheritable JIT through this design |
| audio graph | Nocturne-owned scope, checked there; retain existing explicit clearance until its scoped backend is qualified |

Filesystem path display is informative; authorization follows stable object
identity and the declared root. Rename, hard links, symlinks, rebinding, remount
and remote identity capes must not widen coverage. Until that property is enforced
by the relevant filesystem boundary, refuse the scoped grant. Network views keep
netd's typed endpoint grammar; a principal's mount cape is not permission to
impersonate another remote user. Existing unscoped temporary caps remain visible
as broad authority, not misleadingly labeled as subtree-scoped.

Standing use authority materializes only into authorized login/session roots and
attenuates into children. This is an explicit new grant path, not a weakening of
I-2's native-fork cap rules. Its birth/publication/teardown transitions must be
modeled alongside the existing execution and session scopes.

## 9. Service and handle revocation boundary

Kernel termination alone does not revoke a server connection owned on behalf of
a dead user or a handle independently delegated elsewhere. Every qualified
backend must register its relevant session-generation dependency and implement
`prepare/revoke/ack` with Corvus's barrier. Netd closes affected endpoints and
blocks reopen; filesystem/login owners close governed sessions and private
mappings. Admission is closed before draining or ending them.

A backend may complete bounded work admitted before the revocation barrier; the
barrier does not acknowledge until such work is retired or cancelled. No new
operation may pass its authorization gate afterward. Service acknowledgements
are accepted only from the registered owner incarnation and exact transaction/
generation; a service name or client-supplied PID is not authentication. Provider
registration belongs to the boot/policy authority, not ordinary service posting.
Corvus drives barriers as bounded asynchronous state, continuing to serve status
and cancellation where valid; it does not block its entire 9P loop waiting for
one provider. Owner loss closes the governed admission until reconciliation. Copied data, completed
writes and independent valid grants are not undone. Revoking an issued grant
must revoke its graph dependents, including recipients in other principals.

For a scoped grant to support raw handle export, every derived handle/mapping
must preserve the revocation dependency through dup, fork, 9P transfer and
remount, and its backend must acknowledge invalidation. Until that propagation
exists, reject export and any grant class requiring it; namespace-name removal
alone is not revocation. This qualification gate is part of UA-6, not optional
hardening after shipping. Voluntary proxying of previously learned data remains
outside this authority-revocation guarantee.

## 10. Transaction and trusted-path protocol

Reuse the `/srv/corvus` 9P transport. Add a versioned authority protocol, keeping
old verbs compatible during migration. Symbolic operations:

- `AUTHORITY_QUERY`: paginated bounded discovery/explanation; self by default.
- `AUTHORITY_PREPARE`: one canonical operation, expected revisions and a client
  idempotency ID; validates without mutation and returns a transaction ID.
- `AUTHORITY_REQUEST`: binds that exact transaction to the live peer and parks
  the response for SAK, following the existing deferred-read lifetime rules.
- `AUTHORITY_STATUS`: query the durable outcome after interruption or reconnect;
  bound to actor identity and appropriate disclosure permission.
- `AUTHORITY_CANCEL`: cancel a prepared or awaiting-auth transaction; cannot
  retroactively cancel an already admitted durable commit.

Use explicit protocol versions, length-prefixed fields, LE fixed-width integers,
canonical enums and counted vectors. Numeric IDs and the exact packed layouts
must be reserved in an ABI-only UA-0 commit across C/Rust/Corvus/seat mirrors,
including size assertions and unknown-version refusal, before code consumers.
Do not silently enlarge the current small Corvus frames without memory bounds.

Initial protocol limits: 4096-byte mutation payload, 8192-byte response page,
64 listed records/page, 16 resource/subject selectors per record, 8 supports per
record, graph depth 16, 4096 live records and 256 records per subject. Corvus's
existing 256-user/512-group limits remain until independently changed. Reserve
all mutation, graph and audit space before authentication; exhaustion leaves the
old policy intact. At most one unauthenticated prepared transaction per principal and 64 system-wide
may be retained for two minutes; the existing single trusted episode remains
system-wide. Anonymous preparation is refused. Release transient reservations on
expiry/cancel, while retaining committed outcome identity in the durable ledger.
Rate-limit preparation separately from key verification. Pagination cursors bind
snapshot revision and authorized viewer; stale cursors request a restart, never
silently omit entries.

The trusted path is:

```
Prepared -> AwaitingSAK -> Visible -> Authenticated
         -> Revalidated -> CommitAdmitted -> Committed / Revoking -> Complete
```

Cancellation/death before CommitAdmitted changes no persistent policy. Request
names, peer incarnation, supports, group membership, key generation, policy
revision and target revision are rechecked after authentication. Any difference
invalidates approval; display the new transaction and authenticate again. Never
widen or silently substitute after the operator has seen a preview.

Persistent changes add a distinct completion contract to the current transient
Imperium flow: authentication alone does not mutate policy. First restore the
ordinary seat and receive its acknowledgement, then admit commit while the
single-transaction administrative scope and all dependencies are still live.
Restoration failure cancels the transaction. The durable commit is then allowed
to finish even if the client disconnects or the administrative deadline elapses:
CommitAdmitted is the linearization of authorization, not an indefinitely usable
permit. Its immutable request can neither grow nor be repurposed. A new request
must reauthorize. The operator can query the final outcome by transaction ID.

The Corvus gate serializes conflicting policy changes. Kernel revocation and
spawn admission interlock with commit so that an already admitted restrictive
operation completes conservatively, whereas a privilege-increasing operation
cannot race a source revocation into an effective unsupported grant. Source
revocation has priority at materialization, even if a durable child record was
committed immediately before it; the child becomes revoked through its support.

## 11. Trusted scene and ordinary UI

Use the existing solid-background Lex curiata scene and Ctrl+Alt+F10. Corvus
supplies typed semantics; Lictor lays them out. A mutation shows:

- authenticated actor and requesting process incarnation;
- target name AND stable principal, or stable mandate ID;
- operation and complete before/after authority delta;
- standing access versus activation/administrative eligibility;
- resources, subjects, term, further-delegation bound and source authority;
- whether active sessions will be terminated and which targets are affected.

A multi-page transaction must be paginated on the trusted surface with page
count and explicit final confirmation; no hidden or ellipsized authority.
Finite bounded detail prevents memory abuse. If the semantic model cannot
represent the requested operation, refuse it rather than fall back to a normal
Halcyon confirmation. The existing LCUR semantic wire model needs a versioned
extension; old renderers must reject unsupported transaction frames.

Use `Confer imperium` for an activation and a precise action title for a policy
mutation (for example `Suspend user`). Do not draw the axe just because an
administrator can edit accounts; display it when the actual transaction grants
termination authority. Explain session termination separately. Beacon may
present discovery, readable policy differences and receipts; it never collects
secrets or sends the trusted confirmation.

## 12. Durable commit, crash recovery and audit

Use a Corvus-owned versioned policy ledger under `/var/lib/corvus/album`, with
transaction IDs, canonical records and integrity checks. No cryptographic bearer
ticket is needed for local authority. Stratum integrity and sole-writer isolation
are required; integrity hashes alone do not establish freshness against rollback.

Account, group, eligibility and mandate mutations must have one logical commit.
Do not implement an enrollment as unrelated renames of identity.db, clearance.db
and an album file. Use a durable transaction journal plus versioned snapshots
and one published root, with checked replay/idempotency. UA-0 must demonstrate
the actual Stratum durability primitive and crash semantics; host POSIX fsync
assumptions are not evidence about the guest.

Additions become usable only after durable commit. Restrictive transactions:

1. Durably record intent and the affected dependency closure.
2. Close kernel and service admission; enter Revoking.
3. Revoke dependent sessions/grants and collect completion acknowledgements.
4. Publish the restrictive state and audit result, then acknowledge Complete.

A crash after step 1 replays toward restriction, never resurrects old access.
Corvus restart keeps user admission closed until replay and reconciliation finish.
Its death invalidates active administrative scopes and closes the kernel
admission generation through the issuer-incarnation death hook. Resource-owner
operation admission is tied to that generation; previously admitted work may
retire, but a stale owner epoch cannot admit another operation. Existing user
authority is closed/reconciled through this kernel and service barrier, not an
unbounded best-effort notification. Provider restart cannot adopt old sessions. Founding
recovery has a separate path so this failure mode is recoverable.

A timeout remains pending with admission closed. `AUTHORITY_STATUS` distinguishes
Denied, Cancelled, Committed, Revoking, Complete and recovery-required outcomes.
Never return a generic failure that encourages blindly retrying an already
committed enrollment. Repeated actor + idempotency ID + identical canonical
intent returns the original outcome; reuse with different data is rejected.

Audit records include actor principal/incarnation, source authority IDs,
transaction ID, target, old/new policy revisions, public delta, authentication
method (not secret), outcome and revocation acknowledgements. Mutation plus audit
is atomic; no space for audit means no mutation. Audit reader access is scoped.
This is accountable history, not a claim of tamper resistance against a hostile
kernel or machine owner capable of rolling back storage.

## 13. Account and membership lifecycle

Account states are PendingEnrollment, Active, Suspended and Retired. Admission
is derived from both state and current policy generation. Principal IDs are
never recycled, including failed or cancelled durable enrollments. Renames keep
the principal, existing ownership and attribution stable; both old and new names
appear in audit. Homedir/dataset display rename is a separate recoverable storage
step, not a change of the cryptographic subject binding.

Group membership changes compute the before/after effective authority across
all affected mandates and eligibility. The administrator must cover every added
right, including administrative eligibility. Reductions close affected sessions;
additions require fresh materialization. No group grants of a shared distinct
secret; each eligible human enrolls a separate verifier.

A per-user clearance key reset invalidates that verifier generation, pending
requests and active scopes using it. It does not silently rewrite other users'
durable grants that the subject previously issued under valid policy. The
trusted preview must distinguish that from `authority.revoke` and domain-key
rotation, which revoke the appropriate grant dependencies. After suspected
compromise, policy inspection/revocation is a separate explicit operation; key
reset alone must never claim to undo previously issued authority.

Suspension overrides even a system-issued floor without deleting it. This is
admission authority, not permission to rewrite a protected founding mandate.
Resume requires its own allowed operation and does not restore old activations.
Retirement is irreversible under normal administration, retains attribution,
and leaves encrypted storage subject to a separately authorized retention policy.
Deleting the last ordinary user never reopens bootstrap.

## 14. Installer, founding and recovery

The installer holds a boot-designated founding channel available only in the
explicit installation state. An empty or damaged identity database is an error
outside that state, not permission for unauthenticated USER_CREATE.

The ceremony establishes: installation identity, recovery custody, initial human
principal, baseline template, protected founding roots and that human's bounded
administrative eligibility. It then durably closes founding admission before
launching ordinary sessions. Initial authority is not acquired by choosing a
special UID or username. Installer exits and its temporary authority is destroyed.

Founding recovery requires local trusted attention and the system recovery
credential. It can appoint a replacement administrator or repair policy admission,
with exact changes shown and audited; it cannot silently decrypt an existing
user home without that home's recovery material. No ordinary censor may remove
all recovery roots or retire a protected founding custodian. Changes to recovery
custody require the founding ceremony. Hardware/boot replacement remains outside
the ordinary administration guarantee.

## 15. Migration and compatibility

No code migration occurs in this design commit. Implementation must:

- Preserve current user IDs, group IDs, key verifiers and valid eligibility;
  never infer a permanent grant from a transient elevated process.
- Convert current known baseline policy into versioned founding records and
  current eligibility into Activate records. Require a trusted migration ceremony
  to choose the initial administrative custodian; do not guess from first UID.
- Keep broad hostowner only for install/recovery while routing ordinary user and
  clearance mutations through the new transaction gate. An old admin verb must
  not remain an unreviewed bypass: disable it in production after cutover or have
  it invoke the identical policy transaction machinery.
- Preserve existing operational Imperium behavior and tests. `--for`, capability
  descriptions and policy term discovery may land as an independent compatible
  client improvement; they do not pretend Admin or mandates already exist.
- Treat old clients as unable to manage the new policy safely. Unknown new grants
  are not flattened into the old capability bitmask. Migrations are explicit,
  versioned, restartable and fail closed on partial/corrupt input.

## 16. Implementation sequence and acceptance evidence

| Stage | Deliverable | Required evidence before advancing |
|---|---|---|
| UA-P0/P1 | close precursor-debug taint and define non-bit authority coverage | Aux repair integrated; taint laundering and attach/redeem races denied; ledger remains visible |
| UA-0 | ratify exact contract; codec/ABI reservation; model; limits and durability validation | clean model and deliberate counterexamples; mirror registry and crash primitive evidence |
| UA-1 | pure policy engine, canonical records, coverage, dependencies and read-only discovery | malformed/unknown fields, domain incomparability, group/self-escalation, graph/capacity limits; no mutation endpoint yet |
| UA-2 | kernel Admin scope and principal admission/revocation; peer/ledger/debug coverage | fork/redemption/revoke/death races under SMP; no zero-bit authority invisibility or PID reuse |
| UA-3 | Corvus transaction journal, replay, audit and qualified service barriers | crash at every persistent boundary, owner death, timeouts, idempotent reconnect, full disks, source revoke during commit |
| UA-4 | Imperium commands and LCUR administration scene; enroll/profile/suspend/resume and eligibility | real Halcyon + serial recovery tests; no PTY secret; exact target/delta; failed restoration cannot commit |
| UA-5 | namespace mandates, delegation, groups, installer and production migration | two censors with disjoint domains; persistent grant survives logout; parent revoke cascades; disable floor; no bootstrap reopening |
| UA-6 | bounded resource-scoped use table and qualified filesystem/signal/service backends | no broad-bit approximation; path/alias escape and handle-export/revocation tests; unsupported classes refused |
| UA-7 | integrated production qualification, operator manual and Vault | fresh install, upgrade, recovery, reboot/crash matrix; QEMU and separately qualified physical seat backend |

Initial policy limits are bounded in §10, not inferred from a successful small
fixture. Stage reports include memory at maximum records/depth, admission and
revocation latency, parser input limits, and the largest trusted transaction
layout. Recovery remains testable when ordinary admission is closed. Keep the
/srv registry-headroom repair ahead of multiuser acceptance runs; do not hide it
by shrinking the user fixture.

UA-P0/P1 are explicitly tracked in `docs/ASTRA-2026-09-24-STATUS.md` and
`arc-user-authority`; their regression names must be supplied by the implementing
commits. They block administrative elevation acceptance even if the UI passes.

The required model is `specs/mandate.tla`, already reserved under I-35, extended
or composed with `imperium.tla` and admission/session lifetimes. Actions include
Prepare, Authenticate, Restore, AdmitCommit, Publish, Fork, RevokeSource,
Suspend, Rotate, Crash, Replay and Resume. Properties:

- no grant exceeds its explicit supporting envelope;
- every effective grant has live support to a founding root;
- no scope/child survives the applicable revocation barrier;
- no child or redemption publishes through closed/stale admission;
- shown intent equals committed intent and restoration precedes commit admission;
- no ambient administration; no principal identity substitution;
- no resurrection on crash/replay, expiry or ID reuse;
- policy mutation and audit are one logical commit.

Negative models deliberately omit one of support-generation recheck, spawn
admission, descendant cascade, group-delta coverage, restore gating or replay
restriction and must produce a counterexample. They are detection witnesses,
not a substitute for runtime coverage. The existing relevant model mutants also
run when their mechanisms change. This honors the mandate-specific spec-first
reservation rather than relying on the general suspension.

Runtime tests must include an unprivileged actor forging a transaction ID, a
network censor attempting a filesystem grant, a use-only holder attempting
permanent delegation, an administrator compromised between preview and commit,
a stale eligibility verifier after reset, nested/aliased self-escalation, target
rename and group edits during confirmation, loss of Corvus/Lictor, and disconnect
on either side of commit admission. Successful grant tests must prove the actual
operation works and its adjacent denied operation remains denied.

Reviews follow the operator's standing single-agent instruction unless changed;
self-review must not be labeled independent. Full implementation remains subject
to adversarial review staffing direction and the repository's required build,
boot, SMP, model and interactive gates. Pi 400/500 qualification is measured on
those platforms; QEMU results do not imply it. No gate is claimed run by this
specification-only change.

## 17. Ratification and review checklist

The operator has endorsed the authority separation and Imperium driver. The
following concrete choices are exposed for review, rather than hidden in code:

- Corvus-scoped administrative operations and zero-operational-bit Admin legates,
  with mandatory taint admission and non-bit debug coverage.
- One trusted approval per immutable administrative transaction; no admin shell
  or cached-key proof in this delivery; restore before commit admission.
- Until-revoked delegation from explicit durable envelopes, with dependency
  cascade; scalar clearance ordering replaced by coverage/provenance.
- Terminate all affected user sessions for restrictive policy changes initially;
  never claim that deleting a name revokes previously opened resources.
- Disabled enrollment plus target-owned credential onboarding; trusted founding
  recovery kept separate from ordinary account administration.
- The default terms, resource limits and staged scope in sections 6, 10 and 16.

Ratification should amend I-35 and mirror these decisions in the owning design
and ABI registry before implementation. This document intentionally makes the
costs and behavioral changes reviewable without first deploying them. The
specification does not reserve implementation IDs or claim a full security audit.

## 18. Prior art and rejected alternatives

Plan 9's [factotum](https://9p.io/magic/man2html/4/factotum) places authentication
in a userspace filesystem service with private RPC conversations and explicit
key-use confirmation. Its confirmation helper is not itself Thylacine's trusted
seat: retain the file-service idiom but preserve the stronger kernel/seat boundary.
Plan 9 [users](https://9p.io/magic/man2html/6/users) and
[auth administration](https://9p.io/magic/man2html/8/auth) also distinguish identity
and group administration from ordinary filesystem clients; they are heritage,
not a ready-made mandate implementation.

[seL4](https://docs.sel4.systems/Tutorials/capabilities.html) supplies explicit
object authority and derivation/revocation. Corvus's support graph applies that
lesson to durable human policy. [Genode](https://genode.org/documentation/genode-foundations/25.05/architecture/Recursive_system_structure.html)
mediates session construction through policy-bearing parents; qualify resource
sessions rather than trusting namespace display alone.
[Fuchsia routing](https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities)
and [Zircon rights](https://fuchsia.dev/fuchsia-src/concepts/kernel/rights) show
explicit routes and separate transfer/duplication rights. Their kernel mechanisms
do not supply Thylacine's human account policy automatically.

Rejected: a monolithic CAP_USER_ADMIN; exposing CAP_HOSTOWNER through imperium;
inferring delegation permission from operational power; a signed local token
without revocation ownership; a daemon that trusts the client's user/PID; silent
live privilege additions; pretending unlink/unmount revokes open handles; and
using the absence of users as production install authorization.

The synthesis develops NOVEL angle 12, not a claim that delegation or capability
security is new. The additions with real architectural cost are typed admin
scopes, admission/revocation interlocks, transactional storage and qualified
scoped-resource enforcement. They are explicit delivery stages, not cosmetic
changes concealed in a management client.
