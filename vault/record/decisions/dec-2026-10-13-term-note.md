---
id: dec-2026-10-13-term-note
type: dec
title: "SIGTERM gets its own note, `term`: catchable, default terminate, a death by it recorded as SIGTERM"
date: 2026-10-13
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-pouch-signal, sub-kernel-vivarium]
created: 2026-10-13
---
## Fork

Pouch sends `SIGTERM` as the `interrupt` note, which `SIGINT` shares, so a
child killed with `kill(pid, SIGTERM)` reads `WTERMSIG == SIGINT`, and a
program cannot handle the two differently (ERRORS.md "Two deaths that read
back as another signal"; found in gcpark-b).

## Research

- Plan 9's APE maps `SIGTERM` to its own note in `signal.c`'s table (`term`),
  beside `interrupt` for `SIGINT`.
- POSIX keeps the two signals distinct: separately catchable, separately
  maskable, the same default action (terminate).
- The kernel's note table (`g_known_notes`) is the closed set I-19 names; a
  new row is an ABI addition, which the vote is.

## Options

- **A. A `term` note**: a new catchable note with its own mask bit, default
  terminate, its death recorded as `SIGTERM` (15); the kernel row, Pouch's
  mapping and ERRORS.md follow.
- **B. Keep `SIGTERM` as `interrupt`.**

## The call

A (operator vote, 2026-10-13).

## Rationale

The heritage names the note, and a death must read back as the signal that
was sent.
