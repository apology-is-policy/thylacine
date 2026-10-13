---
id: dec-2026-10-13-phenotype-kill-family
type: dec
title: "The Linux phenotype gets kill, tkill and tgkill, each through the cross-Proc gate Pouch's kill already passes"
date: 2026-10-13
status: standing
decided-by: user-vote
affects: [sub-kernel-vivarium]
created: 2026-10-13
---
## Fork

The Linux phenotype has no `kill`, `tkill` or `tgkill` rows (`vivarium.c`'s
table says they must reuse an existing cross-Proc gate, never invent a third).
So a guest's `abort()` (musl raises `SIGABRT` with `tkill`, which has no
row) falls through to `a_crash()`'s NULL store and dies of `snare:segv`,
not `snare:abort`. ARCHITECTURE 11.5 lists `kill` and `tgkill`.

## Research

- Pouch's `kill` posts a note through `SYS_POSTNOTE`, whose cross-Proc path
  admits only the target's parent; I-26's two-axis check is the `/proc` ctl
  `kill`'s.
- A self-directed `SIGABRT` must die as Pouch's `abort()` does, of
  `snare:abort`; userspace cannot post a `snare:` note, so the kernel
  synthesizes that death.

## Options

- **A. The kill family**: `kill`, `tkill` and `tgkill` rows, each reusing the
  gate Pouch's `kill` passes; a self-directed `SIGABRT` dies of `snare:abort`.
- **B. Only a self-directed `tkill`/`tgkill(SIGABRT)`.**
- **C. No rows.**

## The call

A (operator vote, 2026-10-13).

## Rationale

The rows ARCHITECTURE 11.5 already promises, with no new authority: the gate
is the one a Pouch program already passes.
