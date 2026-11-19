---
id: lock-dev9p-wb-reg
type: lock
title: "g_wb_reg[] bucket lock -- the write-behind holder registry"
kind: spin
guards: "one bucket of dev9p's holder registry (LARDER-DESIGN 12.6): its chain of entries, its chain of pins, and the linked flag of each entry and pin in them"
orders-before: [lock-dev9p-wb-priv]
created: 2026-11-15
updated: 2026-11-19
---
## Discipline
64 static buckets (`g_wb_reg[]`, zeroed = unlocked), keyed by client and
qid.path. An entry is a file's one holder on its client (`dev9p_priv.wb_reg`)
or, past that open's last reference, the closer job carrying its run
(`dev9p_close_job.reg`). A pin (`struct wb_pin`, votes M24 and M24a) is a
change's claim on its file, on the changing operation's stack, from its leg
until its wire operation and drops are done; while one stands no open
becomes the file's holder. Only a pin's owner links or unlinks it, so it
reads its own `linked` without the lock.

Acquisition contexts: `wb_register` (a create- or OTRUNC-born open becoming
eligible: it takes a truncating open's own pin over, then refuses on an entry
or another pin), `wb_reg_leave` (the holder's last close once its run is
gone, the lock-flush job once a last close's flush has left none, its last
reference with none, and the close job's release), `wb_close_swap`
(dev9p_close moving the run into its job and the job into the open's place),
`wb_holder_flush` (every leg: a changing leg links its pin at its first look;
it finds the entry and, for a holder, takes the holder's `wb_lock` to read
whether a run or a flush stands, drops the anchor in place for a changing
leg with nothing held, and `spoor_tryref`s the holder's Spoor), `wb_unpin`
(a change's end, or a leg's failure), and under KERNEL_TESTS
`dev9p_wb_pins`, `dev9p_wb_test_pin`, `dev9p_wb_test_job` and
`dev9p_wb_test_purge`.

Ordered before [[lock-dev9p-wb-priv]]: nothing takes a bucket lock under a
`wb_lock`. Never two buckets at once: `wb_register` releases a pin linked in
another bucket (`wb_unpin`, its own hold) before it takes its own, and the
test purge takes the buckets one at a time. Sleep-illegal; no wire I/O under it -- a leg drops it before it
flushes or waits. The only thing below it besides `wb_lock` is the buddy zone
(`wb_release_dead_locked`'s kfree). A leg never dereferences a job entry after
dropping the lock: it waits and looks again, and the job is freed only after
its release unlinked it under this lock.
