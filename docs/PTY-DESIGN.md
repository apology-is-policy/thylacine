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

**The kernel pts kobject is what makes the seam realizable** (round-1 F1/F2 —
the load-bearing correction). The seam needs a kernel-side pts identity that BOTH
the server's `SYS_TTY_SIGNAL` and the slave's controlling-terminal acquisition +
`tcsetpgrp` name — otherwise the kernel-owned `fg_pgid` has no write path (the
pts `ctl` file is an OPAQUE 9P Twrite to the userspace server; the kernel is only
the 9P transport and never inspects it — the earlier "`tcsetpgrp` → a pts ctl
write, like `tcsetattr` → consctl" analogy was FALSE: `consctl` is a *kernel* Dev
[`devdev.c`] the kernel naturally mediates, a pts ctl is not). So:

- **`/dev/ptmx` open mints a kernel `KObj_Pts`** (a small kernel object, gen-
  stamped — §11.11 F11) alongside the server's pts. The server learns its
  `kernel_pts_id` (a `SYS_PTY_REGISTER(master_fd) → kernel_pts_id`, or the mint is
  kernel-mediated at ptmx-open); the slave's controlling-terminal acquisition
  threads the SAME `kernel_pts_id`. The kobject holds the controlling-terminal
  binding `(session S, fg_pgid, gen)` — kernel state, never server-held.
- **`tcsetpgrp`/`tcgetpgrp` + controlling-terminal acquisition are KERNEL
  syscalls keyed on `kernel_pts_id`** — NOT server-served pts ctl writes. Only
  `termios`/`winsize` stay on the server-served pts ctl (they carry no security
  routing). This keeps P1 intact (the server still owns the rings + cooking +
  termios + winsize — the byte/cooking engine; only the security-routing triple
  is kernel).

**The signal seam is the load-bearing safety property.** The server can NEVER
name a process group — it can only report "a signal-class event occurred on a
pts I serve." `SYS_TTY_SIGNAL(kernel_pts_id, gen, class)` is gated: the caller
must be the registered `ptyfs` server owning that pts, and the kernel does the
routing — `KObj_Pts → its controlling session → that session's fg_pgid → deliver
the note to that pgrp`. So a compromised/buggy ptyfs can, at worst, signal the
foreground groups of the terminals it *already* serves — it cannot reach an
arbitrary Proc or pgrp. The pty analog of netd owning only its `/net` flows
(I-1/I-5). **The seam is NOT the only path to a STOP/CONT/HUP disposition, though**
— those notes are kernel-synthetic-only (§4, round-1 F4), so no ordinary
`SYS_POSTNOTE` reaches them. (Rejected: a generic `SYS_PGRP_SIGNAL(pgid, sig)`
the server calls — ambient authority, I-22.)

**Controlling-terminal acquisition** (the POSIX dance, KERNEL syscall on
`kernel_pts_id`): a session leader (`setsid` → new session, no controlling tty)
that opens a pts slave with neither `O_NOCTTY` nor an existing controlling tty,
**AND where that pts is not already any other session's controlling terminal**
(round-1 F7 — the anti-steal guard; a second such open inherits, never steals;
explicit `TIOCSCTTY` stealing is unbuilt/fail-closed), acquires it: the kernel
binds `session S ↔ KObj_Pts` and sets `fg_pgid = the leader's pgid`.
`tcsetpgrp(pts_fd, pgid)` updates `fg_pgid` (kernel-checked: the caller is in
session S and pgid is a pgrp in S); `tcgetpgrp` reads it — both KERNEL syscalls
resolving the pts fd → `KObj_Pts`, never a server round-trip and never
server-held security state.

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
- `tcsetpgrp`/`tcgetpgrp` + controlling-terminal acquisition are **KERNEL
  syscalls keyed on `KObj_Pts`** (§3, round-1 F1/F2 — NOT server-served ctl
  writes).
- **`SYS_WAIT_PID` gains `WUNTRACED`/`WCONTINUED` + the pgrp selectors**
  (`want_pid == 0` = my pgrp, `< -1` = a named pgrp) — the `wait_pid_for`
  extension (round-1 F5). WITHOUT this a shell cannot detect a `^Z`-stopped
  child (`waitpid` never returns on a stop), so job control is non-functional
  and the PTY-4 gate cannot pass; a stopped/continued child becomes
  waitpid-reportable. **Pulled forward into PTY-1** (a real current-arc
  dependency, not a seam).

**Note-to-pgrp delivery**: generalize `proc_console_post_interrupt` (single
`g_console_owner`) to `notes_post_pgrp(pgid, note)` — deliver a note to every
Proc in a pgrp, under `g_proc_table_lock`, exactly-once per Proc (the I-19
ordering + N-2 consumed-exactly-once composed across the group). The
`g_console_owner` path stays for the physical console (which has no pts); the pts
path routes through the controlling-terminal → fg_pgid lookup.

**New note names + bits** (append-only, ABI — the `snare:*`/errno registry
discipline, ERRORS.md): `tty:winch` (SIGWINCH — resize), `tty:susp` (SIGTSTP —
the default-stop disposition), `tty:cont` (SIGCONT — resume), `tty:quit`
(SIGQUIT), `tty:hup` (SIGHUP — controlling-terminal hangup / session-leader
death). `interrupt` (SIGINT) already exists. Default dispositions:
`interrupt`/`tty:quit` terminate (LS-5 family); `tty:susp` STOPS; `tty:cont`
resumes; `tty:hup` terminates; `tty:winch` is ignored-by-default. **The whole
`tty:*` family is KERNEL-SYNTHETIC-ONLY — `notes_post` REJECTS a userspace
`SYS_POSTNOTE` of a `tty:`-prefixed name, exactly as it rejects `snare:*`**
(round-1 F4 + F15 — the uniform gate-able prefix; `cont`/`hup` renamed to
`tty:cont`/`tty:hup`). Their SOLE entry is the tty seam (`SYS_TTY_SIGNAL`) + the
kernel controlling-terminal / stop paths. WITHOUT this, `tty:cont` would be
postable via the parent-only `SYS_POSTNOTE` gate (`syscall.c` — parent-only, no
`CAP_DEBUG`), letting an unprivileged parent `cont` a debugger-stopped child =
an I-39 leak. A Proc signalling its OWN pgrp still uses ordinary `kill`, which
maps to the standard (non-`tty:`) note authority.

**The generalized stopped-state** (P3 — audit-bearing, I-39): the debug arc's
fully-stopped thread-state (a thread parked on a rendez, resumed under
`g_proc_table_lock`) gets a **second, non-`CAP_DEBUG` entry**. **The as-built
stop is a SINGLE `debug_stop_req` flag (`proc.h`); the job-control stop MUST add
an INDEPENDENT `job_stop_req` owner** (round-1 F3 — NOT a reuse of the one flag):
a thread parks iff `debug_stop_req ∨ job_stop_req`, and each resume clears ONLY
its own owner (`tty:cont` clears `job_stop_req`, never `debug_stop_req`; the
debugger's resume clears only `debug_stop_req`). Reusing the single flag would
make `tty:cont` run a debugger-stopped thread — precisely the `BUGGY_DOUBLE_STOP`
counterexample (`pty.tla`), which the spec's two-owner `stopOwners` set is the
contract against. A `tty:susp` parks the whole fg group's threads; a `tty:cont`
resumes them. The `CAP_DEBUG` gate stays on the *debugger* entry (I-39's
`CTL_VERB_STOP` via `/proc/<pid>/ctl`); the job-control entry's authority is the
kernel-synthetic `tty:*` gate + the tty seam (§3) — you can only Ctrl-Z the fg
group of a terminal you serve, and no ordinary `SYS_POSTNOTE` reaches `tty:cont`
(F4). **The composition obligation (prosecute hard): a job stop + a debug stop of
the same thread must not corrupt the shared stopped-state** (double-stop /
stop-vs-resume / the reap-vs-stop + the 8c-2 nested-sleep detour interaction) —
the audit re-validates I-39 under the new entry, and **`specs/pty_stop.tla` (the
focused stop-composition sibling, the `debug_stop.tla` precedent) models it
(round-1 F6): the death-wins-over-job-stop leg (`DeathWinsOverJobStop` +
`BUGGY_DEATH_BLOCKED`) + the cook↔stop linkage (a `CookSusp` action, so a cooked
Ctrl-Z produces a STOP not a byte/signal) + `StopCompatI39` composed with both —
TLC-green; step-vs-resume composes `debug_step.tla`.**

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
- **multi-waiter reads** (round-1 F12): a pts is a 9P file read via the kernel
  dev9p client, so a MULTI-THREAD slave Proc (peer threads sharing the fd table)
  can have TWO concurrent outstanding `read` Treads on one pts — ptyfs holds a
  per-pts **pending-read SET** (the net-6a-1 `PendingRead` flat-Vec discipline,
  each parked read woken on data/EOF), NOT the cons single-reader (which extincts
  on a 2nd sleeper — a physical-console v1.0 limit that must NOT be inherited).
  The `pty.tla` single-waiter reader is a v1.0 abstraction; the impl is
  multi-waiter (§11.12).
- **teardown** (I-20): master close (last master fd) → the slave's reads return
  EOF + the kernel raises `tty:hup` to the fg pgrp + the controlling process
  (SIGHUP — POSIX carrier-loss target, §11.13); slave close (last slave fd) →
  the master's reads return EOF. Quiesce: in-flight bytes in the rings are
  delivered before EOF (no byte lost at teardown). The `KObj_Pts` frees at last
  close of both ends, gen-bumped before the id is reusable (§11.11).

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

---

## 11. Round-1 holotype close (amendments, 2026-07-17)

The design + spec were prosecuted by the Fable-max holotype reviewer + a
concurrent self-audit: **1 P0 / 5 P1 / 6 P2 / 4 P3** — a heavy round that caught
the seam as originally written was UNREALIZABLE (F1) + a privilege leak (F4).
The four votes (P1–P4) STAND — the fixes correct the *boundary* the design
mis-drew, they do not overturn a vote (F1's kernel pts kobject keeps P1: the
server still owns rings + cooking + termios + winsize; only the security-routing
is kernel). The load-bearing corrections (F1–F5) are folded INLINE (§3/§4/§5);
this section is the authoritative resolution record + binds the rest. Closed
list: `memory/audit_pty_design_closed_list.md`.

**F1 [P0] — the seam was unrealizable (inline §3).** The `tcsetpgrp` → pts-ctl-
write "like `tcsetattr` → consctl" analogy was FALSE (consctl is a kernel Dev;
a pts ctl is an opaque 9P Twrite to the userspace server), so the kernel-owned
`fg_pgid` had no write path. FIXED: a kernel `KObj_Pts` (minted at ptmx-open);
`tcsetpgrp`/`tcgetpgrp` + controlling-terminal acquisition are KERNEL syscalls
keyed on it; only termios/winsize stay server-served. **ABI — signoff at PTY-1**
(the kobject + the syscalls; the biggest new kernel surface).

**F2 [P1] — the pts identity binding (inline §3).** The same `KObj_Pts` is named
by BOTH the server's `SYS_TTY_SIGNAL` and the slave's controlling-terminal
acquisition (`SYS_PTY_REGISTER(master_fd) → kernel_pts_id`, threaded to the slave
open); no free-floating server-only pts identity.

**F3 [P1] — an INDEPENDENT job-stop owner (inline §4).** The as-built stop is a
single `debug_stop_req`; the job-control stop adds a separate `job_stop_req`
(park iff either; each resume clears only its own). Reusing the one flag = the
`BUGGY_DOUBLE_STOP` counterexample by default.

**F4 [P1] — the `tty:*` notes are kernel-synthetic-only (inline §4).**
`notes_post` rejects a userspace `SYS_POSTNOTE` of a `tty:`-prefixed name (like
`snare:*`); their sole entry is the tty seam + the kernel stop/controlling-
terminal paths. Closes the parent-posts-`cont`-to-a-debug-stopped-child I-39 leak
(the ordinary cross-Proc note gate is parent-only, no CAP_DEBUG).

**F5 [P1] — the wait side (inline §4).** `SYS_WAIT_PID` gains
`WUNTRACED`/`WCONTINUED` + the `want_pid == 0`/`< -1` pgrp selectors, pulled
forward into PTY-1 — WITHOUT it a shell cannot detect a `^Z`-stopped child and
job control is non-functional (the PTY-4 gate can't pass).

**F6 [P1] — the spec models the hard stop compositions.** NEW sibling
`specs/pty_stop.tla` (the focused stop-composition module — the `debug_stop.tla`
/ `sched_alpha.tla` sibling pattern; keeps `pty.tla`'s data-path model clean):
`gflag` (death) + `DeathWinsOverJobStop` (death wins over a stop, #811) + a
`CookSusp` action (the cook↔stop linkage — a cooked Ctrl-Z produces a STOP, not
a byte/signal) + `StopCompatI39` composed with both, and the `BUGGY_DEATH_BLOCKED`
counterexample (a stopped group's death never completes). TLC-green (clean +
liveness + `double_stop` + `death_blocked` cfgs); step-vs-resume composes
`debug_step.tla`.

**F7 [P2] — the anti-steal acquisition guard (inline §3).** Acquisition requires
the pts is not already ANY other session's controlling terminal (a second such
open inherits, never steals); explicit `TIOCSCTTY` stealing stays
unbuilt/fail-closed.

**F8 [P2] — server-death + orphaned-stopped-pgrp resume.** On ptyfs death /
last-master-close, the kernel `tty:cont`-resumes any group stopped via that pts
before EOF/`tty:hup` (no group stranded with a dead SIGCONT source); an orphaned
process group with stopped members gets `tty:hup` then `tty:cont` (the POSIX
orphan rule). Both are kernel-side (the `KObj_Pts` teardown path), the true
residual of the dropped `EventuallyResumed`.

**F9 [P2] — the I-20 ordering/tearing legs are prose, not spec.** `pty.tla`'s
conservation counters prove no byte lost/duplicated; the "in order, not torn"
legs rest on the STRUCTURAL single-writer-per-ring + the cook-under-lock memcpy
(a FIFO ring preserves order by construction; a byte is copied whole under the
ring lock) — stated in §6 + the impl header, not modeled. (Modeling byte
identity/order is a v1.x spec lift if a reviewer wants it machine-checked.)

**F10 [P2] — the ECHO-off no-leak guarantee is prose+unit-tested.** The LS-8b
hard guarantee (no input byte reaches OUTPUT when ECHO is clear — the password
mask), carried per-pts, is a data-transform property validated by prose + the
per-pts cooking unit tests (the LS-8b suspension), NOT `pty.tla` (whose count-
rings cannot express origin-tagged bytes). Named a documented spec boundary.

**F11 [P2] — the `KObj_Pts` carries a monotonic generation.** `SYS_TTY_SIGNAL`
and the controlling-terminal binding validate `(pts_id, gen)`; the binding tears
down + in-flight signals drain before the id returns to the free pool (the netd
slot-gen / net-3d F1 mis-route class).

**F12 [P2] — multi-waiter pts reads (inline §5).** ptyfs holds a per-pts
pending-read SET (net-6a-1), not the cons single-reader; a multi-thread slave
Proc reading its pts from two threads loses no wake or byte.

**F13/F14/F15 [P3].** SIGHUP targets the fg pgrp + the controlling process on
carrier loss (session-leader death fans `tty:hup`+`tty:cont` to orphaned stopped
pgrps) — the two distinct POSIX targets (F13, inline §5 teardown). `notes_post_pgrp`
exactly-once-per-Proc fan-out under a membership-mutating group is the focused
PTY-1 audit + prose (F14). The note-name prefix is uniform `tty:*`
(`cont`→`tty:cont`, `hup`→`tty:hup`) for the gate-able reservation (F15, inline
§4).

**Round-1 verified-sound (do not re-litigate):** the `SYS_TTY_SIGNAL` server-
scoping DIRECTION (vs a generic `SYS_PGRP_SIGNAL`) is the right I-22 call — only
its realizability (F1/F2) + non-exclusivity (F4) were the defects; the precedence
fix is correct + non-vacuous; dropping unconditional `EventuallyResumed` is right
(only the server-death/orphan residual F8 remained); the four buggy cfgs each
fire their exact invariant; the setsid POSIX edges + the "setsid drops the
controlling tty" I-2 argument hold; reusing `debug_rendez` (not a second wait/wake
primitive) is the right instinct given F3's two-owner separation.

**Escalation owed to the user (ABI, signoff at PTY-1):** the `KObj_Pts` kobject +
the new syscalls (`SYS_PTY_REGISTER`, the kernel `tcsetpgrp`/`tcgetpgrp` +
acquisition, `SYS_TTY_SIGNAL`, `SYS_SETSID`/`SETPGID`/`GETPGID`/`GETSID`, the
`SYS_WAIT_PID` `WUNTRACED`/`WCONTINUED`+pgrp-selector extension) + the `tty:*`
note-name family. Recorded, not built — the PTY-1 impl surfaces the exact ABI
for signoff, the way the Tapestry DMA-weave ABI defers to G-2.
