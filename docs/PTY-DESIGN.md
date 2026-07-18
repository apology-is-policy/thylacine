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

- **The `KObj_Pts` correlation is SERVER-MEDIATED, at both ends** (round-2 R2-F1
  — the kernel does NOT inspect 9P opens, so "mint kernel-mediated at ptmx-open"
  is UNREALIZABLE, the very opacity break F1 rejected; it is deleted). ptyfs
  registers a `(ptyfs-connection, qid) → KObj_Pts` mapping with the kernel at
  BOTH the master mint AND each slave serve — the only opacity-preserving shape:
  - `SYS_PTY_REGISTER(master_fd) → kernel_pts_id` mints the gen-stamped
    `KObj_Pts` and records the master-side `(connection, master_qid)`.
  - when ptyfs serves a `/dev/pts/<n>` slave open, it registers that slave's
    `(connection, slave_qid) → KObj_Pts` too. So a later slave fd → `KObj_Pts`
    resolution is a **kernel lookup keyed on the ptyfs connection + the fd's qid**
    (the fd is a `KOBJ_SPOOR` on the ptyfs dev9p session — the kernel already
    holds the (connection, qid), the Weft/dev9p grant-is-the-share correlation).
  The kobject holds the controlling-terminal binding `(session S, fg_pgid, gen)`
  — kernel state, never server-held. **The gen threads every `SYS_TTY_SIGNAL`
  (F11).**
- **`SYS_PTY_REGISTER`'s authority anchor** (round-2 R2-F4 — else registration
  IS the ungated authority the seam rests on): the caller must be the ptyfs
  connection that MINTED the pts (the kernel correlates by connection, above),
  and `master_fd` must be a live pts-master `KOBJ_SPOOR` on that connection. So
  `SYS_TTY_SIGNAL`'s "the registered server owning that pts" gate is anchored to
  the minting connection, not a bare assertion — the netd-owns-its-flows analog
  made concrete. (The single-trusted-ptyfs deployment is the v1.0 assumption; a
  second ptyfs cannot register a pts in another's namespace — the connection
  correlation forbids it.)
- **`tcsetpgrp`/`tcgetpgrp` + controlling-terminal acquisition are KERNEL
  syscalls keyed on `KObj_Pts`** (resolved from the pts fd via the server-
  registered (connection, qid) lookup) — NOT server-served pts ctl writes. Only
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
  dependency, not a seam). **The composition (round-2 R2-F6) — a PTY-1 audit
  obligation:** the report is NOT a reap — a `WUNTRACED` stop-report /
  `WCONTINUED` continue-report returns the child's status WITHOUT running the
  `wait_pid_for` teardown (proc_unlink_child / on_cpu-spin / thread_free /
  proc_free); ONLY an exit reaps. A child STOPPING (setting `job_stop_req`) is a
  NEW wake trigger on the parent's `wait_pid_for` rendez — a fresh I-9
  register-then-observe edge (today the rendez wakes only on child *exit*).
  `WCONTINUED` needs a per-child continued-since-last-wait latch (the Linux
  `CLD_CONTINUED` shape). The pgrp selector must handle a child `setpgid`-leaving
  the waited pgrp mid-wait.

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
`tty:*` family is KERNEL-SYNTHETIC-ONLY on the POST axis — `notes_post` REJECTS a
userspace `SYS_POSTNOTE` of a `tty:`-prefixed name, exactly as it rejects
`snare:*`** (round-1 F4 + F15). WITHOUT this, `tty:cont` would be postable via the
parent-only `SYS_POSTNOTE` gate (parent-only, no `CAP_DEBUG`), letting an
unprivileged parent `cont` a debugger-stopped child = an I-39 leak.

**But `tty:*` is a NEW note CLASS — kernel-only-POST *and* CATCHABLE — distinct
from `snare:*` (kernel-only-post AND uncatchable-terminate)** (round-2 R2-F3, the
unresolved round-1 SA-5). POSIX `SIGTSTP`/`SIGWINCH`/`SIGCONT` are catchable +
maskable (bash/vim/tmux install `SIGTSTP` handlers to save terminal state before
stopping; a program may `SIG_IGN` it). So the DELIVERY axis is the LS-5 pattern:
`SYS_TTY_SIGNAL`→`tty:susp`/`tty:quit` applies its **default disposition (STOP /
terminate) ONLY when the target Proc has no handler for it and it is unmasked**
(the `notes_interrupt_should_terminate_locked` analog); a CAUGHT `tty:susp` is
delivered to the async handler and does **NOT** set `job_stop_req` (the program
suspends itself, or not, on its own terms), and a masked one defers. An
uncatchable `tty:susp` would fail the PTY-4 gate itself (tmux/bash catch it).
`tty:winch`/`tty:cont` are likewise catchable (winch ignored-if-uncaught; cont's
resume is the kernel stop-clear, delivered as a note the program may also catch).
A Proc signalling its OWN pgrp still uses ordinary `kill` (the standard non-`tty:`
authority).

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
resumes them.

**`job_stop_req` MUST be threaded through EVERY site that reads `debug_stop_req`
today** (round-2 R2-F2 — the load-bearing realization detail, NOT just the abstract
park predicate): the `sleep`/`tsleep` stop detours (`kernel/sched.c`), the
`el0_return_stop_check` + `debug_stop_wake_cond` (`kernel/proc.c`), AND — the
sharpest — **the 8c-3 elected-9P-reader-role release** (`kernel/9p_client.c::client_stop_pending`,
which today reads `debug_stop_req` ONLY). Miss it and an everyday **unprivileged
Ctrl-Z of a fg child blocked as the elected reader on the shared SYSTEM-Stratum
dev9p client (`/bin`,`/lib`) freezes those trees for every survivor Proc until
`fg`** — the EXACT #89/8c-3 bug, re-opened via a flag the audited machinery does
not read. So the job stop must ALSO release + re-elect the reader role, exactly as
8c-3 makes the debug stop do. **SEQUENCING: PTY-1's job stop composes with 8c-3's
reader-release mechanism, which is LIVE/uncommitted on the main track — PTY-1 is
sequenced AFTER 8c-3 lands, and generalizes its `debug_stop_req` gate to the
`debug_stop_req ∨ job_stop_req` disjunction.** (`pty_stop.tla` models the stop
OWNERSHIP algebra — the per-owner clear = the F3 correctness; the PARK PREDICATE +
the reader-role fan-out are the `debug_stop.tla`/8c-3 park domain + the PTY-1
audit, not re-modeled in the ownership sibling.) The `CAP_DEBUG` gate stays on the *debugger* entry (I-39's
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

A native `libthyla-rs` Proc, **joey-spawned directly with
`T_SPAWN_PERM_MAY_POST_SERVICE`** (the corvus precedent -- NOT the warden: the
warden is hardware-device-bind driven, intersecting DTB device-node resources into
an allowance, and `ptyfs` owns no hardware, so the warden does not fit a device-less
service), posting `/srv/ptyfs`. joey mounts ptyfs's tree at **`/dev/pts`** -- a
`devdev` synthetic mount-point **directory** child (the `devhw` `/hw/pci`
precedent) -- with the clone file *inside* it at `/dev/pts/ptmx` (the Linux-devpts
shape). The POSIX `/dev/ptmx` master path is a PTY-3 concern (a symlink or a
file-mount, deferred until symlinks) because **union-mount *walking* is unbuilt at
v1.0** (`kernel/territory.c`: `MBEFORE`/`MAFTER` are recorded but treated as "append
an entry"; union walking is Phase 5+), so a single directory mount at `/dev/pts` --
not a union over the occupied `/dev` -- is the realizable placement.

*As-built (PTY-2a-1)*: the server + the two byte rings + the Plan 9 clone-mint +
`SYS_PTY_REGISTER` + the deferred multi-waiter read + the in-server ring selftest
are landed; joey spawns it with a bounded liveness connect that gates a silent
selftest failure.

*As-built (PTY-2a-2)*: the `devdev` synthetic `pts` mount-stub child
(`DEV_KIND_PTS`, an empty QTDIR — reference/109-devdev.md) + joey's
`t_mount("/dev/pts", MREPL)` of the ptyfs devpts tree (fresh open=connect of
`/srv/ptyfs`; boot-fatal on any failure, since ptyfs has no hardware/external
dependency) + the boot-fatal master/slave round-trip probe — the FIRST real
client: ptmx clone-mint (the kernel accepts the differing Rlopen qid),
`SYS_PTY_REGISTER` MINT/SLAVE/FREE live, Treaddir over the mount finds the
minted slave, both ring directions, master-close→slave-EOF. The line discipline
is PTY-2b.

*As-built (PTY-2b)*: the per-pts `Ldisc` — the five-flag word + the ICANON
assembly line + the collected-signal set folded into `Pts` (`tio`/`line`/
`sigs`); the input cook (`master_write`: ICRNL → ISIG [the standard
VINTR/VQUIT/VSUSP trio → `SYS_TTY_SIGNAL` INT/QUIT/TSTP, consumed, never a
byte, never echoed] → ICANON [erase `\b \b`, NL flushes the line WITH its
newline; overflow past `LINE_MAX`=256 drops un-echoed] → raw); the output cook
(`slave_write`: ONLCR, pair-atomic back-pressure); the single `echo()`
chokepoint (echo rides the output transform toward the master; ECHO-off is the
hard no-leak guarantee, per-pts). **A fresh pts is FULL COOKED**
(`TIO_DEFAULT` = ICANON|ECHO|ISIG|ICRNL|ONLCR — the Linux fresh-pts posture;
the "CONS_ICANON-default" above realized as the cooked set; the kernel
console's ISIG-only boot word is a console posture, not the pts one). Ring-full
policy: the cooked flush + the echo DROP on full (tty input overrun / the
best-effort echo — the cons reference posture); the raw input + the output
path back-pressure via a short count (the 2a contract). `h_write` raises the
collected classes AFTER the ring work (the syscall stays out of the pure cook,
so the selftest asserts classes directly). The selftest carries the full
cooked truth table (ICRNL+flush+echo+ONLCR, assembly-holds, erase + empty-line
erase, the ISIG trio, ECHO-off no-leak, raw+ISIG, output ONLCR, line
overflow); the boot probe drives the cooked default live (master `"ping\r"` →
slave `"ping\n"`; the master's read sees the echo `"ping\r\n"` then `"pong"`).
The per-pts **ctl surface to change the word is PTY-2c** (`set_tio` is landed,
resets the assembly — the TCSAFLUSH posture).

*As-built (PTY-2c)*: the per-pts ctl at **`/dev/pts/<n>ctl`** (the Plan 9
suffix-ctl idiom — `eia0`/`eia0ctl` — so the flat Linux-devpts slave names
stay POSIX-intact; a ctl fid holds the slot ref but is **not** an EOF-counted
endpoint). The grammar = the LS-8b consctl grammar per-pts (`+name`/`-name`
over the five flags; ALL tokens validated before ANY applied — one malformed
token rejects the whole write, the tcsetattr-atomic posture; a flag apply
resets the assembly = TCSAFLUSH) **+ the winsize op** `winsize <cols> <rows>`
(decimal ≤ 65535; stores the per-pts winsize and raises
`SYS_TTY_SIGNAL(WINCH)` **iff the size changed** — the Linux TIOCSWINSZ
behavior; the kernel routes `tty:winch` to the fg pgrp). Read-back is one
line: `+icanon +echo +isig +icrnl +onlcr winsize C R\n`. **ptsname** needs no
new surface: `t_stat.qid_path` carries the 9P qid verbatim, and a ptyfs
endpoint qid encodes `PTS_FLAG(bit 40) | N<<8 | filekind` — so
`ptsname(master_fd)` = the fstat qid decode (the boot probe proves it live;
the PTY-3 pouch `ptsname()` implements exactly this; the qid encoding is
thereby a documented ptyfs client contract). The boot probe drives the ctl
live: the `-echo` **no-leak proof without a blocking read** (type a line under
`-echo`, then `+echo` and type another — the master's next drain shows ONLY
the second line's echo) + the winsize round-trip via `t_pread` (the ctl fd's
cursor advanced with the writes — positioned read from 0).

*As-built (PTY-2d)*: the teardown legs. Drain-then-EOF was structural since 2a
(`ring_drain` serves Data while non-empty regardless of the peer's closure;
Eof only on empty+peer-closed) — 2d adds the **HUP raise**: `open_dec` reports
the `n_master` 1→0 edge and `Conn::close_endpoint` (now the single
opened-endpoint drop path: clunk / rebind / connection teardown / Tversion
reset — a dying emulator connection IS carrier loss) raises
`SYS_TTY_SIGNAL(HUP)` before the unref, which the kernel routes dual-target
(fg pgrp + session leader, PTY-1d). **HupAtMostOnce is by construction**:
exactly one master fd per pts can ever exist (masters are mint-only — no walk
resolves a master path, 9P forbids walking FROM an opened fid so the master
fid cannot be cloned, and a walk to an existing newfid is rejected), so the
edge fires at most once per pts lifetime. A slave close is never a hup edge
(the master just reads EOF — POSIX). The mint-rollback path keeps the raw
`open_dec` (a failed mint is not carrier loss). The pts free + registry FREE
at last-unref and the kernel gen guard were live since 2a. The selftest pins
the edge algebra (edge-once, saturated-re-dec silent, slave-close silent,
queued-bytes drain-then-EOF both directions); the boot probe drives
queued-line drain-then-EOF live. The live HUP *delivery* (a controlling
session observing `tty:hup`) is the 2e E2E.

*As-built (PTY-2e — the arc close)*: the `/bin/pty-probe` openpty E2E — a
two-role native prover (the emulator mints + ptsname-decodes + spawns itself
as a session-leader child: `t_setsid` → slave open → `t_tty_acquire` → notes
fd first [self-managing]) proving the §9 E2E bar live: the parked deferred
master read (the readiness read parks until the child's slave opens — which
caught the **`slave_opened_once` latch fix** before ever running: a master
read before ANY slave open must PARK, not spuriously EOF, the Linux
master-blocks semantic), Ctrl-C → "interrupt" on the fg session's notes fd,
winsize → `tty:winch`, master close → `tty:hup`. I-20 takes its **enforced**
§28 number (ARCH §28 + the §25.4 "ptyfs" prosecution row); the as-built
server reference is `docs/reference/136-ptyfs.md`; the `pty.tla` data-path
action map is as-built in `specs/SPEC-TO-CODE.md`.

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
- `tcsetpgrp`/`tcgetpgrp` → the §4 KERNEL syscalls keyed on `KObj_Pts` (round-2
  R2-F5 — NOT a pts `ctl` write; that was the unrealizable F1 boundary).
- `setsid`/`setpgid`/`getpgid`/`getsid` → the §4 syscalls 1:1.
- `TIOCSWINSZ`/`TIOCGWINSZ` → the pts `ctl` winsize op (§5).
- `isatty`/`ttyname` → a pts-fd characteristic check.
- SIGINT/SIGQUIT/SIGTSTP/SIGCONT/SIGWINCH/SIGHUP → the §4 notes, mapped by the
  existing notes↔signals boundary-line (ARCH §23.6). `kill(-pgrp, sig)` →
  `notes_post_pgrp` via the standard note path (a Proc killing its OWN pgrp is
  the ordinary note authority; the tty-signal seam is only for the server).

Covers, when they arrive via Pouch: bash job control, tmux/screen, ssh
(client + the sshd pty side), vim/less/`script`, any `isatty()`-sniffer.

**As-built (PTY-3, `0021-pouch-pty.patch`; the full reference is
`docs/reference/78-pouch.md` "The pty boundary-line")**: the whole
surface landed as ONE pouch patch + one ptyfs posture change
(`S_IFCHR` on ptmx/master/slave — the pouch is-a-tty discriminator) —
the kernel byte-unchanged. The centerpiece is the tty ioctl dispatcher
(`src/misc/ioctl.c` rewritten): musl's pty surface is entirely
ioctl-shaped, so one dispatcher makes the unpatched wrappers work —
termios/winsize ↔ the pts ctl grammar; `TIOCSCTTY`/`TIOC[GS]PGRP` →
the §4 kernel syscalls 95–97; `TIOCGPTN` = the fstat qid decode;
`/dev/ptmx` → the `/dev/pts/ptmx` path redirect (the devpts-native
node; a real symlink stays G11-blocked). The landing also moved the
pouch `openat` onto the stalk resolver (the 0009 per-component loop
opened intermediates with the final omode — structurally unable to
open write-mode across a mount) and rerouted musl's direct
`__syscall(SYS_ioctl,…)` bypasses (ptsname_r/isatty/tcdrain/
tc[gs]etwinsize + the stdio line-buffering probes) through the
dispatcher. The signal mappings landed receive-only (`kill`/`raise` of
a tty signum = `EPERM` — the F4 gate surfaced POSIX-shaped); SIG_DFL
`SIGTSTP` is the DOCUMENTED ignore-not-stop seam (pouch's
always-registered bootstrap reads as "caught" to the kernel
pre-delivery stop gate, and NDFLT terminates — the kernel
NDFLT-stop-arm fix needs signoff; `83-pouch-signals.md` "Known
caveats"). `forkpty` fails honestly (pouch has no `fork()` — a
pouch-wide seam, not a pty gap). Proven by `/pouch-hello-pty` (the
joey boot-fatal probe): the full mint→termios→session→live-ISIG/WINCH/
HUP→EPERM ladder through unpatched-musl wrappers.

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
| PTY-4 | The first real consumer + close (**amended — §14**) | the NATIVE consumer: `ut` job control on a pts (the U-7a jobs table's chartered completion) hosted by the native session host `ptyhost`; the focused audit close; the interactive LS-CI PTY scenario. The originally-named Pouch `tmux`/sshd consumers are structurally unreachable at v1.0 — every POSIX pty-master consumer (tmux, sshd, bash) is `fork()`-shaped and Pouch has no fork (the spawn model); they are the v1.x row (§14.1) | job control works end-to-end over a real pty-hosted session (`^Z` stops the fg job, `fg`/`bg` resume, `^C` terminates, clean teardown); audit clean |

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
POSIX mapping; the PTY-4 session host `ptyhost` → **`den`** (the marsupial den —
the session lives inside it). Descriptive names ship until the user rules; the
ABI surface never changes.

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
`specs/pty_stop.tla` (the focused stop-OWNERSHIP module — the `debug_stop.tla` /
`sched_alpha.tla` sibling pattern; keeps `pty.tla`'s data-path model clean):
`gflag` (death) + `DeathWinsOverJobStop` (death wins over a stop, #811) +
`StopCompatI39` (the two-owner per-owner-clear) + the `BUGGY_DOUBLE_STOP` /
`BUGGY_DEATH_BLOCKED` counterexamples. TLC-green. **Scope (round-2 R2-F7):** it
models the stop OWNERSHIP algebra only — the park-predicate + reader-role fan-out
is the `debug_stop.tla`/8c-3 domain (R2-F2), the cook↔stop linkage is `pty.tla`'s
data-path domain, and catchability is the notes boundary (R2-F3) — none re-modeled
here; step-vs-resume composes `debug_step.tla`.

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

## 12. Round-2 holotype close (amendments, 2026-07-17)

The round-1 amendment (§11) was itself prosecuted by a second Fable-max holotype
round + a concurrent self-audit — the dirty-close re-audit (a P0 + 5 P1 warranted
it). **0 P0 / 3 P1 / 3 P2 / 1 P3** — the re-audit prediction held: the
architectural fixes bred fresh gaps, two P1s the round-1 class relocated one
layer down. All three P1s matched the self-audit. Trajectory: R1 (1 P0 / 5 P1) →
R2 (0 P0 / 3 P1) — converging at the "pin the precise mechanism" layer. The votes
P1–P4 STAND. Closed inline (§3/§4/§7) + here. Closed list:
`memory/audit_pty_design_closed_list.md` (round 2).

**R2-F1 [P1] — F1 was relocated, not fixed (inline §3).** The `KObj_Pts`↔pts
correlation had no realizable mechanism, and "mint kernel-mediated at ptmx-open"
was unrealizable for the very reason F1 was (the kernel doesn't inspect 9P opens).
FIXED: ONE server-mediated correlation — ptyfs registers `(connection, qid) →
KObj_Pts` at BOTH master-mint AND slave-serve; a slave-fd → `KObj_Pts` resolve is
a kernel lookup on the ptyfs connection + qid (the Weft/dev9p grant-is-the-share
shape). The ptmx-mediated alternative is DELETED.

**R2-F2 [P1] — the `job_stop_req` re-opened the 8c-3 whole-FS-freeze (inline
§4).** `client_stop_pending` (`9p_client.c`) + the `sleep`/`tsleep` detours
(`sched.c`) read `debug_stop_req` ONLY, so a job-stopped elected-9P-reader would
freeze `/bin`/`/lib` for every survivor — the exact #89 bug via a different flag.
FIXED: `job_stop_req` is threaded through EVERY `debug_stop_req` site + the 8c-3
reader-role-release; the gate generalizes to `debug_stop_req ∨ job_stop_req`,
per-owner clear. **SEQUENCING: PTY-1 is sequenced AFTER 8c-3 lands** (its
reader-release is LIVE/uncommitted on the main track) and generalizes it.

**R2-F3 [P1] — `tty:susp` catchability (inline §4; the unresolved round-1 SA-5).**
`tty:*` is a NEW note class — kernel-only-POST *and* CATCHABLE — distinct from
`snare:*` (kernel-only-post AND uncatchable). FIXED: the default STOP/terminate
fires only if the target has no handler + is unmasked (the LS-5
`notes_interrupt_should_terminate_locked` analog); a caught `tty:susp` delivers to
the handler and does NOT set `job_stop_req` (else an uncatchable SIGTSTP fails
PTY-4's own gate — tmux/bash catch it).

**R2-F4 [P2] — `SYS_PTY_REGISTER`'s authority anchor (inline §3).** The gate is
the caller being the ptyfs connection that MINTED the pts (the kernel correlates
by connection) + `master_fd` a live pts-master `KOBJ_SPOOR` — the seam's I-22
anchor made concrete, not a bare assertion.

**R2-F5 [P2] — §7 stale text (inline §7).** `tcsetpgrp`/`tcgetpgrp` in the Pouch
boundary-line row now points at the §4 kernel syscalls, not the (unrealizable)
pts ctl write.

**R2-F6 [P2] — the wait composition (inline §4).** The `WUNTRACED`/`WCONTINUED`
report is NOT a reap (only exit reaps); a child STOPPING is a NEW I-9
register-then-observe edge on the parent's `wait_pid_for` rendez; `WCONTINUED`
needs a continued-since-last-wait latch; the pgrp selector handles a mid-wait
`setpgid`-leave — the PTY-1 audit obligations.

**R2-F7 [P3] — `pty_stop.tla` decorative CookSusp (fixed in the spec).** The
`CookSusp`/`suspCount`/`inputLeft` machinery was write-only (no invariant read
it). Replaced with an abstract `StopJob`; the module's honest scope is the stop
OWNERSHIP algebra, with the cook↔stop linkage (pty.tla) + catchability (notes
boundary) + park-predicate/reader-role (debug_stop.tla/8c-3) stated as OUT of its
scope (the module header SCOPE block).

**Round-2 verified-sound:** `StopCompatI39`'s `grpDead` exception is correct
(death reaps → no EL0 corpse to keep stopped); `BUGGY_DEATH_BLOCKED` is the right
counterexample; F8's kernel-posted orphan `tty:cont` composes with F4's post gate
(the kernel is the synthetic poster, exempt); F11's gen composes with the R2-F1
correlation at the `SYS_TTY_SIGNAL(pts_id, gen, class)` signature.

**Convergence:** R2 is a dirty close (3 P1). The remaining shape is sound; the
lead items are now (a) PTY-1 impl-precision obligations (the correlation
realization, the flag fan-out, the catchable gate, the wait composition) audited
at impl against real code, and (b) a real EXTERNAL sequencing dependency (8c-3).
A round-3 confirms the R2-F1 correlation + the R2-F2 fan-out shape; if it returns
only P2/P3, the design is converged at design altitude and PTY-1 owns the
precise realization.

## 13. As-built: the PTY-1 kernel arc (LANDED, 2026-07-17)

The kernel half is built — sub-chunks 1a–1f on branch `pty-1` (rebased onto
`a58403fb`, the 8c-3 tip). The as-built technical reference is
`docs/reference/135-pty-kernel.md`; the audit-trigger row (the authoritative
prosecution copy) is ARCH §25.4; the spec is `specs/pty_stop.tla` (the
stop-ownership gate) + `specs/pty.tla` (the design-altitude data-path model).

| Sub-chunk | Commit | Delivered |
|---|---|---|
| 1a | `a418dba0` | sessions + process groups; `sid`/`pgid` on `Proc` (rfork-inherited); `SYS_SETSID`/`SETPGID`/`GETPGID`/`GETSID` = 89–92; `T_E_SRCH = 3`. |
| 1b | `131ea092` | the `tty:*` note class (kernel-only-POST + catchable; `NOTE_BIT_TTY = 5`); the terminate class `{interrupt, tty:quit, tty:hup}` via `PROC_FLAG_TTY_TERMINATE_PENDING`; `notes_post_pgrp`/`notes_post_pid`. |
| 1c | `2a006c3e` | the pts registry (gen-stamped, the `(SrvConn ptr, qid)` correlation, torn-conn GC); `SYS_PTY_REGISTER = 93`; `p9_srvconn_transport_conn` (the identity downcast). |
| 1d | `96900a36` | the tty seam (`SYS_TTY_SIGNAL` server-scoped; HUP's F13 dual target) + the controlling-terminal syscalls 94–97. |
| 1e | `f2ee7f66` | the `SYS_WAIT_PID` extension: `WAIT_UNTRACED`/`WAIT_CONTINUED` + pgrp selectors + report-is-not-reap + the opt-in packed status. |
| 1f | `bce3fe33` | `job_stop_req` (the 2nd stop owner @316) + the park-predicate fan-out + TSTP live + `SYS_TTY_CONT = 98` (signed off) + the F8 teardown fan + the POSIX orphan rule. |

**The ABI beyond the design pin.** The design escalated syscalls 89–97 + the
`tty:*` family + the wait extension (§11 "Escalation owed to the user"). PTY-1f's
impl surfaced one gap the design named but did not resolve — the shell's `fg`/`bg`
resume has no path (F4 keeps `tty:cont` kernel-synthetic-only; F8 covers only
teardown; ordinary `kill` covers only one's own group). Resolved to
**`SYS_TTY_CONT = 98`** (user-signed-off 2026-07-17), gated exactly like
`SYS_TTY_SET_FG` — one number beyond the 89–97 pin. Also appended: `T_E_SRCH = 3`
(1a) under the errno registry discipline.

**The R2 impl-precision obligations, discharged** (the round-2 "lead items" above):
R2-F1 (the correlation) realized as the pointer-identity `(SrvConn, qid)` registry
(reference §5); R2-F2 (the fan-out) realized as `proc_stop_requested`'s disjunction
threaded through every `debug_stop_req` park site + the 8c-3 reader-role release
(reference §7.6); R2-F3 (catchability) as `proc_tty_susp_would_stop_locked`
(reference §7.2); R2-F6 (the wait composition) as report-is-not-reap with the
per-child latches (reference §8).

**Gates at the PTY-1g close**: default 1161/1161 + boot OK + 0 EXTINCTION + login
E2E; the spec gate GREEN (`pty_stop` clean + liveness + both buggy cfgs exact;
`debug_stop` clean + all buggy cfgs exact; `pty` clean + liveness); the SMP gate
PASS (default+UBSan × smp4/smp8 N=10 = 40/40, 0 corruption). The focused
Fable-5-max holotype over 1a–1f + the closed list are the PTY-1g close record
(`memory/audit_pty1_closed_list.md`).

**What remains (PTY-2+)**: the userspace `ptyfs` byte/cooking server, `/dev/ptmx` +
`/dev/pts/<n>`, per-fd `termios`, and `specs/pty.tla`'s master/slave data-path
realization. The pouch boundary-line (`tcsetpgrp`/`tcgetpgrp` → the §4 kernel
syscalls; `O_NOCTTY`; `SIGTSTP`/`SIGCONT` handler install) is PTY-3.

## 14. PTY-4 as-built design: the native consumer (amendment, 2026-07-18)

### 14.1 The fork-collapse — why the consumer is native

The §9 charter named "a Pouch `tmux` OR the sshd pty side (whichever the
roadmap reaches first)". Neither can be reached at v1.0, and not because of
this arc: **every canonical POSIX consumer of the pty master + job control is
`fork()`-shaped** — tmux (`fork`/`forkpty` per window + daemonize), OpenSSH
sshd (fork per connection + privsep), Dropbear (same), bash (fork per job) —
and Pouch has no `fork`/`clone` (the kernel's process model is spawn =
fused fork+exec; PTY-3's prover pins `forkpty() == -1` as the honest fail).
That absence is a Pouch-wide v1.0 property, not a PTY gap; the ported
consumers arrive on the v1.x row (with a Pouch fork, or a no-fork mux
rearchitecture) and ride THIS chunk's machinery unchanged when they do.

The research residue is empty: the ONLY shell that can host job control at
v1.0 is the native `ut` — and the PTY-1 kernel was explicitly built for it
(`SYS_TTY_CONT`'s header names "the shell's `fg`/`bg` resume"; the 1e
`WAIT_UNTRACED` rationale names "a shell cannot detect a `^Z`-stopped child";
`proc_setpgid` allows parent-sets-live-child precisely because spawn has no
pre-exec window; the U-7a jobs table's header reserves pgrp machinery as
"U-PTY territory"). PTY-4 completes that chartered seam. The gate's substance
is unchanged — job control end-to-end over a real pty-hosted shell; the
"multiplexer" word is realized as the mux's CORE (mint / host / pump / relay
is exactly each tmux window's mechanism); multi-window UI is additive atop
the same host (v1.x / Halcyon terminals).

### 14.2 The consumer stack

Two userspace pieces, ZERO expected kernel bytes:

**(a) `ut` job control (libutopia)** — four mechanisms:

1. **pts detection + the session dance.** At REPL startup, `fstat(0)`: if the
   qid decodes as a pts SLAVE (`PTS_FLAG` bit 40 + `fk == 2` — the same
   two-gate discrimination PTY-3 built, native side), ut runs the pty-probe
   session dance: `t_setsid()` → `t_tty_acquire(0)` → `t_tty_set_fg(0,
   getpgid(0))`, and opens `/dev/pts/<n>ctl` as its line-discipline fd.
   Job control is ACTIVE iff the dance succeeded; on the console (no pts)
   every step is skipped and the shell is byte-identical to today.
2. **The ldisc-fd generalization.** `Env.consctl_fd` generalizes to "the
   line-discipline fd" — login's forwarded consctl on the console, the pts
   `ctl` when hosted. `PROMPT_MODE` / `RAW_MODE` apply VERBATIM (the PTY-2c
   ctl grammar and the LS-8b consctl grammar are deliberately one
   vocabulary); the LS-7 raw-command dance keeps its exact shape with the
   fd swapped.
3. **pgrp job formation.** With job control active, every external job (fg
   command, pipeline, `&` background) is placed in its OWN process group:
   ut `t_setpgid(pid, job_pgid)`s each spawned element (job_pgid = the first
   element's pid) immediately after spawn — the parent-sets-live-child rule.
   (The pre-`setpgid` window where a fast child still runs in ut's pgrp is
   benign at v1.0: no TTIN/TTOU exists, and the fg handoff follows
   immediately.) A fg job then gets the terminal: `t_tty_set_fg(0,
   job_pgid)`; the kernel now routes `^C`/`^Z` (via the ptyfs ISIG →
   `SYS_TTY_SIGNAL` → fg-pgrp path) DIRECTLY to the job — the console-path
   note-forwarding in `wait_pids_interruptible` is not consulted on a pts.
4. **The stop-aware fg wait + `fg`/`bg` resume.** The fg wait passes
   `WAIT_UNTRACED`: an exit reaps (status unpacked via `WAIT_EXITSTATUS` —
   the flag opts into the packed encoding); a STOP-report (report-is-not-reap,
   1e) ends the fg wait — ut takes the terminal back (`t_tty_set_fg(0,
   own_pgid)`), records the job STOPPED in the U-7a table, and prints the
   `[N]+ Stopped` line. `fg %N` = terminal to the job + `t_tty_cont(0,
   job_pgid)` + the same stop-aware wait; `bg %N` = `t_tty_cont` only;
   `jobs` renders the Stopped state. At the prompt, ut's own `tty:susp` is
   drained-and-discarded (a shell does not stop itself — the bash SIGTSTP
   posture) and `tty:hup` ends the REPL (carrier loss; the fd-0 EOF path
   backstops it).

**(b) the session host (`usr/ptyhost`; thematic candidate `den`, held per
§10).** The native consumer of the master side: open `/dev/pts/ptmx` (the
clone mint) → decode `N` from the fstat qid (the documented ptsname
contract) → set the pts winsize (default 80×24; `-w`/`-h` args) → spawn the
hosted program (default `/bin/ut`, else argv) with the slave as fds 0/1/2
(`Stdio::File`; the child runs the session dance itself, the pty-probe
pattern) → pump bytes on two threads (console fd 0 → master; master → fd 1
— blocking reads, matching ptyfs's deliberate non-QTPOLL posture) → on the
hosted shell's exit, the master's drain-then-EOF ends the master pump, the
host reaps + propagates the status, and process exit (the #809 group
cascade + #68 close-at-exit) unwinds the console-blocked peer thread + closes
the master (→ `tty:hup` to any survivor, ptyfs teardown).

**The composition that makes the host trivial**: `ptyhost` registers in
`is_raw_command`, so ut's EXISTING LS-7 console dance flips the outer
console RAW (`-isig`) around it and restores it after — the host never
touches consctl (I-27 untouched). The outer console collapses to a byte
pipe; `^C`/`^Z` arrive as raw 0x03/0x1a bytes, are pumped inward, and the
PTS is the one line discipline that cooks them — into the INNER session's
fg-pgrp signals. Nesting is exactly the pty-probe topology (outer session:
console; inner session: the pts).

### 14.3 Invariants + audit posture

No new kernel surface and no new invariant. Composes: **I-20** (the data
path — first sustained real-workload exercise), **I-9** (the 1e stop-report
wake edge + the 1f stop/cont machinery driven by a real shell), **I-24/#68**
(the host's exit-group unwind of its blocked pump peer + close-at-exit
delivering the last-master edge), **I-27** (the host never holds consctl;
ut-hosted is never console-attached; the SAK path is outside the pts
entirely), **I-28** (spawn resolution unchanged), **LS-5** (the console
session's interrupt path stays; it is simply dormant while a raw child — the
host — holds the console). The console (non-pts) ut path must stay
byte-identical — job control keys on the successful session dance, never on
a heuristic. Audit focus (4e): the fg-terminal handoff pairing (every
`t_tty_set_fg(job)` has its restore on EVERY exit path — stop, exit, spawn
failure, death), the stopped-job table state machine (no lost/double
Stopped→Running edges vs the 1e latches), the host's pump-thread lifetime
(no orphaned reader; no fd leak on any error path), and the
console-byte-identical property.

### 14.4 Verification

The in-guest **boot-fatal probe** (the pty-probe pattern, a driver role on
the master): spawn `ut` on a freshly-minted slave, script the ladder —
`cat\r` (fg job reads the pts) → `^Z` → assert the `Stopped` line + a live
prompt → `jobs\r` shows it Stopped → `fg\r` resumes (cat echoes again) →
`^C` terminates it → a live prompt → `exit\r` → clean teardown + reap. Then
the **interactive LS-CI PTY scenario** (`tools/interactive/`): the same
ladder driven through the REAL console — login → `ptyhost` → the inner
prompt → the job-control ladder → `exit` → the outer prompt restored. The
focused audit + the SMP gate close the arc (4e).

Sub-chunks: **4a** this scripture; **4b** ut job control; **4c** the host;
**4d** the probe + LS-CI; **4e** the audit + ARC close.

### 14.5 As-built (PTY-4b/c/d, 2026-07-18)

Landed on `pty-1`: `ut` job control (`libutopia` -- `JobControlState` on `Env`,
the `Repl::init_pts_session` dance, per-job pgrp formation + the stop-aware
`WAIT_UNTRACED` fg wait + `fg`/`bg` via `SYS_TTY_CONT` in `stmt.rs`/`builtin.rs`,
the consctl->line-discipline-fd generalization, `Env::emit_line` routing shell
output to the session terminal), the native session host `usr/ptyhost` (ptmx
mint + qid ptsname + spawn-on-the-slave + the 2-thread blocking pump; registered
in `is_raw_command`), the boot-fatal E2E `usr/jc-probe`, and the interactive
`tools/interactive/pty-4.exp`. **Kernel: one PTY-1e bug fix** (`8f1cb8a2`) -- the
`SYS_WAIT_PID` handler rejected `WAIT_UNTRACED`/`WAIT_CONTINUED` at its
accepted-flag mask, so the job-control stop/continue reports were unreachable
from EL0 (latent since PTY-1e; PTY-4 is the first consumer). Otherwise the
kernel is byte-unchanged.

**Proven end-to-end** (`jc-probe: PASS`, `joey: PTY-4 job-control E2E OK`, boot
OK, 1163/1163): a clean fg pipeline under jc; `sleep` foregrounded + `^Z` ->
`[1]+ Stopped` on the pts; `jobs` lists it Stopped; `fg` -> `SYS_TTY_CONT`
resume -> a clean prompt exit; the shell reclaims the terminal + runs commands;
`exit` -> the orphan-rule teardown + a clean reap.

**The resume-then-re-stop gap: ROOT-CAUSED + FIXED at PTY-4e (task #19).**
The "second `^Z` after `fg` never re-stops" report was NOT a signal-delivery
gap: the stop cascade (`proc_stop_wake_sleepers_locked`) reused the DEATH
cascade's `torpor_wake_all_for_proc`, which fabricates a COMPLETED wait
(`awoken = 1`) -- immaterial for a dying Proc (#811, the wake's original
charter), but a job-stopped Proc SURVIVES: the fabricated `TORPOR_OK` rode
back to EL0 at resume, so a torpor-timed sleep (`time::sleep` ->
`/bin/sleep`) "finished" the instant `fg` resumed it; the job exited, `fg`
returned Done + restored the terminal, and the second `^Z` -- correctly --
found no foreground job (the KDBG chain: `set_fg(job)` -> `cont` -> instant
`set_fg(own)` restore -> `tstp fg=own would=0`). The fix is two-layer:
**(a) kernel** -- the stop cascade now calls the non-completing
`torpor_stop_wake_all_for_proc` (wakes the rendez, leaves `awoken` clear),
so the woken waiter's tsleep re-loop takes the 8c-2 stop detour, parks with
the wait PRESERVED, and on resume re-registers with its original deadline
(the parks-and-reparks contract made total over torpor -- the Linux
SIGSTOP-over-`futex_wait` restart shape; a REAL wake landing during the
park still delivers, since the waiter stays bucket-linked); **(b)
userspace** -- `libthyla_rs::time::sleep` now honors the futex contract and
re-sleeps the remainder on a spurious `Woken` (its "Woken is unreachable"
comment was falsified by the proc-wide stop wake). Regressions:
`proc.job_stop_preserves_torpor_wait` (stop -> parked, wait intact; cont ->
re-sleeps, wait STILL intact -- the pre-fix code completes it right there;
a real wake finishes it) + `proc.job_stop_recycle` (the proc-level
stop/cont/stop cycle) + the jc-probe re-stop leg (`sleep 30`, `^Z`, `fg`,
`^Z` -> `Stopped` AGAIN, `bg`, `fg` + `^C` -> post-resume interrupt).

**One follow-up remains** (`docs/reference/136-ptyfs.md` "PTY-4 job-control
gaps"): **TTIN** foreground-read arbitration -- without it a stopped job
blocked in a terminal read (`cat`) steals the shell's input off the shared
pts slave, so interactive `cat`-under-`^Z` does not work; the fix is a
`SYS_READ`-on-a-pts-slave pgrp gate (task #18, a new kernel mechanism --
scope the user should weigh). The E2E + LS-CI drive `sleep` (no terminal
read), exercising the full stop / re-stop / bg / fg / interrupt cycle
without depending on TTIN.
