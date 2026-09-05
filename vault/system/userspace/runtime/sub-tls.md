---
id: sub-tls
type: sub
title: "tls — one record-layer driver for both roles, and a stall backstop on the wrong loop"
parent: moc-userspace-runtime
code:
  - usr/lib/tls/src/lib.rs
  - usr/lib/tls/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/NET-DESIGN.md"]
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

TLS 1.3 for native programs. The network daemon serves raw byte streams —
there is no transport security anywhere below this crate — so a program
that wants an authenticated encrypted channel wraps its stream here.
`curl` and the HTTPS client dial real internet hosts through it; the echo
server presents a certificate through it.

The crate is a thin adapter, not a cryptographic implementation: rustls
does the protocol, a pure-Rust provider does the primitives. What
Thylacine had to supply is exactly the two things a bare-metal target does
not have and a TLS stack cannot work without — **entropy and the wall
clock**. Both are injected here, once, for every consumer.

## Contract

`TlsStream::connect(sock, host, cfg)` runs a client handshake to
completion over any blocking byte stream, validating the server's
certificate chain against the config's trust anchors and the current wall
time. `TlsServerStream::accept(sock, cfg)` is the symmetric server side.
Both then implement the same read/write traits as the underlying stream,
carrying plaintext — a caller that already speaks to a stream needs no
other change.

Configuration is three functions: a client config over a trust store, a
server config from one certificate chain and key, and a PEM bundle parser
that fails closed when a bundle yields no usable certificate. A fourth
builds a client that trusts *nothing*, and exists so a test can prove the
verifier actually rejects an unchained certificate rather than accepting
everything — the one regression a TLS client must never have.

TLS 1.3 only: the 1.2 feature is not compiled, so "safe defaults" resolve
to a single version. No client authentication. The trust store is the
baked certificate bundle.

## Mechanism

**Two providers, both injected at crate scope.** The randomness backend
has no bare-metal implementation, so the crate registers the kernel
CSPRNG as its custom source — making this the only consumer of that
mechanism in native userspace, on behalf of every program that links it.
The clock is a small provider reading the wall-clock realtime source,
which is what bounds a certificate's validity window; without it a
no-standard-library TLS stack has no notion of "expired".

**The entropy adapter's length check is load-bearing, not defensive.**
It requires the kernel to have filled the buffer *exactly*, and fails the
request otherwise. That matters because the kernel's randomness call
silently caps an oversize request rather than refusing it — so a short
return is a real outcome, not an impossible one, and accepting it would
hand the handshake a partly-zero key. This is the rare case in this area
where a library's own check is the only thing standing between a caller
and a wrong answer rather than a redundant echo of a kernel guard.

**The state machine is a pull loop, and its post-processing is written
once.** The library's no-standard-library connection is unbuffered: you
feed it the bytes received so far and it tells you what to do next —
encode a record, transmit, read decrypted data, or block for more. Only
the entry point differs between client and server; every state it yields,
and every method on those states, is generic over the role. So the
per-state logic lives in one function and a macro generates the three
role-specific methods around it.

**One transport driver serves both roles.** The handshake loop, the
transmit/receive shuttle and the plaintext read/write path are identical
for a client and a server, so they live in one private generic type and
the two public types are thin wrappers. The stated reason is the audit
one: a fix to the driver is a fix to both roles, with no second copy to
drift. That is the same argument [[sub-corvus-crypto]] makes for sharing
one packer between two wrap types, and it is the crate's best structural
decision.

**Four buffers per connection**, and the discipline is which one owns
what: incoming holds peer bytes not yet consumed by the state machine,
outgoing holds records staged for transmission, the inbox holds decrypted
plaintext waiting for a reader, and three flags record whether the
handshake completed, whether the peer sent a clean close, and whether the
connection is terminal. The pump stages into outgoing; the transport
flushes it whole.

**Encoding grows the staging buffer rather than guessing.** The unbuffered
interface reports the size it needs when a buffer is short, so each of the
three encode helpers loops: reserve, attempt, and on a short-buffer error
reserve exactly what was asked. No fixed maximum record size is assumed
anywhere.

**The discard count is clamped before it is applied.** The library's
contract guarantees it never exceeds the incoming buffer's length, and
the code clamps anyway — with the reason written down: draining past the
end panics, and abort-on-panic turns a panic into a self-inflicted denial
of service on a path that talks to hostile servers. Defending against a
dependency's contract *on the hostile-input path specifically* is the
right place to spend a redundant check.

## Data structures

`TlsConn<C>` — the role-generic connection: the unbuffered connection
itself, the three byte buffers, and the three state flags. `TlsTransport
<S, C>` — that plus the socket. The two public types wrap the transport
at a fixed role.

The error type has five variants and the split is by *who to blame*:
configuration (unparseable input, empty trust store, bad server name),
handshake (no convergence), protocol (an unexpected state for the
requested operation), transport (the stream failed), and a wrapped
library error (certificate verification, an alert, unsupported
parameters). Only the last carries detail, which is correct — it is the
only one where the peer's reason matters.

## Concurrency

None. A connection is owned by one caller; there is no shared state, no
statics beyond the two registered providers, and no threads.

## Invariants enforced

None of the enumerated system invariants. This crate is above the
privilege boundary: it reaches the network only through a stream its
caller already holds, and that stream's reachability is the namespace's
property, not this crate's. A defect here corrupts the connection its
caller opened and nothing else.

## Error paths

Every fallible operation returns the crate's error type; the read/write
implementations flatten it to the runtime's transport error, since the
trait signature admits nothing richer. A peer close during the handshake
is a transport error; a peer close after establishment is a clean
end-of-file returning zero, which is the distinction a caller actually
needs.

Closing is best-effort by design: a close-notify alert is staged and
flushed, and a transport failure while doing so is ignored because the
caller is tearing down regardless.

## Performance

Whatever the underlying stream and the pure-Rust provider cost. There is
no buffering strategy beyond "read up to eight kilobytes per fill", no
zero-copy path, and no attempt at record-size tuning. It is a correctness
adapter; a throughput-shaped variant would be a different design.

## Prosecution

- **The clamp before every drain must stay.** All three sites carry it,
  and the reason is that abort-on-panic makes a bounds violation on
  hostile input a self-inflicted denial of service.
- **Every state arm that reports progress must actually advance the
  machine.** The pump loops until a state reports blocked, established or
  closed; an arm that reports progress without consuming input, encoding
  output or advancing state spins forever.
- **A new role goes through the macro, not a copy.** The single shared
  driver is the reason a fix reaches both roles.
- **The no-trust-anchors config must keep failing.** It is the negative
  half of the proof that certificate verification is real, and a change
  that made an empty trust store permissive would pass every positive
  test.
- **The randomness registration is crate-global.** Any second registration
  in a consumer is a conflict, and any change to it silently changes the
  entropy source for every program that links this crate.

## Seams

**Client-only in practice, though the server role is built.** The server
side exists and is exercised, but presenting certificates is not a
Thylacine service's job at v1.0 — the network daemon serves raw streams.

**No session resumption, no client certificates, no negotiated-protocol
surface.** None is wired; the config builders do not expose them.

**No read deadline anywhere.** The underlying stream is blocking and
untimed, so every bound on how long a connection may take is a bound this
crate does not have — see the caveats.

## Caveats

- **The stall backstop is on the one loop that cannot stall** (task
  #151). The in-memory round-trip helper shuttles records between two
  local connections and caps itself at sixty-four iterations, with a
  comment explaining the risk: no peer to wait on, so if nothing flows we
  are stuck. Both its peers are under its own control. Meanwhile the
  handshake loop and the plaintext read loop — the two that talk to a
  remote peer — have no iteration cap, no deadline and no timeout, and
  they exit only on established, closed, or a peer end-of-file. A peer
  that dribbles just enough to keep the socket readable holds a `connect`
  open indefinitely. This is live: the HTTP client and `curl` call it
  against arbitrary hosts.

- **One state arm reports progress without making any.** The
  server-side early-data state is handled as "a no-op step" — but the
  pump's loop continues on progress, so an arm that does nothing and
  reports progress is a non-terminating step rather than a skipped one.
  It is structurally unreachable through this crate's own builders, which
  never enable early data, and every other progress arm genuinely
  advances. Recorded because the comment's framing is exactly backwards:
  a no-op is the one thing that arm must not be.

- **Two comments cite a line number for a clamp that has since moved**,
  pointing twelve lines short of the code they reference. Harmless, and a
  standing argument against line numbers in comments: the reference rots
  the moment anything above it changes, and nothing checks it.

- **No host tests.** The crate is unconditionally `no_std`, so `cargo
  test` cannot build it for the host — unlike [[sub-corvus-crypto]],
  which is `no_std` only when not under test and carries thirteen. Its
  entire proof is the in-guest loopback round-trip, which is genuinely
  good (a real handshake, real certificate verification against real
  trust anchors and the real clock, plus the untrusting negative) but is
  one test executing one path. The parsers, the error mapping and the
  grow-on-short-buffer loops are unexercised except incidentally.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
