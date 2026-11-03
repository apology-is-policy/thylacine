---
id: fnd-thread-c7-r2-f1
type: fnd
round: adt-thread-c7-r2
severity: P3
status: fixed
title: "The note_mask lock contract listed only owner writers, which the spawn arms falsify"
surface: [sub-kernel-notes]
threatens: [inv-i9]
fixed-by: chg-2026-10-29-thread-model-c7
created: 2026-10-29
---
## Prosecution

thread.h and thread_die_pending's header said note_mask is written ONLY by the owning thread, with an exhaustive list; rfork and both spawn arms write a newborn's mask before the link. Sound (the link's lock release orders the store; every cross-thread reader holds threads_lock), but on an unwritten clause a later cross-thread writer would read as conforming. thread.h now states it and the three readers point at it; the edits keep every line count, so the kernel ELF did not change.
