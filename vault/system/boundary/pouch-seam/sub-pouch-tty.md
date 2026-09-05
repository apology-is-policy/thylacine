---
id: sub-pouch-tty
type: sub
parent: moc-pouch-seam
title: "The tty ioctl dispatcher — pts and console"
code:
  - usr/lib/pouch/patches/0021-pouch-pty.patch
  - usr/lib/pouch/patches/0029-pouch-cons-winsize.patch
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
design: ["docs/PTY-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Terminals for ported programs. musl's entire pty/termios/job-control
surface is `ioctl`-shaped — `isatty` is a `TIOCGWINSZ`, `unlockpt` is a
`TIOCSPTLCK`, `ptsname_r` is a `TIOCGPTN`, `tcgetattr`/`tcsetattr` are
`TCGETS`/`TCSETS`, `openpty` is all of them — so ONE dispatcher makes
every upper wrapper work unpatched. Thylacine has no kernel `ioctl` at
all, so the dispatcher replaces musl's raw passthrough with three
different mechanisms.

## Contract

- termios + winsize on a pts ↔ the ptyfs ctl file `/dev/pts/<n>ctl`
  (the five-flag grammar + `winsize`).
- Controlling-terminal ops → kernel syscalls `SYS_TTY_ACQUIRE` (95) /
  `SET_FG` (96) / `GET_FG` (97).
- `ptsname` → an fstat qid decode.
- Console (`/dev/cons`): `TIOCGWINSZ` reads the ungated `/dev/winsize`
  leaf; `TIOCSWINSZ` is `EPERM`; termios is `ENOTTY`.
- Anything unrecognized → `ENOTTY` (the Linux convention; pre-PTY-3 pouch
  answered `ENOSYS` for every ioctl).

## Mechanism

**Is-a-terminal is a two-gate decode, and the first gate is
load-bearing.** `pts_resolve` fstats the fd and requires `S_ISCHR`
**then** the `PTS_FLAG` (bit 40) qid marker with a master/slave filekind.
netd's `/net` qids also use bit 40 (`CONN_FLAG`) — but netd reports
`S_IFREG`, so a socket fd can never pass the `S_ISCHR` gate. A tagged
pouch socket fd is rejected up front, before the fstat. The console arm
adds `cons_resolve` on the pts miss: `S_ISCHR` + bit 41
(`CONS_STAT_QID_FLAG`), deliberately disjoint from bit 40 under the
shared `S_IFCHR` posture.

**The ctl file is read whole in one read, and that is a property, not an
assumption.** `pts_ctl_read` parses the ~54-byte render from a single
`read` — sound only because ptyfs serves the full render at offset 0 in
one Rread, the negotiated msize is far larger, and a ctl read never
defers. A partial read would silently mis-parse flags and winsize into
wrong-but-successful `TCGETS`/`TIOCGWINSZ` answers, so the audit made the
dependency explicit with the condition under which it must become a loop
([[fnd-pty3-r1-f2]]).

**The termios subset is honest about what survives.** Exactly five ldisc
flags round-trip (ICANON / ECHO / ISIG in `c_lflag`, ICRNL in `c_iflag`,
OPOST+ONLCR in `c_oflag`); every other bit and every `c_cc` slot is
accepted-and-ignored on set, and reported at a fixed cooked baseline on
get (VINTR ^C, VQUIT ^\, VSUSP ^Z, VERASE DEL, VMIN 1, VTIME 0).
`cfmakeraw` + `tcsetattr` works because it clears exactly those five.
All three TCSETS variants collapse onto the one atomic ctl write —
drain/flush distinctions are moot because a pts write lands in the peer
ring before the Twrite returns.

**The three controlling-terminal arms have a pre-gate for errno fidelity
only.** They pass the caller's own fd to the kernel, which independently
validates fd-is-a-binding-of-my-controlling-terminal; but without a
`pts_resolve` pre-check a non-tty fd answered the kernel's
`EINVAL`/`EACCES`-shaped refusal where POSIX specifies `ENOTTY`, and a
program branching on `errno == ENOTTY` after `tcsetpgrp(0,…)` on a pipe
would mis-branch. The kernel stays authoritative for real pts fds
([[fnd-pty3-r1-f3]]).

**The raw-`SYS_ioctl` bypasses were the actual bug.** musl reaches its
tty surface two ways — the public `ioctl()` and DIRECT
`__syscall(SYS_ioctl, …)` calls — and the direct ones bypassed the
dispatcher entirely into the `ENOSYS` sentinel, so `ptsname_r` returned
`ENOSYS` and `isatty` was constant-0. Seven callers were rerouted
through the public `ioctl()`. Two of them are the stdio line-buffering
probes (`__fdopen` and the first `__stdout_write`): routing them makes a
pouch program's stdout LINE-BUFFERED on a terminal, which is the POSIX
interactive behavior. Both preserve `errno` across the probe, because a
failed probe on a non-tty is the COMMON path and must not clobber a
successful stdio write's errno.

**The console geometry is physical.** `TIOCSWINSZ` on a cons fd is
`EPERM`: the size is the renderer's cell grid, reported not negotiated,
and Linux-VT-resize semantics are deliberately not offered.
`TIOCGWINSZ` SUCCEEDS even at 0×0 or with `/dev/winsize` unreachable —
because that call is also `isatty()`, and a cons fd IS a terminal; a
0-winsize is the standard CPR-fallback convention. Before #55c a cons fd
was statless, so `isatty` was false on the console and stdio ran fully
buffered — a latent POSIX defect with no interactive victim until
graphics.

## Data structures

`struct pouch_tstat` — the third hand-mirrored `t_stat` in the series
(88 bytes, `_Static_assert`-pinned), consuming only `mode` and
`qid_path`.

## Concurrency

None. Every ctl operation is a stateless per-op open/read-or-write/close
— deliberately, so there is no cached ctl fd to invalidate when a pts is
freed.

## Invariants enforced

No §28 invariant binds this surface: I-20's data path is ptyfs's, and
pouch is a client of it. The obligations here are POSIX-fidelity ones —
the two-gate discrimination, the `ENOTTY` posture, and the errno
pre-gates — plus **P-3**, which is what forces `TCFLSH` / `TIOCGSID` /
`TIOCNOTTY` to answer `ENOTSUP` instead of a silent 0 that would lie
about queued bytes.

## Error paths

`ENOTTY` for an unknown request on any fd, and for a tty request on a
non-pts non-cons fd. `EPERM` for a console `TIOCSWINSZ`. `ENOTSUP` for
flush / sid / notty. The kernel's tty-syscall errors pass through the
seam decode, with one documented divergence: the kernel's EPERM contours
answer `EACCES` (the errno.h -1-alias rule), so `tcsetpgrp`'s POSIX-EPERM
cases read as `EACCES`.

## Performance

Every termios or winsize op is an open + read/write + close of a ctl
file — a few RPCs, on a path nothing hot uses. The stdio tty probe runs
once per stream lifetime.

## Prosecution

- The `S_ISCHR`-then-flag gate order, and the disjointness of bit 40
  (pts) from bit 41 (cons) under the shared `S_IFCHR` posture.
- The single-read ctl parse must stay covered by ptyfs's whole-render
  guarantee.
- Every direct `__syscall(SYS_ioctl, …)` in a newly-vendored musl file
  is a silent bypass — the reroute set must be re-swept on a re-vendor.
- `put_dec`'s scratch is sized for any unsigned caller, not for the two
  `unsigned short` call sites it has today ([[fnd-pty3-r1-f1]]).
- The stdio probes must keep preserving `errno`.

## Seams

[[seam-pouch-forkpty]] (`forkpty` and `login_tty` are structurally dead —
no `fork`, no dup2-onto-target) · [[seam-pouch-sigtstp-ignore]] (the
SIG_DFL `^Z` seam, shared with [[sub-pouch-signal]]).

## Caveats

- Console termios stays `ENOTTY` by design: the console's termios is held
  by the consctl delegation chain (joey → login → ut), not by apps.
- `kill(-pgrp, sig)` has no kernel form (`SYS_POSTNOTE` has no
  process-group arm); a negative pid fails the pid lookup honestly.
- A closed fd yields `ENOTTY` rather than `EBADF`, because `SYS_FSTAT`
  answers a bare -1 for both a bad fd and a valid statless fd — musl's
  `isatty()` normalizes to `ENOTTY` anyway, so the app-visible behavior is
  identical.
- `78-pouch.md` (absorbed) names this patch pair by stale numbers: the
  console arm is `0029`, not `0026` (which is now the process-lifecycle
  patch).

## Provenance

[[chg-2026-07-18-pty3]] (0021; [[adt-pty3-r1]] CLEAN 3 P3) →
[[chg-2026-07-22-55c-cons-winsize]] (0029, the console arm).
