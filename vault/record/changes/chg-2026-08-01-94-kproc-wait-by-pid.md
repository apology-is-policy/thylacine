---
id: chg-2026-08-01-94-kproc-wait-by-pid
type: chg
title: "#94: kproc waits for joey by pid"
date: 2026-08-01
arc: arc-vivarium
commits: ["2c0c4ef4"]
touched:
  - sub-kernel-proc
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
U-7-pre built `wait_pid_for`'s by-pid selector *because* of
`kernel/joey.c`'s kproc-waits-for-joey site, converted the userspace
callers, and then left that site on the reap-any form. The fix was built for
this site and skipped it.

Reachable, and the ordering is the bug: when joey exits early its daemon
children re-parent to kproc, `proc_reparent_children` splices each onto the
FRONT of `kproc->children` (ahead of joey), and the scan breaks on the first
ZOMBIE. An already-dead daemon was therefore reaped instead of joey, and the
`reaped != pid` check extincted with "wrong pid" -- discarding joey's exit
status, the boot-failure diagnostic, on exactly the branches where a dying
daemon is WHY joey is exiting.

Orphan fate unchanged: kproc has never reaped orphans as a service (init is
the reaper; kproc adopts only once init is gone), and the site performed a
single incidental reap before extincting on it.

Pinned by `proc.wait_pid_for_skips_adopted_orphan_zombie`, which builds the
boot's exact arrangement -- an adopted orphan ZOMBIE ahead of the target,
with distinct statuses so the pid AND the status each discriminate.

Absorbed into [[sub-kernel-proc]] when `vault/bootstrap` synced with main at
the substrate sweep; the doc edit landed in `docs/reference/14-process-model.md`,
which the vault had already absorbed into a stub.
