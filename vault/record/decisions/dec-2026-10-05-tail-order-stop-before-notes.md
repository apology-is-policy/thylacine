---
id: dec-2026-10-05-tail-order-stop-before-notes
type: dec
title: "The return-to-user tail checks for a stop before it delivers notes"
date: 2026-10-05
status: standing
decided-by: user-vote
affects: [sub-kernel-exception, sub-kernel-notes, spec-tail-order]
created: 2026-10-06
---
## Fork

The kernel's return-to-user path delivered notes before it checked for a stop.
Plan 9 (`notify()` and `procctl`) and Linux ptrace stop first. Under
stay-stopped, a note that arrived while a target was stopped killed it on the
debugger's next step or breakpoint hit, the step still reported success, and
the debugger could neither see nor discard that note (stay-stopped round 1's
F3; OPEN-BUGS 2026-09-30 10:05Z).

The question put to the operator was "Change the order?", with three options:
stop before notes, so that the order becomes the death check, then the stop
check, then notes, and a resumed thread takes its notes at once; keep the
order but make a note latched during a stop visible to the debugger and
discardable by it, the analogue of ptrace's signal-delivery-stop; or keep the
order and document the hazard where the debugger verbs are.

## Decision

The operator voted on 2026-10-05: **stop before notes**. Landed as
[[chg-2026-10-06-tail-order]]: the kernel's tails, DEBUG-FS-DESIGN 4.2 and the
I-19 and I-24 prose, with the debug_stop, debug_step and death_wake buggy
configurations re-run and a new tail-order model.

## Rationale

The heritage order. A stop is a state the debugger observes; a note delivered
first changes the thread before the debugger can see it, and a step that
reports success over a thread the note killed reports something false.
