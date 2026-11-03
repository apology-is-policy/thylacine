---
id: chg-2026-11-02-walk-overcount
type: chg
title: "A walk reply's count is capped by the walk"
date: 2026-11-02
arc: arc-boosty
commits: ["fb4ba165d", "d576f072a"]
touched:
  - sub-kernel-ninep-session
  - sub-kernel-ninep-client
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-02
---
The 9P session capped a walk reply's `nwqid` at `P9_MAX_WALK`, and the client copied
that many qids and attrs into arrays its caller had sized by the walk's own `nwname`:
`kmalloc(sizeof(struct p9_attr) * nname)` in `dev9p_walk_attrs` and `dev9p_open_cached`,
and `wq[1]` on the stack in `dev9p_create`. Any EL0 Proc can serve 9P over two pipes
through `SYS_ATTACH_9P` with no capability, so one over-counted Rwalk or Rwalkgetattr
overflowed the kernel heap or stack. The code dates from POUNCE, `d9fdff5e8`. Both
arms now parse against the op's own count, recorded as `walk_nwname` for both walk
kinds, and a longer reply is a protocol violation that ends the session. It lands on
`main` ahead of B-3h by operator vote 40, found by that chunk's audit round 2.
