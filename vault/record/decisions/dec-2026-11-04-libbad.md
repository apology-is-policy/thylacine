---
id: dec-2026-11-04-libbad
type: dec
title: "A good program with a bad interpreter reads ELIBBAD, not ENOEXEC"
date: 2026-11-04
status: standing
decided-by: user-vote
affects: [sub-kernel-exec]
created: 2026-11-04
---
## Fork

Round 1 of the ENOEXEC chunk found that a good program whose interpreter is no image read `ENOEXEC`, so a shell ran the program as a script. Vote 26 had registered `T_E_NOEXEC` with this as a documented exception.

## Decision

Operator vote 32: register `T_E_LIBBAD` at 80, Linux's value. ABI signoff for `docs/ERRORS.md`.

## Consequences

From the rewrite's commit on, a refusal of the image is the interpreter's. Before it, a malformed `PT_INTERP` is the program's and stays `T_E_NOEXEC`. An interpreter that is not there still reads its walk's reason. The full spawn call's look in the parent covers the program only, so a spawn with a bad interpreter still reads a pid and exit 1.
