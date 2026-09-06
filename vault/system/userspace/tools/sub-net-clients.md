---
id: sub-net-clients
type: sub
title: "The standalone network clients — five crates that each prove themselves at boot"
parent: moc-userspace-tools
code:
  - usr/curl/src/lib.rs
  - usr/curl/src/bin/curl.rs
  - usr/curl/src/bin/wget.rs
  - usr/curl/Cargo.toml
  - usr/https/src/main.rs
  - usr/sntp/src/main.rs
  - usr/httpd/src/main.rs
  - usr/net-echo/src/main.rs
audit: none
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/NET-DESIGN.md"]
created: 2026-08-04
updated: 2026-09-07
---
## Purpose

The five network programs that live outside the coreutils crate: an
HTTP/HTTPS fetcher (two binaries over one engine), a TLS client, a time
client, a static file server, and an echo server.

They are separate crates for a stated reason — the TLS dependency is heavy
and the fifty-one coreutils binaries should not carry it — and they share
a property the coreutils network tools do not: **each carries a
deterministic self-test that runs at boot and gates it.**

## Contract

Each is a client or server over the granted network tree. None touches
hardware; the network daemon owns the interface, so these reach only the
tree their territory grants. The fetcher and the TLS client additionally
read the baked certificate bundle, so their reach is the network tree plus
one configuration path.

Every one splits into two modes: a deterministic, peer-independent
self-test that runs with no arguments and is a boot gate, and a live
operation that is best-effort and explicitly *not* a gate.

That split is the design decision worth taking away. Whether a real server
answers is environment-dependent — the emulator's networking forwards some
traffic to the host and answers other traffic itself, and which is which
varies by host. Asserting on a live round-trip would therefore flake. So
the gate asserts on what is deterministic (parsing, framing, arithmetic,
capability denial) and the live path only logs.

## Mechanism

**The fetcher is a codec plus an engine.** URL parsing, request building
and response splitting are pure and transport-agnostic; the engine wires
them to a plain connection or a TLS one. Both binaries are thin frontends,
so the parsing lives in exactly one place.

Requests are HTTP/1.0 with an explicit close, which makes the peer's
end-of-stream the end of the body — no chunked or keep-alive framing to
parse. There are two response paths: a buffered one bounded by a response
cap for small fetches the caller wants to inspect, and a streaming one
that writes the body to a sink chunk by chunk while timing connect,
first-byte and total. The streaming path exists so a large download never
accumulates in a fixed heap.

**The time client's self-test asserts a denial.** As the boot process
spawns it unelevated, stepping the clock must fail with a permission
error — and a *non*-denial fails the boot. That is a privilege regression
detector written as a positive assertion, and it is the sharpest
self-test in the group: most tests prove a thing works, this one proves a
gate still refuses.

**And its live query carries a per-request nonce it requires echoed
back.** SNTP is unauthenticated, so trusting the answer at all rests on
one defense: the request writes a fresh nonce into the transmit field,
and a response whose *originate* timestamp does not echo that nonce is
discarded, not stepped. An off-path spoofer cannot guess the nonce, so
this is what lets an unauthenticated time source be trusted against blind
injection. The full validation battery is mode-4 + stratum in [1,15] +
non-zero transmit + the originate echo; failing any one discards the
response. The step itself is the SNTP simple algorithm (`transmit +
rtt/2`) — it steps, it does not slew — and it goes through the
CAP_HOSTOWNER-gated realtime-set syscall, which is the gate the denial
self-test proves still refuses.

**The TLS client's self-test proves the bundle baked and parses** into a
non-empty trust store that composes with the crypto provider in-guest. No
network. The full handshake proof lives in a separate loopback test,
correctly, because a handshake needs a peer.

**The file server streams with back-pressure.** Chunked reads and sends
that wait on writability, so it serves files far larger than the heap —
which is its point: it is how a real download over the real interface gets
measured, where a loopback benchmark cannot. Path traversal is rejected on
top of the namespace containment, which is belt and braces since the
namespace is already the sandbox. Its access log colourizes only when stdout is
an interactive console: the default flipped to `--color=auto` at H-1c-2,
resolved by the real Dev-class TTY check (`stdout_is_terminal` over
`SYS_FD_DEVCLASS`, which retired the always-true stub), so a log piped to a file
or another program now carries no escape bytes.

**The echo server's boot probe is peer-independent because one process is
both ends** — it connects to itself over the resident loopback, and the
blocking accept and receive defer inside the daemon rather than
deadlocking. It also runs a soak that repeats the round-trip and requires
the daemon's live connection count to return to its pre-soak baseline,
which is a leak check rather than a functional one.

## Data structures

A parsed URL; a timing-and-size record for the streaming path; a counting
sink that discards bytes for the benchmark case. Nothing persistent.

## Concurrency

None. Each is single-threaded; the server serves one connection at a time,
which matches the daemon's listener backlog of one.

## Invariants enforced

None. They compose with the daemon's exclusive ownership of the interface
— a client touches no hardware — and with namespace containment, which is
what bounds the file server to the subtree it was given. The TLS paths
additionally need the randomness capability for the handshake key share;
the plain paths need no capability at all.

## Error paths

Errors are strings, formatted for a person, and every failure is fatal to
the operation but not to the boot unless it is the self-test. A malformed
URL, an unresolvable host, a failed handshake and a connect failure are
all distinguished in the message.

The buffered path stops at the response cap; the streaming path stops at a
head cap if it never finds a blank line, which is the guard against a peer
that sends headers forever.

## Performance

The streaming path is the one that matters and it is the one that avoids
buffering. Head-separator scanning re-scans the accumulated head on each
read, which is quadratic but bounded by the head cap.

## Prosecution

- **The self-tests must stay peer-independent.** The moment one asserts on
  a live round-trip it becomes a boot gate on the host's network
  configuration, and the boot fails for reasons that have nothing to do
  with the guest.
- **The time client's denial assertion must stay an assertion.** Softening
  it to a log would silently retire a privilege-regression detector.
- **The streaming path must keep being the download path.** Routing a
  large fetch through the buffered path reintroduces the heap bound the
  streaming path was written to escape.

## Seams

HTTP/1.0 only: no chunked transfer, no keep-alive, no redirects.
Address resolution is version-four only. The server is single-connection
and supports two methods.

**The time client trusts the answering server**, and its boundary is
stated precisely: the originate-nonce echo defends against an *off-path*
spoofer, but an *on-path* attacker who can see the nonce is the documented
limit — NTS or a symmetric-key MAC is the v1.x hardening. Its 1900->Unix
conversion (`ntp_secs - 2_208_988_800`) **saturates** below the epoch
delta rather than wrapping, so the era-2036 rollover is a named seam, not
a silent wrap. And it steps rather than slews: a drift-correcting daemon
is a separate v1.x add.

## Caveats

- **The request write is a write-everything loop, and the shared pump
  module exists because that fails.** The coreutils network library states
  the rule explicitly: the daemon's data write is non-blocking, so a full
  send window returns a zero count, and the runtime's write-everything
  helper turns a zero count into an error. Its own back-pressure-aware
  sender exists for exactly this.

  The fetcher does not use it — it is a separate crate, so it cannot
  without taking a dependency — and writes the request with the naive
  helper. Safe today by size: a request is a couple of hundred bytes
  against a four-kilobyte window. It becomes reachable with a URL whose
  path exceeds the window, which comes from the command line. The failure
  would be a bare "write request failed" for a request that was merely
  large.

- **No tests, in the harness sense.** Zero test blocks across all five.
  Their proof is the boot self-tests, which is a genuinely strong position
  for what they cover — the fetcher's covers six URL shapes, three reject
  cases, the exact request bytes, and both separator conventions — and
  covers nothing of the engine, since the engine needs a peer.

  So the split is clean and worth naming: **the pure half is proved at
  boot, the transport half is proved by nothing that runs unattended.**

- **The plain-connection path does not close explicitly** where the TLS
  path does, relying on the ownership drop instead. Correct, but the
  asymmetry within one function reads as an omission.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
