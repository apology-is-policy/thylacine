---
id: seam-932-devsrv-readdir
type: seam
title: "devsrv .readdir — `ls /srv` enumeration (the devproc twin landed in thread-model C7)"
status: open
surface: [sub-kernel-devsrv]
opened-by: fnd-957-r1-f2
tracker: "task #932"
created: 2026-08-01
updated: 2026-10-29
---
## Owed

`ls /srv` returns -1: devsrv has no `.readdir` slot (grep-confirmed
still absent), so the registry cannot be enumerated through
`SYS_READDIR`. #957 delivered the
prerequisite (`cd /srv` works — the registry root opens as a dir +
stats via `devsrv_stat_native`); the enumeration half is #932.

## What closes it

A `.readdir` impl on devsrv (registry entries as dirents, tombstones
skipped, cookie discipline per the netd/devpci precedents). The devproc
twin is done: thread-model C7 (2026-10-29, [[chg-2026-10-29-thread-model-c7]])
lists the root, the pid, thread and tid directories with a cursor and no
snapshot ([[sub-kernel-devproc]]); its cookie discipline is a precedent here.

## Risk while open

Usability only — services and pids are reachable by NAME (the working
path); no authority or soundness exposure. The per-territory `/srv`
visibility boundary (I-1) is unaffected either way.
