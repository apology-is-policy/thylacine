---
id: fnd-poll-r1-f4
type: fnd
round: adt-poll-r1
severity: P1
status: fixed
title: "A NULL-obj KOBJ_SPOOR slot reached the Dev dispatch"
surface: [sub-kernel-poll]
threatens: []
fixed-by: chg-2026-05-20-p5-poll
regression: "the NULL-obj arm returns POLLNVAL, pinned symmetric with the srv path"
created: 2026-08-01
---
## Prosecution

`handle_alloc` documents that `obj` may be NULL (test scaffolding).
`poll_scan_one`'s KOBJ_SPOOR arm dereferenced `slot->obj` without
the check — a NULL-obj slot NULL-deref'd in the kernel.

## Fix

The malformed-Spoor arm: NULL obj (or NULL dev) ⇒ `POLLNVAL` —
deliberately NOT always-ready, so a buggy caller polling such an fd
cannot spin observing fake readiness. Symmetric with the srv
dispatch's unknown-magic arm.
