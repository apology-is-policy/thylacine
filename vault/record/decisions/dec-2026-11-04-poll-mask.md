---
id: dec-2026-11-04-poll-mask
type: dec
title: "ppoll's and pselect's mask and sigpending sit on SYS_NOTE_WAIT, with no new number"
date: 2026-11-04
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-vivarium, sub-kernel-syscall-abi, sub-pouch-signal]
created: 2026-11-04
---
## Fork

Pouch's `ppoll` and `pselect` ignored their mask, a Linux guest's refused one, and `sigpending`
was a sentinel on both. Options: leave them to a later chunk, or do them in the chunk that owns
the mechanism, [[dec-2026-11-03-note-wait]].

## Decision

Vote 28, the operator, 2026-11-04: they go in this chunk, on the same mechanism, with no new
number. `SYS_NOTE_WAIT` gains `NOTE_WAIT_POLL` and `NOTE_WAIT_PENDING`, and `x4` for the poll
form alone.

## Why

A mask for one call needs what a suspend has: the saved slot, `NOTE_WF_MASK_HELD`, and a handler's
return that puts the caller's mask back. The one thing that differs is that a poll can end some
other way, so a ready fd answers first, and any answer but `EINTR` restores at once, as Linux's
`restore_saved_sigmask_unless` does. A silently ignored mask is the worse of the two old answers:
it reopens the race the caller chose the call to close, and nothing tells them.

## What it costs

In a handler the call holds no mask, which is the stated imprecision a suspend already has.
`rt_sigpending` wants a size of 8 where Linux takes less.
