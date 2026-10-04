# Asynchronous private service connections

Status: APPROVED by the operator, October 4, 2026. Implementation in progress; see ASYNC-SERVICE-STATUS.md.
Approval includes both contracts and async-first implementation order.
Baseline: Astra 8b2212c0e. Companion: SHARED-MEMORY-ACCOUNTING.md.
This extends Loom; it does not introduce a second asynchronous I/O subsystem.
Symbolic operations below are approved contracts, not yet allocated ABI numbers.

## 1. Problem and intended result

A native graphical application must be able to connect, negotiate, walk, open,
exchange data, cancel and retire a private /srv connection without waiting for
an unresponsive server on its UI thread. Nora, terminal clipboard integration,
Boosty and later media applications should share the same library. This is a
native service lifecycle facility, not a promise that every filesystem syscall,
remote mount or device becomes cancellable in the first version.

Today devsrv_open_service creates a SrvConn and synchronously calls
srvconn_attach_dev9p_root before returning a handle. Loom supports asynchronous
operations after attachment. Its registered-handle path currently refuses the
WALK/LOPEN/CLUNK descriptor operations, despite their opcode names existing.
loom_free joins SQPOLL before releasing pins; its mid-frame receive has an
explicit stalled-server trust assumption. A worker around these calls moves the
wait but cannot safely turn a timeout into completed cleanup.

Existing ServiceWorker requires bounded nonblocking work. Its failed join keeps
live storage rather than freeing it. Creating replacement workers after each
stalled connect would accumulate retained stacks and requests. That is the
architectural workaround this proposal replaces.

## 2. Prior art and chosen fit

Plan 9 separates cooperative threads from I/O procs and supplies threadint.
Its 9P flush protocol requires observing Rflush before reusing a tag; a reply
can win the race and represent an already performed mutation. Thylacine has no
public equivalent of targeted helper-thread interruption. Preserve the lineage's
namespace and protocol semantics rather than simulating cancellation with a
process-wide note. [thread](https://9p.io/magic/man2html/2/thread),
[flush](https://9p.io/magic/man2html/5/flush).

Genode made session construction and closure asynchronous state transitions to
remove blocking dependency chains. Fuchsia's directory Open passes a new
endpoint and can report the result through it. Both separate submission from
completion. We adopt that lifetime separation, using Thylacine's existing
Spoor, SrvConn and Loom rather than introducing a capability channel transport.
[Genode 16.11](https://genode.org/documentation/release-notes/16.11),
[Fuchsia I/O](https://fuchsia.dev/reference/fidl/fuchsia.io).

Linux io_uring is useful specifically for completion semantics: cancellation
completion and the target operation's completion are distinct observations.
It is not the namespace/authority model for this design.
[Cancellation](https://man7.org/linux/man-pages/man7/io_uring_cancelation.7.html).

Rejected: one blocking helper per attempt; killing a process to cancel an I/O;
clipboard-specific kernel messages; abandoning a tag and continuing a partially
received stream; a new general-purpose kernel thread pool running blocking
syscalls. A single bounded worker remains an acceptable temporary degraded
fallback, but does not meet this facility's end-to-end retirement contract.

## 3. Ownership and namespace

A new opt-in PRIVATE_SERVICE setup mode gives a Loom ring exclusive ownership
of its service scopes. Legacy rings keep their existing ABI and semantics.
A scope is a kernel object internal to the ring, identified by slot plus a
non-repeating 64-bit incarnation, not a PID, fd number or service spelling.
Each scope owns one fresh SrvConn, one 9P session and all derived fids/operations.
The caller's actual Proc incarnation, identity, stripes, console provenance,
SrvDomain and authority snapshot are captured when CONNECT is consumed, under
the same submission/authority discipline as ordinary opens. SQPOLL must not
borrow its kernel worker's identity. Capture occurs before any externally
visible admission or protocol byte. Revocation semantics stay consistent with
ordinary submission-time pinning and existing session checks.

Target registration accepts an O_PATH capability to a native service registry
and one bounded service basename. Resolve only that registry's native service
entry; apply its ordinary DAC, route, class, domain and post-incarnation checks.
An O_PATH descriptor is navigation, not permission to connect. Neither raw
transport nor registry-factory authority is conferred. An unposted or replaced
incarnation fails Gone; reconnect explicitly resolves the current entry.
No global lookup by name or bypass of the caller's registry/namespace is added.
Initial mode accepts ordinary strict native 9P posts only; byte-mode, caped and
remote-marked posts return Unsupported until their contracts are separately
qualified. A server internally reaching a network is not guaranteed remote
rollback by local transport cancellation.

The GUI launch path supplies a navigation descriptor, not a connected clipboard
fd. Shell-launched clients may acquire it during startup from their namespace.
The interactive client must never fall back to a blocking arbitrary path walk:
if no suitable native descriptor is available, report Unsupported/Unavailable.
A remote/rebound /srv is not silently redirected to the kernel's global registry.
The locator remains a locator; the application creates its own authenticated
connection. Passing a directory capability does not impersonate its sender.

Private scope fids stay ring-local in v1: no exporting into a process fd table,
mount/bind, cross-ring registration or transferring to another Proc. Ring handles
in this mode are non-inheritable/non-transferable; explicit spawn transfer fails,
implicit fork copies omit them and their kernel-owned ring mappings. Private
mode refuses an address space already shared by distinct Procs, and RFMEM sharing
is refused while a private ring is live. Otherwise a sibling could write SQEs
directly and no syscall-side caller check could distinguish the author. Ordinary
COW fork may proceed with those descriptors/mappings omitted. Exec and creator
exit cancel scopes. This restriction is part of the proposed contract, not a
claim that shared-memory writers can be isolated by checking a PID.
Threads of the creator may submit through the library's single serialized owner.
No independently shared mount is ever terminally aborted through this facility.

## 4. Proposed operations and encoding discipline

Keep the existing 64-byte SQE and 16-byte CQE. Negotiate the setup feature before
interpreting new fields. Unknown feature/op/flag/version or nonzero reserved
fields is EINVAL/ENOTSUP, never a legacy reinterpretation. Define C/Rust/Pouch
mirrors, layout/offset assertions and decoder fixtures in the scripture ABI
commit before consumers. Do not reuse retired syscall 30 or HANDLE_RAW.

| Operation | Contract |
| --- | --- |
| REGISTER_SERVICE_TARGET | Bounded synchronous native lookup/capability pin; returns a target slot/incarnation; no peer I/O. |
| CONNECT | SQE names the target, a fresh scope destination and absolute monotonic deadline. Completion returns a ring-local root slot only after version/attach success. |
| WALK / LOPEN / CLUNK | Enable only for private scopes initially. Checked output-slot reservation, ordinary path/DAC rules and ownership; a failed/cancelled walk cannot leak or publish a stale fid. |
| READ / WRITE and existing safe ops | Use the existing 9P/Loom engine and registered buffers, restricted to the scope and its creator. No extra authority follows from this mode. |
| ABORT_SCOPE | Nonblocking control operation through Loom REGISTER, independent of SQ/CQ space. Atomically closes admission, latches local transport shutdown, returns Accepted/AlreadyTerminal/Stale. |
| QUERY_SCOPE / REAP_SCOPE | Read completion/retirement state; reap releases a terminal scope slot only after local retirement. Generation changes on reuse. |

Reserve a destination slot before accepting an operation that creates a fid.
Its SQE carries slot/incarnation, not a user pointer to a later-written handle.
CQE user_data is correlation only; kernel request identity is a separate
monotone sequence. Reject duplicate live identities and wrap before admission.
Fid slots use the existing 64-entry registered-handle envelope for v1; scope
and target slots share that envelope rather than inventing an unaccounted table.
A private ring initially supports CONNECT/WALK/LOPEN/READ/WRITE/CLUNK. Other
existing Loom ops remain refused there until their precise DAC/lifetime paths
are qualified; no blanket claim that every existing opcode is safe to expose.

Deadlines use the existing monotonic clock, absolute nanoseconds. Expired
requests fail before admission; zero explicitly means no deadline. The client
library proposes five seconds for setup and the existing thirty-second clipboard
transaction bound; idle notification reads may intentionally have no deadline.
These are policy defaults, not proof that the peer must reply within that time.
A target operation timeout terminally aborts its scope; the caller is told the
blast radius when submitting. Completion before the timeout remains final.

The exact allocation of feature bits/register sub-ops/SQE opcode numbers is an
ABI-registry task at AS-0, coordinated with Main/Aux. The binding decisions here
are semantics and ownership; numeric reservations must be reviewed and committed
before implementation, not guessed from the largest current syscall number.

## 5. State and linearization

Scope: EMPTY -> ADMITTED -> VERSION -> ATTACH -> READY -> ABORTING -> RETIRED.
Any admitted nonterminal state can enter ABORTING. Setup failure also enters
ABORTING. READY is published once or not at all. Retirement is local quiescence,
not a promise that the server freed its endpoint or undid an operation.

- Admission reserves scope/fid storage, completion capacity and normal weighted
  SrvConn credits before backlog publication. Failure rolls all reservations
  back. Connection credits remain charged until SrvConn's actual last reference.
- Connect success linearizes under the same scope-state lock as cancellation.
  If cancellation wins, no usable root is published. If completion wins, its
  success CQE remains truthful, but the subsequent abort makes the scope unusable.
- ABORT closes submission first, then invokes idempotent SrvConn teardown on
  both directions. It wakes reader/writer role waiters and all relevant pollers.
  It does not wait for Rflush, Tclunk, backlog acceptance or a complete peer frame.
- Responses may still be drained as implementation cleanup, but after the abort
  latch no callback may publish a new success/descriptor. A prior published
  success is not rewritten. Outstanding operations receive exactly one terminal
  CQE each; multishot has exactly one final CQE without MORE. Here publication
  means committing the terminal result under the scope lock. Delivery of that
  already-committed result into a previously full CQ may follow an abort; it
  cannot create fresh authority or make the aborted scope usable.
- RETIRED is published only after all local parsers, callbacks, user-buffer
  borrows and fid cleanup references have relinquished the scope. A cancellation
  acknowledgement alone is never permission to reuse user storage.
- A server-retained torn endpoint can keep SrvConn credits/storage charged. Report
  that separately; do not force free it or pretend local retirement refunded it.
  Retry is subject to the same ordinary admission limit, preventing leak growth.

On timeout use exactly the abort path; do not discard one RPC and reuse an
ambiguous stream. Timeout and cancellation say that no further local result
will arrive after retirement. They do not establish remote rollback. A write
whose bytes reached the server can have OutcomeUnknown. Clipboard CommitCopy
must not be retried automatically after that outcome.

## 6. Nonblocking progress and teardown

Refactor the existing native SrvConn adapter/9P handshake into resumable version,
attach, partial-send and partial-receive states. Reuse codecs, tag/fid validation
and response demultiplexing. p9_client_submit_async currently requires an OPEN
session, so it cannot merely be called for pre-attach work unchanged.

A private ring's existing SQPOLL executor drives these states with nonblocking
ring reads/writes, per-scope parser storage bounded by negotiated msize and a
bounded work quantum. No worker parks inside one peer's receive. Register wait
interest before rechecking readiness; sleep only on the combined work/deadline
predicate. A partial frame from one scope cannot starve another scope, deadlines
or cancellation. For explicit ENTER progress use the same state machine; a
NONBLOCK enter never waits for peer data. Use existing thread accounting for
SQPOLL, not an extra thread for every connection or operation.

Proposed scheduling quantum: at most one msize of transport copying and one
complete response per scope visit, round-robin across ready scopes. Requeue if
more work remains. Check abort/deadline between bounded copies, never while
holding an allocator or protocol lock across sleep. A slow sender does not
reset the absolute deadline by sending one byte occasionally.

Legacy shared-session pumps remain untouched initially. The private mode must
not call a blocking legacy reader pump, handshake or synchronous Tclunk in its
progress/close path. This is a real refactor with a separate qualification gate,
not a wrapper that advertises stronger guarantees than its transport supplies.

Closing the private ring initiates abort for every scope. Kernel-owned refs keep
its pages, registered buffers and address space alive while a bounded retirement
queue finishes local work. Close returns without waiting for the server. That
queue owns existing admitted storage; it cannot allocate a new stack per close.
Reserve its queue link and terminal state at setup so memory pressure cannot
prevent cancellation. Its length is bounded by already charged ring/scope slots.
There is no hard real-time wall-clock promise under a stalled kernel/scheduler;
there is no peer-dependent wait. Watchdog counters expose failure of local
retirement without freeing live references to make the number look better.

## 7. Completion backpressure and library lifetime

Admission reserves a terminal-completion obligation for every accepted request.
If the CQ is full, retain completions in bounded precharged request records and
stop new submissions. ABORT/QUERY remain callable without CQ capacity. During
whole-ring close, queued completions can be discarded only after detaching the
user consumer and ending every local borrow. No unbounded overflow list.

Rust provides an owned AsyncService scope and typed futures/transactions on top
of the existing Ring. C gets opaque handles, submit/poll/cancel/reap operations
and the same completion records. A stack-borrowed buffer API is not introduced:
pending I/O owns registered storage or an explicit lifetime-checked lease.
Dropping a future requests cancellation; storage returns to the pool only on its
terminal completion/retirement. Dropping the whole client hands its owned storage
to a bounded library retirement list, which the kernel pins independently.
A process cannot obtain a fresh budget by forgetting/dropping the Rust wrapper.
Cancellation may abort the whole private connection; this is explicit in the API,
not a surprising per-request promise. Use separate scopes for independent work.

The clipboard adapter has one transaction and one coalesced mode update at a
time. It snapshots pane/binding/seat epoch, buffer identity/revision and selection
before paste, validates all again before insertion, and discards stale output.
Strict SAK cancellation still follows the approved Halcyon coordinator protocol;
this facility supplies local retirement, not proof of remote clipboard erasure.

## 8. Resource and error contract

Use existing ring entry/buffer/handle and creator-thread limits; no increases.
A pending connection costs its usual 1 or 4 service credits from admission to
actual destruction. Bound handshake storage by existing msize classes, one parser
and one send buffer per connection, with checked arithmetic. Pin the sizeof and
allocation ledger before activation; existing p9_client is approximately 36 KiB
before bulk additions, so metadata is not zero. All retirement records and pins
remain counted, including failed attaches and completed-but-unreaped requests.

Distinguish WouldBlock, NotFound/Gone, PermissionDenied, ResourceLimit,
NoMemory, Unsupported, ProtocolError, Cancelled and TimedOut. Include the phase
and whether request bytes may have escaped; do not return an invented definitive
"not applied" after a write was sent. Diagnostics expose counts/ages/charges and
terminal reasons, never clipboard payloads, passwords or imperium keys.

## 9. Required proofs and implementation order

AS-0: ratify and reserve ABI mirrors; actual-source tests/model scenarios for
connect-vs-abort, result-vs-abort, fid reuse, full CQ, exit and quota retention.
AS-1: native nonblocking framing + cancellable handshake + terminal transport;
fault at every header/body byte and allocator point; no producer depends on a
UI task for cancellation progress. Existing synchronous callers unchanged.
AS-2: private scopes/descriptor slots, accounting, retirement, deadline and
poll wakeup path. Prove no PID/fd ABA, double completion/refund or late buffer
access; dishonest ring producers cannot widen authority or change captured IDs.
AS-3: C/Rust clients and native fixtures. Sleeping server before accept, during
version/attach/walk/open, mid-header/body/write, malformed lengths, unexpected
reply, simultaneous close/exec/exit, held torn endpoint, 1000 failed reconnects,
and independent media scope remaining live. Gate full-CQ cancellation and all
quota failures. CPU1 and SMP; relevant models/mutants, sanitizers per current policy.
AS-4: adopt clipboard, MODE and later graphical clients; exercise SAK at every
phase, paste after edit/tab/focus/session change, unknown-commit outcome, multiple
sessions, rendering/input latency and logout. Keep nondefault until whole-arc
requirements pass. Do not claim generic remote-mount cancellation or Mycelium
migration; the shared protocol library can adopt Mycelium separately later.

Review conclusion: this is the recommended single design. It costs a native
9P progress refactor, but avoids permanently assigning clipboard availability
to trusted-server promptness. No implementation or new authority is claimed.
