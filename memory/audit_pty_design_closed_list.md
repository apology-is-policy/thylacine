# PTY / I-20 design pass — audit closed list

Cumulative do-not-re-report set for the PTY design (docs/PTY-DESIGN.md +
specs/pty.tla + specs/pty_stop.tla). DESIGN + SPEC review (no impl exists);
findings are design flaws / unsound bindings / model gaps. Later re-audits (incl.
the PTY-1..PTY-4 impl audits) treat these as closed at the DESIGN level —
re-prosecute only their IMPL realization.

---

## Round 1 — 2026-07-17 (Fable-max holotype + concurrent self-audit)

Scope: 5639b496 (docs/PTY-DESIGN.md + ARCH §23.5/§28 + LIFE-SUPPORT syncs) +
1bbb87d2 (specs/pty.tla + 6 cfgs). Prosecutor: holotype-reviewer,
MODEL(start)==MODEL(end)==Fable 5 — no fallback. Self-audit ran in parallel
(SA-1..SA-9); SA-1/2/3/4/7/8 matched reviewer F1/F10/F12/F9/F5/F11.

**Counts: 1 P0 / 5 P1 / 6 P2 / 4 P3.** A HEAVY round — DIRTY close (a P0 + 5 P1).
The review caught the seam as written was UNREALIZABLE (F1) + a privilege leak
(F4). The four votes (P1–P4) STAND — the fixes correct the boundary the design
mis-drew, not a vote. All closed as binding amendments (PTY-DESIGN.md §3/§4/§5
inline + §11 + the new specs/pty_stop.tla). Amendment commit: <pending>.
**Round-2 warranted** (dirty: P0 + 5 P1; the fixes are architectural — the kernel
KObj_Pts, the syscall relocations, the independent stop owner, the wait
extension, the kernel-synthetic note gate, the spec sibling).

### P0 (1) — CLOSED

- **F1 [P0] — the signal seam was UNREALIZABLE.** I analogized `tcsetpgrp` →
  a server-served pts ctl write to `tcsetattr` → consctl, but consctl is a KERNEL
  Dev (devdev.c) the kernel mediates, while a pts ctl write is an OPAQUE 9P Twrite
  to the userspace ptyfs → the kernel-owned `fg_pgid` had NO write path (either
  it lives in the server, collapsing the seam safety, or the kernel intercepts 9P
  writes, breaking P1 opacity). FIXED (inline §3): a kernel `KObj_Pts` minted at
  ptmx-open; `tcsetpgrp`/`tcgetpgrp` + controlling-terminal acquisition are KERNEL
  syscalls keyed on it; only termios/winsize stay server-served. KEEPS P1 (server
  owns rings+cooking+termios+winsize; only security-routing is kernel). **ABI —
  signoff at PTY-1.**

### P1 (5) — all CLOSED

- **F2 [P1]** no kernel pts identity bound the userspace slave open to the kernel
  controlling-terminal state → the same `KObj_Pts` is named by BOTH the server's
  `SYS_TTY_SIGNAL` and the slave's acquisition (`SYS_PTY_REGISTER`). Inline §3.
- **F3 [P1]** the as-built stop is a SINGLE `debug_stop_req` flag (proc.h:532); a
  naive reuse makes `tty:cont` run a debugger-stopped thread (= BUGGY_DOUBLE_STOP
  by default). FIXED (inline §4): an INDEPENDENT `job_stop_req` owner (park iff
  either; each resume clears only its own).
- **F4 [P1]** the STOP/CONT notes were postable via the ordinary PARENT-ONLY note
  path (no CAP_DEBUG) → a parent could `cont` a debug-stopped child (I-39 leak).
  FIXED (inline §4): the whole `tty:*` family is KERNEL-SYNTHETIC-ONLY (rejected
  from SYS_POSTNOTE like snare:*); `cont`/`hup` → `tty:cont`/`tty:hup` for the
  gate-able prefix (F15).
- **F5 [P1]** "full POSIX job control 1:1" but the WAIT side omitted (no
  WUNTRACED/WCONTINUED, no waitpid(-pgid); proc.h:999) → a shell can't detect a
  ^Z-stopped child, job control non-functional. FIXED (inline §4): SYS_WAIT_PID
  gains WUNTRACED/WCONTINUED + the want_pid==0/<-1 pgrp selectors, PULLED FORWARD
  into PTY-1.
- **F6 [P1]** pty.tla omitted the "prosecute hard" stop compositions §4 flags.
  FIXED: NEW sibling specs/pty_stop.tla (the debug_stop.tla pattern) — `gflag`
  (death) + `DeathWinsOverJobStop` (#811) + a `CookSusp` action (cook↔stop
  linkage) + `StopCompatI39` composed + the `BUGGY_DEATH_BLOCKED` counterexample.
  TLC-green (clean+liveness+double_stop+death_blocked).

### P2 (6) — all CLOSED (§11 F7–F12 + inline)

- F7 anti-steal acquisition guard (pts not already another session's controlling
  tty; inline §3). F8 server-death + orphaned-stopped-pgrp `tty:cont`/`tty:hup`
  resume (§11 — the dropped-EventuallyResumed residual). F9 the ordering/tearing
  legs are STRUCTURAL prose (single-writer FIFO ring + cook-under-lock), not spec
  (§11 + §6). F10 the ECHO-off no-leak guarantee is prose+unit-tested (LS-8b), a
  documented spec boundary (§11). F11 the KObj_Pts carries a monotonic generation
  (SYS_TTY_SIGNAL + the binding validate (pts_id,gen); the netd slot-gen class)
  (§11 + inline §5). F12 multi-waiter pts reads (per-pts pending-read SET,
  net-6a-1, NOT the cons single-reader; inline §5).

### P3 (4) — all CLOSED (§11 F13–F15 + inline)

- F13 SIGHUP targets the fg pgrp + controlling process on carrier loss;
  session-leader death fans tty:hup+tty:cont to orphaned stopped pgrps (inline §5
  + §11). F14 notes_post_pgrp exactly-once-per-Proc fan-out under a
  membership-mutating group = the focused PTY-1 audit + prose (§11). F15 the
  uniform `tty:*` prefix (cont→tty:cont, hup→tty:hup) for the gate-able
  reservation (inline §4).

### Round-1 verified-sound (do not re-litigate)

The SYS_TTY_SIGNAL server-scoping DIRECTION (vs a generic SYS_PGRP_SIGNAL) is the
right I-22 call — only realizability (F1/F2) + non-exclusivity (F4) were defects;
the TLA+ precedence fix is correct + non-vacuous; dropping unconditional
EventuallyResumed is right (only the server-death/orphan residual F8 remained);
the 4 pty.tla buggy cfgs each fire their exact invariant; the setsid POSIX edges
+ the "setsid drops the controlling tty" I-2 argument hold; reusing debug_rendez
(not a second wait/wake primitive) is right given F3's two-owner separation.

### Escalation owed (ABI, signoff at PTY-1)

The `KObj_Pts` kobject + the new syscalls (`SYS_PTY_REGISTER`, kernel
`tcsetpgrp`/`tcgetpgrp`+acquisition, `SYS_TTY_SIGNAL`,
`SYS_SETSID`/`SETPGID`/`GETPGID`/`GETSID`, the `SYS_WAIT_PID`
`WUNTRACED`/`WCONTINUED`+pgrp-selector extension) + the `tty:*` note-name family.
Recorded, not built — PTY-1 impl surfaces the exact ABI for signoff (the Tapestry
DMA-weave-at-G-2 precedent).

---

## Round 2 — 2026-07-17 (Fable-max holotype re-audit of the round-1 fixes)

The dirty-close re-audit (round-1: 1 P0 + 5 P1). Prosecutor: holotype-reviewer,
MODEL(start)==MODEL(end)==Fable 5 — no fallback. Scope: 9a3a0057 (the fixes).
Self-audit ran in parallel; all 3 P1s matched (R2-SA-a/b/c). **0 P0 / 3 P1 / 3 P2
/ 1 P3 — DIRTY.** Amendment commit: <pending>. Trajectory R1 1P0/5P1 → R2 0P0/3P1
(converging at the "pin the mechanism" layer). The re-audit prediction held: 2 P1s
are the R1 class relocated one layer down.

- **R2-F1 [P1]** F1 relocated — the KObj_Pts↔pts correlation had no realizable
  mechanism; "mint kernel-mediated at ptmx-open" was unrealizable (kernel doesn't
  inspect 9P opens). FIXED (§3): ONE server-mediated correlation — ptyfs registers
  (connection,qid)→KObj_Pts at master-mint AND slave-serve; slave-fd→KObj_Pts is a
  kernel lookup on the ptyfs connection+qid (Weft/dev9p pattern). ptmx-mediated
  alt DELETED.
- **R2-F2 [P1]** the job_stop_req re-opened the 8c-3 whole-FS-freeze
  (client_stop_pending + the sleep/tsleep detours read debug_stop_req ONLY).
  FIXED (§4): thread job_stop_req through EVERY debug_stop_req site + the 8c-3
  reader-role-release (gate = debug_stop_req ∨ job_stop_req). **SEQUENCING: PTY-1
  AFTER 8c-3 (its reader-release is LIVE/uncommitted main-track).**
- **R2-F3 [P1]** tty:susp catchability (the unresolved R1 SA-5) — an uncatchable
  SIGTSTP breaks POSIX + fails PTY-4's gate. FIXED (§4): tty:* is a NEW class
  (kernel-only-post + CATCHABLE); default STOP fires only if no handler + unmasked
  (LS-5 analog); a caught tty:susp delivers to the handler, no job_stop_req.
- R2-F4 [P2] SYS_PTY_REGISTER authority anchor = the minting connection (§3).
  R2-F5 [P2] §7 stale "pts ctl op" → §4 kernel syscall (§7). R2-F6 [P2] the wait
  composition (reap-vs-report split; the stop→parent I-9 edge; the WCONTINUED
  latch) = PTY-1 items (§4). R2-F7 [P3] pty_stop.tla CookSusp decorative →
  replaced with abstract StopJob; the cook-linkage/catchability/park-fan-out
  stated OUT of the module's scope (header SCOPE block). TLC-green re-run.
- Verified-sound: StopCompatI39 grpDead exception; BUGGY_DEATH_BLOCKED; F8 orphan
  composes F4; F11 gen composes R2-F1.

**Convergence**: dirty (3 P1) but converging — the remaining lead items are PTY-1
impl-precision obligations (audited at impl vs real code) + a real external dep
(8c-3). Round-3 confirms the R2-F1 correlation + R2-F2 fan-out shape; if it returns
only P2/P3, the design is converged at design altitude.
