# 135 — PTY kernel arc (sessions, process groups, the pts registry, the tty seam, job control) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-pty-kernel-doc-absorb`).
The PTY-1 kernel arc — the **security-sensitive half** of pseudo-terminal support:
everything that must not be forgeable (signal routing to a process group,
controlling-terminal ownership, the job-control stop) lives in the kernel, bounded
by one property (I-1/I-22): **a terminal server can never name a process group** —
its sole authority is a pts-scoped `SYS_TTY_SIGNAL(pts_id, class)` the kernel
resolves to `pts → controlling session → foreground group`. Its content lives,
code-verified and current — across six fresh audit:hard dossiers — in:

- **sessions + process groups (1a) + the wait extension (1e)** — `sid`/`pgid` on
  `Proc` (rfork-inherited), `setsid`/`setpgid`, and the `SYS_WAIT_PID`
  `WUNTRACED`/`WCONTINUED` + pgrp-selector + **report-is-not-reap** extension:

      vault/system/kernel/execution/sub-kernel-proc.md   (audit: hard)

- **the tty:* note class (1b) + the STOP-class consumption** — the kernel-only-POST
  catchable class, the POST gate (the I-39 F4 barrier: `tty:cont` never postable via
  `SYS_POSTNOTE`), the two terminate latches, and — **now folded** — the masked-susp
  pending/consume mechanism (the #252 all-masked reader + the P1 "two scans are two
  predicates" `notes_stop_dequeue`):

      vault/system/kernel/ipc-wake/sub-kernel-notes.md   (audit: hard — folded here)
      vault/system/boundary/registries/abi-note-names.md

- **the pts registry (1c) + the tty seam (1d)** — the `(SrvConn pointer, qid)`
  identity (the magic-checked transport downcast, fail-closed for loopback), the
  gen guard (F11), the MAY_POST_SERVICE mint gate, and `SYS_TTY_SIGNAL`/acquire/
  `tcsetpgrp`/`tcgetpgrp` (the F7 anti-steal, HUP's dual target, the snapshot-then-post
  no-redirect property):

      vault/system/kernel/execution/sub-kernel-pts.md   (audit: hard)

- **the job-control stop (1f)** — `job_stop_req` as the second independent stop
  owner on one park (each resume clears only its owner; death wins), the catchability
  gate + the #15 self-stop, the #240 freshness guard, the POSIX orphan rule, the
  **#19 non-completing torpor wake** (a completing wake would fabricate `TORPOR_OK`
  for a *surviving* job-stopped Proc), the group-global-IPI-once fan (F2), the 8c-3
  elected-9P-reader release, and `SYS_TTY_CONT`:

      vault/system/kernel/execution/sub-kernel-jobctl.md   (audit: hard)

- **the invariant + the specs** — I-20 (the stop leg), `specs/pty_stop.tla` (the
  stop-ownership algebra + `BUGGY_DOUBLE_STOP`/`BUGGY_DEATH_BLOCKED`), `specs/pty.tla`:

      vault/invariants/inv-i20.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **One genuine gap, now folded — the STOP-class note consumption.** The job-control
  stop, the pts registry, the tty:* class registration, sessions and the wait
  extension were all covered by their fresh dedicated dossiers, but the
  masked-susp *dequeue* mechanism was homeless: the #252 all-masked reader (a
  masked-then-pending `^Z` fell through every EL0-tail arm and was silently lost,
  one of sixteen slots with it) and the P1 lesson (`notes_stop_dequeue_locked` is
  class-filtered, not the class-blind FIFO pop — a `child_exit` in front of the susp
  diverges, silently destroying a wait notification). Now in sub-kernel-notes
  (`chg-2026-09-07-pty-kernel-doc-absorb`).
- **The c8ab2744 per-note phenotype-sigtab gate** (both class scans gating on
  `notes_proc_default_applies` per note, so a phenotyped `SIG_DFL` susp isn't
  `exits()`ed on a caught terminate note behind it) is noted at the fold but its
  regression + full treatment ride the vivarium phenotype surface —
  `145-vivarium.md`'s "The class scans read the sigtab per note", verified when that
  doc's dedicated pass lands.
- **The TTIN gap (#18)** — foreground-read arbitration — is a documented v1.x kernel
  read-gate *seam*, not an implemented mechanism; it stays a seam (carried in
  sub-ptyfs's PTY-4 discussion). Zero code change.
