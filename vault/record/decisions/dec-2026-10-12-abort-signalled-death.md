---
id: dec-2026-10-12-abort-signalled-death
type: dec
title: "gcpark-b: Pouch's abort() is a snare:abort fault, and every death by a note records its signal for the packed wait word"
date: 2026-10-12
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-death, sub-kernel-proc, sub-kernel-exception, sub-kernel-syscall-dispatch, sub-pouch-process, sub-pouch-signal, sub-utopia-eval, abi-note-names]
created: 2026-10-12
---
## Fork

B-2c found that Pouch's `abort()` ended a program with `_Exit(127)` and no
message: every WebKit `CRASH()` and `RELEASE_ASSERT` looked like "command not
found", and B-0 lost an hour to it. The operator voted (2026-10-12, with the GC
fix and the order) that abort is a signalled death. Two questions followed: what
the kernel calls the death, and how a parent learns the signal, since the
kernel had no signalled wait shape (`proc.h`: exited `code << 8`, stopped
`0x7f | 20 << 8`, continued `0xffff`), every fault reaped as exit 1, and
Pouch's note map had no `SIGABRT`.

## Research

- **Heritage.** Plan 9's `abort()` is a deliberate fault (it loads from address
  0); the parent reads the fault text from `Waitmsg.msg`. musl's `abort()` raises
  `SIGABRT`, then `a_crash()`. Linux reports the signal in the low seven bits
  of the wait word, with bit 7 for a core dump.
- **What Thylacine had.** `EC_BRK` from EL0 already terminated with
  `snare:brk`; `proc_fault_terminate` prints the `user fault:` line. Termination
  sites: the fault path, the uncaught-terminate default (`notes.c`), the
  legate's kill, the session hang-up, the launcher's `EXITKILL`. The phenotype's
  `wait4` re-packed a raw word as exited.
- **Consumers of the old shapes.** `pouch-hello-sockets` expected 127 from
  `abort()`; `pouch-hello-dlopen`'s RELRO leg read a fault death as exit 1;
  ut's job-control wait would have read a signalled word as `$status` 0;
  mallocng's assert was a second `_Exit(127)`.

## Options

Name: (a) `snare:abort`, a new kernel-only fault note reached by one dedicated
`brk` immediate; (b) reuse `snare:brk`; (c) a catchable `abort` note.
Wait status: (1) every death by a note records a signal; (2) faults only;
(3) Pouch alone, from a side channel.

## The call

Operator, 2026-10-12 18:02Z (AskUserQuestion): **"snare:abort
(Recommended)"** and **"Every note death (Recommended)"**. `abort()` executes
`brk #0xab06`; the EL0 `brk` handler recognises the immediate and ends the Proc
with `snare:abort` (POSIX `SIGABRT`), printing the usual `user fault:` line;
`SIGABRT` handlers still do not run. The kernel records a termination signal
for faults and for uncaught terminating notes; the packed wait word gains the
POSIX signalled shape; a new `SYS_WAIT_PID` flag, `WAIT_PACKED`, gives Pouch's
`waitpid` the packed word; native raw waits keep status 1; ut's job-control
wait sets `$status` to 128 + signal. ERRORS.md "Exit-status semantics" and
"The wait status" carry the rules.

## Rationale

A dedicated immediate keeps abort distinguishable from a debugger's or a
compiler's `brk` without a new syscall, and a kernel-only note cannot be
caught, so an abort cannot be resumed by a handler that does not exist in
Pouch. Recording the signal at every note death, not only faults, makes `^C`
on a foreground job read as SIGINT to a POSIX parent and to ut (`$status`
130), which a faults-only rule would have left as exit 1. The packed word is
opt-in per wait, so native callers that read the raw status see nothing new.
