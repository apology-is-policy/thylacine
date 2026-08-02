---
id: sub-ptyfs
type: sub
title: "ptyfs — the pseudoterminal server: the pts pairs, the per-pts line discipline, and the teardown algebra"
parent: moc-userspace
code: [usr/ptyfs/src/server.rs, usr/ptyfs/src/main.rs]
audit: hard
guarded-by: [inv-i20, inv-i9, inv-i1]
validated-by: [spec-pty, prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/PTY-DESIGN.md"]
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

The userspace half of the PTY. ptyfs owns the pts pairs — two byte
rings per pair plus a per-pts line discipline — and serves them as a
Linux-devpts-shaped 9P tree that joey mounts at `/dev/pts`. The kernel
owns everything about *authority*: sessions, process groups, the
controlling terminal, and the note routing.

A native libthyla-rs server owning no hardware — the corvus lineage, not
the warden's. joey spawns it with `MAY_POST_SERVICE`, it posts
`/srv/ptyfs`, and joey mounts the result.

## Contract

**The tree:**

| path | qid | what |
|---|---|---|
| `/` | `P_ROOT` (0) | the devpts directory |
| `/ptmx` | `P_PTMX` (1) | the clone file — **open** mints a pts |
| `/<n>` | `PTS_FLAG\|n<<8\|FK_SLAVE` | the slave byte channel |
| `/<n>ctl` | `…\|FK_CTL` | per-pts termios + winsize (the Plan 9 `eia0`/`eia0ctl` suffix idiom, so the slave names stay POSIX-flat) |
| — | `…\|FK_MASTER` | the master, minted by clone; **not walkable, not in readdir** |

`PTS_FLAG` is bit 40 — the same qid template netd and tapestryd use, and
the reason a pts qid can never collide with the attach root the kernel
reserves at 0. The master's unwalkability is not tidiness; it is the
whole of `HupAtMostOnce`.

**The authority bound.** ptyfs's sole signal power is
`SYS_TTY_SIGNAL(pts_id, class)`: it reports a signal-class event on a pts
it serves, and the kernel resolves pts → ct_sid → fg_pgid. **It cannot
name a process group** — there is no parameter for one. That is the
I-1/I-22 bound realized as an absent argument rather than as a check.

**Modes.** ptmx, master and slave report `S_IFCHR` (the Linux pts
posture) — that plus the `PTS_FLAG` qid decode is the pouch
`isatty()`/`ptsname()` discriminator. The ctl file stays `S_IFREG`: a
control file is not a terminal.

## Mechanism

**The clone.** Opening `/ptmx` mints a pts and **rebinds the opened fid
onto the master endpoint** — Plan 9's clone idiom, which the kernel dev9p
client accommodates by accepting an `Rlopen` qid that differs from the
walked one. Register-then-build, so a build failure rolls back whole:
`mint` → `t_pty_register(MINT)` → ref → `open_inc` → build `Rlopen`; the
`Err` arm undoes the count and the ref, and the ref hitting zero frees
the slot and releases the kernel registry entry. A slave open registers
the slave binding with the same registry; a ctl open registers nothing.

**The line discipline** is per-pts, five flags, carrying the kernel
`cons.c` bit values so the ctl grammar speaks the same `+name`/`-name`
set as `consctl`. A fresh pts is **full cooked** — the Linux fresh-pts
posture, unlike the boot console, which comes up ISIG-only.

Input (a master write — the emulator's keystrokes), in order:

1. **ICRNL** — CR folds to NL.
2. **ISIG** — an INTR/QUIT/SUSP char collects its class into a 3-bit
   **set** and is *consumed*: never a byte toward the slave, never
   echoed. The set (rather than a queue) dedups repeats and cannot
   overflow, so a distinct class can never be lost behind a same-class
   run — the G-3-era F2 fix.
3. **ICANON** — erase pops the last unflushed byte and echoes `\b \b`;
   NL flushes the line *including its newline*; anything else assembles.
4. **raw** — straight into `m2s`.

Output (a slave write) is ONLCR only, and its expansion is pair-atomic:
a `\r\n` that does not fully fit stops *before* its input byte, so a
retry can never double the CR.

`echo()` is the single chokepoint every echo staging passes through,
gated on ECHO at the top.

Signal classes are collected in the pure cook and raised by `h_write`
*after* the ring work — keeping the syscall out of the cook is what lets
the selftest assert classes on a local pts with no kernel registration.

**The ctl grammar** is whitespace-separated tokens: `+name`/`-name` over
the five flags, and `winsize <cols> <rows>` (decimal, canonical,
≤ 65535). **Two passes** — everything validates before anything applies,
so one malformed token rejects the whole write with the mode unchanged
(the tcsetattr-atomic posture). A flag change resets the assembly line
(TCSAFLUSH). A winsize write raises `tty:winch` **only if the size
actually changed** — the Linux `TIOCSWINSZ` behaviour.

## Data structures

`Pts`: the two `VecDeque` rings (`m2s`, `s2m`, 4 KiB each — the classic
tty buffer size), the termios word, a 256-byte ICANON assembly line, the
3-bit signal set, the winsize pair, the kernel `pts_id`, and **two
distinct counts**:

| count | meaning | frees the slot? |
|---|---|---|
| `refs` | fids **bound** to this pts (master, slave, **or ctl**) | yes — reaching 0 is the only free path |
| `n_master` / `n_slave` | fds **opened** on the two data endpoints | no |

`refs` is lifetime; the open counts are the EOF signal. A ctl fid holds
`refs` but is deliberately **not** an endpoint — `is_endpoint_path`
excludes `FK_CTL` and every `open_dec` site gates on it. Without that
gate an opened ctl would count toward the peer-closed EOF signal and a
ctl close would forge carrier loss.

**`slave_opened_once`** is the asymmetry. A master read before the slave
side has ever opened must *park*, not EOF — EOF means the slave is gone,
which requires it to have existed. Without the latch, an emulator
reading for the child's first output races the child's slave open and
gets a spurious 0. The master needs no such latch: the mint **is** its
open, so `n_master == 0` implies it once was 1.

`Conn` carries a 32-entry fid table and a flat `Vec<PendingRead>`.
Bounds: 8 connections, 32 fids, **16 pts pairs** — a bound rather than
headroom, since an unbounded pts table is a DoS vector.

## Concurrency

Single-threaded: one Proc, one serve loop, every 9P frame across every
session processed sequentially. The pts table needs no lock.

**That is also the I-9 argument.** A read on an empty-but-open ring
returns `WouldBlock` and parks a `PendingRead` holding its tag;
`Disp::Deferred` suppresses the reply. `poll_reads` runs at the
serve-loop top, re-attempting every parked read *before* the loop can
block in `t_poll`. A ring fills only via a client write in a serviced
frame, and every serviced frame is followed by a `poll_reads` pass — so
there is no window in which a wake could arrive unobserved.

`pending_reads` is a flat `Vec`, not a single slot: two peer threads
reading one fd both park and drain in order. Deliberately *not* the
console's single-reader discipline, and the shape netd's `net-4d F1` had
to retrofit after a single deferred slot clobbered a held reply.

`read_ready` exists so a long-parked read does not allocate a drain
buffer on every 1-second poll tick. A pure mirror of the drain's ring +
EOF logic; the drains stay authoritative.

The serve loop drops the listener from the poll set while the connection
table is full — otherwise a pending 9th connection keeps the listener
perpetually readable, the accept is skipped, and the loop busy-spins at
full CPU.

## Invariants enforced

[[inv-i20]], all four clauses:

- **Signal XOR byte** — one `continue` in the ISIG arm.
- **Byte conservation** — in the raw arm (see Seams for the cooked arm).
- **Foreground group only** — structural; there is no group parameter.
- **Drain-then-EOF, hang up once** — `ring_drain` returns `Data` while
  the ring is non-empty **regardless of the peer's closure**, and `Eof`
  only on an empty ring, so queued bytes survive a close with nothing
  needing to happen at close time.

**`HupAtMostOnce` is by construction**, and the construction is a chain
of four facts: masters are mint-only (no walk resolves `FK_MASTER`); 9P
forbids walking *from* an opened fid and `h_walk` enforces it; `h_walk`
rejects a walk to an already-bound newfid; therefore at most one master
fd per pts can ever exist, and the 1→0 edge fires at most once per pts
lifetime. Fact 3 is a two-line check that reads like protocol hygiene
and is load-bearing for a safety property — its absence in
[[sub-tapestryd]] is batch 27's finding.

[[inv-i9]] via the `poll_reads` ordering above.

## Error paths

`close_endpoint` runs at **every** opened-endpoint drop — the ordinary
clunk, a connection teardown, and a `Tversion` reset (a dying emulator
connection *is* carrier loss). On the `n_master` 1→0 edge it raises
`tty:hup` **before** the caller's unref, so the slot and its `pts_id` are
still live. A slave close is never a hup edge — POSIX gives no SIGHUP
there; the master simply reads EOF.

`fid_set` refs the new path **before** unreffing the old, so a
within-connection rebind never transits `refs == 0` and frees a pts the
same operation is about to re-reference.

`fid_clunk` removes the fid's pending reads *first*, then unrefs — so a
parked read can never survive into a freed slot.

A ctl read past its rendered length returns empty rather than an error;
a 0-count data read returns 0 at once rather than parking (an empty
drain is `Data(0)`, which would otherwise look like `WouldBlock`
forever). A mint failure propagates the kernel errno, since `T_E_*` is
the POSIX value is the 9P ecode.

## Performance

Terminal-scale, and deliberately so: 4 KiB per direction, a 32 KiB
negotiated msize matching the kernel client's proposal so a full data
frame crosses in one op, a 256-byte canonical line. Reads are
one-`t_read`-per-serviced-event with a partial frame waiting for the
next readable event. `read_ready` keeps a parked read allocation-free.
The global FRAME coalescing that a compositor needs has no analogue here
— a pts has no pacing signal.

## Prosecution

- The **ECHO-off** guarantee is the one to re-derive on any ldisc
  change: every echo staging must still pass `echo()`, and `echo()` must
  still return early. A second staging path would silently unmask a
  password.
- **`SignalXorByte`** breaks if any ISIG arm forgets its `continue`.
- The **ctl atomicity** breaks if validation and application are ever
  interleaved.
- `is_endpoint_path` must gate **every** `open_dec` site.
- The `refs`-before-unref order in `fid_set` must survive any fid-table
  edit.

## Seams

- **Any Proc that can name a live pts can read, inject into, and
  re-termios it.** The mode is `0666` SYSTEM-owned, so the kernel's
  `perm_check` passes any principal as *other* with rw, and the pts
  registry gates only the controlling-terminal syscalls — never
  slave/ctl open, read, or write. Inert at single-session v1.0; live
  under concurrent multi-user. The code states this honestly at
  `FILE_RW`, and the fix (per-pts owner + `0600`) needs either
  per-session submounts or per-op principal forwarding, because the
  shared kernel mount arrives as SYSTEM.
- **The cooked arm drops where the raw arm back-pressures.** A byte past
  `LINE_MAX` is consumed and discarded un-echoed; a line flush into a
  full `m2s` discards the tail (`let _ = ring_push(...)` — the result is
  deliberately ignored); `echo()` drops on full unconditionally. All
  three are the classic tty-overrun semantic that the kernel console
  reference also carries. They matter because the model does not cover
  them — see [[spec-pty]] and task #48.
- The POSIX `/dev/ptmx` master path is a PTY-3 concern; at v1.0 the
  clone file is reached as `/dev/pts/ptmx`, because a symlink or
  file-mount would need union-mount walking.

## Caveats

- Readdir ordinals are positions in a live table: a mint or free between
  two paginated `Treaddir` calls shifts them, so an entry can be skipped
  or repeated. The netd precedent dispositions this as a benign listing
  artifact.
- `parse_dec` rejects leading zeros — one canonical name per pts. Worth
  knowing because its tapestryd twin does not, so the two servers'
  directory namespaces differ in a way neither documents against the
  other.
- A ctl read is offset-served against a freshly rendered line, so a read
  paginated across a concurrent mode change sees two different lines.
  Bounded to 64 bytes and single-threaded, so it cannot tear mid-token,
  but the halves need not agree.

## Provenance

The PTY-2 arc: the server skeleton + rings + registration (2a), the
per-pts line discipline (2b), the termios/winsize ctl (2c), teardown and
SIGHUP (2d), the focused audit (2e). Swept into the vault by
[[chg-2026-08-02-server-sweeps]], which mints [[inv-i20]] and
[[spec-pty]].

## Tests

`server::selftest()` runs before the listener posts, and a failure gates
the boot. It drives the rings and the whole ldisc truth table against a
local pts with no kernel registration — the netd `echo_e2e` pattern,
deterministic and mount-independent. Its legs: the raw battery (FIFO
order, transparency, no echo), the cooked battery (ICRNL+flush+ONLCR
echo in one stroke, assembly-holds-until-NL, erase, erase-on-empty, the
ISIG trio, the same-class-run overflow, ECHO-off no-leak, raw+ISIG,
output ONLCR, line overflow), the ctl battery (render, atomic reject,
winsize-changed-only, mixed apply, TCSAFLUSH, the walk grammar), and the
teardown tail (drain-then-EOF, the hup edge fires once, a slave close is
not an edge, free-on-last-unref).

The 9P layer and the kernel registration are proven separately by the
in-guest `/dev/pts` boot probe and the `pty-probe` openpty E2E, which
drives a live controlling session.

## Referenced by

[[spec-pty]] · [[inv-i20]] · [[sub-tapestryd]] · [[sub-pouch-tty]] ·
[[moc-userspace]].
