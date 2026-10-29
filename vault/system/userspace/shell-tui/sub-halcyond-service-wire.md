---
id: sub-halcyond-service-wire
type: sub
title: "Halcyon service transport — bounded nonblocking frames and retained replies"
parent: moc-userspace-shell-tui
code:
  - usr/halcyond/src/servicewire.rs
  - usr/halcyond/src/serviceio.rs
  - usr/halcyond/src/servicepool.rs
  - tools/test-service-replies.py
audit: hard
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/HALCYON-INTERACTION.md", "docs/HALCYON-INTERACTION-READINESS.md"]
created: 2026-09-25
updated: 2026-10-03
---
## Purpose

Own the common byte-stream pump for Halcyon's console and per-user media
services. The protocol adapters in [[sub-halcyond]] keep all fid, token,
principal and image decisions. This mechanism prepares the existing service
for persistent interaction clients; it does not enable a clipboard endpoint.

## Native integration update (October 3)

The qualification build of the native session adapter subtracts actual input capacity from
all-fid record allowance, negotiates 8 KiB frames and grows output lazily. Exact
park tickets map to native9P tags; cross-fid synchronous cancellation can progress
while one admission is parked. Pool accounting releases only after descriptor
close; earliest deadlines bound otherwise-idle polls. WORKING_RESERVE separately
reserves 7.375 MiB payload/protocol plus 512 KiB metadata/mapping slack; stack/guard
are charged by the session. Full all-slot pressure evidence remains HI1-R24.

## Contract

`Endpoint` is strictly nonblocking: Again preserves state, zero read is EOF,
zero write or another error closes the connection. `NativeEndpoint` maps
SYS_READ/SYS_WRITE and existing EAGAIN. Both media adapters must successfully
call SYS_SET_NONBLOCK on each accepted Spoor before publishing it. Default
SrvConn server writes block; readiness alone never guarantees a whole reply
fits. Failure to set the mode closes the new connection.

`Handler::dispatch` receives exactly one complete bounded 9P frame. An immediate
reply remains immutable until its last byte is written; a later request cannot
overtake that output. Explicit Park accepts one nonzero increasing local ticket
with no reply bytes. While parked, later requests including Tflush can dispatch.
Cancel retires the exact ticket and queues the ordinary flush reply. The protocol
owns pending tags, fids and semantic state; tickets are never raw client tags.

`resume_reply` checks the exact ticket and empty output slot BEFORE invoking its
builder, so stale or busy resumptions cannot overwrite output. A failed builder
or malformed reply permanently closes the stream. `cancel_output` discards
buffered input, unsent output and the park, preserving ticket history. Any byte
already sent from the current frame poisons the connection: its suffix cannot
be replaced. The protocol must separately retire cached/pending authority and
bytes before HSC acknowledgement; this primitive alone is not the SAK barrier.

## Aggregate cache accounting

`dispatch_buffered` passes the actual input allocation capacity, including any
spare capacity and pipelined bytes. `input_allowance` excludes protocol/fid
caches; the pump refuses growth beyond it. `output_reserved` and
`output_allowance` likewise bound reply capacity before any byte is sent, both
for immediate and resumed replies. The defaults preserve media behavior; an
interaction adapter must override them with the complete connection ledger.
Cancellation releases the input allocation, rather than retaining its capacity.
The protocol still owns and must retire its output/caches before HSC ACK.

These hooks prevent a future adapter from silently counting only live lengths.
They do not activate [[sub-halcyond-interaction-record]], raise quotas, or prove
the full session allocation ledger. The native media adapter remains two slots.

## Mechanism

The pump first resumes any retained reply at its sent offset, then dispatches
complete buffered requests, then reads more data. READ is desired when no reply
is pending; WRITE is desired while a reply is pending. The two directions never
need separate poll entries. Input length is validated at four bytes, before a
frame is exposed. Input is borrowed during dispatch; there is no full-frame copy.

A turn permits eight frames and 64 KiB of combined read/write progress. The
adapters additionally share a two-millisecond monotonic deadline per service
pass and rotate connection order. These checks are cooperative: a single bounded
handler/syscall can cross the deadline; this is not a measured hard latency
bound. A complete or malformed buffered frame sets `runnable()`. Both compositor
loops fold that into a zero-duration wait; partial frames and blocked replies
wait on real readiness, without a periodic service timer.

## Data structures

Stream metadata is compile-time bounded to 64 bytes. Input grows fallibly in
at most 4096-byte steps, capped at 32768 bytes. The protocol owns one reply
allocation capped at 32768 bytes; it reserves fallibly before dispatch. All fids
share these two per-connection buffers. The console still admits one connection,
the session two. No control/handshake reserve or full 38-connection ledger is
claimed by this checkpoint.

## Concurrency

The session adapter's dedicated ServiceWorker owns protocol parsing and I/O;
its UI exchanges bounded durable metadata and completed images through a mailbox.
Its independent HSC lane runs before ordinary transport each executor pass.
The console adapter retains direct UI-thread polling. The native transport probe
also exercises PollWorker owned-descriptor readiness. Neither adapter borrows a
reply buffer across threads; no lock is held for a service syscall.

## Invariants enforced

- A partially accepted reply retains its byte offset and buffer unchanged, or
  cancellation closes the connection permanently.
- Park tickets never repeat; cancelled or busy resumptions never run the builder.
- A committed request dispatches once, even across multiple WouldBlock returns.
- Buffered complete frames cannot lose their wake when the kernel ring is empty.
- A full reply ring yields to other connections and UI events.
- EOF, invalid lengths and terminal I/O errors close the connection, discarding
  its uncommitted protocol state. Session handles close on their service owner before join; console handles close on Drop;
  the service name itself remains registered until poster process exit.

## Connection capacity (prepared, not active)

`servicepool::Pool` is fixed single-owner metadata (at most 4 KiB), with monotonically
unique connection IDs, 32 controller slots derived from the shared MAX_PANES,
two media slots and four two-second handshakes. One handshake per kernel peer;
one controller per live leaf. The adapter must authenticate leaf ownership before
promotion; capacity reservation is not authority. Failed promotion retains the
handshake. Explicit retirement retains its class quota and peer/leaf exclusion
until worker reclamation/close; release rejects live or stale IDs. Deadline
reporting permits a nearest-deadline timer without periodic polling. Overflow
refuses rather than reusing an identity or wrapping a deadline.

The module declares 7.375 MiB for connection buffer budgets plus clipboard
storage. This excludes the explicitly bounded pool metadata, worker reservation
and still-owed kernel/protocol allocation ledger. The current media adapter
continues to admit two connections; it does not yet use this pool. Tests exercise
full concurrent reserves, failed promotion, peer/leaf exclusion, exact expiry,
retirement and stale completions. No live clipboard or expanded service is claimed.

## Error paths

Allocation refusal, malformed frame/reply size, invalid I/O count, EOF and
non-EAGAIN I/O errors close the stream. Failure does not claim delivery of a
partially written reply. A client that disconnects after a mutation must treat
its outcome as unknown; transport retention does not create cross-connection
retry semantics.

## Performance

At most 32 KiB input plus 32 KiB output payload allocation per connection, plus
bounded metadata; image accumulator accounting is unchanged. Each syscall's
byte count is bounded by the remaining turn credit. No eager input maximum
allocation, periodic service wake or connection-limit increase. The session
service owner adds one thread, separately accounted in its runtime dossier.

## Prosecution

`servicewire::tests` exercises split/full frames, short writes, blocked readers,
ordered replies without redispatch, already-buffered continuation, malformed
lengths, truncated EOF, byte/deadline yields and another peer's progress. Added
park/flush/resume, exact/stale tickets, cancelled buffered input and every partial
write boundary are checked by actual-source tests and six named mutations.
`kaua-term-probe --service` compiles this exact pump and native adapter and
exercises real SrvConn rings plus PollWorker owned-descriptor write readiness.
It additionally exercises parked requests, immediate progress, exact resumption,
flush and unsent cancellation on real SrvConn endpoints. It also compiles the production PanePlaceServer source and runs two waves of two child
clients through the real kernel 9P client, checking routed image bytes, clean
child exits, one UI service fd and quiet waits between slot-reuse waves. The `service-wire`
interactive gate requires CI Imperium enrollment and uses an explicit serial
recovery posture. Runtime results and artifact provenance are recorded in
[[arc-halcyon-interaction]] and docs/HALCYON-INTERACTION-STATUS.md; do not infer
a graphical or expanded-service pass from the standalone probe.

## Seams

[[sub-halcyond]] supplies bounded 9P dispatch and both UI loops.
[[sub-libthyla-rs]] supplies native nonblocking I/O and readiness aggregation.
[[sub-substrate-interactive]] drives the guest probe and enforces fixture setup.

## Caveats

Persistent interaction admission, compositor/session failure recovery,
38-connection accounting, Tapestry terminal checks and clipboard clients remain
unfinished. This checkpoint changes transport behavior on the existing media
paths; it adds no user-facing command or UI mode.

## Provenance
