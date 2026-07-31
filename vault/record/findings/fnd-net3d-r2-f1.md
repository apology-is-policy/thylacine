---
id: fnd-net3d-r2-f1
type: fnd
title: "register_accept's out-of-range else-branch is unreachable — kept as a fail-safe, now documented"
round: adt-net3d-r2
severity: P3
status: documented
surface: [sub-netd-server]
threatens: []
created: 2026-07-31
---
## Prosecution

The `listening_n >= MAX_SLOTS` else-arm (recording gen 0) is
unreachable — every FK_LISTEN path is parse_dec-bounded below
MAX_SLOTS. An unreachable arm invites bit-rot suspicion.

## Disposition

Kept DELIBERATELY as a fail-safe (gen 0 is never a live slot's gen, so
a stray pending recorded that way is dropped by poll_accepts, never a
panic — fail-safe over fail-loud for a daemon); the comment now
documents the invariant and why the arm is safe.
