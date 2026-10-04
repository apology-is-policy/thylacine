---
id: chg-2026-10-04-async-native-admission
type: chg
title: "Prepare exact native service admission before backlog publication"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-devsrv, sub-kernel-perm, sub-kernel-proc, sub-kernel-death, sub-kernel-caps, sub-kernel-jobctl]
established: []
closed: []
opened: []
depth: skeletal
---
# AS-2b native admission prerequisite

Implements the approved asynchronous lifecycle's internal admission separation.
Exact registry view, fixed service slot and post generation identify a target;
creator stripes and its pinned address space identify the submitting image.
Permission/identity values survive completion without a Proc pointer. Ordinary
DAC and native open share these helpers; private preparation never handshakes.

All local connection resources are reserved before a separate registry-locked
publication. The enclosing scope must serialize that step with its abort latch;
this checkpoint alone does not provide an asynchronous scope. Wakeups occur
after caller locks, and server references retain storage/credits until released.
No new ABI activation, sharing/exec hook or completed clipboard claim.

Actual-source ASan/UBSan and ten intended counterexamples pass; six ARM64 units
compile; fresh CI CPU1 boot1830/1830 passes after correcting the new repost test
to exercise actual poster death. The three required Corvus connection mutants produce the expected aggregate
Invariants counterexamples. Full clean Corvus remains suspended; its accidental
partial run was stopped and is not counted as verification.
Evidence and the initial failed native fixture are preserved in
work/oct4-async-service/as2b. Single-agent self-review; four drafts unchanged.
