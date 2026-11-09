---
id: dec-2026-11-05-commit-the-death
type: dec
title: "A wait that unwinds for a terminate latch commits the death, decided by one compare-and-swap against the latch's clear"
date: 2026-11-05
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-rendez, sub-kernel-poll, sub-kernel-ninep-client, sub-kernel-ninep-dev9p]
created: 2026-11-09
---
## Fork

A terminate latch ends a wait on the footing that the thread dies at its tail, and the wait answers
accordingly: `poll` 0, a console read EOF, a 9P call abandoned. A peer that installs a handler or
opens the notes fd between the unwind and the tail disarms the latch, and the thread lives with
that answer: an untimed `poll` reads a timeout. Options: (a) per site, a death arm with nothing
done answers `-T_E_INTR`; (b) as Linux's `complete_signal` commits a fatal signal at send, a wait
that unwinds for a latch commits the note's default, decided by one atomic compare against the
handler's clear, and the tail obeys it.

## Decision

Vote 35, the operator, 2026-11-05: (b), in a later chunk of its own, with its own audit, and the note
specs re-run.

## Why

(a) closes only the sites it reaches, and the readers of the death predicate are not all waits: the
9P client abandons requests and the close hands its work to the closer on the same footing. (b)
closes every reader at once, because the commit is the predicate. A compare-and-swap rather than a
lock is what lets it run under the spinlocks several readers hold.

## What it costs

A handler installed a moment after the commit does not save the Proc, which is Linux's behaviour for
a signal already committed. `tty:quit` and `tty:hup` latch separately, one more `proc_flags` bit, so
the committed latch names its note.
