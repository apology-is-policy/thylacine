---
id: seam-66c-proc-fd
type: seam
title: "/proc/<pid>/fd — blocked on the at-exit handle-table lifetime"
status: open
surface: [sub-kernel-path, sub-kernel-territory]
opened-by: fnd-66b-r1-f1
tracker: "task #66c"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

The third #66 consumer: render a Proc's open fds with the namespace
names they were reached by, the Plan 9 `/proc/n/fd` equivalent. The
substrate is done — every Spoor carries its `Path`, and `SYS_FD2PATH`
proves the read — so this is the CROSS-PROC read of it.

## What closes it

The blocker is not the rendering, it is the target's handle-table
lifetime. `#926` closes a Proc's handle table at EXIT while the Proc is
still ALIVE, and that close is lockless — so a cross-Proc reader holding
`g_proc_table_lock` (which is what keeps the Territory alive, the #57a
envelope) can find a live Proc whose table is being freed underneath it.

The fix is the table-lifetime restructure the Territory already models:
split `handle_table_free` into a lock-protected `close_all` (at exit)
and a reap-time struct free, so a reader can hold the table stable the
way `ns_lock` holds the mount table.

Also owed here: [[fnd-66b-r1-f1]]'s coverage gap — the single-hop
`sys_walk_open` adoption arm has no dedicated regression, and this is
where connection-fd names get exercised end to end.

## Risk while open

None as an omission. The risk is in NOT doing the restructure: the
lockless at-exit free is a real property of the tree today, and
`/proc/fd` is simply the first reader that would have to reason about
it. Anything else that wants to read another Proc's fds meets the same
wall.
