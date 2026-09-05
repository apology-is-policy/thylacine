---
id: chg-2026-05-31-808-directmap-pagemap
type: chg
title: "#808: page-map the buddy direct map at boot — the durable #806 fix"
date: 2026-05-31
arc: arc-deep-smp-review
commits: ["d80e1600", "3b52ab6b"]
touched: [sub-kernel-mm-phys]
established: []
closed: []
opened: [seam-mm-directmap-cap-absolute]
mirrors-checked: []
depth: rich
---
## What

The #806 class: the runtime kstack-guard path demoted a direct-map
BLOCK mapping to a table on demand — a break-before-make whose
invalid window an IRQ (same-CPU) or a peer's walk (cross-CPU) could
land in. #808 removes the class by construction: `phys_init`'s final
step page-maps the whole buddy zone to L3 granularity at boot —
single-CPU, fully IRQ-masked, buddy live (it allocates the L2/L3
tables from the zone it is mapping), before the first thread exists.
After it, the runtime path only ever FLIPS present L3 leaves; no BBM
remains on the buddy zone.

The table cost is measured and surfaced
(`phys_directmap_table_pages`, a boot-banner diagnostic).

## The close

[[adt-808-r1]] 0/0/0/3. Its F2 ([[fnd-808-f2]]) is the one carrying
forward: the F3 RAM cap is mem_base-RELATIVE while the direct map is
ABSOLUTE — coincident only on QEMU virt's `mem_base == 1 GiB`.
Dormant everywhere we boot today; a one-line fix owed at the first
board with different geometry ([[seam-mm-directmap-cap-absolute]]).
