---
id: dec-2026-10-19-mount-table-dynamic
type: dec
title: "A Territory's mount table is dynamic, in Plan 9's shape, under a DoS cap"
date: 2026-10-19
status: standing
decided-by: user-vote
affects: [sub-kernel-territory, sub-kernel-stalk, spec-territory]
created: 2026-10-19
---
## Fork

A Territory's mount table is a fixed array of `PGRP_MAX_MOUNTS` = 32
entries, inline in `struct Territory`, and a union's first mount takes
two of them: the covered directory's and the member's. On 2026-10-19
the ls-libs gate measured a login shell's table on the device before
its overlays: 19 entries (17 that joey makes at boot, the user's home
and ut's `/tmp`), none of them orphans. Each `bind -bc` overlay onto a
fresh point added two, so the seventh found 31 of the 32 taken and
failed, and `SYS_MOUNT`'s bare -1 named no cause. The same session
leaves a container's recipe mounts (up to 11) two entries to spare. The
cap has been raised four times for one cause (8, 12, 16, 20, 32); the
#80 shed ([[chg-2026-09-21-mount-shed]]) removed the orphans that were
that cause then, and kept 32.

## Research

- **Plan 9 (heritage).** No bound. A namespace (`Pgrp`) hashes its
  `Mhead`s by the mount point's qid (`MNTHASH`); each head holds its
  `Mount` list in search order, one allocated per mount (`newmount`),
  and `pgrpcpy` copies what is present. ARCH 9.1 sketches this shape:
  a per-point list, found by a keyed structure. The array shipped
  instead.
- **Linux.** Mounts are allocated one per mount and hashed by parent
  mount and dentry. Since 4.9 a mount namespace may hold at most
  `fs.mount-max` entries (100,000 by default), and a mount past it fails
  `ENOSPC`. The cap was set far above any known use, to stop runaway
  growth: a DoS bound, not a usage bound.
- **Fuchsia (SOTA).** A component's namespace is a flat table that the
  component manager builds from its manifest, so it holds what the
  manifest names, and nothing inherits it.
- **The tree.** `mount_lookup`, `mount_members_snapshot` and the shed's
  closure scan the array under the `ns_lock` spinlock. Stack arrays at
  seven sites are sized from the cap: the four union member snapshots
  in `stalk.c`, `mount()`'s deferred-clunk list, the cycle check's reach
  set (the cap plus one) and the shed's (the cap plus two).
  `territory_clone` copies the whole array, and the struct is pinned at
  1,400 bytes.

## Options

1. **Dynamic, in Plan 9's shape**, with a large per-Territory cap as a
   DoS bound (Linux's shape); a clone copies only the entries present.
2. **Raise the cap to 64.** One line, the sixth raise; the next
   workload finds the next cliff.
3. **A union's first mount costs one entry.** Buys back one entry per
   union by taking the covered directory out of the table, which
   changes the representation for every consumer of `MCOVERED`.
4. **Change the gate only.** The cliff stays for every user.

## The call

Option 1 (operator, 2026-10-19). Entries are allocated per mount and
grouped by mount point, each point holding its members in search order.
A lookup finds the point by its key and costs that point's members, not
the table's size. A Territory holds at most a large cap of entries, a
DoS bound whose memory cost is stated beside its value, and a mount past
it answers `ENOSPC` ([[dec-2026-10-19-mount-errnos]]). An entry is
allocated before `ns_lock` is taken, so an allocation failure refuses
before any edit, and freed after it is dropped. Invariant-bearing:
`territory.tla` and `territory_shed.tla` re-run with their buggy
configs, and an audit round precedes the merge. ARCH 9.1 states the
rule.

## Rationale

The bound has been a usage limit since the table was built, and use has
reached it five times. Every raise cost memory in every Territory,
whether it mounted anything or not, because the array is inline. Plan 9
never bounded a namespace and allocates what is mounted, and ARCH 9.1
always described that shape. A DoS bound still belongs here, because the
entries are kernel memory that any Proc can ask for, so the cap is set
as Linux sets `mount-max`: far above use, and low enough to stop a
runaway.
