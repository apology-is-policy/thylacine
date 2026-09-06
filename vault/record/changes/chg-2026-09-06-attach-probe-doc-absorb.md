---
id: chg-2026-09-06-attach-probe-doc-absorb
type: chg
title: "absorb docs/reference/57-attach-probe (mount-surface E2E test binary): zero-fold, test-probe redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["4b3868fb"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The /attach-probe reference (226 lines) -- a TEST BINARY doc (SYS_ATTACH_9P +
SYS_MOUNT + SYS_UNMOUNT E2E), following the 90-u-test test-probe disposition: no
dossier owns a test binary, so redirect to the surfaces it exercises (the
mount/attach handlers -> sub-kernel-syscall-dispatch; the attach mechanism ->
sub-kernel-ninep-attach; the territory mount composition -> sub-kernel-territory;
the 9P transport -> sub-kernel-ninep-transport) + name the test itself (usr/attach-
probe/ + joey orchestration + the phase-5 row) as the record. Nothing stale -- a
live integration probe.

ZERO fold. Render clean; lint 0-fail. view-absorption 85 -> 86.
