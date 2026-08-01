---
id: arc-phase5-namespace
type: arc
title: "Phase 5: the namespace reaches a real filesystem"
status: active
design: ["docs/ARCHITECTURE.md"]
chunks:
  - chg-2026-05-13-p5-attach-mount
  - chg-2026-05-14-p5-mount-syscall
  - chg-2026-05-21-p5-chroot
follow-ons: []
created: 2026-08-01
---
## Goal

Turn the Territory from a data structure into a namespace. Phase 5
arrived with a Territory that held bind edges and nothing else, and a 9P
client with no way to expose what it attached. Three chunks closed the
gap: the mount TABLE (a Spoor grafted at a path, one refcount per entry,
pinned by an extended `territory.tla`), the mount SYSCALLS that let
userspace drive it, and the ROOT pivot that made "walk from my root" a
resolvable thing.

The through-line is lifetime. Each chunk added a new holder class for a
Spoor — a mount entry, then a user handle coexisting with one, then a
`root_spoor` — and each exposed a lifecycle assumption that had been
true only because there had never been two holders at once. The Plan 9
`cclose` fix at P5-mount-syscall is the clearest case: the eager-close
behaviour was indistinguishable from correct until the mount table gave
a Spoor a second holder.

What this arc did NOT build is as load-bearing as what it did. There was
still no multi-component walker — that is stalk-1, many chunks later —
so mount targets were abstract path IDs and the table was keyed on a
number. stalk-2 re-keyed the whole table to Plan 9 Spoor identity once a
real resolver existed to cross it.

## Planned chunks

Historical; the three below landed in 2026-05 and the arc's work is
finished. It stays `active` because the vault has recorded only these
three of Phase 5's namespace-facing chunks — the 9P attach half sits
partly under [[arc-corvus-srv]], and further backfill joins here rather
than opening a new arc.

- [[chg-2026-05-13-p5-attach-mount]] — the mount table + the spec extension.
- [[chg-2026-05-14-p5-mount-syscall]] — `SYS_MOUNT`/`SYS_UNMOUNT` + the cclose fix.
- [[chg-2026-05-21-p5-chroot]] — `SYS_CHROOT` + `root_spoor` + walk-from-root.

## Close summary

(pending — see above; the arc is historically finished but its record is
partial.)
