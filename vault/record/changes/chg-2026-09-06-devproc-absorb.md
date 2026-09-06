---
id: chg-2026-09-06-devproc-absorb
type: chg
title: "docs/reference retirement: absorb 32-devproc -- fold the 6 read-side field contracts (cpu_ns/name/exe/cwd/qid-pid0/ns-pheno) the debug-heavy dossier lacked, then stub (52 absorbed / 105 live)"
date: 2026-09-06
arc: arc-vault
commits: []
touched: [sub-kernel-devproc]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Second devices/introspection file. sub-kernel-devproc is a large, current
dossier -- but debug-surface-heavy, and it compressed away the read-side
telemetry contracts the P4-C/prowl/VIVARIUM half of 32-devproc documents. Each
folded atom was verified against the legacy doc's exact wording before folding.

FOLDED (a new "rendered field contracts" subsection): cpu_ns is
cumulative+monotonic (Sum run_ns; htop-diff method; excludes the in-flight
slice); name is the UNFORGEABLE basename of the resolved path (not argv[0]) so a
process cannot spoof its status name; exe/cwd are bare bytes (no NUL/newline)
differing on emptiness (exe empty-is-valid per I-33; cwd never empty for a live
Proc, NULL dot_path -> "/"); the qid disambiguates pid 0 ((0<<32)|PQS_PID_DIR=1,
distinct from the apex); ns renders a conditional root: pheno-linux line.

NOT folded (owned elsewhere, per the split-ownership rule): the "why
proc_group_terminate over a bare note post" rationale is the death/jobctl
primitive's, not devproc's (devproc only calls it); devproc's secondary-file
mentions (proc.c/spoor.c/territory.c/env.c) are cross-refs owned by their own
dossiers, so the single-redirect stub orphans nothing.

Stubbed with honest drift: the legacy doc underweights the Go-IDE debug control
surface (stop/step/hw-breakpoints, the fully-stopped conjunction + the
park-predicate race fix, the SPSR guard, the kstack capability split) that is now
the dossier's bulk.

No code touched; no audit owed. sub-kernel-devproc was already updated:2026-09-06.
view-absorption re-rendered: 51 -> 52 absorbed, 105 live.
