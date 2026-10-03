# Halcyon interaction wire reservations

HI-1a typed wire contract for the approved HALCYON-INTERACTION design. This
pins identifiers, exact operation bodies, errors and fragmented assembly; it
does not enable a clipboard endpoint. Ownership/focus integration, aggregate
allocation accounting and asynchronous C/Rust clients remain service prerequisites.

Rust definitions: `usr/lib/libhalcyon/src/interaction_wire.rs`. C mirror:
`usr/lib/libhalcyon/include/halcyon_interaction.h`. All integers are explicitly
little-endian. No pointer-sized fields, native struct casts or implicit padding.

| Byte offset | Field | Width |
|---:|---|---:|
| 0 | Magic `HIN1` | 4 |
| 4 | Version, exactly 1 | 2 |
| 6 | Operation | 2 |
| 8 | Exact total record length, including this header | 4 |
| 12 | Flags: bit 0 response; every other bit zero | 4 |
| 16 | Nonzero request ID, echoed unchanged in response | 8 |
| 24 | Operation body | variable |

Maximum record length is 32768 bytes; minimum 24. Unknown version, operation,
flags, direction mismatch or any trailing bytes are refused. An envelope parser
does not accept an operation body implicitly: the typed operation decoder must
require exact exhaustion before dispatch. Transport fragments have contiguous
offsets starting at zero and produce no operation before complete validation.
Request IDs increase on a fid; only an exact immediate duplicate may return its
cached result. Changed bytes with the same ID are a protocol error. A client
opens a fresh fid before exhaustion; reconnect is not a mutation retry.

Operation numbers 1..10, respectively: Hello, BindController, ReportMode,
GetClipboard, ReadClipboard, BeginCopy, WriteCopy, CommitCopy, Cancel,
UnbindController. These are local to HIN1, not 9P message IDs or Lictor opcodes.
Modes are INS=1, NOR=2, VIS=3, CMD=4, APP=5; readonly remains a separate flag.
The matching Hello envelope fixture carries request ID 0x0102030405060708.

Limits: 1 MiB canonical clipboard UTF-8, 16 KiB transfer chunks, 64-byte context
labels, 4096-byte search queries. Clipboard validation rejects Unicode control
characters except TAB/LF; export adapters normalize CRLF/CR before submission.
Unicode format characters and combining sequences are preserved. No terminal
escape sequence or NUL may enter the clipboard through a canonical upload.

The existing pane service uses qid class low bits 0=root, 1=directory, 2=place.
Class 3 is reserved for interaction in that service. Routing still uses the full
pane token and live ownership records; the folded qid is never authority. No new
srv slot, 9P message type, kernel syscall or kernel capability is reserved here.

The operation-body/error mapping and C/Rust fixture gate remain prerequisites
for service integration. The current source tests exercise only envelope bounds,
canonical text and malformed input; they are not end-to-end clipboard evidence.

## Typed bodies (HI-1a)

All offsets here start after the 24-byte envelope. IDs are u64 and nonzero,
except clipboard generations (initial value zero). Every body is exact: extra
bytes, nonzero reserved fields, unsupported modes/flags and malformed text fail.
The session ID is the Hello generation, not a username. Context/epoch name the
host-published binding; a peer cannot create authority by choosing those values.

`Scope` is four u64 fields: session, controller generation, context ID, context
epoch, at offsets 0/8/16/24. Scope is 32 bytes. Peer process incarnation comes
from the connection's kernel peer record, never from this payload.

| Operation | Request body | Successful response body |
|---|---|---|
| Hello | empty | session u64; max text/chunk/label/record u32; write/read/controller slots u16; reserved u16; inactivity/lifetime milliseconds u32 (40 bytes) |
| BindController | session/context/epoch u64 (24) | controller generation u64 (8) |
| ReportMode | Scope; sequence u64; mode u8; readonly flags u8 (only bit 0); reserved u16; label length u32; UTF-8 label (48 + length) | empty |
| GetClipboard | Scope (32) | transfer u64; generation u64; length u32; reserved u32 (24) |
| ReadClipboard | transfer u64; offset/count u32 (16) | offset/count u32; bytes (8 + count) |
| BeginCopy | Scope; length u32; reserved u32 (40) | transfer u64 (8) |
| WriteCopy | transfer u64; offset/count u32; bytes (16 + count) | accepted count u32; reserved u32 (8) |
| CommitCopy | Scope; transfer u64; expected generation u64 (48) | published generation u64 (8) |
| Cancel | transfer u64 (8) | empty |
| UnbindController | Scope (32) | empty |

Read count and write payload are nonzero and at most 16384. Read responses may
be empty at EOF. Offset plus requested/returned count must not exceed the 1 MiB
text limit; actual transfer bounds are checked by the service. Empty copies are
valid and publish an empty clipboard. Labels are at most 64 UTF-8 bytes with no
control characters, including tabs/newlines; they are never trusted identity.
Sequences are nonzero. The Hello slot counts are 2 writes, 2 reads, 32 controllers;
transfer inactivity/lifetime are 30000/120000 milliseconds. These are fixed v1
limits. Consumers must assert MAX_PANES equals the controller count.

`interaction_body` decodes borrowed bytes, and encoding uses those same typed
bounds. `interaction_frame` accumulates one record without exposing a partial
operation. Its caller must supply the remaining connection input budget; the
receiver counts its fixed header plus body capacity. Any framing error poisons
that receiver until explicit reset. Per-fid duplicate replay, response retention,
connection-wide accounting and application admission remain broker obligations.

## Error mapping

Failures use existing 9P Rlerror values, never a successful empty HIN1 response:
Denied=EPERM(1), Gone/invalidated generation=ENOENT(2), TooLarge=E2BIG(7),
BadHandle=EBADF(9), Conflict=EAGAIN(11), NoMemory=ENOMEM(12), Busy=EBUSY(16),
Invalid=EINVAL(22), Unsupported=EOPNOTSUPP(95), Timeout=ETIMEDOUT(110).
`interaction_wire::Failure` and the C enum mirror these existing registry values.
Unknown transport errors are preserved as transport failures. Framing budget
exhaustion maps to Busy, malformed offset/body to Invalid; transfer timeout and
lost owner remain distinct. This allocates no new kernel errno.

## Prepared transaction owner

Halcyon's `apprecord::Record` implements one transaction fid above Receiver.
The connection owner assigns a never-reused fid incarnation and subtracts other
fids plus transport storage before supplying input/output allowances. A separate
24-byte prefix distinguishes a new request from replay while retaining one exact
old body. All partial bytes and allocated capacity count. Typed body validation
precedes dispatch; changed same-ID records poison assembly until cancellation.
Pending writes return Busy without replacing the original admission. Completion
requires the exact incarnation/request ticket. Cancel drops request/result bytes
but retains the ID watermark, so a cancelled ID cannot become a new mutation.
Cached results may be semantic values or references to the admitted snapshot;
the final adapter must not copy a read snapshot into an uncounted cache.

This component does not activate an endpoint, sample a kernel peer, map Tflush
or perform the aggregate HSC output barrier. `servicewire` exposes actual input
capacity at dispatch and remaining-budget hooks for the adapter to reconcile
transport and caches. Native operation dispatch and the total ledger remain
prerequisites to activation.

## Prepared application dispatcher and borrowed responses

Application now owns eight transaction records per accepted-connection identity
and dispatches all ten typed operations through Interaction. Its caller provides
fresh kernel metadata and the remaining input budget after transport charges;
the component subtracts other fids. It pins the route across all fid lifetimes.
The native HIA decision entry retires dead/replaced peers before delivering a
Publish or CHECK receipt. Trusted local cancellation completions retain exact
target matching; cross-fid Unbind forwards its cancelled admission immediately.

Read caches hold transfer coordinates, not another payload allocation. Response
encoding uses a fixed64-byte prefix and borrowed snapshot, copying only requested
ranges into existing output storage. Existing wire values/bounds are unchanged.
Public9P dispatch, native peer sampling, pool admission, pending-tag/Tflush mapping,
aggregate transport ledger and the HSC output barrier remain activation gates.

## Native session adapter

With the explicit nondefault interaction-qualification feature, the pane service
exports `<token>/interaction` alongside `<token>/place` and
snapshots its locator into the sealed tile child's HALCYON_INTERACTION env.
The route is not authority: each accepted peer and each admission completion
is authenticated through native connection metadata and ordered PTY checks.

The negotiated 9P msize is at most 8192. Larger HIN1 records span contiguous
Twrite offsets. Eight 9P fids have local monotone application incarnations; input
reservations include actual stream capacity and every other fid. One parked
admission per connection still permits synchronous second-fid Unbind/Cancel.
Tflush cancels its exact tag, and Tclunk its exact fid incarnation. Repeated
Tversion after initial negotiation closes this connection rather than silently
resetting authority. Cached Read replies contain coordinates and borrow the
still-admitted snapshot at each positioned read; no second payload cache exists.

After a foreground transition, Bind may report Gone before the ordered new
epoch arrives. A client may make a bounded new registration attempt with a new
request ID. This does not permit retry of uncertain commits. Production UI
clients and the full capacity/pressure qualification remain separate work.
