---
id: lock-dev9p-wb-reg
type: lock
title: "g_wb_reg[] bucket lock -- the write-behind holder registry"
kind: spin
guards: "one bucket of dev9p's holder registry (LARDER-DESIGN 12.6): its chain of entries and each entry's linked flag"
orders-before: [lock-dev9p-wb-priv]
created: 2026-11-15
updated: 2026-11-15
---
## Discipline
64 static buckets (`g_wb_reg[]`, zeroed = unlocked), keyed by client and
qid.path. An entry is a file's one holder on its client (`dev9p_priv.wb_reg`)
or, past that open's last reference, the closer job carrying its run
(`dev9p_close_job.reg`).

Acquisition contexts: `wb_register` (a create- or OTRUNC-born open becoming
eligible), `wb_reg_leave` (the holder's last close once its run is gone, its
last reference with none, and the close job's release), `wb_close_swap`
(dev9p_close moving the run into its job and the job into the open's place),
and `wb_holder_flush` (every leg: it finds the entry and, for a holder, takes
the holder's `wb_lock` to read whether a run or a flush stands, drops the
anchor in place for a changing leg with nothing held, and `spoor_tryref`s the
holder's Spoor).

Ordered before [[lock-dev9p-wb-priv]]: nothing takes a bucket lock under a
`wb_lock`. Sleep-illegal; no wire I/O under it -- a leg drops it before it
flushes or waits. The only thing below it besides `wb_lock` is the buddy zone
(`wb_release_dead_locked`'s kfree). A leg never dereferences a job entry after
dropping the lock: it waits and looks again, and the job is freed only after
its release unlinked it under this lock.
