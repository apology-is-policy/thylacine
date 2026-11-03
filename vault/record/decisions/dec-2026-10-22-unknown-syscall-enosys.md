---
id: dec-2026-10-22-unknown-syscall-enosys
type: dec
title: "An unknown syscall number answers ENOSYS"
date: 2026-10-22
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-dispatch, sub-kernel-syscall-abi]
created: 2026-10-22
---
## Fork

The dispatcher's default arm answers a bare -1 for a number it does not
know, under a comment that says ENOSYS. -1 is `-T_E_PERM`, which ERRORS.md
forbids a handler to return, and every consumer reads it as EPERM. A
consumer of a new call cannot tell "this kernel lacks the call" from a
refusal; SYS_LSTAT answers a bare -1 itself for an over-long relative
join. The Go fork's `os.Lstat` (go-thylacine `3065caf`) read EPERM for
every lstat on a kernel without B-3, which `02a4b74` works around with a
one-time probe.

## Options

1. **ENOSYS**: the default arm answers `-T_E_NOSYS` (38), as Linux and
   POSIX do. Pouch's existing mapping of the legacy -1 to ENOSYS stays for
   older kernels.
2. Keep -1 and document it.

## The call

Option 1 (operator, 2026-10-22, the recommended option), on `b3`: one
kernel commit with a kernel test run both ways, ERRORS.md and the
dispatch dossiers. Kernels before it keep answering -1; consumers that
must run on them keep their probes.

## Rationale

The registry's own rule, the comment's intent and every other system's
answer agree, and a new call's consumers should not need a probe.
