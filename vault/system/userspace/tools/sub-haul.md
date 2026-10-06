---
id: sub-haul
type: sub
title: "Haul — remote 9P relay and same-shell service posting"
parent: moc-userspace-tools
code: [usr/haul/src/addr.rs, usr/haul/src/cmdline.rs, usr/haul/src/frame.rs, usr/haul/src/lib.rs, usr/haul/src/main.rs, usr/haul/src/npxf.rs, usr/haul/Cargo.toml]
audit: hard
guarded-by: [inv-i1, inv-i2]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: [docs/HAUL-DESIGN.md]
created: 2026-09-17
updated: 2026-10-06
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
A token shorter than 16 bytes is accepted with a warning (`haul: warning: the
token is only N bytes`): npxf's handshake lets anyone who can reach the server
test guesses offline (HAUL-DESIGN 3.1, 5).

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

The dial is `TcpStream::dial`, `Dialing::wait` for `SLOW_DIAL` (2 s), then
`Dialing::finish` ([[sub-libthyla-rs]]). A dial still out at 2 s prints
`no answer from ADDR yet -- still trying` and keeps waiting for netd's verdict
(its 15 s connect deadline, [[sub-netd-server]]). A failure names the address
as typed and the reason: `connection refused`, `no answer (timed out)`, or `no
network` when `/net/tcp` is absent.

Every line goes through `tell`: stderr when fd 2 was open at start, else the
kernel console (SYS_PUTS). The fallback fires only for a CLOSED fd 2, as with a
stdio-less exec. `/dev/null` is open, so a launcher that hands over /dev/null
discards the lines. `rs_main` samples fd 2 before anything is opened.
With slot 2 empty, the kernel hands out haul's own token and connection fds from
the lowest free slot, so a later check could find the TCP data file there. The
console was once the ONLY sink. It is a different device from a Halcyon tile's
pty, so every haul line reached serial and the tile showed nothing. The serial
fleet could not see the difference, because there fd 2 is the console.

The private path uses two pipes and SYS_ATTACH_9P. The posted path uses one
accepted byte-service descriptor, which the shell attaches with
SYS_ATTACH_9P_SRV. [[sub-kernel-devsrv]] owns admission bounds and tombstone
recycling. The main loop checks pump completion and listener readiness every
50 ms; process exit tears down the transport. Unmount drops the last client
reference and lets the relay exit. Abdication revokes every process in the
scope, including a relay with an active mount.

**Replies are held to the session's msize** (HAUL-DESIGN 2.2). The kernel caps
its receive at the msize it proposes -- 4 KiB on the direct mount
(`SYS_ATTACH_DEFAULT_MSIZE`), 32 KiB on a mount of a posted service
(`SRVCONN_MSIZE`) -- and holds what a reply carries to the lower msize an
Rversion agrees. A reply past either marks the whole session dead, after which
nothing reads the reply pipe or tells haul: a relayed one left the park form
over a dead mount with nothing in the log. `frame::ReplyBound` (`REPLY`) starts
at `MSG_MAX`. The up pump sets it from every Tversion before forwarding the
frame; the down pump lowers it to the Rversion's msize and refuses any reply
above it. The refusal ends the down pump with `STOP_REFUSED` and closes the
kernel's reply pipe as a hang-up does, so a waiting call fails and the main
thread exits non-zero. Read from the Tversion rather than fixed at 4 KiB, the
bound also passes a posted mount's legitimate 32 KiB replies.

**A session Thylacine ends is named as Thylacine's** (HAUL-DESIGN 2.x;
ARCHITECTURE 21.10, "A death hangs up"). A reply the kernel refuses after the
version exchange -- one carrying a tag it never issued, say -- kills the
session in the 9P client's demux, where haul cannot see it: nothing about the
frame's size is wrong, so haul relays it. The kernel then hangs up its end of
the c2s pipe. The up pump reads EOF there, or the down pump's write into the
kernel's reply pipe is refused, and either records `STOP_KERNEL`: haul prints
`haul: Thylacine ended the 9P session with ADDR -- the mount is dead` (the
command form: `... while the command was running`) and exits 1, naming the
session rather than the server, whose connection is still open. The `-v` mount
check says `mount check: Thylacine ended the 9P session during the listing`.
Before the hangup a dead session left haul parked on a dead mount that told no
one.

The stop recorded first names the side, and main asks the pipe rather than
waiting for a pump. The kernel hangs up as it marks the session dead, before
the call that met the death returns, so the up pump may not have run yet.
`kernel_ended(c2s_rd)` answers true for a recorded `STOP_KERNEL`, or for no
recorded stop and a zero-timeout poll that finds POLLHUP on the kernel's end;
a pump that stopped for the peer or for haul recorded that before the kernel
could see anything. At the attach the order matters. A failed attach drops the
kernel's references to the pipes, so once haul closes its own copy of the c2s
write end the pipe reads POLLHUP whoever refused. haul therefore asks while its
copy still holds the end open, when only the kernel's hangup can raise it:
`attach (Thylacine refused the server's reply)` for a session the kernel
killed, and `attach (9P handshake refused)` for an Rlerror the server sent. A
refused Rversion is not a death: the exchange runs before there is a session
to kill, so the attach fails with no hangup and reads as the handshake
refused.

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

**Both paths declare the session remote** (LR-1, HAUL-DESIGN 4.8, the
operator's `la` vote). Haul holds the TCP connection, so Haul is the program
that knows the session leaves the machine: `run` adds `T_ATTACH_9P_REMOTE`
beside the cape, and `post_listener` adds `T_WALK_CREATE_DMSRVREMOTE` to the
post. The declaration rides the session, not the mount call, so the shell's
plain `mount /srv/NAME` over a posted service is marked too, though the
shell never learns what is behind the service. `ls -l` and `stat` then show
`remote` at the mount point, `realm` prints `remote`, and `ns` ends the line
in `remote`. The declaration grants nothing: the kernel reads it to render
`/proc/<pid>/ns` and to contain the links the export serves (below), which
only narrows. The ns line names the session by the file it came over (operator vote 2026-09-28): the shell's mount of a posted
service reads `mount /tmp/NAME /srv/NAME remote`, and `run`'s private form,
whose session rides pipes, `mount PATH #| remote`. `haul-npxf` (the
child's `ls -l` and `ns`, with the shell's unmounted view of the same
directory as the control) and `haul-post` (the shell's mount of the posted
service beside an unmounted sibling) hold it on the device. With the
declaration stripped from both paths, each gate passed every earlier leg and
failed at its first LR-1 leg (2026-09-28). Their `ns` legs assert the file's
name too; with the kernel's two stamps removed (and the two kernel tests that
catch that unregistered, so the boot reaches a login), each gate passed every
earlier leg and failed there, its line reading `/` (2026-09-29).

**The export's links resolve inside the export** (DISTRO 4.6, operator vote
2026-10-05, built 2026-10-06). The kernel resolves a link whose session is
declared remote beneath the mount it was reached through
([[sub-kernel-stalk]]): an absolute target names a path from the mount's root,
and no `..` climbs above it. Haul needs no code for it beyond the declaration
it already makes on both paths. npxf serves a link's target text unchanged, so
a host link to a path outside the export names the same path inside the export
when the guest follows it. `haul-links` holds the rule on the device: it starts
its own npxf export holding links, plants a guest decoy at every path a link
names, and reads through the direct mount (an absolute link, a `..` target, the
caller's `..` after a link, an internal link) and through a plain `mount` of a
posted service; an escaped leg reads the guest's decoy.

## Data structures

`UpCtx` owns the outgoing sealer, `DownCtx` the incoming opener. `Ready` marks
the fd that actually carries readiness: TCP uses its `/ready` sibling; a pipe
or accepted byte connection uses its own fd. `STOPPED` atomically publishes
which pump ended, that haul refused what came down (`STOP_REFUSED`), or that
the kernel let go of the session (`STOP_KERNEL`). `REPLY`
is the reply bound, one `AtomicU32` (`frame::ReplyBound`). Tokens are wiped
after handshake; record buffers are bounded by `frame::MSG_MAX` (64 KiB), set
when libthyla-rs's heap was a fixed 4 MiB (two records
were a quarter of it); since B-1c the heap grows, and the bound stays the
record's own.

## Concurrency

One thread per direction; the main thread owns admission and process lifetime.
Posted pumps share their accepted fd, so neither closes it while its peer
could use it; process exit performs teardown. On the private path the down
pump alone owns its reply writer and closes it on failure, waking synchronous
attach/read waiters. TCP zero writes are retried through POLLOUT readiness
within WRITE_STALL_MS. Handshake reads have a total deadline. The up pump
publishes the reply bound before it forwards a Tversion, and the down pump
reads it only once a reply has arrived. The server cannot answer a frame it has
not received, so that read sees the store; a pump that read the bound and then
blocked for the reply would hold `MSG_MAX`.

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
remote close fails a blocked attach via transport teardown. A reply over the
session's msize is refused: `haul: ADDR sent a N-byte reply, over the session's
M-byte msize -- refusing it`, then `haul: the 9P session with ADDR is broken --
the mount is dead` (the command form: `... broke while the command was
running`; the posted form: `... is broken -- the posted mount is dead`; during
the attach: `attach (haul refused the server's reply)`), exit 1. The record layer's refusals -- a record claiming too much, a failed tag, a
size field that disagrees with the record -- end the session the same way.
A session the kernel ends prints `haul: Thylacine ended the 9P session with
ADDR -- the mount is dead`, exit 1; during the attach it is `attach
(Thylacine refused the server's reply)`, and a refusal the server sends is
`attach (9P handshake refused)`.
Mount/unmount
builtins expose failures through `$status` and `$errstr`. A private mount at a
point that is not a directory fails with `haul: mount PATH: not a directory`,
the kernel's `ENOTDIR` for Plan 9's `Emount`; the kernel names no other cause,
so any other refusal says only `haul: mount`. A failed dial exits 1
before anything is mounted or pumped. It says `connection refused` for a RST,
and `no answer (timed out)` at netd's deadline, preceded by the 2 s progress
line. The texts are the operator's (manual 14).

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
`haul-hangup` cover the private/child path and remote-close regression. Every
hang-up leg (`haul-hangup`, `haul-npxf`'s relay leg, `haul-post`'s remote-FIN
arm) requires the peer's own verdict on haul's side -- `the other side closed`
or `reset` -- and fails on `STILL OPEN`; they used to wait for that line and
assert nothing about it. `haul-hangup` also holds the reply bound: its peer
serves Tversion and Tattach, answers the `-v` mount check's first request with
an oversized frame and keeps the connection open. 4608 bytes against the
kernel's 4096-byte proposal after an Rversion claiming 65536, which lowers
nothing, and 3072 bytes after an Rversion agreeing to 2048 must each be refused
and named, and haul must exit and close the connection. Each leg leaves one
half of the bound as the only defence, and a haul without that half parks, its
mount check unable to read. Three legs hold the kernel's hangup. The
stray-reply leg answers the mount check's first request with tag 0xFFFE: the
kernel kills the session and hangs up, haul names Thylacine and exits, and the
peer sees the connection closed. With the hangup removed from the kernel (a
`TESTS=n` image, so the boot reaches the leg), the mount check said only that
it CANNOT read, three attempts of three (2026-10-05). The attach pair runs one
variable apart: an Rattach carrying a tag the kernel never issued must name
Thylacine, and an Rlerror answering the Tattach must read as the handshake
refused, with no hangup. With the hangup removed, the first went red as `attach
(9P handshake refused)`; with haul asking after its close, the second went red
as `attach (Thylacine ...)`, each three attempts of three (2026-10-05). Its
last legs hold the short-token warning: a 15-byte token warns, a 16-byte one
does not. The bound's rules are host-tested in `frame.rs`: the proposal, the
ceiling, a Tversion cut short, an Rversion that would raise the bound, a
second Tversion.
`haul-unreachable` starts haul through `exec-probe stderr-to FILE`, which points
fd 2 at a file before exec, because `ut` has no `2>`. It asserts that no haul line
reached the console. Its three legs:
- host port 1, which nothing listens on and no scenario's port-0 bind is ever
  handed -> `connection refused` in the file;
- 10.0.2.99, which never answers ARP (guestfwd unset) -> the progress line, then
  `no answer (timed out)`;
- a stdio-less park form dialing a peer that accepts and hangs up. haul's own
  data fid then occupies slot 2 when the attach fails, so the line must reach
  the console. Sampling fd 2 at print time would instead write it into the
  connection.

The pumps mask the `pipe` note at entry. `tell` from a pump can meet a stderr
that `ut` handed over as a pipe nobody reads, and the unmasked default would
kill haul before its main thread reported.

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
Imperium interaction currently uses the serial SAK path. A reply the kernel
refuses for a reason other than its size -- a tag it never issued, a type that
does not answer the request -- still marks the session dead out of haul's
sight, and the park form then waits until unmount. Closing it needs the kernel
to hang up its end of a dead session's transport (P3b, the kernel half of the
2026-09-29 Haul review's F1).

## Provenance

- 2026-09-17/18: the relay, `--post`, and the npxf OpenSSL host migration.
- 2026-09-23 (L): the identity cape on both paths; the `haul-cape` gate.
- 2026-09-24: the operator's "haul doesn't error" (Halcyon).
  - Lines go to stderr; the console is only the fallback.
  - The dial reports its address, its reason, and a slow-dial progress line.
  - With netd's dial verdict, a refused dial is refused at the dial rather than
    racing into the attach.
  - `haul-unreachable` added; `haul-hangup`'s "connect to a closed port
    succeeds" header corrected.
- 2026-10-05 (P3a, the 2026-09-29 Haul review's F1 haul half and F3-F5):
  replies held to the session's msize (`frame::ReplyBound`); the short-token
  warning; the hang-up legs assert the peer's verdict. Each half of the bound,
  and each side of the warning's threshold, removed on its own failed exactly
  its own `haul-hangup` leg, every leg before it passing.
