---
id: chg-2026-07-27-30-fopen-append
type: chg
title: "0030: the O_APPEND seek the gfx-4 merge dropped"
date: 2026-07-27
arc: arc-clade
commits: ["de451566"]
touched:
  - sub-pouch-fs
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The gfx-4 merge dropped the aux branch's fopen-create patch as subsumed
-- correct for its O_CREAT and O_TRUNC arms, which main's 0023 and 0024
already carried, and WRONG for its `O_APPEND` arm, which main had
nowhere. The merge kept the prover precisely so the subsumption claim
could be checked, and on the first post-merge boot it failed exactly
there: `fopen("a")` + `fputs` overwrote at offset 0 instead of extending.

Thylacine has no kernel append mode, and musl asks for one via
`fcntl(F_SETFL)` which pouch answers `ENOSYS`, so nothing positioned the
cursor. The fix seeks to END once at open -- applied through a single
helper every successful-open exit routes through, because `openat` has
three of them and a per-site fix would silently miss one (the CL-4 F2
shape). Single-writer append is thereby correct; concurrent appenders may
interleave, which is documented-absent rather than silently claimed.
