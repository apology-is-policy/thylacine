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
| I-1 | [[inv-i1]] | spec | sub-kernel-territory, sub-kernel-devsrv, sub-pouch-net, sub-kernel-content | spec-territory, gate-smp |
| I-10 | [[inv-i10]] | spec | sub-kernel-ninep-client, sub-kernel-ninep-session | spec-9p-client |
| I-11 | [[inv-i11]] | spec | sub-kernel-ninep-client, sub-kernel-ninep-session | spec-9p-client |
| I-12 | [[inv-i12]] | prose | sub-kernel-mmu, sub-kernel-vma, sub-kernel-fault, sub-kernel-elf, sub-kernel-exec | prose, gate-smp |
| I-13 | [[inv-i13]] | prose | sub-kernel-uaccess, sub-kernel-exception | prose, gate-smp |
| I-15 | [[inv-i15]] | prose | sub-kernel-dtb, sub-kernel-boot-sequence, sub-kernel-gic, sub-kernel-timer, sub-kernel-discovery | prose, gate-smp |
| I-16 | [[inv-i16]] | prose | sub-kernel-kaslr, sub-kernel-boot-entry, sub-kernel-content | prose, gate-smp |
| I-17 | [[inv-i17]] | prose | sub-kernel-sched, sub-kernel-timer | spec-scheduler, gate-smp |
| I-18 | [[inv-i18]] | spec | sub-kernel-sched-smp, sub-kernel-gic | spec-scheduler, gate-smp |
| I-19 | [[inv-i19]] | prose | sub-kernel-notes | prose, gate-smp |
| I-20 | [[inv-i20]] | spec | sub-ptyfs, sub-kernel-pts, sub-kernel-jobctl, sub-kernel-proc | spec-pty, spec-pty-stop, prose, gate-smp |
| I-21 | [[inv-i21]] | spec | sub-kernel-sched-smp, sub-kernel-thread, sub-kernel-exception | spec-sched-alpha, spec-sched-oncpu, spec-sched-ctxsw, gate-smp |
| I-22 | [[inv-i22]] | prose | sub-kernel-perm, sub-kernel-caps, sub-kernel-devproc | prose, gate-smp |
| I-24 | [[inv-i24]] | spec | sub-kernel-death, sub-kernel-torpor, sub-pouch-process, sub-pouch-signal | spec-death-wake, gate-smp |
| I-26 | [[inv-i26]] | prose | sub-kernel-devproc | prose, gate-smp |
| I-27 | [[inv-i27]] | prose | sub-kernel-cons, sub-kernel-devdev | prose, gate-interactive, gate-smp |
| I-28 | [[inv-i28]] | prose | sub-kernel-stalk, sub-pouch-fs, sub-pouch-net, sub-kernel-content | gate-smp |
| I-29 | [[inv-i29]] | spec | sub-kernel-loom | spec-loom, spec-loom-multishot, spec-loom-order, spec-loom-devgone, gate-smp |
| I-3 | [[inv-i3]] | spec | sub-kernel-territory | spec-territory, gate-smp |
| I-30 | [[inv-i30]] | spec | sub-kernel-loom, sub-kernel-weft | spec-loom, spec-weft, gate-smp |
| I-31 | [[inv-i31]] | spec | sub-kernel-asid | spec-asid, gate-smp |
| I-32 | [[inv-i32]] | prose | sub-kernel-proc, sub-kernel-hwcap, sub-kernel-content | gate-smp |
| I-33 | [[inv-i33]] | prose | sub-kernel-path, sub-kernel-spoor, sub-kernel-stalk, sub-kernel-territory, sub-kernel-content | gate-smp |
| I-34 | [[inv-i34]] | spec | sub-kernel-allowance, sub-kernel-hwcap, sub-kernel-discovery | spec-allowance, prose, gate-smp |
| I-36 | [[inv-i36]] | prose | sub-kernel-exec, sub-kernel-image, sub-kernel-fault, sub-kernel-burrow | prose, gate-smp |
| I-37 | [[inv-i37]] | spec | sub-kernel-weft | spec-weft, spec-weft-readiness, gate-smp |
| I-38 | [[inv-i38]] | spec | sub-kernel-ninep-dev9p, sub-kernel-larder | spec-fs-cache |
| I-39 | [[inv-i39]] | spec | sub-kernel-devproc, sub-kernel-proc | spec-debug-stop, prose, gate-smp |
| I-40 | [[inv-i40]] | spec | sub-tapestryd, sub-kernel-weft | spec-tapestry-present, prose, gate-smp |
| I-5 | [[inv-i5]] | spec | sub-kernel-hwcap, sub-kernel-handle, sub-kernel-gic, sub-kernel-discovery | prose, gate-smp |
| I-7 | [[inv-i7]] | spec | sub-kernel-burrow | spec-burrow, gate-smp |
| I-8 | [[inv-i8]] | spec | sub-kernel-sched, sub-kernel-sched-smp, sub-kernel-rendez | spec-scheduler, spec-sched-alpha, spec-sched-rebalance, spec-sched-tickless, gate-smp |
| I-9 | [[inv-i9]] | spec | sub-kernel-rendez, sub-kernel-sched-smp, sub-kernel-death, sub-kernel-ninep-client, sub-kernel-ninep-dev9p-poll, sub-kernel-srvconn, sub-netd-server, sub-kernel-poll, sub-kernel-pipe, sub-kernel-torpor, sub-pouch-thread, sub-kernel-irqfwd | spec-scheduler, spec-tsleep, spec-sched-tickless, spec-sched-rebalance, spec-death-wake, spec-reader-frame, spec-9p-client, spec-net-poll, spec-net-poll-teardown, spec-poll, spec-pipe, gate-smp |
<!-- generated:end -->
