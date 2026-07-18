# 136 — ptyfs: the pseudoterminal server + /dev/pts (PTY-2)

**Status**: as-built at the PTY-2 arc close (2a `d567ed57`/`4b26a159`, 2b
`79f885e9`, 2c `6f7ca63e`, 2d `a946012b`, 2e). Kernel-side companion:
`docs/reference/135-pty-kernel.md` (the PTY-1 arc: sessions/pgrps, the pts
registry, the tty signal seam, the job-control stop). Design authority:
`docs/PTY-DESIGN.md` (§5 carries the per-sub-chunk as-built notes). The
prosecution row: `ARCHITECTURE.md §25.4` "ptyfs".

## Purpose

`/sbin/ptyfs` is the pseudoterminal server: a native libthyla-rs, device-less
`/srv` 9P server (the corvus/netd grain) owning the pts pairs — the two byte
rings, the per-pts line discipline, the per-pts termios word + winsize, and
the teardown semantics. It realizes **I-20's data path**; the kernel owns the
security-routing half (sessions/pgrps/controlling terminal — PTY-1). ptyfs's
sole signal authority is the pts-scoped `SYS_TTY_SIGNAL`: it names only
`(pts_id, class)`; the kernel resolves pts → ct_sid → fg_pgid (the I-1/I-22
bound — ptyfs can never name a process group).

joey spawns it corvus-direct (`T_SPAWN_PERM_MAY_POST_SERVICE`, no fds/caps),
gates on a bounded liveness connect, and MREPL-mounts its tree at `/dev/pts`
(the devdev `DEV_KIND_PTS` synthetic mount-stub child — reference/109). All
of spawn/liveness/mount/probes are **boot-fatal**: ptyfs has no
hardware/external dependency, so failure to come up is always a defect.

## The tree (Linux-devpts shape + the suffix-ctl idiom)

| Path | qid.path | What |
|---|---|---|
| `/` (mounted at `/dev/pts`) | 0 (the attach root) | the devpts directory: `{ptmx, <n>, <n>ctl}` |
| `/dev/pts/ptmx` | 1 | the clone file: open = mint pts N, rebind the fid onto the MASTER endpoint |
| `/dev/pts/<n>` | `PTS_FLAG\|N<<8\|2` | the slave byte channel (resolves only while the slot is live) |
| `/dev/pts/<n>ctl` | `PTS_FLAG\|N<<8\|3` | the per-pts ctl (termios + winsize; the Plan 9 `eia0`/`eia0ctl` idiom) |
| (master, minted) | `PTS_FLAG\|N<<8\|1` | the master byte channel — never walkable, never in readdir |

`PTS_FLAG` = bit 40. **The qid encoding is a documented client contract**:
`t_stat.qid_path` carries the 9P qid verbatim, so
`ptsname(master_fd) = (fstat(mfd).qid_path >> 8) & 0xffffff` (guarded on
`PTS_FLAG`) — no extra file or ioctl; the PTY-3 pouch `ptsname()` implements
exactly this decode. The POSIX `/dev/ptmx` alias is a PTY-3 concern
(symlink/file-mount); the clone lives at `/dev/pts/ptmx` meanwhile.

## Data path + the line discipline (PTY-2b)

Two rings per pts (`RING_CAP` 4 KiB each): `m2s` (master→slave = INPUT) and
`s2m` (slave→master = OUTPUT + echo). Five per-pts flags (the kernel LS-8
bit values, so the ctl grammar speaks the same names): `ICANON 0x01`,
`ECHO 0x02`, `ISIG 0x04`, `ICRNL 0x08`, `ONLCR 0x10`. **A fresh pts is FULL
COOKED** (`TIO_DEFAULT` = all five — the Linux fresh-pts posture; the kernel
console's ISIG-only boot word is a console posture, not the pts one).

- **Input cook** (`master_write`, per byte, the cons.c order): ICRNL first
  (CR→NL) → ISIG (the standard trio: `0x03`→INT, `0x1c`→QUIT, `0x1a`→TSTP —
  the class is collected and the byte CONSUMED: never enqueued, never echoed
  = `pty.tla` SignalXorByte) → ICANON (erase `\b \b`; NL flushes the line
  INCLUDING its newline; a byte past `LINE_MAX`=256 drops un-echoed) → raw.
- **Output cook** (`slave_write`): ONLCR NL→CR NL, pair-atomic back-pressure
  (an expansion that doesn't fit stops BEFORE its input byte — a torn pair
  would double the CR on retry).
- **The `echo()` chokepoint**: every echo staging passes ONE `ECHO` gate (the
  LS-8b ECHO-off hard no-leak guarantee, per-pts — the password mask); echo
  rides the output transform (echo IS output, the Linux model).
- **Ring-full policy split**: the cooked flush + the echo DROP on full (tty
  input overrun — the slave has 4 KiB unread, a wedged reader; the console
  posture); the raw input + the output path BACK-PRESSURE via a short Rwrite.
- `h_write`'s master arm raises the collected classes AFTER the ring work via
  `t_tty_signal` (the syscall stays out of the pure cook, so the selftest
  asserts classes directly; `pts_id 0` = a selftest-local pts, never signals).

## The ctl (PTY-2c)

Write = the tcsetattr-atomic grammar: whitespace tokens, `+name`/`-name` over
the five flags + `winsize <cols> <rows>` (decimal ≤ 65535); ALL tokens are
validated before ANY applies — one malformed token rejects the whole write
with the mode unchanged; a flag apply resets the ICANON assembly (TCSAFLUSH).
A winsize CHANGE raises `TTY_SIG_WINCH` (iff changed — the Linux TIOCSWINSZ
behavior); the kernel routes `tty:winch` to the fg pgrp. Read = one
offset-served line: `+icanon +echo +isig +icrnl +onlcr winsize C R\n`
(never defers). A ctl fid holds the slot ref but is **NOT** an EOF-counted
endpoint (`is_endpoint_path` gates all four `open_dec` sites).

## Blocking reads (the deferred multi-waiter set)

An endpoint read on an empty-but-open ring parks a `PendingRead`
(`Disp::Deferred` — the held Rread tag); `poll_reads` at the serve-loop top
re-drains and delivers (Data or the EOF 0). A flat multi-waiter Vec (the
net-6a-1 discipline), NOT the cons single-reader. I-9 holds by
single-threadedness: a ring fills only via a serviced frame, so every parked
read is re-checked before the loop parks. Cancel paths: `Tflush` (by oldtag),
clunk (by fid), teardown/Tversion (all).

## Teardown (PTY-2d)

- **Drain-then-EOF**: `ring_drain` serves Data while non-empty regardless of
  the peer's closure; `Eof` only on empty + peer-gone.
- **The `slave_opened_once` latch**: a master read BEFORE any slave open
  PARKS (WouldBlock), never a spurious EOF — the Linux
  master-blocks-until-the-slave-comes-up semantic (EOF means the slave side
  is GONE, which requires it to have existed). The master needs no latch: it
  is born open (the mint IS its open).
- **HUP**: `open_dec` reports the `n_master` 1→0 edge; `Conn::close_endpoint`
  (the single opened-endpoint drop path: clunk / rebind / connection teardown
  / Tversion reset — a dying emulator connection IS carrier loss) raises
  `TTY_SIG_HUP` BEFORE the unref; the kernel routes `tty:hup` dual-target
  (fg pgrp + session leader). **HupAtMostOnce is by construction**: exactly
  one master fd per pts can ever exist (masters are mint-only; no walk
  resolves a master path; 9P forbids walking FROM an opened fid; a walk to an
  existing newfid is rejected). A slave close is never a hup edge. The
  mint-rollback keeps the raw `open_dec` (a failed mint is not carrier loss).
- **Free**: bound `refs` (one per fid naming the pts — master/slave/ctl) is
  the ONLY free decision; the last unref frees the slot and
  `SYS_PTY_REGISTER(FREE)`s the kernel entry (the gen guard makes a stale
  `pts_id` fail closed).

## Cacheability (load-bearing)

ptyfs answers `Twalkgetattr` (and every unknown T-message) with `E_NOSYS`
via the dispatch default → the kernel client never latches `cacheable` → the
**Larder never caches pts bytes** (attr/dentry/page all off for the session —
the netd precedent). A cached tty read would replay stale bytes; this
fail-safe posture is a prosecuted property, not an accident.

## Concurrency model

Single-threaded by construction: one Proc, one serve loop; every 9P frame
across every session is processed sequentially, so the pts table needs no
lock and the I-9 argument above holds. A future threading lift must revisit
BOTH properties (the ARCH row records this). A buggy or hostile 9P client
corrupts only its own connection's state — the kernel validates the syscall
surface and ptyfs validates frames; the fid/refcount discipline
(refs-new-before-unref-old on rebind; the netd live-slot walk property: a
stale/forged qid is unreachable) bounds cross-connection effects to the
shared pts a client legitimately names.

## Tests

- **The in-server selftest** (boot-gated; runs before the post): the ring
  battery (empty/round-trip/FIFO/raw transparency), the full ldisc truth
  table (ICRNL+flush+echo+ONLCR; assembly-holds; erase + empty-erase; the
  ISIG trio collected + not-a-byte + not-echoed; ECHO-off no-leak; raw+ISIG;
  output ONLCR; line overflow), the ctl battery (render format; atomic
  reject; winsize raise-iff-changed + band/arity rejects; mixed atomic write;
  TCSAFLUSH; the walk grammar), the teardown algebra (queued-bytes
  drain-then-EOF both directions; the hup edge fires once; slave-close
  silent; the master-read-before-slave park; free-on-last-unref).
- **The joey 2a-2 probe** (boot-fatal, every boot): the wire data path over
  the mounted `/dev/pts` — clone-mint, Treaddir, cooked lines, echo+ONLCR,
  the ptsname qid decode, the ctl `-echo` no-leak proof (no blocking read
  needed: a masked line then an unmasked one — only the second echoes),
  winsize via `t_pread`, queued-line drain-then-EOF.
- **The `/bin/pty-probe` openpty E2E** (boot-fatal): a two-role prover — the
  emulator mints + spawns itself as a session-leader child (`t_setsid` →
  slave open → `t_tty_acquire` → notes fd FIRST [self-managing]) and drives
  the signal seam live: Ctrl-C → "interrupt" observed on the child's notes
  fd, winsize → `tty:winch`, master close → `tty:hup`; plus the FIRST live
  parked deferred master read (the readiness read parks until the child's
  slave opens and writes).
- `devdev.walk_pts_dir` (the kernel mount-stub, reference/109).

## Known caveats / seams

- **PTY-3 (pouch)**: `openpty`/`forkpty`/`ptsname`/`tcsetattr` decompose onto
  this surface (the boundary-line); `/dev/ptmx` as a POSIX alias needs
  symlinks or a file-mount.
- The per-pts termios is the five-flag LS-8 word; VMIN/VTIME, additional
  c_cc chars (VERASE beyond DEL/BS, VKILL, VEOF), and per-fd (vs per-pts)
  termios are v1.x lifts.
- Echo drop-on-full can tear a CR-NL echo pair at the ring boundary
  (harmless: echo is best-effort; the data path never tears).
- `PTS_MAX` = 16 concurrent pts pairs (the #65 resource floor; ENFILE-class
  reject at mint).
