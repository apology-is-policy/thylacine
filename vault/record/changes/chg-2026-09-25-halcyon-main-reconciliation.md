---
id: chg-2026-09-25-halcyon-main-reconciliation
type: chg
title: "Reconcile Main loader and cleared TC-1a with Astra interaction foundations"
date: 2026-09-25
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, sub-halcyond-service-wire, sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-libhalcyon, sub-lantern]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
Main 473cd0c0 (including cleared TC-1a 1cc9a300) is reconciled with Astra
1f87fc69. Both documentation histories survive; the source merged without
textual conflicts. MAP_FILE syscall 126 coexists with PTY suboperations 16--21.
The separate authority/settings drafts remain uncommitted and byte-identical.
Main's checkout and branch are unchanged; uncleared Aux TC-1b is not imported.

Fresh verification: default boot 1727/1727; canonical Mac host gate 2073 tests;
compiled PTY ABI fixtures; 50/50 full default/UBSan SMP boots with zero failures
in every category; native observer and readiness; graphical session-media and
cleared Lantern legs 1--5. Paired CI artifacts, UART logs and inspected screenshots
are retained in work/hi1-main-evidence/. The final reconciliation section of
`docs/HALCYON-INTERACTION-STATUS.md` records invocation details and measured limits.

Blind to: full graphical worker-failure recovery, all display modes, bare-metal
Pi qualification, the newly requested clipboard/modal workflows and a Main
landing. SAK coverage here is the legacy chord/no-pending scene and return.
The full Linux host gate failed: native-runtime classifier gaps (HI1-R14), a
missing copied manual fixture (corrected; targeted recheck passed), and leaked
host allocator test arenas causing OOM (HI1-R16). All 12 unchanged allocator
tests passed in separate processes, but the default harness still needs cleanup.
No skipped or unsupported test is reported as passed. Review remains single-agent.
