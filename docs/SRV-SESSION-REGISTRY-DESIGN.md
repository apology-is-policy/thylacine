# Session service registries: D7 completion

Status: APPROVED by the operator on October 1, 2026: "Approve the proposed
contract and implementation." D7's private namespace direction, the factory
authority/ABI and the resource partition below are ratified.
Owner: Astra, single-agent. Work item: O1-SRV-1. Base: `71bcf7aad`.

## Outcome

Each login gets its own mortal `/srv`. Home proxies, Halcyon and capability-
posted Haul servers publish there. Logging in under another name does not
consume another permanent slot in boot's registry; simultaneous logins of the
same principal get distinct registries. Resident services remain reachable
through fixed routes. Existing CAP_TCB_DIAL and scoped posting checks continue
to decide authority; possessing a route never substitutes for either check.

This completes STALK-DESIGN 5.1/D7, preserving D8. It does not introduce a root
user, recycle trusted names within a live registry, or increase the 16-entry
local service table. The completed listener-retention and atomic connection-
admission checkpoints are prerequisites, not the completed isolation feature.

## Why this shape

Plan 9's `/srv` publishes references to open channels, and publication holds a
reference independently of a process's descriptor. Thylacine additionally
stamps each new client's identity and protects trusted restart names; copying
Plan 9's unlink semantics alone would discard those guarantees.
[Plan 9 srv](https://9p.io/magic/man2html/3/srv)

Fuchsia explicitly routes provider protocols into a consumer's namespace.
That supports a selected resident-service view rather than a mutable parent
registry inherited as a fallback.
[Fuchsia protocol capabilities](https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities/protocol)

Genode's parent assigns child resources and routes or denies session requests.
Here the trusted session launcher selects routes while kernel bookkeeping
keeps the resulting allocation budget attached to its lifetime.
[Genode init](https://genode.org/documentation/genode-foundations/25.05/system_configuration/The_init_component.html)

Alternatives considered:

- Raising 16 to 32 postpones name accumulation, preserves cross-session names,
  and enlarges the existing drain/exit stack arrays. Rejected.
- Recycling every trusted tombstone permits a different poster to capture a
  resident service's name during restart. Rejected.
- A union with boot's mutable registry exposes posting/name resolution outside
  the intended private domain and complicates charging. Rejected.
- A transferable factory descriptor can express narrower delegated creation
  rights in a future session supervisor. It adds a new handle/control protocol
  now, while login already receives bootstrap roles through spawn. The proposed
  dedicated spawn role fits the current launcher model; factory authority must
  not be folded into MAY_POST_SERVICE or CAP_SET_IDENTITY.

## Factory authority and ABI

Add `SPAWN_PERM_SESSION_REGISTRY` as bit 10, using the existing one-hop explicit
spawn-permission discipline. Bootstrap Joey is the initial grant root. Joey
passes the role only to login; login does not pass it to a shell, compositor,
home proxy or application. Rfork does not automatically inherit the role; exec may retain the calling
Proc's existing role but never mint it. Test every spawn form.
Holding CAP_POST_SERVICE, CAP_TCB_DIAL or MAY_POST_SERVICE alone is insufficient.
Do not add an Imperium-grantable capability for creating fresh quota domains.

Reserved native syscall 127:

```
SYS_SRV_REGISTRY_NEW(source_fd, routes, route_count, flags) -> root_fd | -errno
```

The number is unused in the inspected stable and unfinished stop branches;
reserve it, bit 10 and all mirrors in the scripture/ABI commit before consumers.
Update native-ceiling assertions and the C/Rust syscall mirrors together. The
separate authority drafts reserve no conflicting kernel number in this base.
Recheck Yip and refs immediately before assigning numbers. Internal Proc role bit 29 is reserved; the public
spawn-permission bit is 10. Only a holder can delegate it; console attachment
alone is not a factory grant root. Kernel bootstrap explicitly stamps Joey.

`source_fd` must be an O_PATH devsrv root for the boot registry. A private root,
service leaf, connection or unrelated Dev is refused. This first implementation
has exactly one routing level; no parent cycles or recursive factory trees.
`flags` must be zero. Route count is 0..16. Each route is a fixed 40-byte record:

| Offset | Field | Requirement |
| --- | --- | --- |
| 0 | u32 name_len | 1..32 |
| 4 | u32 reserved | zero |
| 8 | u8 name[32] | printable single component; zero unused bytes |

Names are unique; slash, control bytes, `.` and `..` are refused. Copy and
validate the complete bounded manifest before publishing anything. The returned
KOBJ_SPOOR root owns one registry reference. Existing devsrv non-alias/transfer
restrictions remain; namespace clone/walk retains the registry normally. The
factory does not mount or alter the caller's namespace itself.

Authority refusal is EACCES; malformed flags/manifest is EINVAL; an invalid descriptor
is EBADF, a valid descriptor of the wrong kind is EINVAL; memory failure
is ENOMEM; exhausted registry-domain capacity is ENOSPC; exhausted fd table is
EMFILE. Rollback returns every allocation/reference/admission ticket. No new
errno number is introduced. A failed construction publishes no partial root.

## Routes and local names

A registry contains up to 16 private service entries plus up to 16 immutable
route names. Route names do not consume private posting slots. Its source root
is retained internally, never exposed as a namespace alias or child fd.

A route names a boot service, not an already-open transport. Every connect
resolves its current posting, snapshots generation/mode/cape/remote/poster under
that registry's lock and validates the generation again on enqueue. Route
resolution must refuse a capability-posted source entry: a route to a resident
service can only resolve a trusted posting. An absent/offline resident remains
an unavailable reserved route, never a locally claimable gap. A later trusted
restart becomes reachable without reconstructing every session.

Routes cannot be overridden by local create, including by a session's trusted
home proxy/compositor. User-created local services cannot shadow `corvus` or
other routed names. Local trusted tombstones keep their existing restart
semantics until the entire registry retires. Capability-posted recycling and
its four-slot/two-per-scope bounds remain local to the session registry.

Walk metadata uses a stable, registry-local route identity disjoint from local
posting qids. A route holds no borrowed service pointer across unlock. Local
posts retain existing fresh-qid behavior. Test leaf mounts, clone/walk, offline
routes, restart and forbidden shadowing explicitly; do not assume old service-
ref Spoors pin a posting generation (today they hold a name and registry).

Login's initial route manifest is explicit and reviewed against producers:
`corvus`, `net`, `nocturne`, `nocturne-ctl`, `lictor`, `tapestry`, `warp`,
`stratum-fs`, `stratum-ctl`, `ptyfs`, `diorama`. Missing optional residents may
stay reserved/offline; no wildcard fallback. Add another resident only by an
intentional login-policy update. No home-*, halcyon-* or boot-test service routes.
Existing byte-service gates still deny the ordinary shell's direct coordinator
connection while admitting its sealed CAP_TCB_DIAL home proxy.

## Resource policy and ownership

Ratified initial bounds:

| Resource | Bound | Meaning |
| --- | --- | --- |
| All allocated/in-flight connections | 64 | existing global bound |
| Connections charged to session domains combined | 48 | preserves 16 slots from session consumption |
| Connections charged to one session domain | 16 | one session cannot monopolize the guest partition |
| Retained session resource domains | 16 | includes retired registries with retained connections |
| Private names per registry | 16 | unchanged local table; routes separate |

Boot may borrow unused capacity up to the global bound; it is not capped at
16. Session traffic cannot consume the protected 16-slot margin. This is
bounded admission, not a promise that sixteen simultaneous sessions can each
obtain sixteen connections. Saturation fails promptly with ENOSPC. No forced
connection revocation or blocking credit wait is introduced. Validate actual
boot, console and graphical demand before activation; if these proposed bounds
cannot satisfy the acceptance workload, report the measurement and revise the
policy before claiming completion.

The requesting namespace view owns the charge, including connects routed to
boot. Accepted server endpoints, kernel 9P attachments, clones and retained
poll operations continue to consume that SAME ticket until final destruction.
Do not charge solely to the destination server or infer a quota from the
caller's mutable `/srv` path after an operation has begun.

Use a small refcounted resource-domain object, separate from the name registry.
The registry and its connections retain this object; it does not retain the
registry. This avoids a registry -> backlog connection -> registry reference
cycle and keeps retired connections charged without keeping all private names
alive. Domain capacity returns only when registry and connection references
are gone. Namespace rebinding, closing a listener, teardown, fork or selecting
another pathname cannot create a new domain. Only the trusted factory can.

A single short admission critical section checks and updates global, guest and
domain counters together; no allocation occurs under that lock. Partially
reserving the global pool before a failed domain check must not transiently
consume boot's protected margin. Rollback and final destruction release all
three counts after freeing transport storage. Preserve the focused constructor
failure/interleaving tests from O1-SRV-2 and extend them for the partition.

## Posting and process death

Track posting membership by actual registry object, independently of current
namespace and listener handle lifetime. Use a per-Proc posting lock/closed latch
and a deduplicated membership list retaining every registry in which that Proc
successfully posts. This is a lifetime ledger, not the registry selected for
name resolution; D7's namespace residency is unchanged. Membership is not
inherited by child Procs.

Prepare membership storage before taking spinlocks. Hold the posting lock
across reservation, fixed-table handle installation and commit, with registry
and handle locks taken separately beneath it. This makes each post either
fully precede death or fail after the closed latch. There must be no allocating,
sleeping, connection teardown or final registry free under the posting lock.
Failed posts roll back their provisional slot/ref and unpublished membership.

Exit closes admission and detaches the membership list under that lock, then
outside it tombstones/drains each owned registry and releases its membership
reference. Never scan a global list of all session registries by stripes or
re-resolve `/srv`. Keep the existing drain-under-registry-lock / teardown-and-
wake-after-unlock discipline, and the accepter generation/identity pin. Repeated
notifies are idempotent. `proc_free` covers construction/rollback paths that did
not pass through ordinary exits; all existing exit and group-death paths need
review. A closed listener does not unpost its service.

## Login and teardown

Create and MREPL the private `/srv` before opening Corvus/authentication, so even
pre-authentication connections use the session budget. Login closes its source
boot-root descriptor and constructor root fd after mounting. The replacement
must remove the inherited boot mount, not leave an unmountable hidden alias that
a user can reveal. Test unmount and `..` resolution, source descriptor handling,
spawn inheritance and direct-Dev refusal.

Then run existing authentication, home provisioning and proxy launch; those
operations find resident services through fixed routes. The proxy and session
leader inherit the private registry. Login retains only its already-open
control connections, never passes the factory role or boot root to children,
and still applies the established sealed-process/debug rules.

Failure before or after home launch kills/reaps only owned children, closes
control channels and releases mounts/references. Ordinary logout keeps the
existing session-hangup, home unmount/proxy reap, DEK eviction and session-close
ordering. Registry and domain reclamation follow actual references; do not
force-free a registry or connection because its login process exited. A
same-user second login must neither collide with nor retire the first registry.

## Verification and delivery

1. Scripture/ABI reservation and complete C/Rust layout fixture before consumers.
2. Kernel factory/route, posting-death and quota primitives, with focused tests.
3. Login activation; error causes propagate through native/pouch callers.
4. Three distinct simultaneous users plus a capability-posted Haul service;
   repeated login/logout beyond sixteen distinct names; two same-user sessions.
5. Isolation, reserved offline-route/restart tests, D8 positive/negative control,
   source-root hiding, permission/delegation negatives, all allocation/handle/
   mount rollback paths, poster-exit versus post and accept/poll lifetimes.
6. Domain/global exhaustion and release, protected boot margin, cross-domain
   independence, routed-connection charging, retained closed connections and
   namespace replacement. Publish measured normal demand and remaining margin.
7. Ordinary single-CPU boot and focused console/graphical media/SAK/manual/Haul
   regressions, with real screenshots. Update manual, status and Vault, perform
   single-agent adversarial self-review, use normal hooks, and notify Main/Aux.

Astra's October 1-2 waiver suspends 50-boot, ASan, UBSan and SMP gates only.
Do not call them passed for this work. Existing applicable model negatives still
run; the broad Corvus clean run was incomplete at 180 seconds in the preceding
checkpoint and cannot be cited as clean. Constructor/domain schedules need
focused witnesses; that existing model does not represent their allocation.
No Main landing, unfinished stop-branch import, clipboard activation or Pi
hardware qualification is implied by this specification.

## Baseline observation before activation

On `71bcf7aad`, a fresh CI console login produced 14 `conn` rows in one
`/ctl/9p-sessions` snapshot. This is a point-in-time global count, not a session
peak or per-domain attribution. The new diagnostic/test fixtures must measure
domain charging after implementation. Evidence and matched kernel/ramfs/pool/key
artifacts: `work/oct1-srv-sessions/baseline-ci/` and `baseline-console.log`. The
run used one CPU, completed a real home-backed login and logout, and exited zero.

## Discovered authentication prerequisite

The D7 real overlap witness reached the first private home and then Corvus
refused the second AUTH. Its singleton AUTH slot is an implementation narrowing
of `corvus.tla` AuthSuccess, which already permits separate owner Procs. Completing
the approved concurrent-login acceptance therefore also requires lifting that
narrowing. Keep the existing eight-connection Corvus bound and at most one AUTH
session per kernel-stamped owner stripes, with the exact creating connection
owning teardown. No wire verb, token format, new capability or larger connection
limit is introduced. Store each immutable user/principal, token and keypair
independently. Select token-bearing operations by that token, never a global
current-session variable. Forwarded tokens retain their existing UNWRAP use;
only their creating connection can SESSION_CLOSE them. Closing another connection
cannot erase them. Wipe every retired slot's secrets before reuse. CLEARANCE_ACTIVATE_SELF
continues to require a live proof for the requesting principal; with multiple
records, compare its captured principal identity rather than choosing whichever
session happened to be installed last. The same-user overlap and first-logout
witness must also verify the storage coordinator's per-connection DEK leases.

The same-user acceptance also requires Stratum to retain one authenticated
DEK lease per connection/dataset pair. Each new connection proves UNWRAP even
if another session has installed the key; only the final lease release evicts.
The existing 64 lease entries bound pairs, and provisioning reserves its lease
before installing a new key. Disk/wire formats and SYSTEM-only gates stay fixed.
Implementation is isolated in `stratum-astra`, branch `codex/astra-session-dek`.

## Storage failure boundary discovered during acceptance

Final home-key eviction must first drain dirty filesystem buffers while their
DEKs are installed. Otherwise the next whole-pool commit can fail ELOCKED on
the logged-out home, breaking unrelated Corvus account persistence. The Stratum
fix drains under `fs->global` EX, excluding writers through key removal; a drain
failure preserves the key and returns the error. This is not a durability
commit, and only the last proven lease requests eviction.

Explicit eviction retains the lease on failure so the caller can retry.
Connection destruction retains Stratum's existing best-effort policy: it cannot
retain the dying connection identity, and failed final eviction can leave a key
resident. D7 does not claim storage-failure recovery or prompt key erasure in
that case; follow-up must specify recovery without losing dirty data or allowing
new accesses. Normal logout, subsequent commit, relogin and surviving sessions
are the acceptance witnesses for this change.

## Post-activation demand measurement

Measured console demand in `haul-measured-1790862575463068000` (exit zero,
42.18s): three live users consume 4+4+4 session connections before mount,
12/48 aggregate and 22/64 global, with 3/16 retained domains. After a completed
read while Haul remains mounted, the active session uses 5/16, the other two
4 each, aggregate 13/48 and global 23/64 (boot 10). Remaining margins at that
snapshot are 11 active-session, 35 session-aggregate, 41 global connections,
and 13 domains. These are measured snapshots, not peak-workload guarantees.
