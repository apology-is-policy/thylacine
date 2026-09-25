---
id: sub-halcyond-service-wire
type: sub
title: "Halcyon service transport — bounded nonblocking frames and retained replies"
parent: moc-userspace-shell-tui
code:
  - usr/halcyond/src/servicewire.rs
  - usr/halcyond/src/serviceio.rs
audit: hard
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/HALCYON-INTERACTION.md", "docs/HALCYON-INTERACTION-READINESS.md"]
created: 2026-09-25
updated: 2026-09-25
---
## Purpose

Own the common byte-stream pump for Halcyon's console and per-user media
services. The protocol adapters in [[sub-halcyond]] keep all fid, token,
principal and image decisions. This mechanism prepares the existing service
for persistent interaction clients; it does not enable a clipboard endpoint.

## Contract

`Endpoint` is strictly nonblocking: Again preserves state, zero read is EOF,
zero write or another error closes the connection. `NativeEndpoint` maps
SYS_READ/SYS_WRITE and existing EAGAIN. Both media adapters must successfully
call SYS_SET_NONBLOCK on each accepted Spoor before publishing it. Default
SrvConn server writes block; readiness alone never guarantees a whole reply
fits. Failure to set the mode closes the new connection.

`Handler::dispatch` receives exactly one complete bounded 9P frame. It replaces
one reply buffer, which remains immutable until the last byte is written.
`Stream::service` never redispatches a request merely because its reply blocks.
A later request, including Tflush, cannot overtake the preceding reply.

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

All state and protocol work stay on the UI thread. No locks or worker references
are introduced here. The standalone native readiness worker remains a separate
mechanism in [[sub-libthyla-rs]], not yet connected to Halcyon's service loop.

## Invariants enforced

- A partially accepted reply retains its byte offset and buffer unchanged.
- A committed request dispatches once, even across multiple WouldBlock returns.
- Buffered complete frames cannot lose their wake when the kernel ring is empty.
- A full reply ring yields to other connections and UI events.
- EOF, invalid lengths and terminal I/O errors close the connection, discarding
  its uncommitted protocol state. Native Conn and listener handles close on Drop;
  the service name itself remains registered until poster process exit.

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
allocation, new thread, periodic wake or connection-limit increase.

## Prosecution

`servicewire::tests` exercises split/full frames, short writes, blocked readers,
ordered replies without redispatch, already-buffered continuation, malformed
lengths, truncated EOF, byte/deadline yields and another peer's progress.
`kaua-term-probe --service` compiles this exact pump and native adapter and
exercises real SrvConn rings plus PollWorker owned-descriptor write readiness.
It also compiles the production PanePlaceServer source and runs two child
clients through the real kernel 9P client, checking routed image bytes and clean
child exits. The `service-wire`
interactive gate requires CI Imperium enrollment and uses an explicit serial
recovery posture. Runtime results and artifact provenance are recorded in
[[arc-halcyon-interaction]] and docs/HALCYON-INTERACTION-STATUS.md; do not infer
a graphical or expanded-service pass from the standalone probe.

## Seams

[[sub-halcyond]] supplies bounded 9P dispatch and both UI loops.
[[sub-libthyla-rs]] supplies native nonblocking I/O and readiness aggregation.
[[sub-substrate-interactive]] drives the guest probe and enforces fixture setup.

## Caveats

Persistent interaction admission, worker failure/interleaving qualification,
38-connection accounting, Tapestry terminal checks and clipboard clients remain
unfinished. This checkpoint changes transport behavior on the existing media
paths; it adds no user-facing command or UI mode.

## Provenance
