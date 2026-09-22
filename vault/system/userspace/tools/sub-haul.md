---
id: sub-haul
type: sub
title: "Haul — remote 9P relay and same-shell service posting"
parent: moc-userspace-tools
code: [usr/haul/src/addr.rs, usr/haul/src/cmdline.rs, usr/haul/src/lib.rs, usr/haul/src/main.rs, usr/haul/src/npxf.rs, usr/haul/Cargo.toml]
audit: hard
guarded-by: [inv-i1, inv-i2]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: [docs/HAUL-DESIGN.md]
created: 2026-09-17
updated: 2026-09-18
---
## Purpose

Haul relays 9P over IPv4 TCP, optionally inside npxf's authenticated encrypted
channel. It supports a private mount with a child command, and a posted byte
service which the existing shell can mount in its own namespace.

## Contract

`haul [-t FILE | --token-env VAR] ADDR MOUNTPOINT [COMMAND ...]` attaches in
Haul's Territory, then runs the command there or parks. `haul --post NAME
[-t FILE | --token-env VAR] ADDR` requires [[sub-imperium]]'s `post` capability
and publishes `/srv/NAME`. The existing shell runs `mount /srv/NAME PATH /`.
ADDR accepts `host!port` or `host:port`, with dotted IPv4 and a bounded port.
Post mode rejects a child command or `-a`; the shell supplies the attach name.
No credential source means explicitly announced PLAIN 9P, not encryption.

The supported host-side example is [npxf](https://github.com/apology-is-policy/npxf),
a separate C++20/CMake project targeting Linux and Darwin with OpenSSL 3 EVP
primitives. NPXF v1 framing, transcript labels, token-file CR/LF trimming and
counter nonces remain wire-compatible. OpenSSL is the host implementation;
Haul retains its existing RustCrypto implementation. This is not TLS.
The remote-files operator section covers host build, token provisioning and
read-only export, including QEMU's 10.0.2.2 host-loopback route.

## Mechanism

`cmdline::plan` fixes the options/child boundary at two operands and refuses
ambiguous late options. `run` creates a post listener before dialing, then
performs the entire npxf handshake before launching pumps. `accept_owner`
uses fresh kernel SRV_PEER identity to reject dead or different-principal
clients. Exactly one accepted connection owns the remote session; subsequent
connections are closed instead of mixing their 9P tags and fids.

The private path uses two pipes and SYS_ATTACH_9P. The posted path uses one
accepted byte-service descriptor, which the shell attaches with
SYS_ATTACH_9P_SRV. [[sub-kernel-devsrv]] owns admission bounds and tombstone
recycling. The main loop checks pump completion and listener readiness every
50 ms; process exit tears down the transport. Unmount drops the last client
reference and lets the relay exit. Abdication revokes every process in the
scope, including a relay with an active mount.

## Data structures

`UpCtx` owns the outgoing sealer, `DownCtx` the incoming opener. `Ready` marks
the fd that actually carries readiness: TCP uses its `/ready` sibling; a pipe
or accepted byte connection uses its own fd. `STOPPED` atomically publishes
which pump ended. Tokens are wiped after handshake; record buffers are bounded
by MSG_MAX (64 KiB).

## Concurrency

One thread per direction; the main thread owns admission and process lifetime.
Posted pumps share their accepted fd, so neither closes it while its peer
could use it; process exit performs teardown. On the private path the down
pump alone owns its reply writer and closes it on failure, waking synchronous
attach/read waiters. TCP zero writes are retried through POLLOUT readiness
within WRITE_STALL_MS. Handshake reads have a total deadline.

## Invariants enforced

![[inv-i1#Statement]]

Service identity comes from SRV_PEER, not client data. A second client never
inherits the first client's remote 9P session.

![[inv-i2#Statement]]

The post capability is elevation-only, flow-limited by the scope and bounded
at kernel reservation. Haul cannot publish a TCB name or elevate itself.

## Error paths

Bad arguments, inaccessible tokens, denied posts, handshake failures, thread
creation failures, and relay completion all exit the process and release its
service/connection resources. Post creation failure never dials. An unexpected
remote close fails a blocked attach via transport teardown. Mount/unmount
builtins expose failures through `$status` and `$errstr`.

## Performance

There is no filesystem cache in Haul. Each 9P frame travels through a relay
thread and, when selected, one npxf record. One post serves one mount; it is not
a multi-client remote filesystem proxy.

## Prosecution

Attack token-source ambiguity, handshake deadlines, malformed and oversized
frames, direction/counter separation, writes returning zero, peer identity,
second-client admission, remote EOF during attach, fd reuse, scope exit during
fork, service quota races, and stale openers during slot recycling. Host KATs
and live interoperability validate wire compatibility; interactive tests must
read actual remote data and witness teardown, not merely a startup banner.
`haul-post` covers denied unprivileged posting, encrypted same-shell reads,
second-attach rejection, unmount/reap, repost and abdication. The added remote-FIN
arm checks an authenticated server disconnect during a posted attach. `haul-npxf` and
`haul-hangup` cover the private/child path and remote-close regression.

The OpenSSL host migration (npxf `cd35c64`, 2026-09-18) passes all 53 Haul
host tests with live native macOS interoperability. The explicit CI-profile
guest passes `haul-npxf` and `haul-post`, 56s each, against that same server.
Native npxf CTest also passes on Linux arm64 and macOS; macOS LLVM ASan/UBSan
passes. The Pi GCC ASan runtime fails before main even for an empty program;
that lane is unavailable coverage, not a passing sanitizer result.

## Seams

Credential retrieval through corvus remains a separate operator-deferred
integration. The current interface intentionally uses a token file or an
explicit environment source. No new vault seam identifier is assigned here.

## Caveats

The inherited npxf review debt in `docs/HAUL-DESIGN.md` remains visible: stack
copies of secret intermediates and a deterministic TCP back-pressure witness
are not closed by a small-file E2E. The user requested single-agent work for
this integration; self-review is not an independent adversarial audit. Trusted
Imperium interaction currently uses the serial SAK path.

## Provenance
