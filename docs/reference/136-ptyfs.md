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
exactly this decode (via its `TIOCGPTN` dispatcher arm). The POSIX
`/dev/ptmx` alias resolved at PTY-3 as a **pouch path redirect** (musl's
`posix_openpt`/`openpty` open `/dev/pts/ptmx` — the devpts-NATIVE node
Linux also has; a real `/dev/ptmx` symlink stays G11-blocked pre-symlinks).

**Getattr mode posture (PTY-3)**: ptmx + master + slave report
`S_IFCHR|0666` (`CHR_RW` — a pts node IS a character device, the Linux
model); the per-pts ctl stays `S_IFREG|0666` (a control file, not a
terminal). The mode flows VERBATIM through the kernel
(`t_stat_from_p9_attr` passes `attr->mode` when the valid bit is set) to
the pouch `st_mode`, making `S_ISCHR` + the qid decode the pouch
is-a-tty discriminator — load-bearing against qid-shape collisions
(netd's `/net` qids also use bit 40 but report `S_IFREG`). The `S_IFMT`
bits sit above the 9 rwx bits, so the kernel dev9p perm_check/X-search
are unaffected. Pinned end-to-end by the joey (e2b) probe leg (a kernel
mode-masking regression or a posture revert both fail the boot).

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
BOTH properties (the ARCH row records this). A buggy or malformed 9P frame
corrupts nothing outside its own connection — the kernel validates the
syscall surface, ptyfs validates frames (every `unwrap`/index guarded;
malformed → `Rlerror`, never a panic), and the fid/refcount discipline
(refs-new-before-unref-old on rebind; the netd live-slot walk property: a
stale/forged qid is unreachable) is sound.

## Access control (the honest v1.0 posture — a known gap)

**The pts registry gates only the controlling-terminal syscalls**
(`SYS_TTY_ACQUIRE`/`SET_FG`/`CONT`), NOT slave/ctl `open`/`read`/`write`. The
*only* gate on slave and ctl I/O is the file mode — which is **0666
SYSTEM-owned** (`FILE_RW`), so the kernel dev9p `perm_check` passes any
principal as "other" with rw. The `Ptys` table is global across connections
and `walk_child` resolves any live pts's `<n>`/`<n>ctl` for any connection.
So on the shared `/dev/pts` mount, **any Proc that can name a live pts can
read its input, inject output, flip its termios, or WINCH-spam its fg pgrp**
— an **I-1 isolation gap**. It is inert at single-session v1.0 (one
interactive console session) but goes live under A-5b concurrent multi-user;
it is the same shared-mount world-reachability posture `/net` flows carry.

The proper fix (task #13, pre-A-5b-multi-user) is the Unix pts model —
per-pts owner + mode 0600/0620 so the kernel gate denies a non-owner — which
needs either per-session `/dev/pts` submounts or per-op principal forwarding,
because on the *shared* kernel dev9p mount every pts arrives on the one
kernel-client connection whose peer principal is SYSTEM (ptyfs cannot see
which user opened ptmx via SO_PEERCRED). It is a design fork for that chunk,
not a cheap patch. ptyfs itself never names a process group — its sole
signal authority is the pts-scoped `SYS_TTY_SIGNAL` (I-1/I-22, prosecuted at
PTY-1); this gap is the DAC on the byte channels, orthogonal to the signal
seam.

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

### PTY-4 job-control gaps (surfaced by the first real consumer)

PTY-4 (`ut` job control over a pts, `docs/PTY-DESIGN.md` §14) exercises the
whole stop/report/resume machinery and proves it end-to-end for a foreground
job that holds **no** outstanding terminal read (`sleep`): `^Z` stops it, the
`WAIT_UNTRACED` report is delivered (once the PTY-1e syscall-gate fix admits
the flag -- `8f1cb8a2`), `jobs` lists it Stopped, `fg` resumes it via
`SYS_TTY_CONT` and it runs to a clean prompt exit. Two gaps remain, both real
and both PTY-4 follow-ups:

- **No foreground-read arbitration (TTIN).** A v1.0 pts serves every slave
  read the moment data arrives, with no check of the reader's process group
  vs the terminal's foreground group. So a stopped foreground job that was
  blocked in a terminal READ (e.g. `cat`) keeps its dev9p `Tread` outstanding
  at ptyfs; when the shell reclaims the terminal and the user types the next
  command, ptyfs delivers those bytes to the stopped job's stale read instead
  of the shell -- the shell never sees its input. Interactive `cat`-under-`^Z`
  therefore does not work at v1.0. The POSIX fix is TTIN: a slave read from a
  non-foreground-group process is suspended (or `SIGTTIN`'d) rather than
  served. This wants the reader's pgrp at the read gate -- a kernel check
  (`SYS_READ` on a pts slave, caller pgrp vs the pts `fg_pgid` via the
  registry), since ptyfs (a 9P server) has no pgrp per `Tread`.

- **Resume-then-re-stop -- ROOT-CAUSED + FIXED at PTY-4e (task #19).** The
  "second `^Z` after `fg` never re-stops" report was NOT a signal-delivery
  gap. The stop cascade (`proc_stop_wake_sleepers_locked`) reused the DEATH
  cascade's `torpor_wake_all_for_proc`, which fabricates a COMPLETED wait
  (`awoken = 1`) -- immaterial for a dying Proc (#811, that wake's charter),
  but a job-stopped Proc SURVIVES: the fabricated `TORPOR_OK` rode back to
  EL0 at resume, so a torpor-timed sleep (`libthyla_rs::time::sleep` ->
  `/bin/sleep`) "finished" the instant `fg` resumed it -- the job exited,
  `fg` returned Done + restored the terminal, and the second `^Z` correctly
  found no foreground job (routed to the self-managing shell as a note; no
  `proc_job_stop_one` -- exactly the observed KDBG signature). FIXED
  two-layer: (a) the stop cascade now uses the NON-COMPLETING
  `torpor_stop_wake_all_for_proc` (wakes the rendez, leaves `awoken` clear:
  the woken waiter's tsleep re-loop takes the 8c-2 stop detour, parks with
  the wait preserved, and re-registers its original deadline on resume --
  parks-and-reparks made total over torpor; a real wake during the park
  still delivers, the waiter stays bucket-linked); (b) `time::sleep`
  re-sleeps the remainder on a spurious `Woken` (the futex contract its
  "Woken is unreachable" comment violated). Pinned by
  `proc.job_stop_preserves_torpor_wait` (stop -> parked, wait intact; cont
  -> RE-SLEEPS, wait still intact -- the pre-fix code completes it right
  there; only a real wake finishes it), `proc.job_stop_recycle`, and the
  jc-probe re-stop/bg/fg-interrupt ladder.

The remaining gap (TTIN) needs a new kernel read-gate mechanism (task #18,
signoff) and is the natural PTY-4 continuation. The PTY-4 boot E2E
(`/bin/jc-probe`) and the interactive `pty-4.exp` scenario both drive `sleep`
(no terminal read) so they exercise the full stop / re-stop / bg / fg /
interrupt cycle without depending on TTIN.
