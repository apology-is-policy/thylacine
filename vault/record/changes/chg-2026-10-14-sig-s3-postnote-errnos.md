---
id: chg-2026-10-14-sig-s3-postnote-errnos
type: chg
title: "SYS_POSTNOTE answers errnos; a self-posted fault snare; Go sends notes (sig S-3)"
date: 2026-10-14
arc: arc-boosty
commits: ["53d5ac122", "da9018c31", "90e147757", "66f8e6573", "9418ae69b", "8c0115189", "a5dd0cb9c", "01d1806ff", "a16aaf850", "7020ef795", "3b726c9d7", "110cb25a9", "37a8b9022", "44ccd28a6", "9bd66d00e", "b0d3c7c47", "a29cef5ac", "6d8b6c0ff", "43d18cac4", "cdfe2e22d"]
touched:
  - sub-kernel-notes
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-vivarium
  - sub-kernel-death
  - sub-pouch-signal
  - sub-libthyla-rs
  - sub-utopia-eval
  - abi-note-names
  - sub-stratum-boot
  - sub-kernel-ninep-transport
established: []
closed: []
opened: []
mirrors-checked: [usr/lib/pouch/patches/0007-pouch-signals.patch, usr/lib/pouch/patches/0021-pouch-pty.patch, usr/lib/pouch/patches/0011-pouch-abort.patch, usr/lib/pouch/patches/0049-pouch-sigterm-term-note.patch, kernel/vivarium.c, usr/lib/pouch/patches/0050-pouch-self-fault-signals-errnos.patch]
depth: rich
created: 2026-10-14
---
S-3 of the signal follow-up to gcpark-b (operator votes 2026-10-14, items 9
and 10 of the 10-13 list). `SYS_POSTNOTE` answers every refusal with its own
errno instead of a bare -1 ([[dec-2026-10-14-postnote-errnos]]), and a Proc
may end itself with a fault snare through the syscall's self arm
([[dec-2026-10-14-self-death-snare]]). Go programs can send notes other than
`kill`, and read a death by note as a signal.

**What.** [[sub-kernel-notes]]: `notes_post` answers `-T_E_INVAL` for every
name it refuses and `-T_E_AGAIN` for a full queue, as its header always said.
The userspace name gates live in one PURE predicate,
`notes_name_user_postable` (snare:*, tty:*, `sak`, the supported set).
`notes_self_death_snare` returns the kernel's literal for `snare:segv`,
`bus`, `ill`, `fpe` and `abort` -- the phenotype's self-death set, pinned
equal row by row. [[sub-kernel-syscall-dispatch]], [[sub-kernel-syscall-abi]]:
`sys_postnote_handler` takes the exception context. Its self arm ends the
caller through `proc_fault_terminate` for a self-death snare; its cross arm,
`postnote_cross`, judges the name first, then walks: `-T_E_SRCH` for no such
Proc, `-T_E_ACCES` for the parent gate (never `-T_E_PERM`, the flat -1), then
the post's own errno. A dead, unreaped child answers 0 to a name it may be
sent, and receives nothing, on both the native and phenotype arms (the walk
owns that rule; `target_dead` is gone). [[sub-kernel-vivarium]]: `viv_kill`
spells the gate's refusal as Linux's `EPERM`. [[sub-pouch-signal]]: patch 0050
makes `raise()` and a self `kill()` of SIGSEGV, SIGBUS, SIGILL or SIGFPE post
the matching snare to the caller, and maps the kernel's `EACCES` to POSIX
`EPERM`. [[sub-libthyla-rs]]: `Error::NoSuchProcess`; `notes::send` passes
the kernel's errno through. [[sub-utopia-eval]]: `kill` names the reason
("no such process", "not a child of this shell", "its note queue is full",
"not a note a program can send"). [[sub-kernel-death]]:
`proc_fault_terminate`'s contract names its three callers. go-thylacine
(local, the operator pushes it): `(*Process).Signal` posts any note but
`kill` through `syscall.Postnote`; `Wait` asks for the packed wait word.

**Witnesses.**
- Kernel: `notes.postnote_errnos` (each errno on each arm; a missing pid, a
  dead child and a non-child, live or dead, each with a refused name);
  `notes.self_death_snare`; `notes.kill_family_gate` (the refused signal
  before the walk: a dead child, a missing pid); the updated
  `unknown_name_rejected`, `snare_forge_rejected`,
  `queue_full_returns_eagain`, `discard_name_purges_pending`,
  `tty.note_post_gate`.
- pouch-hello-signals: `raise(SIGSEGV)`, `kill(getpid(), SIGBUS)`,
  `raise(SIGILL)` and `kill(0, SIGFPE)` deaths read that signal; `kill` of a
  missing pid reads ESRCH; a child's `kill` of its parent reads EPERM.
- u-job-test 11b (a kill of the shell's own parent reads "not a child of this
  shell") and 12 (a bad name, before and after the child dies; a reaped pid
  reads "no such process").
- go-exec checks 3 (`Signal(SIGTERM)` then `Wait` reads signal 15) and 4 (a
  missing pid reads `os.ErrProcessDone`).

Twelve REDs at 90e147757, each red at its own leg. Kernel: the gate answering PERM plus `snare:align` in the self set (1998/2000: postnote_errnos, self_death_snare); viv_kill's EPERM mapping gone plus a full queue back to -1 (1996/2000: four tests); a dead child refused again (1998/2000); ESRCH and an unknown name back to -1 (1997/2000); the name no longer judged first (1999/2000). Device, each with the kernel suite green: the syscall's self-snare branch gone (the four self-fault deaths read status 0); Pouch's EACCES mapping and raise's snare gone (kill(parent) reads errno 13; raise-segv and raise-ill read 0 while kill's two stay signalled); ESRCH flattened to -1 at the syscall (Pouch reads EIO, u-job-test's reaped pid and go-exec's missing pid red); the name no longer judged first, with the kernel's pins silenced (u-job-test 12's bad name to the dead child reads 0); ut's two messages swapped (u-job-test 11b). Go, on a private copy of the fork: Signal through ctl again (EPERM); the raw wait word again (/bin/false reads -1, the packed decode's first consumer). The closing green run: 2000/2000 and every device leg green. Two mistakes in my runner cost a re-run: its `G*` case matched `GREEN` first, so the first closing run never ran, and D4's silencing predated round 2's two new pins, so its first run went red in the kernel suite and hid the device leg.

**Audit.** Fable 5.1. Round 1: 1 P0, 1 P1, 0 P2, 1 P3, all fixed. The P0 was
five kernel-test pins still expecting -1. The P1: the walk answered a dead
child 0 before the name gates, so a dead child accepted a name no program may
send, and a device leg raced the child's death; the name is now judged first.
Round 2 (the close was dirty): 0 P0, 0 P1, 0 P2, 4 P3, all fixed. Closed list:
repo `memory/audit_sig3_closed_list.md`.

**Land gates.** Two reds at 90e147757, each hunted to an observed cause and fixed: pty-4's type-ahead anchor required a CRLF that tapestryd's bare-LF test-mode line can split (a witness defect, also red on main; a5dd0cb9c), and p9_closer.kill_in_final_close_keeps_exit's smp8 red was a lost store of the mq loopback's EAGAIN budget in the test (a29cef5ac; Fable 5.1 0/0/0/4, fixed 43d18cac4), verified by a 300-trial in-boot hunt -- Mac default-smp8 40/40 and thyla-pi KVM smp8 200/200, against about one boot in six to twenty-three before. On cdfe2e22d: CI image (b2c, webkit fork 7cf380d5ea): build, ls-jsc 59/59, pty-4 PASS (FG 130, CS 139, SX 139). Default image: build, suite PASS (2000/2000, boot OK), test-fault 8/8, ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8), 0 corruption, check-production --all PASS.
