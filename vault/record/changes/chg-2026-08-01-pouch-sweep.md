---
id: chg-2026-08-01-pouch-sweep
type: chg
title: "vault sweep: the pouch boundary line"
date: 2026-08-01
arc: arc-vault
commits: []
touched:
  - sub-pouch-seam
  - sub-pouch-fs
  - sub-pouch-thread
  - sub-pouch-process
  - sub-pouch-signal
  - sub-pouch-net
  - sub-pouch-tty
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
Batch 10. The 31-patch series read in full (~11.5 kLOC) plus the four
reference docs it absorbs; seven dossiers under the first populated
boundary area, fourteen seams, one userspace lock, three arcs, and the
Record-plane backfill for twenty-five landings.

The headline finding is a numbering collision the docs never absorbed:
the gfx-4 merge renumbered two aux-branch patches (0025 -> 0028,
0026 -> 0029) and DROPPED a third, while main independently landed new
0024 / 0025 / 0026. `78-pouch.md` documents all three of those numbers
under their old meanings -- including a whole section for
`0024-pouch-fopen-create.patch`, a file that no longer exists -- so three
patch numbers each name a different patch in the doc than in the tree.

Its companions: `REFERENCE.md`'s row still says "seven patches" (31) and
"Ten pouch binaries" (24 baked); `78-pouch.md` carries a caveat asserting
`exit_group` maps to `SYS_EXITS` when the patch it documents says
otherwise, and a "terminal detection always reports not-a-tty" caveat
retired 400 lines below it by its own PTY-3 section;
`87-pouch-fstat-lseek.md` documents `struct t_stat` at 80 bytes while all
three pouch mirrors assert 88; and `83-pouch-signals.md` proposes as a
"v1.x extension" the abort override that shipped in 0011 -- documented in
a different reference doc, so the two never met.

`87` also had **no row in `REFERENCE.md` at all** -- a reference doc the
index never listed, which is the mechanical reason nothing ever brought
its `t_stat` table forward. Three rows were repointed; the fourth had
nothing to repoint.

Also recorded: [[seam-pouch-select-fd-bound]], found by reading 0005
against the post-#355 kernel -- `select()` still rejects fds >= 64 as
"unreachable", which stopped being true when the fd table grew to 256.
