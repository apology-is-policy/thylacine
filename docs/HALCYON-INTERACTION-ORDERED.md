# Ordered interaction delivery

Implementation of the approved dedicated service owner and ordered admission
contract. No new kernel authority or syscall; no public clipboard activation.
Drawing's TEV_FOCUS/TEV_LAYOUT remain coalesced. The session service instead
selects an ordered mode on one setup-opened ctl of its existing authenticated
Tapestry connection. Ownership changes and admission decisions share that stream:
there is no race between a notification watermark and a separate success reply.

The normal compositor owner appends changes before its corresponding decision.
A single service executor consumes every record in order. Focus loss retains
registration but cancels unfinished staging and records the earliest outstanding
Get/Commit boundary. A terminal snapshot includes binding, foreground epoch and
nominated subject (zero if unacknowledged), including same-epoch ACK changes.
Binding retirement removes that binding's local route, never a replacement with
just the same leaf. A seat reset retires controller/payload state, never restores
normal authority: HSC remains the independent source of normal-seat membership.
A successful decision still requires exact pending request/operation/binding,
fresh peer publication checks, foreground/seat and focus admission checks.

## Encoding and bounds

A 16-byte write selects ordered mode: bytes 0..4 HIO1, little-endian u16 version
1 at 4, and ten zero reserved bytes. It is legal only on an unused ctl fid from
the current declared session peer, once per connection. Fresh kernel metadata
must match the connection's live stripes and principal. This is the HSR session
identity, not the distinct console-renderer role. Opening creates a new non-reused
server stream identity and queues Ready followed by current bound-terminal
snapshots. Clunk/reset/disconnect removes the stream and its pending read.
The existing HIA1 request remains 80 bytes. On the selected fid its successful
Rwrite means accepted into the protocol, not granted authority. The decision,
including a refused decision, arrives in the ordered stream. Exact request
retries use the cached decision and never repeat the authority operation.
The existing setup-only HIA exchange stays usable by terminal binding setup and
qualification probes; application dispatch must use the ordered mode.

Each Tread requests at least 80 bytes at offset zero and returns exactly one
80-byte record, or parks if empty. Only one read may be pending on the stream.
The record is HIO1 at 0, version u16 at 4, kind u16 at 6, sequence u64 at 8,
request u64 at 16, leaf u32 at 24, reserved-zero u32 at 28, binding u64 at 32,
and five u64 payload words at 40,48,56,64,72. Unused fields must be zero.

| Kind | Request / leaf / binding | Payload words |
|---|---|---|
| Ready = 1 | all zero | stream identity,0,0,0,0 |
| FocusLost = 2 | 0 / live leaf / binding | layout loss epoch,0,0,0,0 |
| Terminal = 3 | 0 / live leaf / binding | foreground epoch, nominated subject or 0,0,0,0 |
| Retired = 4 | 0 / old leaf / old binding | all zero |
| Reset = 5 | all zero | all zero |
| Decision = 6 | original request / leaf / binding | HIA op, errno (0 for success), focus, seat, foreground |

A successful Decision may have seat generation zero: that is the initial normal
seat before any secure-attention episode. Seat membership is an explicit option,
not a nonzero test. A failed Decision has zero focus/seat/foreground. Receipt validation remains in
the existing controller/Broker owner. Sequences start at one for Ready and never
wrap or skip; duplicate, missing, malformed or stale records poison the client.
No application can select a stream or manufacture its source metadata.

Each subscribed connection has a 64-record fixed journal, at most eight journals
(the existing Tapestry connection ceiling). Inline metadata is compile-time
bounded to 8 KiB per journal, 64 KiB total, plus eight small routing records.
Each consumer retains one 80-byte read buffer and one 80-byte write buffer,
page-rounded by Loom registration. One read and one write may be in flight,
with distinct buffers/tags. Teardown joins/removes the ring before freeing
registered memory. The exchange exposes a runnable hint when it holds a deliverable record or has
an unsubmitted write/read rearm. The service chooses a zero poll timeout only
for that local work; an in-flight read or a decision waiting for its write CQE
sleeps on readiness. Consuming a record therefore cannot depend on an unrelated
HSC timeout to arm the next read. No 100 Hz polling or unsolicited 9P reply is
introduced.

Overflow or sequence exhaustion poisons the stream, discards unread entries,
invalidates that connection's published interaction contexts and fails pending
reads. It never overwrites an old loss event with a new focus state. Consumers
retire local controllers/transfers and stop admission on any stream fault.
Recovery requires a fresh stream and fresh controller publication; a cached
success cannot revive a poisoned stream. Committed clipboard text follows the
existing store/session lifetime; protocol failure cannot silently recopy it.

## Verification and activation

Exercise loss A->B->A around a pending decision, same-epoch subject replacement,
retirement plus leaf reuse, source/receiver exhaustion and overflow, delayed and
reordered Loom CQEs, exact retry caching, clunk/flush/reset, rejected readers,
and independent HSC cancellation while normal compositor I/O is parked.
Use actual codec/journal/producer/consumer sources and named mutations. Native
checks must prove ordered source events and results on a real declared renderer
connection. Public activation additionally needs authenticated app registration,
reply cancellation, total allocation accounting and two-client qualification.
