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
updated: 2026-09-23
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

Every file on a Haul mount, private or posted, is owned by the principal that
mounted it and that principal's primary group. The server's per-file mode is
kept, and chown and chgrp there are refused (HAUL-DESIGN 4.7).

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

**Both paths cape the session** (HAUL-DESIGN 4.7). npxf reports the host's
owners (uid 501 and group staff on a Mac). No Thylacine principal holds them,
so the kernel's rwx check made every guest user "other", and a private
0700/0600 export was unreadable to the user who mounted it. `run` therefore
passes `T_ATTACH_9P_CAPE` on `SYS_ATTACH_9P`'s flags word, and `post_listener`
creates the service with `T_WALK_CREATE_DMSRVCAPE` beside `DMSRVBYTE`. The
service carries the mark, so every attach over one of its connections is caped
and the shell's plain `mount /srv/NAME` needs no option. What a caped session
reports and refuses is [[sub-kernel-ninep-dev9p]]'s; where the cape is decided
is [[sub-kernel-ninep-attach]]'s. The mark grants nothing new. The token
already gives the mounter everything the server serves, and the kernel admits
the mark only on a byte-mode post, whose attacher holds the raw connection.

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

`haul-cape` serves a writable 0700/0600 export from its own npxf and compares
the guest's view with the host file's own ids, so a fixture whose ids happened
to match cannot pass. On the private mount, a 0600 file reads, and so does one
under a 0700 directory; `stat` reports the mounter's uid and primary gid with
the host's mode; a create, a mkdir and a chmod land on the host, checked there.
A plain mount of a `--post` service is caped too. Sabotage (2026-09-23): the
private attach with flags 0 fails the first read with
`cat: /tmp/cape/secret.txt: permission denied`, the operator's symptom; a post
without `DMSRVCAPE` passes the private legs and fails the posted read with
`cat: /tmp/cape-post/posted.txt: permission denied`. chown is not reachable
from the guest's tools, so the refusal is held in the kernel suite
(`dev9p.cape`).

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

- 2026-09-17/18: the relay, `--post`, and the npxf OpenSSL host migration.
- 2026-09-23 (L): the identity cape on both paths; the `haul-cape` gate.
