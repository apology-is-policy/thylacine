---
id: view-invariants
type: view
title: "Invariant matrix"
query: invariants
---
# Invariant matrix

Generated from note fields — do not edit between the markers
(`quaestor render`). Replaces: the ARCH section-28 condensed table.

<!-- generated:begin -->
| # | invariant | strength | guards | validated by |
|---|---|---|---|---|
| I-1 | [[inv-i1]] | spec | sub-kernel-territory, sub-kernel-devsrv, sub-pouch-net | spec-territory, gate-smp |
| I-10 | [[inv-i10]] | spec | sub-kernel-ninep-client, sub-kernel-ninep-session | spec-9p-client |
| I-11 | [[inv-i11]] | spec | sub-kernel-ninep-client, sub-kernel-ninep-session | spec-9p-client |
| I-13 | [[inv-i13]] | prose | sub-kernel-uaccess, sub-kernel-exception | prose, gate-smp |
| I-17 | [[inv-i17]] | prose | sub-kernel-sched | spec-scheduler, gate-smp |
| I-18 | [[inv-i18]] | spec | sub-kernel-sched-smp | spec-scheduler, gate-smp |
| I-21 | [[inv-i21]] | spec | sub-kernel-sched-smp, sub-kernel-thread, sub-kernel-exception | spec-sched-alpha, spec-sched-oncpu, spec-sched-ctxsw, gate-smp |
| I-22 | [[inv-i22]] | prose | sub-kernel-perm, sub-kernel-caps, sub-kernel-devproc | prose, gate-smp |
| I-24 | [[inv-i24]] | spec | sub-kernel-death, sub-kernel-torpor, sub-pouch-process, sub-pouch-signal | spec-death-wake, gate-smp |
| I-26 | [[inv-i26]] | prose | sub-kernel-devproc | prose, gate-smp |
| I-27 | [[inv-i27]] | prose | sub-kernel-cons, sub-kernel-devdev | prose, gate-interactive, gate-smp |
| I-28 | [[inv-i28]] | prose | sub-kernel-stalk, sub-pouch-fs, sub-pouch-net | gate-smp |
| I-29 | [[inv-i29]] | spec | sub-kernel-loom | spec-loom, spec-loom-multishot, spec-loom-order, spec-loom-devgone, gate-smp |
| I-3 | [[inv-i3]] | spec | sub-kernel-territory | spec-territory, gate-smp |
| I-30 | [[inv-i30]] | spec | sub-kernel-loom, sub-kernel-weft | spec-loom, spec-weft, gate-smp |
| I-32 | [[inv-i32]] | prose | sub-kernel-proc | gate-smp |
| I-33 | [[inv-i33]] | prose | sub-kernel-path, sub-kernel-stalk, sub-kernel-territory | gate-smp |
| I-37 | [[inv-i37]] | spec | sub-kernel-weft | spec-weft, spec-weft-readiness, gate-smp |
| I-38 | [[inv-i38]] | spec | sub-kernel-ninep-dev9p, sub-kernel-larder | spec-fs-cache |
| I-39 | [[inv-i39]] | spec | sub-kernel-devproc, sub-kernel-proc | spec-debug-stop, prose, gate-smp |
| I-8 | [[inv-i8]] | spec | sub-kernel-sched, sub-kernel-sched-smp, sub-kernel-rendez | spec-scheduler, spec-sched-alpha, spec-sched-rebalance, spec-sched-tickless, gate-smp |
| I-9 | [[inv-i9]] | spec | sub-kernel-rendez, sub-kernel-sched-smp, sub-kernel-death, sub-kernel-ninep-client, sub-kernel-ninep-dev9p-poll, sub-kernel-srvconn, sub-netd-server, sub-kernel-poll, sub-kernel-pipe, sub-kernel-torpor, sub-pouch-thread | spec-scheduler, spec-tsleep, spec-sched-tickless, spec-sched-rebalance, spec-death-wake, spec-reader-frame, spec-9p-client, spec-net-poll, spec-net-poll-teardown, spec-poll, spec-pipe, gate-smp |
<!-- generated:end -->
