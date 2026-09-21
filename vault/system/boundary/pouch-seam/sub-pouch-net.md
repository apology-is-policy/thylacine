---
id: sub-pouch-net
type: sub
parent: moc-pouch-seam
title: "Sockets and readiness — poll/select, AF_UNIX over /srv, AF_INET over /net"
code:
  - usr/lib/pouch/patches/0005-pouch-poll.patch
  - usr/lib/pouch/patches/0006-pouch-sockets.patch
  - usr/lib/pouch/patches/0014-pouch-srv-stubs.patch
  - usr/lib/pouch/patches/0015-pouch-poll-tag.patch
  - usr/lib/pouch/patches/0016-pouch-net-sockets.patch
  - usr/lib/pouch/patches/0017-pouch-net-datacalls.patch
  - usr/lib/pouch/patches/0018-pouch-net-poll.patch
  - usr/lib/pouch/patches/0020-pouch-srv-bulk.patch
  - usr/lib/pouch/patches/0028-pouch-net-nonblock.patch
  - usr/lib/pouch/patches/0038-pouch-stdio-socket-fds.patch
  - usr/lib/pouch/patches/0039-pouch-fdset-guard-ppoll-tag.patch
  - usr/lib/pouch/patches/0041-pouch-poll-stream-socket-shape.patch
  - usr/pouch-hello/pouch-hello-sockets.c
audit: hard
guarded-by: [inv-i1, inv-i28]
validated-by: [prose, gate-smp]
locks: [lock-pouch-sock-table]
design: ["docs/POUCH-DESIGN.md", "docs/NET-DESIGN.md"]
created: 2026-08-01
updated: 2026-09-21
---
## Purpose

The BSD socket API on a kernel with **no socket syscalls at all** — the
committed ARCH §11.5 / NOVEL #1 position. A pouch socket is a userspace
slot; its I/O is file I/O on `/srv` (AF_UNIX) or on netd's `/net` tree
(AF_INET), the Genode `socket_fs`-in-libc model. `poll` / `select` /
`ppoll` / `pselect` live here because on a tagged socket fd they are the
same translation problem.

## Contract

- `socket()` returns a **tagged** fd (`0x40000000 | slot`), never a
  kernel fd. Every fd-consuming call must dispatch on the tag.
- AF_UNIX: `bind` = create=post (`SYS_open /srv O_PATH` +
  `SYS_WALK_CREATE <name>` with `DMSRVBYTE`); `connect` = open=connect
  (`SYS_open "/srv/<name>" ORDWR`); `accept` = `SYS_SRV_ACCEPT` → a raw
  kernel Spoor fd (deliberately UNtagged); `getsockopt(SO_PEERCRED)` =
  `SYS_SRV_PEER` marshalled into a `struct ucred` — and since A-3 that
  `ucred` carries the peer's **kernel-stamped principal**, not a `0/0` stub.
- AF_INET: `socket` opens `/net/<proto>/clone` (the fid IS the new
  connection's ctl; reading it yields N); `connect`/`listen`/`shutdown`
  write ctl verbs; `accept` opens `listen`; data rides
  `/net/<proto>/N/data`; names come from `local` / `remote`.
- `poll` on a tagged fd targets the slot's POLL fd — which for AF_INET is
  the `ready` sibling, not the data fd.
- No new kernel surface anywhere: `SYS_open` / `read` / `write` / `close`
  / `poll` only.

## Mechanism

**The tag bit sits above `PROC_HANDLE_MAX`** so a pouch socket fd can
never collide with a kernel fd — and a tagged fd reaching an unshimmed
call is the recurring failure mode of this surface, not a hypothetical:
0015 exists because `poll()` was the one fd-consuming call sub-chunk 12
left pristine, so stratumd's `poll(listener)` got `POLLNVAL`, its accept
loop bailed, and the wrong-pid reap chain took the boot down.

**`accept` deliberately returns an untagged fd.** The accepted endpoint
is a plain kernel Spoor, so server-side I/O stays on the syscall fast
path with no dispatch at all. (The AF_INET arm is the exception: it mints
a fresh tagged slot, because an AF_INET connection is a *pair* of files
plus a connection number, not one stream.)

**Three fds per AF_INET socket.** `ctl` (held for the socket's lifetime,
from `socket()`), `data` (the I/O target, opened at connect/accept), and
lazily `ready` (the poll target, opened at first `poll`). All three close
at `close()`.

**The readiness fd is the whole point of 0018.** A `/net/<proto>/N/data`
fd is a REGULAR dev9p file, and the kernel's `dev9p.poll` treats a
regular 9P file as POSIX always-ready (it probes only a `QTPOLL`-marked
Spoor). So polling the data fd reported ready unconditionally and
DEFEATED the wait — `poll(POLLIN)` returned immediately, and every
`select()` reported every `/net` fd ready always. `pouch_sock_poll_fd` is
therefore a distinct resolver from `pouch_sock_kernel_fd`: I/O target vs
poll target, and confusing them is silently wrong rather than loud.

**Nonblocking reads are netd's job, not a readiness poll.** `FIONBIO`
writes a `nonblock` ctl verb so an empty-but-open `data` read answers
`E_AGAIN`; the read shims stay pristine. The rejected design — gating
each read on a 0-timeout poll — churned the shared session's tag pool to
EXHAUSTION, because every probe parks an op the kernel kthread
Tflush-abandons and the abandoned tag sits `awaiting_flush` until netd
Rflushes it; a tight read loop out-ran the Rflush and every subsequent
data read got a spurious `EIO`. That is why `FIONREAD` — which *does*
touch the bridge — is documented as a COLD path.

**The `/srv` stubs are Plan-9-shaped truths, not lies.** `lstat` on
`/srv/<name>` answers `ENOENT` ("no stale socket"), `unlink` answers 0
("nothing to remove"), `chmod` answers 0 ("no mode bits on a byte-mode
SrvConn") — which lets the idiomatic unlink-then-bind dance proceed. If
the name is genuinely taken, `bind` still fails `EACCES` from the kernel
post-gate, so the stub's optimism is never load-bearing.

**`connect` can walk one component.** `connect("/srv/<name>/<walk>")`
does the open=connect-then-walk two-step a native client performs
(open the service OREAD → the dev9p root, relative-open `<walk>` ORDWR →
the sub-fid, drop the root), which is how the coordinator stratumd
reaches corvus's verb protocol on its `ctl` file.

**`SO_PEERCRED` carries the connecting Proc's kernel-stamped principal
(A-3).** `getsockopt(SO_PEERCRED)` reads `SYS_srv_peer` into the 40-byte
`pouch_srv_peer_info` and marshals `principal_id -> ucred.uid` +
`primary_gid -> ucred.gid` — a v1.0 change from the `0/0` stub written when
"Thylacine has no uid model." The principal is *kernel-stamped* (the kernel
fills it from the peer Proc's durable `principal_id` at `SYS_srv_peer`), so
a connecting Proc cannot forge the identity it presents — the property that
lets a trusted-local server (a per-user stratumd) stamp create-ownership
from the peer cred and be reconciled against the kernel's dev9p rwx
enforcement without a `Tauth` handshake. This is the load-bearing local
identity channel; the 9P `n_uname` field is the vestigial one, demoted to
the v1.x foreign/authenticated path ([[inv-i22]] — the identity is asserted
by the kernel, never self-elevated by the client). See
[[sub-kernel-syscall-dispatch]] for the kernel `SYS_srv_peer` stamp and the
attach-time `n_uname = principal` substitution.

**`select` translates in userspace**, because the kernel speaks only
`pollfd`: it builds a compacted `pollfd[]`, then clears the output sets
and re-sets only the bits whose `revents` survive the events-mask gate
(POLLERR/POLLHUP forward to whichever sets the fd was requested in).
`poll`'s own slow path EXCLUDES a negative fd from the kernel array
entirely rather than passing -1 — because the kernel answers `POLLNVAL`
for `fd < 0`, which counts as ready, defeats the timeout, and busy-spins
the caller.

## Data structures

`struct pouch_sock_slot[8]` — `in_use` / `state` / `kernel_fd`, plus the
AF_INET fields (`family`, `net_proto`, `ctl_fd`, `conn_n`, `ready_fd`,
the bound local addr, `bulk_hint`, `nonblock`). `family` is set once at
`socket()` and the two families never mix; `FAM_UNIX` is 0 so a zeroed
slot defaults to the older path.

## Concurrency

[[lock-pouch-sock-table]] guards every structural change. The read
discipline is the audit's F8: capture BOTH `in_use` and `kernel_fd`
inside the lock and set errno from the locals — reading `in_use` again
outside would let a concurrent `close` flip it and mis-categorize
`EBADF` vs `ENOTCONN`. The lazy `ready` open is the one place the lock is
DROPPED across a syscall, and it reconciles both outcomes on re-acquire
(slot closed → close the orphan, `EBADF`; a peer won the race → close
ours, return the winner).

Above that lock sits an inherited **single-user-per-socket** discipline:
post-resolve state inspection and the slot writes in `bind`/`connect`/
`accept` are not re-locked, so two threads racing `bind()` on one fresh
fd can both pass `state == FRESH` and orphan a listener handle. Every
later patch's direct slot writes ride the same assumption.

## Invariants enforced

- **[[inv-i1]] / [[inv-i28]] (composed).** A pouch socket reaches only
  the `/srv` and `/net` its Territory grants; the namespace IS the
  firewall, and pouch never touches hardware (netd owns the NIC).
- **P-1** — the socket family is the strongest evidence for it: the whole
  BSD API is implemented with four file syscalls, so ARCH §11.5's
  zero-socket-syscalls commitment holds without a single sentinel
  exception.
- **P-3** — `SOCK_NONBLOCK` at `socket()`, AF_INET6, `SOCK_RAW`, a
  protocol/type mismatch, and every unsupported `setsockopt` fail loud.

## Error paths

`ECONNREFUSED` (any connect failure), `EACCES` (any bind failure) — both
coarse by the flat-`-1` collapse. `ENOTCONN` / `EBADF` from the slot
resolver. `ENOPROTOOPT` for unsupported options. `EOPNOTSUPP` for
unsupported flags. `POLLNVAL` for a FRESH or vacant tagged slot (the
POSIX EBADF surface for a poll).

## Performance

`poll` has a zero-copy fast path when no tagged and no negative fd is
present. An AF_INET `connect` is a ctl write plus a data open; a send is
one `SYS_write`. `SO_SNDBUF ≥ 128 KiB` before an AF_UNIX `bind` marks the
service BULK, so its connections get 128 KiB rings and a
kernel-attached mount negotiates a 128 KiB msize (CF-3 B).

## Prosecution

- **Every fd-consuming call must be tag-aware.** The completeness of that
  set is this surface's central obligation and has been breached three
  times (0015's `poll`, 0017's `shutdown`/`sendto`/`recvfrom`, 0038's
  stdio backends). The first two were fail-closed rather than dangerous,
  which is the tag design working. The third was not quite: the four
  `FILE` backends (`__stdio_read` / `_write` / `_close` / `_seek`) issue
  RAW syscalls on `f->fd`, so they are fd consumers that a sweep of the
  public wrappers never sees. `fdopen(sock)` gave a stream on which every
  operation failed (fail-closed), but `fclose()` on it "closed" a number
  the kernel never issued and STRANDED the slot with its kernel handles —
  a leak against a table of `POUCH_SOCK_MAX` = 8. Found by the audit of
  0035, not by a consumer: the ports that call `fdopen` (GNU make,
  dosbox-x) wrap FILE descriptors, and nobody wraps a socket.
  0038 maps the tag in read/write (`pouch_sock_kernel_fd`), routes close
  through `pouch_sock_close`, and answers `ESPIPE` to a seek.
  `/pouch-hello-sockets` pins it: it wraps the connected client end in a
  `FILE`, writes and reads through it (including a `fscanf` pushback),
  requires `fseek` to fail with `ESPIPE`, `fclose`s it, and then requires
  the number of free slots — MEASURED by opening sockets until refusal,
  before and after — to be unchanged. The sweep rule that follows: a
  tag-awareness sweep lists every site that passes an fd to `__syscall` /
  `syscall` / `syscall_cp`, not every public function that takes an fd.
  Still raw and recorded: `freopen`'s `dup3` onto a socket stream (fails
  cleanly) and `__fdopen`'s `F_SETFD` / `F_SETFL` for the `e` / `a` modes
  (results ignored upstream too).
- `pouch_sock_poll_fd` vs `pouch_sock_kernel_fd` at every poll site.
- The slot-reuse reset list must cover EVERY field — `bulk_hint` was
  missing from it until #52, so a recycled slot could spuriously post
  `DMSRVBULK` on a later AF_UNIX bind.
- Every error path must close every already-opened fd exactly once (three
  fds per AF_INET socket, two of them opened at different times).
- `sun_path` must be explicitly NUL-terminated within the bound —
  without that check a fully-packed `sun_path` passes caller stack
  fragments as a service name.

## Seams

[[seam-pouch-sock-single-user]] (the multi-thread bind/connect race and
every direct slot write built on it) · [[seam-pouch-select-fd-bound]]
(select/pselect reject valid fds ≥ 64) · [[seam-pouch-sendmsg]]
(scatter-gather + cmsg stay `ENOSYS`) · [[seam-pouch-readyfd-aba]] (the
lazy ready-fd slot-reuse ABA, task #222 — pre-registered by the net-6b
round against this surface before it had a node).

## Caveats

- **The `select()`/`pselect()` fd-VALUE bound is stale and now wrong.**
  Both reject any fd ≥ 64 set in an input set, commented as "unreachable
  through any Thylacine syscall — `PROC_HANDLE_MAX`". That was true when
  `PROC_HANDLE_MAX` was 64; the fd table is 1024 today (`handle.h`) and only the
  `SYS_POLL` *nfds count* is bounded at `POLL_MAX_NFDS` = 64. So a
  program holding fds ≥ 64 gets valid fds wrongly `EBADF`'d by
  `select()`. `poll()` is unaffected (fd values pass through; only the
  count is bounded, matching the kernel). Latent — no in-tree pouch
  consumer holds >64 fds — but reachable by any ported program with a
  large fd population, and the three patches that mirror the constant
  (0005 / 0015 / 0018) all still name it `PROC_HANDLE_MAX`.
- `POUCH_SOCK_MAX` is 8 concurrent sockets per Proc.
- **A socket fd cannot live in an `fd_set`, and that was a WILD WRITE until
  0039** (audit B-0 r2 F2; pre-existing). The tag makes a socket fd
  `0x40000000 | slot`; upstream's `FD_SET(d, s)` indexes `fds_bits[d / 64]`
  with no bound, so `FD_SET(sock, &set)` stored 128 MiB past a 16-long
  array — in APPLICATION code, before libc was entered; `select()` refuses
  the fd, but only afterwards. From the main stack that address is usually
  unmapped; from a heap-resident set it is someone's Burrow. 0039 makes
  `FD_SET` / `FD_CLR` / `FD_ISSET` `abort()` (status 127) on a descriptor
  that does not fit the set — glibc's `__fdelt_chk`, and like it made in the
  descriptor's FULL width against the set's OWN size,
  `(unsigned long long)(d) < 8*sizeof(fd_set)`: the first version's
  `(unsigned)(d) < FD_SETSIZE` let a `long` 0x100000005 through as 5 and
  read `FD_SETSIZE` in the APPLICATION's macro context (audit r3 F3). It
  says why only when fd 2 is a terminal: fd 2 is routinely not stderr here
  (a prover is spawned with {0,1}; stratumd with none, so its fd 2 is its
  third real kernel handle), and the first version wrote ~110 bytes of
  English at that handle's cursor (r3 F4). **0039 is the series' first patch
  into a PUBLIC header**: the guard is compiled into the PORT's objects, so a
  port is rebuilt, not relinked, to have it (in-tree that is automatic — a
  stale sysroot rebuild removes every port's output). It also
  routes `ppoll()` through the tag-aware `poll()` (it was a raw
  `SYS_poll`: POLLNVAL, counted ready, a busy-spin; the failure 0015 fixed
  in `poll()` and not there). **That is the honest minimum, not the fix.**
  The fix is socket fds that are small integers (a placeholder kernel
  handle per slot + an fd→slot side table), which lets `select()` work on
  sockets and retires the tag from every fd-consuming call — a redesign of
  0006 / 0016, its own chunk and audit, OWED. Until then a port that
  `select()`s on a socket stops.
- **`poll()` on an AF_UNIX socket: what was claimed, what was true, what is
  true now.** This dossier said on 2026-09-21 that a port which `poll()`s
  "works" and that `/pouch-hello-sockets` "pins both halves". Both false
  (audit r3 F1). 0039 only made `ppoll()` REACH the kernel's poll with the
  right handle; the kernel then sampled the SERVER's end of the connection
  for a client (`POLLIN` from the client's own unread request) and walked no
  hook list when a reply arrived. The prover leg that "pinned" it was decided
  by a scheduling race over that defect — green on the one boot the gate's
  first stage ran, then 57 of 58 failed ci-fleet boots and 17 of 17 failed
  SMP-gate boots, one signature. Now: the kernel polls both endpoints and
  walks the list on every ring mutation ([[sub-kernel-srvconn]]), and **0041**
  gives a CONNECTED AF_UNIX slot the stream-socket SHAPE in `poll()`: the
  kernel's row is pipe-like (`POLLHUP|POLLERR`, no `POLLIN` at a drained EOF —
  what the native 9P servers are written to), and a program written to sockets
  expects a peer's orderly close to read `POLLIN|POLLHUP` with no `POLLERR`.
  The loop every port has — `POLLIN`? then `read`; 0 means closed — never sees
  its `POLLIN` otherwise and spins on a `poll()` that returns at once. Done
  once, at the boundary line. The prover's leg is sequenced by barriers so no
  schedule can turn it green over a broken kernel: the client polls only AFTER
  the server consumed its request, must see EXACTLY `POLLIN` with the
  connection still open, and after the server's close EXACTLY
  `POLLIN|POLLHUP`, then EOF.
- **An ACCEPTED AF_UNIX socket is not covered by 0041.** `accept()` returns
  the kernel handle untagged (0006's design), so `poll()` cannot tell it from a
  pipe without a kernel query per fd per call, and a pouch SERVER still sees
  the pipe-like row when its client closes. No in-tree pouch server polls an
  accepted socket (stratumd blocks in `read()`, thread per connection). Folds
  into the small-integer-socket-fd redesign above — an fd→slot side table
  covers accepted sockets too.
- **The tag-aware set is still not the POSIX set** (census 2026-09-21: every
  site in the patched `src/` that passes an fd to a raw syscall, outside
  `src/network/`). That census method cannot see a call that takes a
  BITMAP or an ARRAY of fds — it missed `select` and `ppoll`, above, and
  its first version claimed here that every remaining call "fails
  visibly". Of the scalar-fd calls that remain, each does fail visibly, so
  none fabricates a value or leaks a slot — they are missing surface, and
  a port that needs one needs a patch. `fstat(sock)` reaches the kernel with a
  number it never issued and gets `EBADF` (no `S_ISSOCK` test). `fcntl`,
  `dup`, `dup3`, `readv`, `writev` never reach it at all: their numbers are
  sentinel-parked for EVERY fd (`ENOSYS`; `dup2` onto a target is a
  documented kernel seam, [[sub-pouch-process]]). So the commonest way to
  make a socket non-blocking, `fcntl(sock, F_SETFL, O_NONBLOCK)`, does not
  work — `ioctl(FIONBIO)` and 0028's `SOCK_NONBLOCK` do — and `writev` to a
  socket does not exist, although 0002 rewrote the stdio backends around
  exactly that absence. OPEN, tracked with the pouch-net completeness work.
- **A stale doc-comment contradicts the live `SO_PEERCRED` marshal.**
  `getsockopt.c`'s top-of-file comment still says `ucred.uid` / `ucred.gid`
  are "0 at v1.0 (Thylacine has no uid model)"; the live A-3 marshal below
  sets them from the peer's `principal_id` / `primary_gid`. The comment is
  wrong, the assignment is right — read the code, not the header.
- `recvfrom`'s `src` is the connection's RECORDED remote, not the
  per-datagram sender — right for a connected socket or the UDP
  request/reply idiom, wrong for a promiscuous receiver.
- `ppoll`/`pselect` IGNORE their `sigset_t` — precisely the race those
  calls exist to eliminate.
- `select(0, NULL, NULL, NULL, +tv)` (the portable-sleep idiom) returns
  `ENOSYS`; the zero-timeout form returns 0. The comment says "Thylacine
  has no sleep syscall at v1.0", which 0022 has since retired for
  `nanosleep` — the select arm was never revisited.
- netd's `check_ready` reports `can_recv()`, false for a TCP listener, so
  `poll(listener, POLLIN)` does not wake on a pending accept (task #220);
  the blocking `open(listen)` is the working path.

## Provenance

[[chg-2026-05-23-p6-poll]] (0005) → [[chg-2026-05-23-p6-sockets]] (0006 +
the kernel byte-mode SrvConn; [[adt-sockets12-r1]] 2 P1) →
[[chg-2026-05-26-16c-pre]] (0014 + 0015) →
[[chg-2026-06-18-net5-af-inet]] (0016; [[adt-net5-r1]]) →
[[chg-2026-06-18-net6a2-datacalls]] (0017) →
[[chg-2026-06-18-net6b-poll-bridge]] (0018, the readiness fd) →
[[chg-2026-07-08-cf3b-bulk-ring]] (0020, the bulk hint) →
[[chg-2026-07-22-52-nonblock]] (0028) →
[[chg-2026-09-06-9p-identity-absorb]] (the A-3 `SO_PEERCRED`-carries-principal
marshal in 0006, folded at the docs/reference retirement). 0038 (stdio over
a tagged fd) landed with the Boosty B-0 libc fixes, 2026-09-21; its audit
record is `memory/audit_pouch_0033_0035_closed_list.md` finding F4.
