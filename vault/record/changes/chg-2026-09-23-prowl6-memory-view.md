---
id: chg-2026-09-23-prowl6-memory-view
type: chg
title: "prowl-6 (the memory view): the TABLES column in /ctl/procs, every consumer moved with the layout, prowl's pool meter and footprint line, the manual's Processes and memory section"
date: 2026-09-23
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-kernel-devctl
  - sub-prowl
  - sub-coreutils-presenters
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-23
---
The B-1a' capacity figures reach the screen. `/ctl/procs` gains `TABLES` after
`PAGES` (the page-table share of the holder count, an atomic load like its
neighbours), and a census of the layout's consumers -- run with a control, the
header string known to live in exactly one file -- found the three that parse
by count or from the end and moved them: prowl's nine-token parse, `ps`'s
end-anchored parse with its `TBL` column and nine beacon alignments, and
coreutil-smoke's frame check; Halcyon's loaded-systems list and diorama read
only the leading columns and are untouched. prowl's header gains the pool
meter from `/ctl/memory`, its table the `TBL` column, its detail pane a first
line from `/proc/<pid>/status` (ungated, so it renders where the sched half is
denied). The kernel test `devctl.procs_tables_column` forks a child of the test's
Proc (the walker lists the tree, never an orphan from proc_alloc); the child
reserves one lazy page, touches it and reads its own row back: PAGES is the
holder count (the page plus its three tables, 4 -- a one-slot pagemap is an
inline leaf, uncharged) and TABLES the three tables, so a renderer printing
PAGES twice reads 4 4 and fails; the child is reaped before the assertions. The manual gains `13-processes.md`. No new
authority and no new surface; a small holotype round ran on the diff, its
record on the B-1a' audit-trigger row.
