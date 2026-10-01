# Halcyon terminal ownership: implementation contract

Scope approved September 24, 2026; see HALCYON-INTERACTION-PTY-REVIEW.md.
The kernel implements these operations on Astra, with userspace consumers still
pending. The constants and record mirrors are pinned independently of runtime
verification. See HALCYON-INTERACTION-STATUS.md for the measured coverage.
Existing SYS_PTY_REGISTER operations 0..2 remain unchanged.
New operations use that syscall's unused suboperation range 16..21, not a new
syscall number. Main's pending SYS_BURROW_MAP_FILE = 126 remains untouched.

## Authority and storage

The existing kernel pts registry remains the only foreground-group authority.
A separate, fixed pool of 64 interaction bindings carries observers of those
pts incarnations. A binding is granted by a holder of the real master Spoor;
a guessed terminal number, principal or route token cannot mint one. The binder
must be sealed before its first user instruction. The compositor recipient is
the live poster incarnation resolved from an actual SrvConn-backed connection
held by the binder, not a caller-supplied PID. It need not have the binder's
principal: Tapestry is a system service. The binder's principal must match the
terminal controller being nominated.

A binding carries a nonzero monotonic identifier, pts generation, binder and
observer process stripes, a foreground epoch, acknowledged epoch, nominated
controller stripes and a change revision. It grants no terminal bytes, signal,
foreground-setting, identity or general process-inspection authority. The
connection used to identify the recipient may be closed after binding; this is
a grant to the exact live poster process, not a connection keepalive.

One binding per pts; replacement requires revocation. Live master holders do not
silently steal an existing binding. There are at most two watcher objects per
binding, one for each role. A duplicate fd shares its watcher's read cursor.
Revoked bindings with outstanding watch references occupy their bounded pool
slots until those references disappear; a new bind then returns a capacity
error, rather than allocating an unbounded retired list. Terminal allocation and
legacy job control continue even if the interaction pool is exhausted.

## Operations

All registers are u64. Handle arguments must fit the native handle range before
narrowing. Every unused argument and reserved byte must be zero. Structures use
fixed little-endian integers, no pointers embedded in the ABI and no native
padding. Kernel fronts copy/validate complete inputs before locking and copy
outputs only after unlocking. Constants require kernel, libt C and libthyla-rs
mirrors plus byte/offset assertions before consumers.

| Value/name | x1 | x2 | x3 | Result |
|---|---|---|---|---|
| 16 BIND | master fd | connection to observer service | 0 | positive binding ID |
| 17 UNBIND | binding ID | 0 | 0 | 0 |
| 18 WATCH | binding ID | 0 | 0 | read-only pollable fd |
| 19 STATE | binding ID | output address | 80 | 0 |
| 20 ACK | binding ID | input address | 24 | 0 |
| 21 CHECK | binding ID | input address | 24 | 0 |

BIND resolves the master through the existing dev9p/SrvConn/qid correlation,
requires the master side, and holds all borrowed Spoors until every derived
identity has been captured. A devsrv CLIENT endpoint or a dev9p root backed by
a SrvConn may identify the observer service; a server endpoint, unrelated pipe,
loopback/spoor transport or unbacked object is refused. Resolve server stripes
from the transport, then revalidate live binder/observer under the lifecycle
lock. No raw SrvConn pointer survives its borrow.

UNBIND is available to the live binder or observer. STATE and WATCH are likewise
role-bound. ACK and CHECK are observer-only. Every operation revalidates role
identity and binding lifetime, not just the original open. An inherited watcher
fd is not another process's admission credential. The watcher read/poll paths
must apply the same caller-incarnation gate; an asynchronous path that cannot
supply the correct caller context is refused, never attributed to a worker.

The 80-byte state/notification record is:

| Offset | Field |
|---:|---|
| 0 | u32 version = 1 |
| 4 | u32 flags: bit 0 LIVE, bit 1 ACKNOWLEDGED; other bits zero |
| 8 | u64 binding ID |
| 16 | u64 pts incarnation ID |
| 24 | u64 foreground epoch |
| 32 | u64 acknowledged epoch (0 until acknowledged) |
| 40 | u64 change revision |
| 48 | u32 controlling session |
| 52 | u32 foreground group |
| 56 | u64 nominated controller stripes (0 for APP/no participating controller) |
| 64 | u64 binder process stripes |
| 72 | u32 binder PID (live kernel-issued PID, not a request claim) |
| 76 | u32 reserved = 0 |

The ACK/CHECK input is 24 bytes: u32 version = 1, u32 size = 24, u64 expected
foreground epoch, u64 subject stripes. ACK permits zero subject for APP; CHECK
requires nonzero. ACK validates a nonzero subject's live process membership,
principal and incarnation under the same lock interval as the epoch. Replacing
a nomination updates the revision. It does not change the POSIX foreground
group. Tapestry revokes its previous controller/context before acknowledging a
replacement. A pipeline nominates exactly one participant.

CHECK succeeds only when the binding is live, current epoch equals the supplied
epoch, that epoch was acknowledged, the nomination equals the supplied subject,
and that same live process is still a member of the current controlling session
and foreground group, with the binder's principal. This is not a graphical
focus check: Tapestry must also validate the exact declared Halcyon connection,
leaf/surface incarnation, controller/context epochs and normal-seat state while
processing the one matching operation. It then replies with the broker request
ID and its focus epoch. No reusable application bearer credential is returned.

Errors use POSIX-aligned errnos (ENOSPC=28 is added to the kernel/Rust registry):
EINVAL for malformed operands/record; EBADF for a
bad handle; EACCES for missing master/role/membership/seal authority; ENOENT for
retired identity or binding; EAGAIN for an epoch/acknowledgement mismatch or a
watch with no unread revision; EBUSY for an occupied per-terminal binding or
role watcher; ENOSPC for the fixed binding pool or identifier exhaustion; ENOMEM
for a refused watcher/Spoor allocation. No error becomes an empty-success reply.

## Epochs, lifetime and readiness

Every successful controlling-terminal acquisition or SET_FG advances the
foreground epoch and clears acknowledgement/nomination, including A -> B -> A
and a redundant SET_FG(A). There is no new refusal of otherwise-valid legacy
job control: on epoch exhaustion, revoke the interaction binding and disable
new interaction binding on that pts incarnation. The terminal continues its
ordinary path. Binding identifiers lie in 1..INT64_MAX so a successful syscall return cannot
alias a negative errno; they never wrap or alias a retired binding.

An acknowledged controller's exit, exec, setpgid or setsid invalidates that
nomination and advances the foreground epoch before the process lifecycle
change is published, even if the numeric foreground group remains the same.
This prevents a delayed ACK from reviving the previous nomination after exec
or a group change. Binder/observer
death or exec retires the entire binding. Other foreground-group members need
not invalidate a still-live nomination merely by forking; CHECK validates the
nominated process itself. A fork does not create a controller registration.
Libraries close inherited interaction connections in the child and require a
fresh direct bind; intentionally delegating an existing endpoint is not a new
identity and never changes the nominated subject checked by the kernel.

PTY free/re-mint retires the binding. Lazy pts GC performs the same retirement.
A hanging terminal or a lost observer cannot stall legacy SET_FG: the syscall
never waits for userspace acknowledgement. The acknowledgement gates enhanced
modal/controller admission, not the existence of the legacy PTY. An unknown or
unacknowledged epoch is APP, with native clipboard actions unavailable. No
queued enhanced key is relabelled for a new epoch: it is dropped/refused on
mismatch. The terminal host checks the acknowledged epoch before using an
application-owned modal path. Ordinary PTY byte handling retains its established
job-control semantics; the feature must not claim retroactive recall of bytes
already queued to a terminal.

A watcher is an anonymous, nonseekable, read-only Spoor with a bounded cursor,
not an extra /srv listener or namespace-visible device. A read returns exactly
one 80-byte snapshot of the newest revision and advances its cursor; a short
buffer refuses without advancing. Intermediate notifications coalesce. State is
the authority, notification only readiness. EOF/POLLHUP denotes retirement.
Read with no new revision is nonblocking EAGAIN. WATCH starts unread, so initial
state is obtained without a sample/subscribe race.

Use the existing poll waiter discipline: register and observe under the pts
lock; wake after releasing it. No timer polling, callback into userspace,
unbounded event queue or dependency on best-effort note delivery. The watcher
holds its binding reference until all borrowed I/O/poll references retire; the
poll list cannot be reset or recycled with a linked stack waiter. Tapestry
opens watchers only for its registered live leaves: its current 32-pane limit,
8 connection limit and two listeners leave the poll set at most 42 entries.
Pin that combined bound against POLL_MAX_NFDS=64 before integration rather
than silently dropping descriptors. Unrelated kernel bindings do not create
Tapestry watchers automatically. A revision
counter that cannot advance retires the binding and wakes its observers.

## Locking and integration order

The order is process lifecycle (`g_proc_table_lock`) -> pts registry -> poll-list
-> rendez. Membership and identity checks happen while both lifecycle and pts
state are stable. Never take the process lock from inside the pts lock. Existing
proc child-wait readiness already permits lifecycle -> list -> rendez; the new
wait condition must read readiness flags only, never reacquire lifecycle/pts.

Implement a narrowly named process-side helper for the combined live-subject
check, using a bounded iterative tree walk under the lifecycle lock. Do not use
separate proc snapshots and later pts comparison. Lifecycle hooks call the pts
invalidation helper with the process lock already held; it mutates under pts,
drops pts, then wakes stable binding poll lists. Binding/Spoor allocation,
user-memory access, transport I/O, final ref drops and any sleeping occur
outside both locks. The exact implementation must audit every exit path and
poll borrow before the frontend is enabled.

Tapestry's ordered control path owns the operation's graphical admission point.
For terminal clients it includes CHECK above. A later graphical focus change
does not undo an admitted operation. A later terminal-controller invalidation
cancels requests still pending in the broker. Replies are matched to one live
pending operation and scope; revocation already processed before a reply makes
that reply stale. Tests must explicitly order these events rather than relying
on delay lengths. Trusted-seat takeover cancels all normal pending work through
the existing Lictor/Tapestry exclusion path before accepting trusted input.

Before controller registration is exposed, complete the authenticated host-to-
Halcyon binding announcement, Tapestry context publication and acknowledgement,
and direct application peer checks. The kaua-term binding announcement uses Control subtag 9. The earlier unused reservation of 7 collided with the
integrated SyncBegin=7 / SyncEnd=8 records; those shipped meanings remain
unchanged (October 1, Yip 0108 note 40). ScreenErased remains 6. Its fixed body is u32 version 1,
u32 reserved zero, u64 binding ID. Decode requires exactly 16 body bytes and a
nonzero ID, without a variable-length allocation. The record is a locator:
Halcyon sends the binding ID and the child PID it actually spawned for this
leaf over its authenticated declared-session connection. Tapestry, in its
observer role, obtains STATE and checks that snapshot's binder PID against
that expected child before confirming registration. Halcyon is not a binding
role and does not call STATE itself; the wire announcement alone is no proof. The PID comparison relies on the kernel's
existing never-reused PID contract; internal liveness still uses stripes. Both
wire ends must ship together because unknown control subtags are fatal.
Seal only the authority-bearing terminal host at spawn; do not silently seal
ordinary applications. The precise spawn/seal checks use Aux's cleared H3+C
base, including shared-address-space seal propagation and monotonic debug taint.

## Delivery and verification

1. Pin this ABI and mirrors without exposing a working syscall suboperation.
2. Integrate Aux's cleared lifecycle base, preserving the separate authority
   drafts. Add pts state, bounded watchers, lifecycle hooks and frontend gates.
3. Add pure and kernel regressions for every refusal, generation transition,
   pool limit, inherited-handle attempt and poll retirement race. Re-run existing
   clean/mutant PTY models and affected poll models under a Yip lease.
4. Wire the sealed host, Tapestry admission and session broker. Verify actual
   direct SET_FG, nested shell, pipeline nomination, exec/death, SAK and late
   reply cases before enabling Nora/ut clients.
5. Complete the real copy/paste/mode workflow and screenshots, then update the
   operator manual and as-built Vault dossiers. Untested ABI source is not a
   working clipboard, and QEMU evidence is not Pi bare-metal qualification.


## Source integration anchors (reviewed before lifecycle edits)

The current `pts_clear_locked` is the common FREE/GC retirement point; mint
resets `ct_sid`/`fg_pgid`, and `pts_tty_acquire` plus `pts_tty_set_fg` are their
only runtime mutators. The already-owned acquisition branch also returns
success and must advance the interaction epoch under the stated contract.
Do not change `pts_tty_cont` or signal fan semantics while adding observation.

Process hooks belong immediately before publication in successful
`proc_setsid`, `proc_setpgid`, the `proc_exec_replace` address-space swap and
`proc_become_zombie_locked`. Refused operations do not invalidate anything.
Group changes invalidate a nominated subject; exec/death also retire a binder
or observer. Re-read these anchors on Aux's cleared base. Identity is presently
immutable for running processes (`proc_apply_identity` is a spawn stamp); any
future running-identity mutation must join this invalidation discipline.

`poll_scan_one` already retains its Spoor after registering a waiter, and
`poll_unhook_all` unregisters before releasing that reference. The new watch's
close callback therefore releases its binding reference only after every
registered poll borrow has gone. A binding also needs a temporary wake reference
captured **under pts** before staging a post-unlock wake. Hold it through
`poll_waiter_list_wake`, then release it under pts. Otherwise a concurrent final
watch close could recycle/reset the binding's poll list between retirement and
wake. A fixed at-most-64-entry wake batch suffices for process invalidation;
there must be no unbounded retired or pending-wake allocation.

Reserve each role's watcher slot and binding reference before allocating the
Spoor outside locks. Revalidate when publishing it, and roll back both the slot
and reference on allocation failure or intervening revocation. This keeps the
two-watch bound true during construction as well as after publication.

Native `spoor_read_common` invokes Dev.read on the synchronous calling thread;
`poll_scan_one` invokes Dev.poll likewise. Those callbacks can resolve the real
current process and fail a role mismatch before exposing state or registering
a waiter. Kernel `_for_proc` test helpers do not supply an alternate caller to
the Dev callback and must not be mistaken for such a mechanism. Current Loom
payload submission requires `dev9p_client_fid`; the anonymous watcher is not a
9P Spoor and must remain refused there. The synchronous reader's kernel scratch
copy precedes syscall copyout: a copyout fault may consume a notification cursor,
so STATE is the repeatable recovery query, and readiness is never the authority.
Tests must cover this distinction without treating a lost notification as a
successful admission or permission to retain stale ownership.

## Ordered compositor control encoding (October 1 integration)

The existing Tapestry `ctl` fid on the renderer's declared connection also
accepts fixed HIA1 records. This is an internal renderer/compositor operation,
not an application endpoint. Each request is exactly 80 bytes: magic HIA1 at0,
u16 version1 at4, u16 operation at6, u64 nonzero request ID at8, u32 leaf at16,
u32 expected host PID at20, and u64 binding/foreground epoch/subject stripes/
controller generation/context ID/context epoch at24/32/40/48/56/64. The final
u64 at72 is reserved zero. Operations1..4 are Bind, Publish, Check, Unbind.
Bind uses leaf/host PID/binding; Unbind uses leaf/binding. Their remaining fields
are zero. Publish/Check use every identity/epoch field, all nonzero, with PID0.
A fixed40-byte reply echoes the prefix/op/request, then u64 focus, seat and
foreground epochs at16/24/32. Failures use existing Rlerror values. Reads on the
same fid return that immutable reply; one operation per fid prevents replay
from becoming a second admission. Close and reopen for another operation.

Bind verifies the declared session, its exact live surface incarnation and
kernel STATE's binder PID before retaining an observer watch. A locator cannot
select another tile's host. Publish invalidates the old context before ACK;
CHECK revalidates the stored complete scope, normal seat, actual focused live
surface and kernel CHECK. The response is bound to one request, never a bearer
credential. The broker must match pending scope and process all observed
revocations before using it. This encoding alone does not activate clipboard
access or replace the requirement for an asynchronous broker adapter.

Replies use positioned reads from offset zero: writing advances a seekable
9P fid by 80 bytes. The immutable reply is exactly 40 bytes. A successful
operation seals the fid against further writes; rejected operations have no
receipt. New context IDs require increasing context epochs within a controller
generation. Foreground retirement and SAK invalidate that generation. Both the
loop and each request sample the seat. Connection and surface-incarnation IDs never wrap; layout epochs
saturate and saturated admission is refused. No stale identity is reused.
