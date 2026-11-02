---
id: chg-2026-11-02-b3h-sa3-write-count
type: chg
title: "A server write count is bounded by the request"
date: 2026-11-02
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-kernel-ninep-client
  - sub-kernel-loom
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-02
---
Found by a self-audit alongside B-3h's third round, surveying the over-count class of
[[chg-2026-11-02-b3h-audit-r2-fixes]]'s P0. It predates B-3h. The client handed a server's
Rwrite and Rweftio count back unbounded, so `write(2)` and a Loom completion could name
more than the program asked, and any process can serve a mount. No kernel memory is
at stake: `wb_write_run` and the Spoor transport loop on `done < total`. The program is:
`n -= w` underflows, and its loop sends memory past its buffer to that server. Every real
server answers within the request (netd, placesrv, paneplace, nocturned, tapestryd, corvus,
ptyfs, lictor, Stratum). The sync routes fail the op with `EIO`, as an over-long Rread already
did, and the session stays up; Loom holds the count to the op, as its reads already did.
