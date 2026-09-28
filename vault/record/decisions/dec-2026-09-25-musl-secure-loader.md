---
id: dec-2026-09-25-musl-secure-loader
type: dec
title: "The loader never honours LD_* variables; static programs stay as found"
date: 2026-09-25
status: standing
decided-by: user-vote
affects: [sub-kernel-exec, sub-pouch-mem, inv-i22]
created: 2026-09-25
---
## Fork

`exec_fill_auxv` (`kernel/exec.c`) supplies none of `AT_UID`, `AT_EUID`,
`AT_GID`, `AT_EGID` or `AT_SECURE`, for either phenotype. musl reads that
absence in two different ways, and nobody had chosen either. ARCH 6.5 recorded
the loader's half as the tree found it and left the choice to the operator.

## Research

- **The dynamic loader** (`ldso/dynlink.c:1819-1820`) records which auxv
  entries are present, and sets `libc.secure` unless all four ids are present
  and equal and `AT_SECURE` is clear. With none present, every dynamically
  loaded program runs secure: the loader reads neither `LD_LIBRARY_PATH` nor
  `LD_PRELOAD`; musl ignores `MUSL_LOCPATH` and `NLSPATH` and accepts no TZ
  file but `/etc/localtime`; `secure_getenv` returns nothing; `issetugid()` and
  `getauxval(AT_SECURE)` return 1. That covers native PIEs since B-1d, and ARCH
  6.5 already recorded the same refusal for the Linux phenotype's dynamic
  binaries.
- **A static program's start** (`src/env/__libc_start_main.c:42-43`) compares
  the ids as values. Absent ids are all zero, so they compare equal, and a
  static program runs with `libc.secure` clear and honours those variables.
  Neither path runs musl's fd 0-2 check (a poll, and `/dev/null` opened over a
  closed descriptor): the static start returns before it, and the loader does
  not run it.
- **Linux** sets `AT_SECURE` when an exec gains privilege: a setuid or setgid
  file, file capabilities, a security-module transition. Thylacine has no such
  exec. A Proc's capabilities only shrink at fork ([[inv-i2]]), and elevation
  happens after exec, through the cap device and the legate ([[inv-i22]],
  [[inv-i25]]). Plan 9 has no dynamic loading.
- **A correction.** The first question put to the operator said every process
  ran secure and every start ran the fd check. musl's source showed both were
  wrong. The operator was told, and voted on the static half separately.

## Options

1. **Keep the loader secure and document it.** No kernel change.
2. **Supply the ids with `AT_SECURE` 0**, so the `LD_*` variables work, and add
   a rule for a Proc that elevates after a preload (for example, the legate
   refusing it).

After the correction, for static programs: leave them as found, or patch the
static start to treat missing ids as secure.

## The call

Option 1, and static programs as found (operator, 2026-09-25, two votes). No
program honours `LD_LIBRARY_PATH` or `LD_PRELOAD`: a dynamic program's loader
ignores them, and a static program has no loader. Dynamic programs keep musl's
other secure-mode refusals; static programs keep honouring `MUSL_LOCPATH`,
`NLSPATH` and TZ file paths. Nothing changes in the kernel or in libc. ARCH 6.5
states the policy, and the Operator's Manual (Processes and memory) says what an
operator sees.

## Rationale

`LD_PRELOAD` and `LD_LIBRARY_PATH` let the environment choose code that runs
inside a process. Linux's loader ignores them only when the exec raises
privilege. Here privilege is raised after exec, so a library the environment
chose would already be running inside a Proc when that Proc elevates, and no
flag set at exec can tell which Proc will. Ignoring them always keeps that
library out, and it is what the tree already did. The other variables musl
gates name data files (locale catalogues, message catalogues, time zones). A
static program honours them, as Linux does for a program started without a
privilege gain, and the operator kept that.
