---
id: chg-2026-09-24-astra-seal-integration
type: chg
title: "Integrate the cleared Aux seal prerequisite into Astra"
date: 2026-09-24
arc: arc-user-authority
commits: ["*(pending)*"]
touched: [sub-kernel-proc, sub-kernel-devproc, sub-kernel-syscall-dispatch, sub-substrate-build]
established: []
closed: []
opened: []
mirrors-checked: ["usr/Cargo.toml", "tools/build.sh", "kernel/test/test_loom.c", "kernel/test/test_9p_client.c"]
depth: skeletal
created: 2026-09-24
---
Integrate verified Aux3fd54782 into the permanent Astra branch, preserving
main's Loom resource-exemption argument and Aux's ring identity binding.
Seven new test call sites pass false, coordinated with Main on Yip0123.
Build lists retain both branches' binaries and the authority crate.1691/1691
kernel tests and debug-probe pass; production shape builds. External Alpine
and Clade fixture gates remain skipped, not qualified. Main integration waits
for Main's B-1c close; no UA-P0 repair is part of this merge.
