# PTY-DESIGN.md — pseudoterminals, process groups, and job control (I-20)

**Binding design — SIGNED OFF 2026-07-17** (the Fable design pass; four user
votes P1–P4 below). This is the canonical scripture for the Phase-8 PTY
master/slave surface that ARCH §23.5 sketched and LS-8 deferred. It concretizes
**I-20** (master/slave atomicity, reserved-and-unwritten until now) and lands
the kernel process-group / session / controlling-terminal apparatus job control
needs. Grounded in a full as-built substrate survey (2026-07-17) — every "exists
/ does not exist" claim is verified against the tree, not assumed.

Cross-refs: ARCH §23.5 + §23.5.1 (the LS-8 line discipline this reuses) + §28
I-20; LIFE-SUPPORT.md LS-8 (the deferred list this closes); UTOPIA-SHELL-DESIGN
§10 (the job-control consumer requirements); `specs/pty.tla` (the model, landed
with this pass); `docs/reference/111-cons.md` (the ldisc being de-globalized).

---

## 1. The four votes (P1–P4, user 2026-07-17)

- **P1 — the PTY is a USERSPACE 9P server.** `ptyfs` (a native `libthyla-rs`
  Proc) serves `/dev/ptmx` + `/dev/pts/<n>` over 9P, modeled on netd's `/net`
  clone idiom (the only working dynamic-instance-mint precedent — and it is
  userspace). This is the unanimous grain: Plan 9 (a rio window IS a pty,
  serving `/dev/cons` over a pipe), Fuchsia `ptysvc`, Genode `terminal`, Hurd
  `term` — all userspace; ARCH §23.5 already said "a 9P server." The I-27
  trusted-path argument that forced the *console* cooking into the kernel does
  NOT apply — a PTY slave hosts tmux/vim/ssh, never the login prompt (that stays
  `/dev/cons`, kernel).
- **P2 — full POSIX sessions + process groups.** `sid` + `pgid` on `struct
  Proc`; `setsid`/`setpgid`/`getpgid`/`getsid`. Sessions (not just groups) carry
  controlling-terminal acquisition, `SIGHUP`-on-leader-death, and orphaned-pgrp
  handling — the parts a foreground-only model silently omits, and what Pouch
  bash/tmux/ssh need 1:1.
- **P3 — Ctrl-Z generalizes the existing debug stopped-state.** The Go-debugger
  arc built an audited fully-stopped thread-state (`debug_rendez` + stop/resume
  under `g_proc_table_lock`, `specs/debug_stop.tla`, I-39). Job-control stop gets
  a second, non-`CAP_DEBUG` entry (SIGTSTP self-stops the foreground group,
  SIGCONT resumes) rather than a duplicate mechanism — audit-bearing: it must not
  weaken I-39.
- **P4 — spec-first RE-ENABLED for I-20** (`specs/pty.tla`, the 7th instance of
  re-enable point (a), after Tapestry's T-1): the master/slave atomicity is
  literally the concurrency class the discipline exists for; the model lands
  before the impl.

---

## 2. The as-built ground truth (survey 2026-07-17)

Reusable (LS-8 + the tree already have it):
- The **line-discipline cooking** — the five-flag truth table
  (`ICANON/ECHO/ISIG/ICRNL/ONLCR`) + the cooked line buffer + echo/erase +
  `ONLCR` (`kernel/cons.c::cons_rx_input`), complete + unit-tested. **But
  `g_cons`-WELDED** (a single file-scope `static struct cons_input`); PTY must
  de-globalize it into an instantiable **ldisc** unit.
- The **consctl parse/render grammar** (`cons_set_mode_cmd`/`cons_render_mode`) —
  the `+name`/`-name` atomic-multi-flag surface, the intended `tcsetattr`↔string
  seam; reusable per-pts.
- The **poll mechanism** — per-object `poll_waiter_list` + register-then-observe
  (`poll.h`); cleanly backs two independent pollable endpoints (master + slave).
  A RAM-backed PTY runs in **process context**, so it does NOT inherit the cons
  IRQ→`console_mgr` deferred-wake relay (`cons_poll.tla`'s complexity is
  cons-only).
- The **interrupt-note path** (`ISIG`→`interrupt`) — the Ctrl-C delivery
  primitive, retargeted from the single `g_console_owner` to a foreground pgrp.
- The **dynamic-instance qid-encoding** — netd's clone idiom
  (`CONN_FLAG|proto|N|filekind`; the kernel 9P client accepts the clone-rebind
  `Rlopen` qid) is the `/dev/ptmx`→pts blueprint.

New machinery (does not exist):
- **No process groups / sessions / controlling terminal.** `struct Proc` has no
  `pgid`/`sid`; the Ctrl-C target is a single global `g_console_owner` Proc.
- **No job-control stopped thread-state** (only the I-39 debugger stop).
- **No per-fd termios** (one global `termios` word on `g_cons`).
- **No `winch`/`susp`/`cont`/`quit`/`hup` notes** (the set is closed to
  `interrupt`/`kill`/`pipe`/`child_exit`/`snare:*`).
- **No kernel Dev that mints dynamic per-open instances by qid** (the precedent
  is userspace-only — which P1 embraces).

First real consumer: NONE in-tree today (Aurora is cons-level per AURORA §10;
Utopia uses `/dev/cons` directly). The first consumer is a future terminal
multiplexer or the sshd server side (both Pouch, both later). So PTY is built to
the POSIX shape those need, proven by a **synthetic `openpty` round-trip E2E**
(the netd-loopback analog), not gated on a live multiplexer.

---

## 3. Architecture — the kernel/userspace seam (the crux)

The split follows the trust boundary: **the security-sensitive signal routing is
kernel; the byte/cooking engine is the userspace server.**

```
   Terminal emulator / tmux / sshd  (master fd)
        |  9P read/write over /dev/ptmx-minted master
        v
   ptyfs (userspace 9P server, native libthyla-rs) ── owns ──►
        the M2S + S2M rings, the per-pts LDISC (cooking + per-pts termios),
        the per-pts winsize, the qid-encoded /dev/pts/<n> presentation
        |  its ONLY signal ask: SYS_TTY_SIGNAL(pts_fd, class)
        |  -- "a signal-class event (INT/QUIT/TSTP/WINCH) occurred on MY pts N"
        v
   KERNEL ── owns ──► the session/pgrp state (sid, pgid on Proc),
        the controlling-terminal binding (pts N ↔ session S ↔ fg_pgid),
        the note-to-PGRP delivery, the generalized stopped-state (Ctrl-Z)
        v
   The child under its controlling terminal  (slave fd = /dev/pts/<n>)
```

**The signal seam is the load-bearing safety property.** The server can NEVER
name a process group — it can only report "a signal-class event occurred on a
pts I serve." `SYS_TTY_SIGNAL(pts_fd, class)` is gated: the caller must be the
registered `ptyfs` server owning that pts (the connection that minted it), and
the kernel does the routing — `pts N → its controlling session → that session's
fg_pgid → deliver the note to that pgrp`. So a compromised/buggy ptyfs can, at
worst, signal the foreground groups of the terminals it *already* serves (its own
clients) — it cannot reach an arbitrary Proc or pgrp. This is the pty analog of
netd owning only its `/net` flows (I-1/I-5): authority bounded to the resource
the server was granted. (Rejected: a generic `SYS_PGRP_SIGNAL(pgid, sig)` the
server calls — that hands a userspace Proc the authority to signal any pgrp, the
ambient-authority hole I-22 forbids.)

**Controlling-terminal acquisition** (the POSIX dance, kernel-side): a session
leader (`setsid` → new session, no controlling tty) that opens a pts slave with
neither `O_NOCTTY` nor an existing controlling tty acquires pts N as its
controlling terminal — the kernel binds `session S ↔ pts N` and sets `fg_pgid =
the leader's pgid`. `tcsetpgrp(pts_fd, pgid)` updates `fg_pgid` (kernel-checked:
the caller is in session S and pgid is a pgrp in S); `tcgetpgrp` reads it. These
two are **pts `ctl` operations the kernel mediates** (the Plan 9 "it's a file"
idiom — no new `tcsetpgrp` syscall; the Pouch boundary-line maps
`tcsetpgrp(fd)` → the pts ctl write, like `tcsetattr` → consctl), with the
foreground-pgid held in KERNEL controlling-terminal state (not the server), so
the signal routing needs no server round-trip and no server-held security state.

---

## 4. The kernel surface (new; PTY-1)

**`struct Proc` fields**: `u32 sid` (session id = the session-leader's pid) +
`u32 pgid` (process-group id = the group-leader's pid). Inherited across
`rfork`/spawn from the parent (a child joins its parent's session + group);
`_Static_assert`-pinned offsets.

**Syscalls** (the POSIX pgrp/session membership — kernel because `wait()` +
signal routing need it):
- `SYS_SETSID` → new session (caller becomes leader: `sid = pgid = pid`), no
  controlling terminal; fails if the caller is already a pgrp leader (POSIX).
- `SYS_SETPGID(pid, pgid)` → move a Proc into a pgrp in its session (the
  self-or-child, same-session, not-a-session-leader POSIX rules).
- `SYS_GETPGID(pid)` / `SYS_GETSID(pid)` → reads.
- `tcsetpgrp`/`tcgetpgrp` are **pts ctl ops** (§3), not syscalls.

**Note-to-pgrp delivery**: generalize `proc_console_post_interrupt` (single
`g_console_owner`) to `notes_post_pgrp(pgid, note)` — deliver a note to every
Proc in a pgrp, under `g_proc_table_lock`, exactly-once per Proc (the I-19
ordering + N-2 consumed-exactly-once composed across the group). The
`g_console_owner` path stays for the physical console (which has no pts); the pts
path routes through the controlling-terminal → fg_pgid lookup.

**New note names + bits** (append-only, ABI — the `snare:*`/errno registry
discipline, ERRORS.md): `tty:winch` (SIGWINCH — resize), `tty:susp` (SIGTSTP —
the default-stop disposition), `cont` (SIGCONT — resume), `tty:quit` (SIGQUIT),
`hup` (SIGHUP — controlling-terminal hangup / session-leader death). `interrupt`
(SIGINT) already exists. Default dispositions: `interrupt`/`tty:quit` terminate
(LS-5 disposition family); `tty:susp` STOPS; `cont` resumes; `hup` terminates;
`tty:winch` is ignored-by-default (a resize notification a handler opts into).

**The generalized stopped-state** (P3 — audit-bearing, I-39): the debug arc's
fully-stopped thread-state (a thread parked on a rendez, resumed under
`g_proc_table_lock`) gets a **second, non-`CAP_DEBUG` entry**: a `tty:susp` with
the default STOP disposition parks the target Proc's threads (the whole
foreground group's Procs) in the job-control stopped state; a `cont` note resumes
them. The `CAP_DEBUG` gate stays on the *debugger* entry (I-39's `CTL_VERB_STOP`
via `/proc/<pid>/ctl`); the job-control entry's authority is the tty signal seam
(§3) — you can only Ctrl-Z the fg group of a terminal you serve. **The
composition obligation (prosecute hard): a job-control stop and a debug stop of
the same thread must not corrupt the shared stopped-state** (double-stop /
stop-vs-resume races / the reap-vs-stop interaction) — the audit re-validates
I-39 under the new entry, and `debug_stop.tla` extends (or `pty.tla` models the
interaction) to cover it.

---

## 5. The userspace `ptyfs` server (PTY-2)

A native `libthyla-rs` Proc, warden-manifested `lifecycle = persistent` (the netd
precedent), posting `/srv/ptyfs`; joey mounts `/dev/ptmx` + `/dev/pts` from it.

- **`/dev/ptmx` open → mint pts N** (the netd clone idiom): allocate the pts
  slot, its M2S + S2M rings, its per-pts `Ldisc` (the de-globalized LS-8 cooking
  + a per-pts `termios` word, `CONS_ICANON`-default), its winsize, and return the
  MASTER endpoint (the opened fid rebinds onto the master, qid-encoding N). The
  slave appears at `/dev/pts/<n>`.
- **The master/slave data path** (two rings; I-20):
  - master write → the **input ldisc** cooks (ICANON line-assembly, ICRNL, ISIG
    → a `SYS_TTY_SIGNAL` + NO byte when ISIG set, ECHO → write the echo into
    S2M so the emulator sees it) → cooked bytes into the **M2S** ring → slave
    read drains.
  - slave write → the **output ldisc** (ONLCR) → the **S2M** ring → master read
    drains.
  - ECHO writes into S2M (the emulator on the master sees what it typed); ECHO-off
    is the hard no-leak guarantee (LS-8b), now per-pts.
- **per-pts termios**: the de-globalized `Ldisc` struct holds the 5-flag word;
  `tcsetattr`/`tcgetattr` ↔ the pts `ctl` `+name`/`-name` grammar (the LS-8b
  consctl surface, per-pts). Independent per pts (the whole point vs the global
  console).
- **winsize + SIGWINCH**: `TIOCSWINSZ` (a pts ctl write) sets the per-pts winsize
  AND raises `SYS_TTY_SIGNAL(pts, WINCH)` → the kernel routes `tty:winch` to the
  fg pgrp; `TIOCGWINSZ` reads it.
- **isatty**: the pts presents as a terminal (the Pouch boundary-line's
  `isatty()` returns true for a pts fd; a regular file/pipe returns false).
- **teardown** (I-20): master close (last master fd) → the slave's reads return
  EOF + the kernel raises `hup` to the controlling session (SIGHUP); slave close
  (last slave fd) → the master's reads return EOF. Quiesce: in-flight bytes in
  the rings are delivered before EOF (no byte lost at teardown).

---

## 6. I-20 formalized + the spec (P4)

**I-20 (master/slave atomicity), the binding statement**: the master/slave pair
+ its line discipline compose a byte channel in which (a) every byte written to
one end appears at the other end's read exactly once, in order, transformed by
the ldisc, none lost / torn / duplicated (incl. under concurrent master-write +
slave-read on different CPUs); (b) a cooked signal-class control char raises
exactly the one signal to exactly the controlling terminal's foreground process
group, exactly once, and (with ISIG set) is NOT also delivered as a byte; (c) no
wake is lost between a write producing data and a blocked read on the other end
(the I-9 family, per-endpoint); (d) teardown (either close) delivers every
in-flight byte, then EOF, and strands no reader, and master-close raises SIGHUP
to the session exactly once.

**`specs/pty.tla`** (model-first, TLC-green before the ptyfs impl):
- State: the two rings (bounded), each endpoint's open-count + blocked-reader,
  the per-pts termios flags (ISIG/ICANON/ECHO relevant to atomicity), the ldisc
  line buffer, the fg_pgid + the delivered-signal multiset, the stopped-state of
  the fg group, teardown.
- Invariants: `NoByteLostOrDuped` (M2S/S2M conservation across the cook),
  `SignalExactlyOnceToFg` (a cooked ISIG char → exactly-one note to fg_pgid, none
  to others; not-also-a-byte), `NoLostWake` (I-9 per endpoint),
  `TeardownDrainsThenEof` + `HupExactlyOnce`, and `StopCompatI39` (a job-control
  stop composes with a debug stop with no double-stop / lost-resume).
- Liveness: `EventuallyDelivered` (a written byte eventually readable),
  `EventuallyResumed` (a SIGCONT after SIGTSTP eventually runs the group).
- Buggy cfgs: `signal_also_byte` (ISIG char double-delivered), `lost_teardown_byte`
  (EOF before drain), `double_stop` (job + debug stop corrupt the shared state),
  `signal_wrong_pgrp` (delivery escapes the fg group — the seam violation).

---

## 7. The Pouch boundary-line (PTY-3)

The POSIX porters' surface (ABI names stay standard — `isatty`, `/dev/ptmx`,
`struct termios` — that is the compat contract):
- `openpty`/`forkpty`/`posix_openpt` → open `/dev/ptmx` (mint), read the pts
  name.
- `tcgetattr`/`tcsetattr(struct termios)` → the boundary-line decomposes into the
  per-flag pts `ctl` `+name`/`-name` writes / reads (the LS-8b mapping, per-pts).
- `tcsetpgrp`/`tcgetpgrp` → the pts `ctl` foreground-pgrp op (§3).
- `setsid`/`setpgid`/`getpgid`/`getsid` → the §4 syscalls 1:1.
- `TIOCSWINSZ`/`TIOCGWINSZ` → the pts `ctl` winsize op (§5).
- `isatty`/`ttyname` → a pts-fd characteristic check.
- SIGINT/SIGQUIT/SIGTSTP/SIGCONT/SIGWINCH/SIGHUP → the §4 notes, mapped by the
  existing notes↔signals boundary-line (ARCH §23.6). `kill(-pgrp, sig)` →
  `notes_post_pgrp` via the standard note path (a Proc killing its OWN pgrp is
  the ordinary note authority; the tty-signal seam is only for the server).

Covers, when they arrive via Pouch: bash job control, tmux/screen, ssh
(client + the sshd pty side), vim/less/`script`, any `isatty()`-sniffer.

---

## 8. Invariants + audit-trigger surface

- **I-20** takes its enforced §28 number when PTY-2 lands (reserved-then-enforced,
  the T-1 precedent), validated by `specs/pty.tla` + the focused audit.
- **No new privilege invariant** — the tty-signal seam (§3) composes I-1/I-22
  (authority bounded to the served pts; no ambient pgrp-signal), and the
  generalized stop composes **I-39** (the composition obligation is an audit
  gate, not a new invariant). The pgrp/session syscalls compose I-2 (inherited,
  monotone) — a child cannot escape its session's confinement via setsid to reach
  another's controlling terminal (setsid drops the controlling tty, POSIX).
- **Audit-trigger rows** (join CLAUDE.md + ARCH §25.4 at the landing sub-chunk):
  PTY-1 the kernel pgrp/session/signal-to-pgrp/generalized-stop surface
  (death-path + wait/wake + I-39 lineage — prosecute hard); PTY-2 the ptyfs
  server + the master/slave rings + the de-globalized ldisc + the `SYS_TTY_SIGNAL`
  gate; PTY-3 the pouch boundary-line. The generalized-stop touches the
  most-bug-prone lineage in the tree (the #788…#926 death-path family + I-39) —
  the audit re-validates I-39 under the new non-CAP_DEBUG entry.

## 9. The build arc — PTY-1..PTY-4

| # | Chunk | Contents | Gate |
|---|---|---|---|
| PTY-1 | Kernel primitives + spec | `sid`/`pgid` on Proc + `setsid`/`setpgid`/`getpgid`/`getsid`; `notes_post_pgrp` + the new note names/bits; the controlling-terminal kernel state + the `SYS_TTY_SIGNAL` gate; the generalized (non-CAP_DEBUG) stopped-state; `specs/pty.tla` model-first + the focused audit (I-39 re-validation) | `pty.tla` TLC-green + audit close + kernel unit tests (pgrp membership, pgrp-signal exactly-once, stop/cont, the seam gate rejects a non-owner) + SMP gate |
| PTY-2 | The `ptyfs` server | de-globalize the LS-8 ldisc into a per-pts `Ldisc`; the `/dev/ptmx`→pts clone mint; the M2S/S2M rings + cooking; per-pts termios via the pts `ctl`; winsize + SIGWINCH; teardown/EOF/SIGHUP; the synthetic `openpty` round-trip E2E (the netd-loopback analog) | E2E: a master/slave round-trip cooks + echoes + raises SIGINT to the fg group + resizes + tears down cleanly; boot OK; SMP gate |
| PTY-3 | The Pouch boundary-line | `openpty`/`forkpty`/`tcsetattr`/`tcsetpgrp`/`TIOCSWINSZ`/`isatty` + setsid/setpgid + the signal mappings (§7) | a Pouch `openpty`+`forkpty`+job-control control-surface probe |
| PTY-4 | The first real consumer + close | a Pouch `tmux` OR the sshd pty side (whichever the roadmap reaches first) hosting a shell under job control; the focused audit close; the interactive LS-CI PTY scenario | job control works end-to-end over a real multiplexer; audit clean |

The v1.0-rc textual OS does NOT depend on PTY (like Halcyon/Tapestry, it is
additive — Aurora + Utopia run cons-level). PTY unblocks the Pouch terminal
ecosystem (tmux/ssh/editors under job control) and is a Halcyon dependency
(§11.6) — Halcyon hosts Aurora terminals + arbitrary programs, which want real
PTYs.

## 10. Naming (thematic proposals, held per the CLAUDE.md discipline)

The POSIX ABI names stay standard (`/dev/ptmx`, `/dev/pts`, `termios`, `isatty` —
the compat contract; renaming them breaks the porters). Held thematic proposals
for the Thylacine-authored pieces, for the user's call (not unilaterally
renamed): the server `ptyfs` → a dasyurid sibling (e.g. **`quoll`** — a smaller
marsupial cousin, a fitting "stand-in terminal" server); the process-group →
session grouping could carry a marsupial-aggregation word (**`mob`**, a group of
kangaroos) in internal identifiers where it adds color without obscuring the
POSIX mapping. Descriptive names ship until the user rules; the ABI surface never
changes.
