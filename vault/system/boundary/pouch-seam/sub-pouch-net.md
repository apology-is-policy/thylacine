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
audit: hard
guarded-by: [inv-i1, inv-i28]
validated-by: [prose, gate-smp]
locks: [lock-pouch-sock-table]
design: ["docs/POUCH-DESIGN.md", "docs/NET-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-01
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
  `SYS_SRV_PEER`.
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
  set is this surface's central obligation and has been breached twice
  (0015's `poll`, 0017's `shutdown`/`sendto`/`recvfrom`). Both were
  fail-closed rather than dangerous, which is the tag design working.
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
  `PROC_HANDLE_MAX` was 64; since #355 the fd table is 256 and only the
  `SYS_POLL` *nfds count* is bounded at `POLL_MAX_NFDS` = 64. So a
  program holding fds ≥ 64 gets valid fds wrongly `EBADF`'d by
  `select()`. `poll()` is unaffected (fd values pass through; only the
  count is bounded, matching the kernel). Latent — no in-tree pouch
  consumer holds >64 fds — but reachable by any ported program with a
  large fd population, and the three patches that mirror the constant
  (0005 / 0015 / 0018) all still name it `PROC_HANDLE_MAX`.
- `POUCH_SOCK_MAX` is 8 concurrent sockets per Proc.
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
[[chg-2026-07-22-52-nonblock]] (0028).
