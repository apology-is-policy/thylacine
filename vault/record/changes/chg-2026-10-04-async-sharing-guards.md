---
id: chg-2026-10-04-async-sharing-guards
type: chg
title: "Exclude private ring aliases from process sharing and COW"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-addrspace, sub-kernel-vma, sub-kernel-proc, sub-kernel-death, sub-kernel-jobctl, sub-kernel-caps]
established: []
closed: []
opened: []
depth: skeletal
---
# AS-2c sharing and fork prerequisites

Implements the approved async contract with an AS-locked exclusion guard,
retained descriptor pin and selective kernel-only ring VMA omission on COW.
Ordinary buffers keep existing semantics. No private userspace activation.

Actual-source ASan/UBSan and eleven mutations,200 competing sharing schedules,
100 final-drop races, two native source mutations and fresh CPU1 boot1830/1830
pass. Existing COW models pass three clean and seven buggy configurations.
The initial native mutant revealed a helper failure-label overwrite; explicit
first-error propagation corrects the fixture. Original and corrected evidence
is retained under work/oct4-async-service/as2c. Single-agent self-review; four
separate authority/settings drafts preserved. Broad consumer qualification and
request/retirement integration remain pending.
