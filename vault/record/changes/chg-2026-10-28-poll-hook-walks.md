---
id: chg-2026-10-28-poll-hook-walks
type: chg
title: "Hook walks are batched, and none runs under its owner's lock (poll chunk, votes 12 + 13)"
date: 2026-10-28
arc: arc-phase6-pouch
commits: ["49c3e034b", "73fc51435", "ea54736c8", "dfa2fe372", "e9aeccb4f", "8d18a87cd", "543dacebf", "96f259231", "64b13b19a", "d1a0bf094", "71402f7f6", "f6f49c134", "cc82c1d17"]
touched:
  - sub-kernel-poll
  - sub-kernel-proc
  - sub-kernel-cons
  - sub-kernel-notes
  - sub-kernel-ninep-client
  - sub-kernel-ninep-transport
  - sub-kernel-ninep-dev9p-poll
  - sub-kernel-srvconn
  - sub-kernel-pipe
  - sub-kernel-loom
  - sub-kernel-thread
  - lock-poll-list
established: [dec-2026-10-27-poll-walk-span, dec-2026-10-27-locked-walks-out]
closed: [seam-poll-hooks-per-list]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-28
---
The aux poll chunk. P-a/P-b: a wake walks at most 32 hooks per hold behind a
stack bookmark, unregister is O(1), a poll call keeps one hook per list
([[spec-poll-walk]]). P-c: walks owed under `g_proc_table_lock` begin under it and
finish at every release through `proc_table_lock_acquire`/`_release`, the only
path to the lock ([[spec-poll-owed]]). P-d: the console episode's four walks are
owed the same way. P-e: walks reached under a 9P client's `c->lock` or
`g_dev9p_poll_lock` are deferred to the outermost release; srvconn and pipe ring
take a reference per owe, the Loom, the client and the test transports' frees
wait ([[spec-poll-defer]]). Lease A's first boot found the WAIT owners' wait
borrowing the table lock's hook check (71402f7f6) and the first TLC run a
fairness gap in the spec (f6f49c134); audit r1 ([[adt-pollwalk-r1]]) found the
bookmark waits spinning while a preemptible drainer sat runnable on the same CPU
(cc82c1d17). Gates on cc82c1d17: suite 2068/0, all 19 new tests shown red under
sabotage, every poll cfg as claimed, SMP gate 5 x 10/10 with 0 corruption.
