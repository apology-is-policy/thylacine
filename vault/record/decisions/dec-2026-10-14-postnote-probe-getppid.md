---
id: dec-2026-10-14-postnote-probe-getppid
type: dec
title: "kill(pid, 0) is a POSTNOTE probe with an empty name, and getppid is a syscall"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-syscall-abi, sub-pouch-signal, sub-pouch-process]
created: 2026-10-14
---
## Fork

Pouch's `kill(pid, 0)` -- the existence probe -- answers EINVAL
(`__pouch_sig_to_note` has no case 0, and `SYS_POSTNOTE` has no native probe
arm), so a liveness check reads every pid as invalid. `getppid` answers
ENOSYS (the sentinel). The kernel shows the parent in `/proc/<pid>/status`.

## Research

- POSIX: signal 0 performs the permission and existence checks and sends
  nothing; `getppid` never fails.
- The kernel's note walk already has a probe for the Linux phenotype's
  `viv_kill` (the NULL-name walk), and S-3 gave `SYS_POSTNOTE` an errno per
  refusal ([[dec-2026-10-14-postnote-errnos]]).
- Plan 9 reads the parent from a file (`#c/ppid`); `/proc` is not bound in
  every Thylacine namespace.

## Options

- **POSTNOTE probe + SYS_GETPPID**: an empty note name probes through the same
  gate with S-3's errnos (ESRCH, EACCES read as EPERM); a new `SYS_GETPPID`
  beside `SYS_GETPID`. Both work without `/proc`.
- **Both via `/proc`**: no ABI; they fail where `/proc` is not bound, and the
  probe cannot report EPERM.
- **Probe only**, getppid from `/proc/self/status`, 1 when absent.

## The call

POSTNOTE probe + SYS_GETPPID (operator vote, 2026-10-14).

## Rationale

The probe reuses the gate a real post passes, so it answers exactly what a
post would; a namespace without `/proc` changes nothing. `getppid` must not
fail, so it cannot depend on a file the namespace may lack.
