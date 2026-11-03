---
id: dec-2026-10-14-fd-calls-on-astra
type: dec
title: "B-3b builds on Astra's approved native descriptor calls, extended, instead of minting its own"
date: 2026-10-15
status: standing
decided-by: user-vote
affects: [sub-kernel-handle, sub-kernel-syscall-abi, sub-pouch-process, sub-pouch-net]
supersedes: [dec-2026-10-14-native-fd-calls]
created: 2026-10-15
---
## Fork

[[dec-2026-10-14-native-fd-calls]] chose two native calls -- a Plan 9-shaped
dup and a descriptor-status read -- over the kernel's existing POSIX dup and
status primitives. Designing B-3b then found the same primitives already
wrapped on Astra's branch (`codex/astra`, not on main): three native calls
the operator approved on 2026-10-11 (`docs/NATIVE-DESCRIPTOR-OPERATIONS-
PROPOSAL.md` there) -- `SYS_HANDLE_FLAGS_GET` (reads the close-on-exec bit),
`SYS_HANDLE_FLAGS_SET` (sets it) and `SYS_HANDLE_DUP_TO` (atomic duplication
onto an exact number), numbered 128-130. Main took 127 and 128 for the JIT
(handoff 048), so they renumber. The vote of 2026-10-14 was asked without
knowing of them.

## Research

- Astra's chain on `codex/astra`: `3320e0d00` (Pouch spawn action
  provenance), `0eed6905e` (the native handle query and Pouch's spawn source
  query, patch 0050 there), `2e2ba81b9` (dup2 self-alias validation, 0051
  there), `238891f9c` (the flag setter and exact-target dup, 0052 there,
  with host tests, three `handles` model configs and a Pouch prover). Her
  review was a single-agent one, not an independent audit.
- Her calls cover close-on-exec and exact-target dup. They do not cover
  `F_DUPFD` (lowest free at or above a floor, which only an atomic kernel
  call can serve: a probe-then-dup races another thread into the hole) nor
  `F_GETFL` (access mode, nonblock, append), which the 2026-10-14 vote
  needed for curl and OpenSSL.

## Options

- **Build on Astra's calls**: one ABI. Main brings her chain into B-3b with
  her authorship, renumbered above main's top, extends `SYS_HANDLE_DUP_TO`
  with a "lowest free at or above" flag and `SYS_HANDLE_FLAGS_GET` with the
  access mode, nonblock and append bits, and audits the result.
- **Today's two calls**; Astra drops hers and moves her consumers.
- **Both sets**: five calls, two pairs wrapping the same primitives.

## The call

Build on Astra's calls (operator vote, 2026-10-14).

## Rationale

Two native ABIs over the same handle primitives would be permanent
duplication. Astra's calls are already approved, implemented and qualified;
the extensions add the two operations B-3 needs as flags and bits on them,
which keeps the approved shapes. A handoff tells Astra so she rebases rather
than landing a second copy.
