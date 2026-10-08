---
id: seam-9p-async-share-poll-reserve
type: seam
title: "The 9P async share has no poll reservation"
status: open
surface: [sub-kernel-ninep-client]
opened-by: chg-2026-10-07-tag-pool
tracker: "tag-pool audit r1 F6 (Fable 5.1)"
created: 2026-10-07
updated: 2026-10-07
---
## Owed

A reservation for poll(2)'s readiness traffic inside a session's async share.
ARCH 21.11 gives Loom ops and dev9p's poll arms and snapshots one async share
(`P9_ASYNC_MAX` = 16384) and refuses a submit past it with `-P9_E_AGAIN`. A
Loom tenant that holds the whole share on a shared session -- a server that
defers its ops -- leaves every other Proc's `poll(2)` on that session unable to
send a snapshot: `poll_settle` resends an unsent snapshot every
`POLL_SNAP_RESEND_NS` and expires it at `POLL_SNAP_BOUND_NS`, so the fd reads
not-ready rather than hanging. ARCH calls async ops the ones "no thread waits
on", but a poll(2) caller does wait on its snapshot.

## What closes it

A sub-share: the snapshot (and possibly the arm) admitted from a small
reservation Loom ops cannot take, or a per-Loom cap below the async share.
Either is a scripture change to ARCH 21.11 part 2 and a re-run of
`tag_pool.tla` with the reservation modelled.

## Risk while open

poll(2) on a session one Loom tenant has filled reports its fds not ready
until the tenant's ops drain -- degraded readiness, never a hang, never a lost
wakeup. Strictly better than before 2026-10-07, when all 64 tags were one
shared pool for every kind of op.
