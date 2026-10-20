---
id: dec-2026-10-13-ut-status-packed
type: dec
title: "ut's $status is 128 plus the signal for a death by note on every wait, not only the job-control wait"
date: 2026-10-13
status: standing
decided-by: user-vote
affects: [sub-utopia-eval]
created: 2026-10-13
---
## Fork

gcpark-b made a death by note read back as its signal in a packed wait word
([[dec-2026-10-12-abort-signalled-death]]). ut's job-control wait asks for the
packed word and sets `$status` to 128 plus the signal; its other waits (the
console path, a command substitution, a pipeline, a script or `ut -c` without
job control, `wait`, the background reaper) wait raw and read the same death
as 1. Should one death read the same everywhere? (gcpark-b audit F3.)

## Research

- POSIX shells set `$?` to 128 plus the signal for a signalled child wherever
  they set it, interactive or not; a script cannot tell a SIGINT death from
  `exit 1` otherwise.
- Plan 9's rc reports the note text as `$status`, so a note death is never
  mistaken for an ordinary failure either.
- The packed word decodes an exit identically (`(N & 0xff) << 8`), so passing
  `WAIT_PACKED` everywhere changes only how a death reads.

## Options

- **A. 128 plus the signal everywhere**: every ut wait passes `WAIT_PACKED`
  (the job-control wait keeps `WAIT_UNTRACED`) and decodes with
  `status_of_packed`; ERRORS.md drops ut from the raw keepers.
- **B. Keep the plain waits raw** (the 2026-10-12 shape).

## The call

A (operator vote, 2026-10-13).

## Rationale

One death, one status: a script's `$status` after an interrupted command reads
130 as it does at the prompt.
