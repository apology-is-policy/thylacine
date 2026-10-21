---
id: chg-2026-10-13-sig-s2-term-kill-family
type: chg
title: "SIGTERM is the `term` note; the phenotype kills through Pouch's gate (sig S-2)"
date: 2026-10-13
arc: arc-boosty
commits: ["58d70f702", "366f2fcc3", "d61277209", "968038ee4", "e3914c1c0", "833cf160d", "9b159ac19", "8b7b3f83b", "66a8b175a"]
touched:
  - sub-kernel-notes
  - sub-kernel-proc
  - sub-kernel-death
  - sub-kernel-vivarium
  - sub-kernel-syscall-dispatch
  - sub-pouch-signal
  - sub-libthyla-rs
  - sub-utopia-eval
  - abi-note-names
established: []
closed: []
opened: []
mirrors-checked: [usr/lib/pouch/patches/0007-pouch-signals.patch, usr/lib/pouch/patches/0021-pouch-pty.patch, usr/lib/pouch/patches/0011-pouch-abort.patch, usr/lib/pouch/patches/0049-pouch-sigterm-term-note.patch, kernel/vivarium.c]
depth: rich
created: 2026-10-13
---
S-2 of the signal follow-up to gcpark-b (operator votes 2026-10-13, items 5,
6 and 8). `SIGTERM` has its own note, `term`: catchable, terminating by
default, and recorded as `SIGTERM` at death
([[dec-2026-10-13-term-note]]). The Linux phenotype gains `kill`, `tkill` and
`tgkill`, which reach exactly the Procs a Pouch program's `kill` reaches
([[dec-2026-10-13-phenotype-kill-family]]). ut's `kill` sends `term` by
default (vote 8).

**What.** [[sub-kernel-notes]]: `g_known_notes` gains the `term` row
(`NOTE_BIT_TERM` 7, `NOTE_MASK_SUPPORTED` 0xff, `NOTE_SIG_TERM` 15). `term`
has its own terminate latch, paired with `NOTE_BIT_TERM` alone in
`thread_die_pending`, so masking `interrupt` never masks `term` and the
reverse holds too. [[sub-kernel-proc]]: `proc_flags` had no free bit for that
latch. The caught-note latch and its claim moved out of it into
`Proc.note_caught`, a u16 that was reserved padding, and bit 11 became
`PROC_FLAG_TERM_TERMINATE_PENDING`. Every reader and writer touches only
those two bytes, and the claim CAS keeps both bytes in one word. The renamed
macros mean stale `proc_flags` code cannot compile; that caught a test that
would have set `PROC_FLAG_LEGATE_ROOT`. `tools/check-flag-words.py` is the
derived guard. [[sub-kernel-vivarium]],
[[sub-kernel-syscall-dispatch]]: `SIGTERM` maps to `term` and `SIGABRT` to
`snare:abort` (SIG_DFL only). One shell, `viv_kill`, serves all three kill
numbers. It posts only through `SYS_POSTNOTE`'s own two arms: the caller
itself, or the parent-gated walk, which now resolves a tid to its Proc,
probes on signal 0 and reports ESRCH, EPERM and EAGAIN. A self-directed
`snare:*` signal ends the caller through `proc_fault_terminate`. A child that
is dead but not yet reaped accepts the signal, as Linux's zombie does. `pid`,
`tid` and `sig` are narrowed to `int` the way Linux's prototypes narrow them.
[[sub-pouch-signal]]: patch 0049 maps `SIGTERM` to `term` and drops the
interrupt arbitration (the mask becomes 0xaf), and `raise(SIGABRT)` and
`kill(self, SIGABRT)` are `abort()`. [[sub-libthyla-rs]]: `NoteClass::Term`.
[[sub-utopia-eval]]: `kill`'s default is `term`; an interactive shell drops
an unhandled `term`, as bash ignores SIGTERM. Go's `syscall.SIGTERM` names
`term` too (go-thylacine, local); the Go port still sends no note but `kill`
(OPEN-BUGS).

**Witnesses.**
- Kernel: `notes.term_die_pending`; `notes.kill_family_gate` (self, child,
  non-child, missing pid, group, probe, SIGHUP, SIGABRT across and to self,
  signal 65, the tid forms, dead child and dead non-child);
  `vivarium.kill_decide`.
- `notes.default_action_table` and `notes.death_signal_table` now pin
  `term`. The latter derives both phenotype default tables from
  `g_known_notes`.
- pouch-hello-signals: SIGTERM's handler runs apart from SIGINT's ignore and
  mask; `raise` and `killself` deaths read SIGABRT; a death by SIGTERM reads
  SIGTERM.
- u-job-test 12: `kill %1` reads 143.
- u-7-test flow 5: 130 for `interrupt`, then 143 for the default.
- viv-pheno-probe L334-L338: self SIGABRT, kill of a child, the parent gate,
  the refusals and narrowing, and a handled self SIGTERM.

Ten device REDs, each red at its own leg: the kernel tests (term on interrupt's latch, the gate skipped, the terminate table without term, SIGHUP posted: 1993/1998; a dead child back to ESRCH: 1997/1998); the probe legs L334-L338 (each with the kernel suite green); Pouch (SIGTERM back to `interrupt`; the self-SIGABRT redirect gone, with kill's SIGTERM sent as SIGINT: raise, killself and term deaths all red); ut (the default back to `interrupt`: u-job-test and u-7-test both red).

**Audit.** Fable 5.1, round 1: 0 P0, 0 P1, 0 P2, 6 P3, plus one P3 from
corona (the `sig` narrowing). Five P3s fixed: the phenotype terminate table
lacked `term`; `term` missing from the table test; no audit-trigger row;
four prose errors; a zombie child read ESRCH. The tid race was withdrawn.
Closed list: repo `memory/audit_sig2_closed_list.md`.
`specs/check-caught-target.sh`: all cfgs as claimed.

**Land gates.** CI image (b2c, with the webkit fork at 7cf380d5ea): build, ls-jsc 59/59, pty-4 PASS (FG 130, CS 139, SX 139). Default image: build, suite PASS (1998/1998, boot OK), test-fault 8/8, ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8), 0 corruption, check-production --all PASS.
