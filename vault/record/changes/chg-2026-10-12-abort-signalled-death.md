---
id: chg-2026-10-12-abort-signalled-death
type: chg
title: "Pouch's abort() ends in snare:abort, and a death by a note reaps as a signalled wait word"
date: 2026-10-12
arc: arc-boosty
commits: ["e158cdf13", "44ca1048d", "ff3bb9508", "9ca970a3c", "7ac6f0c63", "e7f5c085b", "34e66529f", "0b4276ce0"]
touched:
  - sub-kernel-notes
  - abi-note-names
  - sub-kernel-death
  - sub-kernel-proc
  - sub-kernel-exception
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-devproc
  - sub-kernel-birth-hold
  - sub-kernel-caps
  - sub-kernel-jobctl
  - sub-pouch-process
  - sub-pouch-signal
  - sub-pouch-net
  - sub-pouch-seam
  - sub-pouch-mem
  - sub-utopia-eval
  - sub-stratum-boot
  - sub-webkit
established: []
closed: []
opened: []
mirrors-checked: [usr/lib/pouch/patches/0007-pouch-signals.patch, usr/lib/pouch/patches/0021-pouch-pty.patch, usr/lib/pouch/patches/0011-pouch-abort.patch]
depth: rich
created: 2026-10-12
---
Operator votes 2026-10-12: abort is a signalled death; then (18:02Z)
"snare:abort" and "every note death" ([[dec-2026-10-12-abort-signalled-death]]).
Found by B-2c: Pouch's `abort()` was a silent `_Exit(127)`, so a WebKit
`CRASH()` read as "command not found".

**The fault** ([[sub-kernel-exception]], [[sub-kernel-notes]],
[[abi-note-names]], [[sub-pouch-process]]). Patch 0011's `abort()` executes
`brk #0xab06` (`SNARE_ABORT_BRK_IMM`), then `_Exit(127)` as a fallback that
should never run; the EL0 `EC_BRK` arm reads the immediate from ESR's ISS and
calls `proc_fault_terminate` with the new kernel-only note `snare:abort`, so
the console prints the usual `user fault: ... reason="snare:abort"` line. Any
other immediate stays `snare:brk`. Patch 0012's mallocng assert calls
`abort()` instead of a second `_Exit(127)`.

**The signal** ([[sub-kernel-death]], [[sub-kernel-proc]],
[[sub-kernel-devproc]], [[sub-kernel-jobctl]]). The note table gains a
death-signal column (faults: segv 11, bus and align 7, ill and bti 4, brk 5,
fpe 8, abort 6; terminating notes: interrupt 2, kill 9, pipe 13, tty:quit 3,
tty:hup 1). Every death site leaves through `exits_note(name)` or
`exits_signalled(sig, msg)`, which share `exits_term`; a Proc records
`term_sig`, and a dying group `group_term_sig`, set once with
`group_exit_msg` by the first claim (I-24), so a fault in a group already dying
of `exit_group(7)` reaps `EXITED(7)`, not `SIGNALED(7)` (SIGBUS is 7).
`proc_group_terminate` takes the signal: the legate's kill and `EXITKILL`
send SIGKILL, a session hang-up SIGHUP.

**The wait word** ([[sub-kernel-syscall-dispatch]],
[[sub-kernel-syscall-abi]], [[sub-pouch-signal]], [[sub-utopia-eval]]).
`wait_pid_for` packs a signalled zombie as `sig & 0x7f` when the caller asked
for a packed word: `WAIT_UNTRACED`, `WAIT_CONTINUED`, or the new
`WAIT_PACKED` (8), which asks for the packed word without stop or continue
reports. A raw wait still reads status 1. `SYS_WAIT_PID` admits the flag;
the phenotype's `wait4` always passes it and no longer re-packs. libt and
libthyla-rs mirror the flag and decoders; patch 0026's `__pouch_wait` always
passes it and forwards the word, so `WIFSIGNALED`/`WTERMSIG` are real; ut's
job-control wait decodes it (`jobs::status_of_packed`), so `^C` on a
foreground job sets `$status` 130 (pty-4 asserts it).

**Consumers** ([[sub-pouch-net]], [[sub-pouch-seam]], [[sub-pouch-mem]],
[[sub-webkit]]). `pouch-hello-sockets`' out-of-range `FD_SET` now reads
SIGABRT; `pouch-hello-dlopen`'s RELRO leg reads 128 + SIGSEGV;
`pouch-hello-signals` gains deaths legs (abort and assert -> SIGABRT,
`brk #1` -> SIGTRAP, a NULL store -> SIGSEGV); viv-pheno L329/L330 require
SIGPIPE; the fork probe passes `T_WAIT_PACKED`. A census of every wait-status
decoder (9 C files, 14 Rust files, the Go port, ut, the phenotype probe) found
no other reader of the old shapes.

**The note-name pin's mirrors** ([[abi-note-names]]), checked at the land: 0011's `abort()` is `brk #0xab06`, the `SNARE_ABORT_BRK_IMM` the kernel matches; 0007 maps SIGINT and SIGTERM to `interrupt`, SIGKILL to `kill`, SIGPIPE to `pipe` and SIGCHLD to `child_exit`, and 0021 the five `tty:` names, each the kernel table's spelling. `snare:abort` is not deliverable, so neither map gains a row.

**Found on the way.** `raise(SIGABRT)` in Pouch answers EINVAL and the program
runs on (OPEN-BUGS, P3).

**Verification.** Audit Fable 5.1 r1 0/0/0/7, closed: F1 (`exits_note` stops the kernel on a name that records no signal), F2 (the table test walks both tables), F6 (`/proc/<pid>/status` shows a zombie's `signal:`) and F7 (ARCH 28's I-24 row) fixed; F3 (ut's raw waits read a death as 1) and F5 (a kill in the final close reaps differently on the two last-out paths) are operator vote items; F4 (a phenotype guest's `abort()` reads SIGSEGV) is queued with Pouch's `raise(SIGABRT)`. Kernel REDs K1-K10, each red at its witness: the packed word, the first claim's signal, the live-peer reap, the `SYS_WAIT_PID` gate and its latch, the brk immediate (the sockets fdset leg first, then the signals abort leg with that leg relaxed), the pipe row, a late fault's signal, the `/proc` line, and a new TERMINATE row with no signal. On the tip 0b4276ce0: ls-jsc PASS on the CI image, all 25 legs (jsc relinked against the new Pouch, its `abort()` disassembles to `brk #0xab06`; SunSpider 1.0.2 111 ms with the JIT vs 363 ms denied, the int32 loop 450 vs 962 ms); pty-4 PASS (`$status` 130 after `fg` + Ctrl-C); suite 1991/1991 on the default image (deaths legs abort and assert -> SIGABRT, `brk #1` -> SIGTRAP, a NULL store -> SIGSEGV; three `snare:abort` console lines; the dlopen RELRO legs read SIGSEGV); test-fault 8/8; ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8), 0 corruption; check-production --all PASS (the lean image builds, boots, and both dev accounts log in)
